#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <ksmedia.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using CoCreateInstanceFn = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
using EnumAudioEndpointsFn = HRESULT(STDMETHODCALLTYPE*)(IMMDeviceEnumerator*, EDataFlow, DWORD, IMMDeviceCollection**);
using GetDefaultAudioEndpointFn = HRESULT(STDMETHODCALLTYPE*)(IMMDeviceEnumerator*, EDataFlow, ERole, IMMDevice**);
using GetDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IMMDeviceEnumerator*, LPCWSTR, IMMDevice**);
using ActivateFn = HRESULT(STDMETHODCALLTYPE*)(IMMDevice*, REFIID, DWORD, PROPVARIANT*, void**);
using QueryInterfaceFn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, REFIID, void**);
using InitializeFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME, const WAVEFORMATEX*, LPCGUID);
using InitializeSharedAudioStreamFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient3*, DWORD, UINT32, const WAVEFORMATEX*, LPCGUID);
using GetServiceFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, REFIID, void**);
using GetBufferSizeFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, UINT32*);
using GetStreamLatencyFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, REFERENCE_TIME*);
using GetCurrentPaddingFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, UINT32*);
using IsFormatSupportedFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, AUDCLNT_SHAREMODE, const WAVEFORMATEX*, WAVEFORMATEX**);
using GetMixFormatFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, WAVEFORMATEX**);
using GetDevicePeriodFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, REFERENCE_TIME*, REFERENCE_TIME*);
using AudioClientSimpleFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*);
using SetEventHandleFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, HANDLE);
using GetSharedModeEnginePeriodFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient3*, const WAVEFORMATEX*, UINT32*, UINT32*, UINT32*, UINT32*);
using GetCurrentSharedModeEnginePeriodFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient3*, WAVEFORMATEX**, UINT32*);
using GetBufferFn = HRESULT(STDMETHODCALLTYPE*)(IAudioRenderClient*, UINT32, BYTE**);
using ReleaseBufferFn = HRESULT(STDMETHODCALLTYPE*)(IAudioRenderClient*, UINT32, DWORD);
using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
using TimePeriodFn = UINT(WINAPI*)(UINT);
using CreateProcessWFn = BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
using CreateProcessAFn = BOOL(WINAPI*)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);

struct AudioClientState {
    WAVEFORMATEXTENSIBLE format{};
    bool hasFormat = false;
    AUDCLNT_SHAREMODE shareMode = AUDCLNT_SHAREMODE_SHARED;
    DWORD streamFlags = 0;
    bool fakeOutput = false;
    bool fakeInitialized = false;
    bool fakeStarted = false;
    UINT32 fakeBufferFrames = 480;
    UINT32 fakePeriodFrames = 480;
    REFERENCE_TIME fakeDefaultPeriod = 100000;
    REFERENCE_TIME fakeMinPeriod = 30000;
    HANDLE fakeEvent = nullptr;
    std::uint64_t fakeFramesReleased = 0;
    ULONGLONG fakeStartTick = 0;
    LONGLONG fakeNextEventQpc = 0;
};

struct RenderClientState {
    IAudioClient* audioClient = nullptr;
    BYTE* pendingBuffer = nullptr;
    UINT32 pendingFrames = 0;
    bool loggedPcm = false;
    bool pcmActive = false;
    ULONGLONG lastPcmTick = 0;
    ULONGLONG lastActiveLogTick = 0;
    std::uint64_t activeFrames = 0;
    std::uint64_t activeBytes = 0;
};

HMODULE g_module = nullptr;
#ifndef AUDIOBRIDGE_WASAPI_HOOK_PIPE_NAME
#define AUDIOBRIDGE_WASAPI_HOOK_PIPE_NAME L"\\\\.\\pipe\\AudioBridgeWasapiHook"
#endif
#ifndef AUDIOBRIDGE_WASAPI_HOOK_CONTROL_MAP_NAME
#define AUDIOBRIDGE_WASAPI_HOOK_CONTROL_MAP_NAME L"Local\\AudioBridgeWasapiHookControl"
#endif

#define AUDIOBRIDGE_HOOK_READY_EVENT_PREFIX L"Local\\AudioBridgeHookReady_"
constexpr wchar_t kPipeName[] = AUDIOBRIDGE_WASAPI_HOOK_PIPE_NAME;
constexpr wchar_t kControlMapName[] = AUDIOBRIDGE_WASAPI_HOOK_CONTROL_MAP_NAME;
constexpr DWORD kPipeMagic = 0x48504241;  // ABPH
constexpr DWORD kPipeText = 1;
constexpr DWORD kPipeFormat = 2;
constexpr DWORD kPipePcm = 3;
constexpr DWORD kPipeFinish = 4;

struct PipeMessageHeader {
    DWORD magic = kPipeMagic;
    DWORD type = 0;
    DWORD pid = 0;
    DWORD reserved = 0;
    std::uint64_t payloadBytes = 0;
};

struct PipeFormatMessage {
    DWORD formatBytes = sizeof(WAVEFORMATEXTENSIBLE);
    WAVEFORMATEXTENSIBLE format{};
    DWORD streamFlags = 0;
    DWORD shareMode = 0;
    DWORD periodFrames = 0;
};

struct HookControlBlock {
    volatile LONG lockedPid = 0;
    volatile LONG finish = 0;
    volatile LONG fakeOutput = 0;
};

struct FakeRenderClient {
    void** vtable = nullptr;
    std::atomic<ULONG> refs{1};
    IAudioClient* audioClient = nullptr;
    std::vector<BYTE> buffer;
    bool bufferOutstanding = false;
};

struct FakeAudioClock {
    void** vtable = nullptr;
    std::atomic<ULONG> refs{1};
    IAudioClient* audioClient = nullptr;
};

std::mutex g_logMutex;
std::mutex g_pipeMutex;
std::mutex g_stateMutex;
std::unordered_map<void**, void*> g_patchedSlots;
std::unordered_set<HMODULE> g_scannedModules;
std::unordered_map<IAudioClient*, AudioClientState> g_audioClients;
std::unordered_map<IAudioRenderClient*, RenderClientState> g_renderClients;
std::atomic<bool> g_running{false};
std::atomic<bool> g_hooksDisabled{false};
std::atomic<DWORD> g_lockedAudioPid{0};
std::atomic<bool> g_finishCapture{false};
std::atomic<bool> g_fakeOutput{false};
std::atomic<std::uint32_t> g_audioClientCount{0};
std::atomic<std::uint32_t> g_renderClientCount{0};
thread_local bool g_isBootstrapping = false;

CoCreateInstanceFn g_originalCoCreateInstance = nullptr;
EnumAudioEndpointsFn g_originalEnumAudioEndpoints = nullptr;
GetDefaultAudioEndpointFn g_originalGetDefaultAudioEndpoint = nullptr;
GetDeviceFn g_originalGetDevice = nullptr;
ActivateFn g_originalActivate = nullptr;
QueryInterfaceFn g_originalAudioClientQueryInterface = nullptr;
InitializeFn g_originalInitialize = nullptr;
InitializeSharedAudioStreamFn g_originalInitializeSharedAudioStream = nullptr;
GetServiceFn g_originalGetService = nullptr;
GetBufferSizeFn g_originalGetBufferSize = nullptr;
GetStreamLatencyFn g_originalGetStreamLatency = nullptr;
GetCurrentPaddingFn g_originalGetCurrentPadding = nullptr;
IsFormatSupportedFn g_originalIsFormatSupported = nullptr;
GetMixFormatFn g_originalGetMixFormat = nullptr;
GetDevicePeriodFn g_originalGetDevicePeriod = nullptr;
AudioClientSimpleFn g_originalStart = nullptr;
AudioClientSimpleFn g_originalStop = nullptr;
AudioClientSimpleFn g_originalReset = nullptr;
SetEventHandleFn g_originalSetEventHandle = nullptr;
GetSharedModeEnginePeriodFn g_originalGetSharedModeEnginePeriod = nullptr;
GetCurrentSharedModeEnginePeriodFn g_originalGetCurrentSharedModeEnginePeriod = nullptr;
GetBufferFn g_originalGetBuffer = nullptr;
ReleaseBufferFn g_originalReleaseBuffer = nullptr;
ReleaseFn g_originalAudioClientRelease = nullptr;
ReleaseFn g_originalRenderClientRelease = nullptr;
CreateProcessWFn g_originalCreateProcessW = nullptr;
CreateProcessAFn g_originalCreateProcessA = nullptr;
thread_local bool g_insideCreateProcessHook = false;
HANDLE g_pipe = INVALID_HANDLE_VALUE;
HANDLE g_controlMapping = nullptr;
HookControlBlock* g_control = nullptr;
HMODULE g_winmm = nullptr;
TimePeriodFn g_timeBeginPeriod = nullptr;
TimePeriodFn g_timeEndPeriod = nullptr;
std::atomic<bool> g_timerResolutionActive{false};

bool EnsurePipeConnectedLocked() {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        return true;
    }

    g_pipe = CreateFileW(kPipeName,
                         GENERIC_WRITE,
                         0,
                         nullptr,
                         OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL,
                         nullptr);
    return g_pipe != INVALID_HANDLE_VALUE;
}

void ClosePipeLocked() {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
}

void SendPipeMessage(DWORD type, const void* payload, std::uint64_t payloadBytes) {
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    if (!EnsurePipeConnectedLocked()) {
        return;
    }

    PipeMessageHeader header{};
    header.type = type;
    header.pid = GetCurrentProcessId();
    header.payloadBytes = payloadBytes;

    DWORD written = 0;
    bool ok = WriteFile(g_pipe, &header, sizeof(header), &written, nullptr) &&
              written == sizeof(header);
    if (ok && payloadBytes > 0) {
        ok = WriteFile(g_pipe,
                       payload,
                       static_cast<DWORD>(payloadBytes),
                       &written,
                       nullptr) &&
             written == payloadBytes;
    }
    if (!ok) {
        ClosePipeLocked();
    }
}

void Log(const char* format, ...) {
    char message[2048]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    SYSTEMTIME now{};
    GetLocalTime(&now);

    char line[2300]{};
    std::snprintf(line,
                  sizeof(line),
                  "[%02u:%02u:%02u.%03u] [pid=%lu] %s\r\n",
                  now.wHour,
                  now.wMinute,
                  now.wSecond,
                  now.wMilliseconds,
                  GetCurrentProcessId(),
                  message);

    OutputDebugStringA(line);
    SendPipeMessage(kPipeText, line, std::strlen(line));
}

std::string GuidToString(REFGUID guid) {
    wchar_t wide[64]{};
    StringFromGUID2(guid, wide, static_cast<int>(std::size(wide)));
    char narrow[64]{};
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, static_cast<int>(std::size(narrow)), nullptr, nullptr);
    return narrow;
}

std::uint16_t BytesPerSample(const WAVEFORMATEX& format);

const char* KnownIidName(REFIID iid) {
    if (iid == __uuidof(IAudioClient)) {
        return "IAudioClient";
    }
    if (iid == __uuidof(IAudioClient2)) {
        return "IAudioClient2";
    }
    if (iid == __uuidof(IAudioClient3)) {
        return "IAudioClient3";
    }
    if (iid == __uuidof(IAudioRenderClient)) {
        return "IAudioRenderClient";
    }
    if (iid == __uuidof(IAudioClock)) {
        return "IAudioClock";
    }
    if (iid == __uuidof(IAudioSessionControl)) {
        return "IAudioSessionControl";
    }
    if (iid == __uuidof(IAudioSessionControl2)) {
        return "IAudioSessionControl2";
    }
    if (iid == __uuidof(ISimpleAudioVolume)) {
        return "ISimpleAudioVolume";
    }
    if (iid == __uuidof(IChannelAudioVolume)) {
        return "IChannelAudioVolume";
    }
    if (iid == __uuidof(IAudioStreamVolume)) {
        return "IAudioStreamVolume";
    }
    if (iid == __uuidof(IUnknown)) {
        return "IUnknown";
    }
    return "unknown";
}

std::size_t WaveFormatByteSize(const WAVEFORMATEX& format) {
    const auto requested = sizeof(WAVEFORMATEX) + static_cast<std::size_t>(format.cbSize);
    return std::min<std::size_t>(sizeof(WAVEFORMATEXTENSIBLE), requested);
}

UINT32 DefaultFramesForRate(UINT32 sampleRate, UINT32 divisor, UINT32 fallback) {
    if (sampleRate == 0 || divisor == 0) {
        return fallback;
    }
    return std::max<UINT32>(1, sampleRate / divisor);
}

UINT32 ClampFakeFrames(UINT32 frames) {
    return std::min<UINT32>(8192, std::max<UINT32>(32, frames));
}

UINT32 FramesFromHns(REFERENCE_TIME hns, UINT32 sampleRate, UINT32 fallbackFrames) {
    if (hns <= 0 || sampleRate == 0) {
        return ClampFakeFrames(fallbackFrames);
    }
    const auto frames = (static_cast<std::uint64_t>(hns) * sampleRate + 9999999ull) / 10000000ull;
    return ClampFakeFrames(static_cast<UINT32>(std::min<std::uint64_t>(frames, 8192ull)));
}

REFERENCE_TIME HnsFromFrames(UINT32 frames, UINT32 sampleRate, REFERENCE_TIME fallbackHns) {
    if (frames == 0 || sampleRate == 0) {
        return fallbackHns;
    }
    return static_cast<REFERENCE_TIME>(
            (static_cast<std::uint64_t>(frames) * 10000000ull) / sampleRate);
}

LONGLONG QpcNow() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

LONGLONG QpcTicksForFakePeriod(const AudioClientState& state) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const UINT32 sampleRate = state.format.Format.nSamplesPerSec != 0
            ? state.format.Format.nSamplesPerSec
            : 48000;
    const UINT32 periodFrames = state.fakePeriodFrames != 0
            ? state.fakePeriodFrames
            : DefaultFramesForRate(sampleRate, 100, 480);
    return std::max<LONGLONG>(
            1,
            (static_cast<LONGLONG>(periodFrames) * frequency.QuadPart + sampleRate - 1) /
                    sampleRate);
}

WAVEFORMATEXTENSIBLE DefaultMixFormat() {
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = 2;
    format.Format.nSamplesPerSec = 48000;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign =
            static_cast<WORD>(format.Format.nChannels * format.Format.wBitsPerSample / 8);
    format.Format.nAvgBytesPerSec =
            format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return format;
}

bool IsUsableWaveFormat(const WAVEFORMATEX* format) {
    return format != nullptr &&
           format->nSamplesPerSec != 0 &&
           format->nChannels != 0 &&
           format->wBitsPerSample != 0 &&
           (format->nBlockAlign != 0 ||
            format->nChannels * BytesPerSample(*format) != 0);
}

std::uint16_t BytesPerSample(const WAVEFORMATEX& format) {
    return static_cast<std::uint16_t>((format.wBitsPerSample + 7U) / 8U);
}

std::uint32_t BytesPerFrame(const AudioClientState& state) {
    if (!state.hasFormat) {
        return 0;
    }
    if (state.format.Format.nBlockAlign != 0) {
        return state.format.Format.nBlockAlign;
    }
    return state.format.Format.nChannels * BytesPerSample(state.format.Format);
}

const char* SampleTypeName(const AudioClientState& state) {
    if (!state.hasFormat) {
        return "unknown";
    }
    const WAVEFORMATEX& format = state.format.Format;
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return "float";
    }
    if (format.wFormatTag == WAVE_FORMAT_PCM) {
        return "pcm-int";
    }
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (IsEqualGUID(state.format.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            return "float";
        }
        if (IsEqualGUID(state.format.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            return "pcm-int";
        }
    }
    return "unknown";
}

struct PcmBufferStats {
    std::uint64_t inspectedBytes = 0;
    std::uint64_t nonZeroBytes = 0;
    std::uint32_t checksum = 2166136261u;
};

PcmBufferStats InspectPcmBuffer(const BYTE* data, std::uint64_t byteCount) {
    PcmBufferStats stats{};
    if (data == nullptr || byteCount == 0) {
        return stats;
    }

    stats.inspectedBytes = std::min<std::uint64_t>(byteCount, 4096);
    for (std::uint64_t i = 0; i < stats.inspectedBytes; ++i) {
        const BYTE value = data[i];
        if (value != 0) {
            ++stats.nonZeroBytes;
        }
        stats.checksum ^= value;
        stats.checksum *= 16777619u;
    }
    return stats;
}

void LogWaveFormat(const char* source,
                   const void* client,
                   const WAVEFORMATEX* format,
                   AUDCLNT_SHAREMODE shareMode,
                   DWORD streamFlags,
                   UINT32 periodInFrames) {
    if (format == nullptr) {
        Log("%s client=%p format=null shareMode=%u flags=0x%08lX periodFrames=%u",
            source,
            client,
            static_cast<unsigned>(shareMode),
            streamFlags,
            periodInFrames);
        return;
    }

    const bool extensible =
            format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    const auto* extensibleFormat = extensible
            ? reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)
            : nullptr;
    const WORD validBits = extensibleFormat != nullptr
            ? extensibleFormat->Samples.wValidBitsPerSample
            : format->wBitsPerSample;
    const DWORD channelMask = extensibleFormat != nullptr
            ? extensibleFormat->dwChannelMask
            : 0;
    const char* sampleType = "unknown";
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        sampleType = "float";
    } else if (format->wFormatTag == WAVE_FORMAT_PCM) {
        sampleType = "pcm-int";
    } else if (extensibleFormat != nullptr &&
               IsEqualGUID(extensibleFormat->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
        sampleType = "float";
    } else if (extensibleFormat != nullptr &&
               IsEqualGUID(extensibleFormat->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
        sampleType = "pcm-int";
    }

    Log("%s client=%p formatTag=0x%04X sampleRate=%u channels=%u bits=%u validBits=%u blockAlign=%u avgBytesPerSec=%u cbSize=%u sampleType=%s channelMask=0x%08lX subFormat=%s shareMode=%u flags=0x%08lX periodFrames=%u",
        source,
        client,
        format->wFormatTag,
        format->nSamplesPerSec,
        format->nChannels,
        format->wBitsPerSample,
        validBits,
        format->nBlockAlign,
        format->nAvgBytesPerSec,
        format->cbSize,
        sampleType,
        channelMask,
        extensibleFormat != nullptr ? GuidToString(extensibleFormat->SubFormat).c_str() : "n/a",
        static_cast<unsigned>(shareMode),
        streamFlags,
        periodInFrames);

    PipeFormatMessage message{};
    CopyMemory(&message.format,
               format,
               std::min<std::size_t>(sizeof(message.format),
                                     sizeof(WAVEFORMATEX) + format->cbSize));
    message.streamFlags = streamFlags;
    message.shareMode = static_cast<DWORD>(shareMode);
    message.periodFrames = periodInFrames;
    SendPipeMessage(kPipeFormat, &message, sizeof(message));
}

void CopyWaveFormat(const WAVEFORMATEX* source, AudioClientState* state) {
    if (source == nullptr || state == nullptr) {
        return;
    }

    std::memset(&state->format, 0, sizeof(state->format));
    const auto copySize = std::min<std::size_t>(
            sizeof(WAVEFORMATEXTENSIBLE),
            sizeof(WAVEFORMATEX) + source->cbSize);
    std::memcpy(&state->format, source, copySize);
    state->hasFormat = true;
}

WAVEFORMATEX* CoTaskMemCopyWaveFormat(const WAVEFORMATEXTENSIBLE& source) {
    const std::size_t bytes = WaveFormatByteSize(source.Format);
    auto* copy = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(bytes));
    if (copy == nullptr) {
        return nullptr;
    }
    std::memcpy(copy, &source, bytes);
    return copy;
}

AudioClientState SnapshotAudioClient(IAudioClient* client) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    const auto it = g_audioClients.find(client);
    return it != g_audioClients.end() ? it->second : AudioClientState{};
}

void StoreFakeInitialization(IAudioClient* client,
                             const WAVEFORMATEX* format,
                             AUDCLNT_SHAREMODE shareMode,
                             DWORD streamFlags,
                             REFERENCE_TIME bufferDuration,
                             REFERENCE_TIME periodicity,
                             UINT32 periodFrames) {
    const UINT32 sampleRate = format != nullptr ? format->nSamplesPerSec : 48000;
    const UINT32 defaultFrames = DefaultFramesForRate(sampleRate, 100, 480);
    const UINT32 selectedPeriodFrames =
            periodFrames != 0 ? ClampFakeFrames(periodFrames) : defaultFrames;
    const UINT32 selectedBufferFrames =
            bufferDuration > 0
                    ? FramesFromHns(bufferDuration, sampleRate, selectedPeriodFrames)
                    : ClampFakeFrames(std::max<UINT32>(selectedPeriodFrames, defaultFrames));

    std::lock_guard<std::mutex> lock(g_stateMutex);
    auto& state = g_audioClients[client];
    CopyWaveFormat(format, &state);
    if (!state.hasFormat) {
        state.format = DefaultMixFormat();
        state.hasFormat = true;
    }
    state.shareMode = shareMode;
    state.streamFlags = streamFlags;
    state.fakeOutput = true;
    state.fakeInitialized = true;
    state.fakeStarted = false;
    state.fakeBufferFrames = selectedBufferFrames;
    state.fakePeriodFrames = selectedPeriodFrames;
    state.fakeDefaultPeriod = HnsFromFrames(selectedPeriodFrames, sampleRate, 100000);
    state.fakeMinPeriod = periodicity > 0
            ? periodicity
            : HnsFromFrames(std::max<UINT32>(1, DefaultFramesForRate(sampleRate, 333, 144)),
                            sampleRate,
                            30000);
    state.fakeFramesReleased = 0;
    state.fakeStartTick = 0;
    state.fakeNextEventQpc = 0;
}

bool PatchPointer(void** slot, void* hook, void** original) {
    if (slot == nullptr || hook == nullptr) {
        return false;
    }
    if (g_hooksDisabled.load()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_stateMutex);
    if (*slot == hook || g_patchedSlots.count(slot) != 0) {
        return true;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    if (original != nullptr && *original == nullptr) {
        *original = *slot;
    }
    g_patchedSlots.emplace(slot, *slot);
    *slot = hook;

    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

void RestorePatchedPointers() {
    std::unordered_map<void**, void*> patchedSlots;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        patchedSlots.swap(g_patchedSlots);
    }

    for (const auto& item : patchedSlots) {
        void** slot = item.first;
        void* original = item.second;
        DWORD oldProtect = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *slot = original;
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
        }
    }
}

void PatchVtableEntry(void* object, std::size_t index, void* hook, void** original) {
    if (object == nullptr) {
        return;
    }
    auto*** vtableAddress = reinterpret_cast<void***>(object);
    if (*vtableAddress == nullptr) {
        return;
    }
    PatchPointer(&((*vtableAddress)[index]), hook, original);
}

void PatchDevice(IMMDevice* device);
void PatchAudioClient(IAudioClient* client);
void PatchAudioClient2(IAudioClient2* client);
void PatchAudioClient3(IAudioClient3* client);
void PatchAudioClientForIid(void* client, REFIID iid);
void PatchRenderClient(IAudioRenderClient* renderClient);
void BootstrapAudioClientVtables(IMMDevice* device);
void OpenControlMapping();
bool FakeOutputEnabled();
void PumpFakeEvents();

bool IsAudioClientIid(REFIID iid) {
    return iid == __uuidof(IAudioClient)
        || iid == __uuidof(IAudioClient2)
        || iid == __uuidof(IAudioClient3);
}

IAudioClient* AudioClientBaseForIid(void* client, REFIID iid) {
    if (client == nullptr) {
        return nullptr;
    }
    if (iid == __uuidof(IAudioClient3)) {
        return static_cast<IAudioClient*>(static_cast<IAudioClient3*>(client));
    }
    if (iid == __uuidof(IAudioClient2)) {
        return static_cast<IAudioClient*>(static_cast<IAudioClient2*>(client));
    }
    if (iid == __uuidof(IAudioClient)) {
        return static_cast<IAudioClient*>(client);
    }
    return nullptr;
}

bool RegisterAudioClient(IAudioClient* client) {
    if (client == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_audioClients.try_emplace(client).second;
}

bool RegisterRenderClient(IAudioRenderClient* renderClient, IAudioClient* audioClient) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    auto [it, inserted] = g_renderClients.try_emplace(renderClient);
    it->second.audioClient = audioClient;
    return inserted;
}

struct CaptureResult {
    BYTE* data = nullptr;
    std::uint64_t bytes = 0;
};

CaptureResult CaptureReleasedBuffer(IAudioRenderClient* self, UINT32 frameCount, DWORD flags) {
    RenderClientState renderState{};
    AudioClientState audioState{};
    bool shouldLogPcm = false;
    bool shouldLogActive = false;
    bool shouldLogResume = false;
    std::uint64_t activeFrames = 0;
    std::uint64_t activeBytes = 0;
    std::uint64_t bytes = 0;
    const ULONGLONG now = GetTickCount64();

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        auto renderIt = g_renderClients.find(self);
        if (renderIt != g_renderClients.end()) {
            renderState = renderIt->second;
            if (!renderIt->second.loggedPcm &&
                renderState.pendingBuffer != nullptr &&
                frameCount > 0 &&
                (flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0) {
                shouldLogPcm = true;
                renderIt->second.loggedPcm = true;
            }

            auto audioIt = g_audioClients.find(renderState.audioClient);
            if (audioIt != g_audioClients.end()) {
                audioState = audioIt->second;
                if (audioIt->second.fakeOutput && frameCount > 0) {
                    audioIt->second.fakeFramesReleased += frameCount;
                }
            }

            const bool hasPcm =
                    renderState.pendingBuffer != nullptr &&
                    frameCount > 0 &&
                    (flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 &&
                    audioState.hasFormat;
            if (hasPcm) {
                const std::uint32_t bytesPerFrame = BytesPerFrame(audioState);
                bytes = static_cast<std::uint64_t>(frameCount) *
                        static_cast<std::uint64_t>(bytesPerFrame);

                if (!renderIt->second.pcmActive) {
                    shouldLogResume = true;
                    renderIt->second.pcmActive = true;
                    renderIt->second.lastActiveLogTick = now;
                    renderIt->second.activeFrames = 0;
                    renderIt->second.activeBytes = 0;
                }

                renderIt->second.lastPcmTick = now;
                renderIt->second.activeFrames += frameCount;
                renderIt->second.activeBytes += bytes;
                if (now - renderIt->second.lastActiveLogTick >= 1000) {
                    shouldLogActive = true;
                    activeFrames = renderIt->second.activeFrames;
                    activeBytes = renderIt->second.activeBytes;
                    renderIt->second.activeFrames = 0;
                    renderIt->second.activeBytes = 0;
                    renderIt->second.lastActiveLogTick = now;
                }
            }
            renderIt->second.pendingBuffer = nullptr;
            renderIt->second.pendingFrames = 0;
        }
    }

    const PcmBufferStats stats = InspectPcmBuffer(renderState.pendingBuffer, bytes);
    const bool captureEnabled =
            g_lockedAudioPid.load() == GetCurrentProcessId() &&
            !g_finishCapture.load();
    if (captureEnabled && bytes > 0 && audioState.hasFormat) {
        SendPipeMessage(kPipePcm, renderState.pendingBuffer, bytes);
    }
    if (shouldLogPcm) {
        Log("PCM captured once. render=%p audio=%p frames=%u bytes=%llu sampleRate=%u channels=%u bits=%u blockAlign=%u sampleType=%s formatSource=%s inspectedBytes=%llu nonZeroBytes=%llu checksum=0x%08X formatTag=0x%04X renderClients=%u audioClients=%u fakeOutput=%s",
            self,
            renderState.audioClient,
            frameCount,
            static_cast<unsigned long long>(bytes),
            audioState.format.Format.nSamplesPerSec,
            audioState.format.Format.nChannels,
            audioState.format.Format.wBitsPerSample,
            audioState.format.Format.nBlockAlign,
            SampleTypeName(audioState),
            audioState.hasFormat ? "mapped-client" : "unknown",
            static_cast<unsigned long long>(stats.inspectedBytes),
            static_cast<unsigned long long>(stats.nonZeroBytes),
            stats.checksum,
            audioState.format.Format.wFormatTag,
            g_renderClientCount.load(),
            g_audioClientCount.load(),
            audioState.fakeOutput ? "on" : "off");
    }
    if (shouldLogResume) {
        Log("PCM active started. render=%p audio=%p sampleRate=%u channels=%u bits=%u sampleType=%s formatSource=%s fakeOutput=%s",
            self,
            renderState.audioClient,
            audioState.format.Format.nSamplesPerSec,
            audioState.format.Format.nChannels,
            audioState.format.Format.wBitsPerSample,
            SampleTypeName(audioState),
            audioState.hasFormat ? "mapped-client" : "unknown",
            audioState.fakeOutput ? "on" : "off");
    }
    if (shouldLogActive) {
        Log("PCM active. render=%p audio=%p frames=%llu bytes=%llu sampleRate=%u channels=%u bits=%u sampleType=%s formatSource=%s fakeOutput=%s",
            self,
            renderState.audioClient,
            static_cast<unsigned long long>(activeFrames),
            static_cast<unsigned long long>(activeBytes),
            audioState.format.Format.nSamplesPerSec,
            audioState.format.Format.nChannels,
            audioState.format.Format.wBitsPerSample,
            SampleTypeName(audioState),
            audioState.hasFormat ? "mapped-client" : "unknown",
            audioState.fakeOutput ? "on" : "off");
    }

    return {renderState.pendingBuffer, bytes};
}

HRESULT STDMETHODCALLTYPE FakeRenderQueryInterface(IAudioRenderClient* self, REFIID iid, void** out) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioRenderClient)) {
        *out = self;
        reinterpret_cast<FakeRenderClient*>(self)->refs.fetch_add(1);
        return S_OK;
    }
    Log("Fake output unsupported IAudioRenderClient::QueryInterface iid=%s name=%s render=%p",
        GuidToString(iid).c_str(),
        KnownIidName(iid),
        self);
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE FakeRenderAddRef(IAudioRenderClient* self) {
    return reinterpret_cast<FakeRenderClient*>(self)->refs.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE FakeRenderRelease(IAudioRenderClient* self) {
    auto* fake = reinterpret_cast<FakeRenderClient*>(self);
    const ULONG refs = fake->refs.fetch_sub(1) - 1;
    if (refs == 0) {
        bool removed = false;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            removed = g_renderClients.erase(self) > 0;
        }
        if (removed) {
            const auto count = g_renderClientCount.fetch_sub(1) - 1;
            Log("Fake IAudioRenderClient released. active renderClients=%u render=%p audio=%p",
                count,
                self,
                fake->audioClient);
        }
        delete fake;
    }
    return refs;
}

HRESULT STDMETHODCALLTYPE FakeRenderGetBuffer(IAudioRenderClient* self, UINT32 frameCount, BYTE** data) {
    if (data == nullptr) {
        return E_POINTER;
    }
    *data = nullptr;

    auto* fake = reinterpret_cast<FakeRenderClient*>(self);
    if (fake->bufferOutstanding) {
        Log("Fake output GetBuffer out-of-order. render=%p frames=%u", self, frameCount);
        return AUDCLNT_E_OUT_OF_ORDER;
    }

    const AudioClientState audioState = SnapshotAudioClient(fake->audioClient);
    if (!audioState.fakeInitialized || !audioState.hasFormat) {
        Log("Fake output GetBuffer before Initialize. render=%p audio=%p frames=%u",
            self,
            fake->audioClient,
            frameCount);
        return AUDCLNT_E_NOT_INITIALIZED;
    }

    const std::uint32_t bytesPerFrame = BytesPerFrame(audioState);
    const std::uint64_t bytes =
            static_cast<std::uint64_t>(frameCount) * static_cast<std::uint64_t>(bytesPerFrame);
    if (bytes > 64ull * 1024ull * 1024ull) {
        Log("Fake output GetBuffer rejected huge request. render=%p frames=%u bytes=%llu",
            self,
            frameCount,
            static_cast<unsigned long long>(bytes));
        return E_OUTOFMEMORY;
    }

    try {
        fake->buffer.assign(static_cast<std::size_t>(bytes), 0);
    } catch (...) {
        return E_OUTOFMEMORY;
    }

    *data = fake->buffer.empty() ? nullptr : fake->buffer.data();
    fake->bufferOutstanding = true;

    std::lock_guard<std::mutex> lock(g_stateMutex);
    auto& state = g_renderClients[self];
    state.audioClient = fake->audioClient;
    state.pendingBuffer = *data;
    state.pendingFrames = frameCount;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeRenderReleaseBuffer(IAudioRenderClient* self, UINT32 frameCount, DWORD flags) {
    auto* fake = reinterpret_cast<FakeRenderClient*>(self);
    if (!fake->bufferOutstanding) {
        Log("Fake output ReleaseBuffer out-of-order. render=%p frames=%u flags=0x%08lX",
            self,
            frameCount,
            flags);
        return AUDCLNT_E_OUT_OF_ORDER;
    }
    fake->bufferOutstanding = false;
    CaptureReleasedBuffer(self, frameCount, flags);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeClockQueryInterface(IAudioClock* self, REFIID iid, void** out) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioClock)) {
        *out = self;
        reinterpret_cast<FakeAudioClock*>(self)->refs.fetch_add(1);
        return S_OK;
    }
    Log("Fake output unsupported IAudioClock::QueryInterface iid=%s name=%s clock=%p",
        GuidToString(iid).c_str(),
        KnownIidName(iid),
        self);
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE FakeClockAddRef(IAudioClock* self) {
    return reinterpret_cast<FakeAudioClock*>(self)->refs.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE FakeClockRelease(IAudioClock* self) {
    auto* fake = reinterpret_cast<FakeAudioClock*>(self);
    const ULONG refs = fake->refs.fetch_sub(1) - 1;
    if (refs == 0) {
        Log("Fake IAudioClock released. clock=%p audio=%p", self, fake->audioClient);
        delete fake;
    }
    return refs;
}

HRESULT STDMETHODCALLTYPE FakeClockGetFrequency(IAudioClock* self, UINT64* frequency) {
    if (frequency == nullptr) {
        return E_POINTER;
    }
    const auto* fake = reinterpret_cast<FakeAudioClock*>(self);
    const AudioClientState audioState = SnapshotAudioClient(fake->audioClient);
    *frequency = audioState.hasFormat && audioState.format.Format.nSamplesPerSec != 0
            ? audioState.format.Format.nSamplesPerSec
            : 48000;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeClockGetPosition(IAudioClock* self, UINT64* position, UINT64* qpcPosition) {
    if (position == nullptr) {
        return E_POINTER;
    }
    const auto* fake = reinterpret_cast<FakeAudioClock*>(self);
    const AudioClientState audioState = SnapshotAudioClient(fake->audioClient);
    *position = audioState.fakeFramesReleased;
    if (qpcPosition != nullptr) {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        *qpcPosition = static_cast<UINT64>(qpc.QuadPart);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeClockGetCharacteristics(IAudioClock*, DWORD* characteristics) {
    if (characteristics == nullptr) {
        return E_POINTER;
    }
    *characteristics = 0;
    return S_OK;
}

void* g_fakeRenderVtable[] = {
        reinterpret_cast<void*>(&FakeRenderQueryInterface),
        reinterpret_cast<void*>(&FakeRenderAddRef),
        reinterpret_cast<void*>(&FakeRenderRelease),
        reinterpret_cast<void*>(&FakeRenderGetBuffer),
        reinterpret_cast<void*>(&FakeRenderReleaseBuffer),
};

void* g_fakeClockVtable[] = {
        reinterpret_cast<void*>(&FakeClockQueryInterface),
        reinterpret_cast<void*>(&FakeClockAddRef),
        reinterpret_cast<void*>(&FakeClockRelease),
        reinterpret_cast<void*>(&FakeClockGetFrequency),
        reinterpret_cast<void*>(&FakeClockGetPosition),
        reinterpret_cast<void*>(&FakeClockGetCharacteristics),
};

HRESULT STDMETHODCALLTYPE HookEnumAudioEndpoints(IMMDeviceEnumerator* self,
                                                 EDataFlow dataFlow,
                                                 DWORD stateMask,
                                                 IMMDeviceCollection** devices) {
    const HRESULT hr = g_originalEnumAudioEndpoints(self, dataFlow, stateMask, devices);
    if (SUCCEEDED(hr) && devices != nullptr && *devices != nullptr) {
        UINT count = 0;
        if (SUCCEEDED((*devices)->GetCount(&count))) {
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* device = nullptr;
                if (SUCCEEDED((*devices)->Item(i, &device)) && device != nullptr) {
                    PatchDevice(device);
                    device->Release();
                }
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookGetDefaultAudioEndpoint(IMMDeviceEnumerator* self,
                                                      EDataFlow dataFlow,
                                                      ERole role,
                                                      IMMDevice** endpoint) {
    const HRESULT hr = g_originalGetDefaultAudioEndpoint(self, dataFlow, role, endpoint);
    if (SUCCEEDED(hr) && endpoint != nullptr && *endpoint != nullptr) {
        PatchDevice(*endpoint);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookGetDevice(IMMDeviceEnumerator* self,
                                        LPCWSTR id,
                                        IMMDevice** device) {
    const HRESULT hr = g_originalGetDevice(self, id, device);
    if (SUCCEEDED(hr) && device != nullptr && *device != nullptr) {
        PatchDevice(*device);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookActivate(IMMDevice* self,
                                       REFIID iid,
                                       DWORD clsCtx,
                                       PROPVARIANT* activationParams,
                                       void** interfaceOut) {
    const HRESULT hr = g_originalActivate(self, iid, clsCtx, activationParams, interfaceOut);
    if (SUCCEEDED(hr) && interfaceOut != nullptr && *interfaceOut != nullptr && IsAudioClientIid(iid)) {
        PatchAudioClientForIid(*interfaceOut, iid);
        if (g_isBootstrapping) {
            return hr;
        }
        auto* client = AudioClientBaseForIid(*interfaceOut, iid);
        if (RegisterAudioClient(client)) {
            const auto count = ++g_audioClientCount;
            Log("IAudioClient created. iid=%s active clients=%u ptr=%p",
                GuidToString(iid).c_str(),
                count,
                client);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookAudioClientQueryInterface(IUnknown* self, REFIID iid, void** out) {
    const HRESULT hr = g_originalAudioClientQueryInterface(self, iid, out);
    if (!g_isBootstrapping && FakeOutputEnabled()) {
        Log("Fake output observed IAudioClient::QueryInterface. self=%p iid=%s name=%s hr=0x%08lX out=%p",
            self,
            GuidToString(iid).c_str(),
            KnownIidName(iid),
            static_cast<unsigned long>(hr),
            out != nullptr ? *out : nullptr);
    }
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && IsAudioClientIid(iid)) {
        PatchAudioClientForIid(*out, iid);
        if (!g_isBootstrapping) {
            auto* client = AudioClientBaseForIid(*out, iid);
            if (RegisterAudioClient(client)) {
                const auto count = ++g_audioClientCount;
                Log("IAudioClient queried. iid=%s active clients=%u ptr=%p",
                    GuidToString(iid).c_str(),
                    count,
                    client);
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookInitialize(IAudioClient* self,
                                        AUDCLNT_SHAREMODE shareMode,
                                        DWORD streamFlags,
                                        REFERENCE_TIME bufferDuration,
                                        REFERENCE_TIME periodicity,
                                        const WAVEFORMATEX* format,
                                        LPCGUID sessionGuid) {
    if (g_isBootstrapping) {
        return g_originalInitialize(self, shareMode, streamFlags, bufferDuration, periodicity, format, sessionGuid);
    }

    if (FakeOutputEnabled()) {
        StoreFakeInitialization(self,
                                format,
                                shareMode,
                                streamFlags,
                                bufferDuration,
                                periodicity,
                                0);
        const AudioClientState state = SnapshotAudioClient(self);
        LogWaveFormat("Fake IAudioClient::Initialize",
                      self,
                      format,
                      shareMode,
                      streamFlags,
                      state.fakePeriodFrames);
        Log("Fake output accepted IAudioClient::Initialize. audio=%p shareMode=%u flags=0x%08lX bufferHns=%lld periodicityHns=%lld fakeBufferFrames=%u fakePeriodFrames=%u",
            self,
            static_cast<unsigned>(shareMode),
            streamFlags,
            static_cast<long long>(bufferDuration),
            static_cast<long long>(periodicity),
            state.fakeBufferFrames,
            state.fakePeriodFrames);
        return S_OK;
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        auto& state = g_audioClients[self];
        CopyWaveFormat(format, &state);
        state.shareMode = shareMode;
        state.streamFlags = streamFlags;
    }

    LogWaveFormat("IAudioClient::Initialize",
                  self,
                  format,
                  shareMode,
                  streamFlags,
                  0);

    return g_originalInitialize(self, shareMode, streamFlags, bufferDuration, periodicity, format, sessionGuid);
}

HRESULT STDMETHODCALLTYPE HookInitializeSharedAudioStream(IAudioClient3* self,
                                                         DWORD streamFlags,
                                                         UINT32 periodInFrames,
                                                         const WAVEFORMATEX* format,
                                                         LPCGUID audioSessionGuid) {
    if (g_isBootstrapping) {
        return g_originalInitializeSharedAudioStream(self, streamFlags, periodInFrames, format, audioSessionGuid);
    }

    auto* audioClient = static_cast<IAudioClient*>(self);
    if (FakeOutputEnabled()) {
        StoreFakeInitialization(audioClient,
                                format,
                                AUDCLNT_SHAREMODE_SHARED,
                                streamFlags,
                                0,
                                0,
                                periodInFrames);
        const AudioClientState state = SnapshotAudioClient(audioClient);
        LogWaveFormat("Fake IAudioClient3::InitializeSharedAudioStream",
                      self,
                      format,
                      AUDCLNT_SHAREMODE_SHARED,
                      streamFlags,
                      periodInFrames);
        Log("Fake output accepted IAudioClient3::InitializeSharedAudioStream. audio=%p flags=0x%08lX requestedPeriodFrames=%u fakeBufferFrames=%u fakePeriodFrames=%u",
            audioClient,
            streamFlags,
            periodInFrames,
            state.fakeBufferFrames,
            state.fakePeriodFrames);
        return S_OK;
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        auto& state = g_audioClients[audioClient];
        CopyWaveFormat(format, &state);
        state.shareMode = AUDCLNT_SHAREMODE_SHARED;
        state.streamFlags = streamFlags;
    }

    LogWaveFormat("IAudioClient3::InitializeSharedAudioStream",
                  self,
                  format,
                  AUDCLNT_SHAREMODE_SHARED,
                  streamFlags,
                  periodInFrames);

    return g_originalInitializeSharedAudioStream(self,
                                                 streamFlags,
                                                 periodInFrames,
                                                 format,
                                                 audioSessionGuid);
}

HRESULT STDMETHODCALLTYPE HookGetService(IAudioClient* self, REFIID iid, void** service) {
    if (service == nullptr) {
        return E_POINTER;
    }
    *service = nullptr;

    if (!g_isBootstrapping && FakeOutputEnabled()) {
        const AudioClientState audioState = SnapshotAudioClient(self);
        if (iid == __uuidof(IAudioRenderClient)) {
            if (!audioState.fakeInitialized) {
                Log("Fake output GetService(IAudioRenderClient) before Initialize. audio=%p", self);
                return AUDCLNT_E_NOT_INITIALIZED;
            }
            auto* fake = new (std::nothrow) FakeRenderClient();
            if (fake == nullptr) {
                return E_OUTOFMEMORY;
            }
            fake->vtable = g_fakeRenderVtable;
            fake->audioClient = self;
            auto* renderClient = reinterpret_cast<IAudioRenderClient*>(fake);
            if (RegisterRenderClient(renderClient, self)) {
                const auto count = ++g_renderClientCount;
                Log("Fake IAudioRenderClient acquired. active renderClients=%u active audioClients=%u render=%p audio=%p",
                    count,
                    g_audioClientCount.load(),
                    renderClient,
                    self);
            }
            *service = renderClient;
            return S_OK;
        }
        if (iid == __uuidof(IAudioClock)) {
            if (!audioState.fakeInitialized) {
                Log("Fake output GetService(IAudioClock) before Initialize. audio=%p", self);
                return AUDCLNT_E_NOT_INITIALIZED;
            }
            auto* fake = new (std::nothrow) FakeAudioClock();
            if (fake == nullptr) {
                return E_OUTOFMEMORY;
            }
            fake->vtable = g_fakeClockVtable;
            fake->audioClient = self;
            *service = reinterpret_cast<IAudioClock*>(fake);
            Log("Fake IAudioClock acquired. clock=%p audio=%p", *service, self);
            return S_OK;
        }

        Log("Fake output unsupported IAudioClient::GetService. audio=%p iid=%s name=%s initialized=%s",
            self,
            GuidToString(iid).c_str(),
            KnownIidName(iid),
            audioState.fakeInitialized ? "yes" : "no");
        return E_NOINTERFACE;
    }

    const HRESULT hr = g_originalGetService(self, iid, service);
    if (SUCCEEDED(hr) && service != nullptr && *service != nullptr && iid == __uuidof(IAudioRenderClient)) {
        auto* renderClient = static_cast<IAudioRenderClient*>(*service);
        PatchRenderClient(renderClient);
        if (g_isBootstrapping) {
            return hr;
        }
        if (RegisterRenderClient(renderClient, self)) {
            const auto count = ++g_renderClientCount;
            Log("IAudioRenderClient acquired. active renderClients=%u active audioClients=%u render=%p audio=%p",
                count,
                g_audioClientCount.load(),
                renderClient,
                self);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookGetBufferSize(IAudioClient* self, UINT32* bufferFrames) {
    if (!g_isBootstrapping && FakeOutputEnabled()) {
        if (bufferFrames == nullptr) {
            return E_POINTER;
        }
        const AudioClientState state = SnapshotAudioClient(self);
        if (!state.fakeInitialized) {
            Log("Fake output GetBufferSize before Initialize. audio=%p", self);
            return AUDCLNT_E_NOT_INITIALIZED;
        }
        *bufferFrames = state.fakeBufferFrames;
        Log("Fake output GetBufferSize. audio=%p frames=%u", self, *bufferFrames);
        return S_OK;
    }
    return g_originalGetBufferSize(self, bufferFrames);
}

HRESULT STDMETHODCALLTYPE HookGetStreamLatency(IAudioClient* self, REFERENCE_TIME* latency) {
    if (!g_isBootstrapping && FakeOutputEnabled()) {
        if (latency == nullptr) {
            return E_POINTER;
        }
        const AudioClientState state = SnapshotAudioClient(self);
        if (!state.fakeInitialized) {
            Log("Fake output GetStreamLatency before Initialize. audio=%p", self);
            return AUDCLNT_E_NOT_INITIALIZED;
        }
        *latency = HnsFromFrames(state.fakeBufferFrames,
                                 state.format.Format.nSamplesPerSec,
                                 state.fakeDefaultPeriod);
        return S_OK;
    }
    return g_originalGetStreamLatency(self, latency);
}

HRESULT STDMETHODCALLTYPE HookGetCurrentPadding(IAudioClient* self, UINT32* paddingFrames) {
    if (!g_isBootstrapping && FakeOutputEnabled()) {
        if (paddingFrames == nullptr) {
            return E_POINTER;
        }
        const AudioClientState state = SnapshotAudioClient(self);
        if (!state.fakeInitialized) {
            Log("Fake output GetCurrentPadding before Initialize. audio=%p", self);
            return AUDCLNT_E_NOT_INITIALIZED;
        }
        *paddingFrames = 0;
        return S_OK;
    }
    return g_originalGetCurrentPadding(self, paddingFrames);
}

HRESULT STDMETHODCALLTYPE HookIsFormatSupported(IAudioClient* self,
                                                AUDCLNT_SHAREMODE shareMode,
                                                const WAVEFORMATEX* format,
                                                WAVEFORMATEX** closestMatch) {
    if (!g_isBootstrapping && FakeOutputEnabled()) {
        if (closestMatch != nullptr) {
            *closestMatch = nullptr;
        }
        const bool supported = IsUsableWaveFormat(format);
        Log("Fake output IsFormatSupported. audio=%p shareMode=%u supported=%s formatTag=0x%04X rate=%u channels=%u bits=%u",
            self,
            static_cast<unsigned>(shareMode),
            supported ? "yes" : "no",
            format != nullptr ? format->wFormatTag : 0,
            format != nullptr ? format->nSamplesPerSec : 0,
            format != nullptr ? format->nChannels : 0,
            format != nullptr ? format->wBitsPerSample : 0);
        return supported ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    return g_originalIsFormatSupported(self, shareMode, format, closestMatch);
}

HRESULT STDMETHODCALLTYPE HookGetMixFormat(IAudioClient* self, WAVEFORMATEX** deviceFormat) {
    if (!g_isBootstrapping && FakeOutputEnabled()) {
        if (deviceFormat == nullptr) {
            return E_POINTER;
        }
        AudioClientState state = SnapshotAudioClient(self);
        const WAVEFORMATEXTENSIBLE format = state.hasFormat ? state.format : DefaultMixFormat();
        *deviceFormat = CoTaskMemCopyWaveFormat(format);
        if (*deviceFormat == nullptr) {
            return E_OUTOFMEMORY;
        }
        Log("Fake output GetMixFormat. audio=%p rate=%u channels=%u bits=%u tag=0x%04X",
            self,
            format.Format.nSamplesPerSec,
            format.Format.nChannels,
            format.Format.wBitsPerSample,
            format.Format.wFormatTag);
        return S_OK;
    }
    return g_originalGetMixFormat(self, deviceFormat);
}

HRESULT STDMETHODCALLTYPE HookGetDevicePeriod(IAudioClient* self,
                                              REFERENCE_TIME* defaultPeriod,
                                              REFERENCE_TIME* minimumPeriod) {
    if (!g_isBootstrapping && FakeOutputEnabled()) {
        const AudioClientState state = SnapshotAudioClient(self);
        if (defaultPeriod != nullptr) {
            *defaultPeriod = state.fakeDefaultPeriod;
        }
        if (minimumPeriod != nullptr) {
            *minimumPeriod = state.fakeMinPeriod;
        }
        Log("Fake output GetDevicePeriod. audio=%p defaultHns=%lld minHns=%lld",
            self,
            static_cast<long long>(state.fakeDefaultPeriod),
            static_cast<long long>(state.fakeMinPeriod));
        return S_OK;
    }
    return g_originalGetDevicePeriod(self, defaultPeriod, minimumPeriod);
}

HRESULT STDMETHODCALLTYPE HookStart(IAudioClient* self) {
    if (!g_isBootstrapping && FakeOutputEnabled()) {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto& state = g_audioClients[self];
            if (!state.fakeInitialized) {
                Log("Fake output Start before Initialize. audio=%p", self);
                return AUDCLNT_E_NOT_INITIALIZED;
            }
            state.fakeStarted = true;
            state.fakeStartTick = GetTickCount64();
            state.fakeNextEventQpc = QpcNow() + QpcTicksForFakePeriod(state);
            if (state.fakeEvent != nullptr) {
                SetEvent(state.fakeEvent);
            }
        }
        Log("Fake output Start. audio=%p", self);
        return S_OK;
    }
    return g_originalStart(self);
}

HRESULT STDMETHODCALLTYPE HookStop(IAudioClient* self) {
    if (!g_isBootstrapping && FakeOutputEnabled()) {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto& state = g_audioClients[self];
            state.fakeStarted = false;
            state.fakeNextEventQpc = 0;
        }
        Log("Fake output Stop. audio=%p", self);
        return S_OK;
    }
    return g_originalStop(self);
}

HRESULT STDMETHODCALLTYPE HookReset(IAudioClient* self) {
    if (!g_isBootstrapping && FakeOutputEnabled()) {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto& state = g_audioClients[self];
            state.fakeFramesReleased = 0;
            state.fakeNextEventQpc = state.fakeStarted
                    ? QpcNow() + QpcTicksForFakePeriod(state)
                    : 0;
        }
        Log("Fake output Reset. audio=%p", self);
        return S_OK;
    }
    return g_originalReset(self);
}

HRESULT STDMETHODCALLTYPE HookSetEventHandle(IAudioClient* self, HANDLE eventHandle) {
    if (!g_isBootstrapping && FakeOutputEnabled()) {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto& state = g_audioClients[self];
            state.fakeEvent = eventHandle;
            if (state.fakeStarted && state.fakeEvent != nullptr) {
                state.fakeNextEventQpc = QpcNow() + QpcTicksForFakePeriod(state);
                SetEvent(state.fakeEvent);
            }
        }
        Log("Fake output SetEventHandle. audio=%p event=%p", self, eventHandle);
        return S_OK;
    }
    return g_originalSetEventHandle(self, eventHandle);
}

HRESULT STDMETHODCALLTYPE HookGetSharedModeEnginePeriod(IAudioClient3* self,
                                                        const WAVEFORMATEX* format,
                                                        UINT32* defaultPeriodInFrames,
                                                        UINT32* fundamentalPeriodInFrames,
                                                        UINT32* minPeriodInFrames,
                                                        UINT32* maxPeriodInFrames) {
    if (!g_isBootstrapping && FakeOutputEnabled()) {
        const UINT32 sampleRate = format != nullptr && format->nSamplesPerSec != 0
                ? format->nSamplesPerSec
                : 48000;
        const UINT32 defaultFrames = ClampFakeFrames(DefaultFramesForRate(sampleRate, 100, 480));
        const UINT32 fundamentalFrames = std::max<UINT32>(1, DefaultFramesForRate(sampleRate, 1000, 48));
        if (defaultPeriodInFrames != nullptr) {
            *defaultPeriodInFrames = defaultFrames;
        }
        if (fundamentalPeriodInFrames != nullptr) {
            *fundamentalPeriodInFrames = fundamentalFrames;
        }
        if (minPeriodInFrames != nullptr) {
            *minPeriodInFrames = fundamentalFrames;
        }
        if (maxPeriodInFrames != nullptr) {
            *maxPeriodInFrames = ClampFakeFrames(DefaultFramesForRate(sampleRate, 10, 4800));
        }
        Log("Fake output IAudioClient3::GetSharedModeEnginePeriod. audio=%p rate=%u default=%u fundamental=%u min=%u max=%u",
            self,
            sampleRate,
            defaultFrames,
            fundamentalFrames,
            fundamentalFrames,
            ClampFakeFrames(DefaultFramesForRate(sampleRate, 10, 4800)));
        return S_OK;
    }
    return g_originalGetSharedModeEnginePeriod(self,
                                               format,
                                               defaultPeriodInFrames,
                                               fundamentalPeriodInFrames,
                                               minPeriodInFrames,
                                               maxPeriodInFrames);
}

HRESULT STDMETHODCALLTYPE HookGetCurrentSharedModeEnginePeriod(IAudioClient3* self,
                                                               WAVEFORMATEX** currentFormat,
                                                               UINT32* currentPeriodInFrames) {
    if (!g_isBootstrapping && FakeOutputEnabled()) {
        if (currentFormat == nullptr || currentPeriodInFrames == nullptr) {
            return E_POINTER;
        }
        AudioClientState state = SnapshotAudioClient(static_cast<IAudioClient*>(self));
        const WAVEFORMATEXTENSIBLE format = state.hasFormat ? state.format : DefaultMixFormat();
        *currentFormat = CoTaskMemCopyWaveFormat(format);
        if (*currentFormat == nullptr) {
            return E_OUTOFMEMORY;
        }
        *currentPeriodInFrames = state.fakePeriodFrames != 0
                ? state.fakePeriodFrames
                : DefaultFramesForRate(format.Format.nSamplesPerSec, 100, 480);
        Log("Fake output IAudioClient3::GetCurrentSharedModeEnginePeriod. audio=%p period=%u rate=%u channels=%u bits=%u",
            self,
            *currentPeriodInFrames,
            format.Format.nSamplesPerSec,
            format.Format.nChannels,
            format.Format.wBitsPerSample);
        return S_OK;
    }
    return g_originalGetCurrentSharedModeEnginePeriod(self, currentFormat, currentPeriodInFrames);
}

HRESULT STDMETHODCALLTYPE HookGetBuffer(IAudioRenderClient* self, UINT32 frameCount, BYTE** data) {
    const HRESULT hr = g_originalGetBuffer(self, frameCount, data);
    if (SUCCEEDED(hr) && data != nullptr) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        auto& state = g_renderClients[self];
        state.pendingBuffer = *data;
        state.pendingFrames = frameCount;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookReleaseBuffer(IAudioRenderClient* self, UINT32 frameCount, DWORD flags) {
    const CaptureResult capture = CaptureReleasedBuffer(self, frameCount, flags);

#if defined(AUDIOBRIDGE_WASAPI_HOOK_SILENCE_ORIGINAL)
    if (capture.data != nullptr && capture.bytes > 0) {
        std::memset(capture.data, 0, static_cast<std::size_t>(capture.bytes));
    }
    return g_originalReleaseBuffer(self, frameCount, flags | AUDCLNT_BUFFERFLAGS_SILENT);
#else
    return g_originalReleaseBuffer(self, frameCount, flags);
#endif
}

ULONG STDMETHODCALLTYPE HookAudioClientRelease(IAudioClient* self) {
    const ULONG refs = g_originalAudioClientRelease(self);
    if (refs == 0) {
        bool removed = false;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            removed = g_audioClients.erase(self) > 0;
        }
        if (removed) {
            const auto count = g_audioClientCount.fetch_sub(1) - 1;
            Log("IAudioClient released. active clients=%u ptr=%p", count, self);
        }
    }
    return refs;
}

ULONG STDMETHODCALLTYPE HookRenderClientRelease(IAudioRenderClient* self) {
    const ULONG refs = g_originalRenderClientRelease(self);
    if (refs == 0) {
        bool removed = false;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            removed = g_renderClients.erase(self) > 0;
        }
        if (removed) {
            const auto count = g_renderClientCount.fetch_sub(1) - 1;
            Log("IAudioRenderClient released. active renderClients=%u ptr=%p", count, self);
        }
    }
    return refs;
}

void PatchEnumerator(IMMDeviceEnumerator* enumerator) {
    PatchVtableEntry(enumerator, 3, reinterpret_cast<void*>(&HookEnumAudioEndpoints), reinterpret_cast<void**>(&g_originalEnumAudioEndpoints));
    PatchVtableEntry(enumerator, 4, reinterpret_cast<void*>(&HookGetDefaultAudioEndpoint), reinterpret_cast<void**>(&g_originalGetDefaultAudioEndpoint));
    PatchVtableEntry(enumerator, 5, reinterpret_cast<void*>(&HookGetDevice), reinterpret_cast<void**>(&g_originalGetDevice));
}

void PatchDevice(IMMDevice* device) {
    PatchVtableEntry(device, 3, reinterpret_cast<void*>(&HookActivate), reinterpret_cast<void**>(&g_originalActivate));
}

void PatchAudioClient(IAudioClient* client) {
    PatchVtableEntry(client, 0, reinterpret_cast<void*>(&HookAudioClientQueryInterface), reinterpret_cast<void**>(&g_originalAudioClientQueryInterface));
    PatchVtableEntry(client, 2, reinterpret_cast<void*>(&HookAudioClientRelease), reinterpret_cast<void**>(&g_originalAudioClientRelease));
    PatchVtableEntry(client, 3, reinterpret_cast<void*>(&HookInitialize), reinterpret_cast<void**>(&g_originalInitialize));
    PatchVtableEntry(client, 4, reinterpret_cast<void*>(&HookGetBufferSize), reinterpret_cast<void**>(&g_originalGetBufferSize));
    PatchVtableEntry(client, 5, reinterpret_cast<void*>(&HookGetStreamLatency), reinterpret_cast<void**>(&g_originalGetStreamLatency));
    PatchVtableEntry(client, 6, reinterpret_cast<void*>(&HookGetCurrentPadding), reinterpret_cast<void**>(&g_originalGetCurrentPadding));
    PatchVtableEntry(client, 7, reinterpret_cast<void*>(&HookIsFormatSupported), reinterpret_cast<void**>(&g_originalIsFormatSupported));
    PatchVtableEntry(client, 8, reinterpret_cast<void*>(&HookGetMixFormat), reinterpret_cast<void**>(&g_originalGetMixFormat));
    PatchVtableEntry(client, 9, reinterpret_cast<void*>(&HookGetDevicePeriod), reinterpret_cast<void**>(&g_originalGetDevicePeriod));
    PatchVtableEntry(client, 10, reinterpret_cast<void*>(&HookStart), reinterpret_cast<void**>(&g_originalStart));
    PatchVtableEntry(client, 11, reinterpret_cast<void*>(&HookStop), reinterpret_cast<void**>(&g_originalStop));
    PatchVtableEntry(client, 12, reinterpret_cast<void*>(&HookReset), reinterpret_cast<void**>(&g_originalReset));
    PatchVtableEntry(client, 13, reinterpret_cast<void*>(&HookSetEventHandle), reinterpret_cast<void**>(&g_originalSetEventHandle));
    PatchVtableEntry(client, 14, reinterpret_cast<void*>(&HookGetService), reinterpret_cast<void**>(&g_originalGetService));
}

void PatchAudioClient2(IAudioClient2* client) {
    if (client == nullptr) {
        return;
    }
    PatchAudioClient(static_cast<IAudioClient*>(client));
}

void PatchAudioClient3(IAudioClient3* client) {
    if (client == nullptr) {
        return;
    }
    PatchAudioClient2(static_cast<IAudioClient2*>(client));
    PatchVtableEntry(client, 18, reinterpret_cast<void*>(&HookGetSharedModeEnginePeriod), reinterpret_cast<void**>(&g_originalGetSharedModeEnginePeriod));
    PatchVtableEntry(client, 19, reinterpret_cast<void*>(&HookGetCurrentSharedModeEnginePeriod), reinterpret_cast<void**>(&g_originalGetCurrentSharedModeEnginePeriod));
    PatchVtableEntry(client, 20, reinterpret_cast<void*>(&HookInitializeSharedAudioStream), reinterpret_cast<void**>(&g_originalInitializeSharedAudioStream));
}

void PatchAudioClientForIid(void* client, REFIID iid) {
    if (client == nullptr) {
        return;
    }
    if (iid == __uuidof(IAudioClient3)) {
        PatchAudioClient3(static_cast<IAudioClient3*>(client));
    } else if (iid == __uuidof(IAudioClient2)) {
        PatchAudioClient2(static_cast<IAudioClient2*>(client));
    } else if (iid == __uuidof(IAudioClient)) {
        PatchAudioClient(static_cast<IAudioClient*>(client));
    }
}

void PatchRenderClient(IAudioRenderClient* renderClient) {
    PatchVtableEntry(renderClient, 2, reinterpret_cast<void*>(&HookRenderClientRelease), reinterpret_cast<void**>(&g_originalRenderClientRelease));
    PatchVtableEntry(renderClient, 3, reinterpret_cast<void*>(&HookGetBuffer), reinterpret_cast<void**>(&g_originalGetBuffer));
    PatchVtableEntry(renderClient, 4, reinterpret_cast<void*>(&HookReleaseBuffer), reinterpret_cast<void**>(&g_originalReleaseBuffer));
}

HRESULT WINAPI HookCoCreateInstance(REFCLSID clsid,
                                    LPUNKNOWN outer,
                                    DWORD clsContext,
                                    REFIID iid,
                                    LPVOID* out) {
    const HRESULT hr = g_originalCoCreateInstance(clsid, outer, clsContext, iid, out);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && clsid == __uuidof(MMDeviceEnumerator)) {
        if (iid == __uuidof(IMMDeviceEnumerator) || iid == __uuidof(IUnknown)) {
            auto* enumerator = static_cast<IMMDeviceEnumerator*>(*out);
            PatchEnumerator(enumerator);
            Log("MMDeviceEnumerator created and patched. iid=%s ptr=%p",
                GuidToString(iid).c_str(),
                enumerator);
        }
    }
    return hr;
}

bool ShouldHookChildProcessCreation() {
    return !g_hooksDisabled.load() &&
           !g_finishCapture.load() &&
           !g_insideCreateProcessHook;
}

bool InjectSelfIntoChildProcess(HANDLE process) {
    if (process == nullptr || process == INVALID_HANDLE_VALUE) {
        return false;
    }

    wchar_t dllPath[MAX_PATH]{};
    if (GetModuleFileNameW(g_module, dllPath, static_cast<DWORD>(std::size(dllPath))) == 0) {
        return false;
    }

    const SIZE_T bytes = (static_cast<SIZE_T>(lstrlenW(dllPath)) + 1) * sizeof(wchar_t);
    void* remoteString = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remoteString == nullptr) {
        Log("CreateProcess hook child injection VirtualAllocEx failed. error=%lu", GetLastError());
        return false;
    }

    if (!WriteProcessMemory(process, remoteString, dllPath, bytes, nullptr)) {
        Log("CreateProcess hook child injection WriteProcessMemory failed. error=%lu", GetLastError());
        VirtualFreeEx(process, remoteString, 0, MEM_RELEASE);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto* loadLibrary = kernel32 != nullptr
            ? reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"))
            : nullptr;
    if (loadLibrary == nullptr) {
        Log("CreateProcess hook child injection LoadLibraryW not found.");
        VirtualFreeEx(process, remoteString, 0, MEM_RELEASE);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remoteString, 0, nullptr);
    if (thread == nullptr) {
        Log("CreateProcess hook child injection CreateRemoteThread failed. error=%lu", GetLastError());
        VirtualFreeEx(process, remoteString, 0, MEM_RELEASE);
        return false;
    }

    WaitForSingleObject(thread, INFINITE);
    DWORD remoteResult = 0;
    GetExitCodeThread(thread, &remoteResult);
    CloseHandle(thread);
    VirtualFreeEx(process, remoteString, 0, MEM_RELEASE);

    if (remoteResult == 0) {
        Log("CreateProcess hook child injection remote LoadLibraryW failed.");
        return false;
    }
    return true;
}

std::wstring HookReadyEventName(DWORD pid) {
    return std::wstring(AUDIOBRIDGE_HOOK_READY_EVENT_PREFIX) + std::to_wstring(pid);
}

void SignalHookReady() {
    const auto eventName = HookReadyEventName(GetCurrentProcessId());
    HANDLE readyEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
    if (readyEvent == nullptr) {
        Log("Hook ready event was not available. error=%lu", GetLastError());
        return;
    }
    SetEvent(readyEvent);
    CloseHandle(readyEvent);
}

void FinishChildProcessHook(const PROCESS_INFORMATION* processInfo, DWORD originalCreationFlags) {
    if (processInfo == nullptr || processInfo->hProcess == nullptr) {
        return;
    }

    const DWORD childPid = processInfo->dwProcessId;
    const auto readyEventName = HookReadyEventName(childPid);
    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, readyEventName.c_str());
    const bool injected = InjectSelfIntoChildProcess(processInfo->hProcess);
    Log("CreateProcess hook child pid=%lu injection=%s", childPid, injected ? "ok" : "failed");

    if (readyEvent != nullptr) {
        const DWORD timeoutMs = injected ? 5000 : 1500;
        const DWORD waitResult = WaitForSingleObject(readyEvent, timeoutMs);
        Log("CreateProcess hook child pid=%lu readyWait=%lu injection=%s",
            childPid,
            waitResult,
            injected ? "ok" : "fallback");
        CloseHandle(readyEvent);
    }

    if ((originalCreationFlags & CREATE_SUSPENDED) == 0 &&
        processInfo->hThread != nullptr &&
        processInfo->hThread != INVALID_HANDLE_VALUE) {
        ResumeThread(processInfo->hThread);
    }
}

BOOL WINAPI HookCreateProcessW(LPCWSTR applicationName,
                               LPWSTR commandLine,
                               LPSECURITY_ATTRIBUTES processAttributes,
                               LPSECURITY_ATTRIBUTES threadAttributes,
                               BOOL inheritHandles,
                               DWORD creationFlags,
                               LPVOID environment,
                               LPCWSTR currentDirectory,
                               LPSTARTUPINFOW startupInfo,
                               LPPROCESS_INFORMATION processInformation) {
    if (g_originalCreateProcessW == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    if (!ShouldHookChildProcessCreation() || processInformation == nullptr) {
        return g_originalCreateProcessW(applicationName,
                                        commandLine,
                                        processAttributes,
                                        threadAttributes,
                                        inheritHandles,
                                        creationFlags,
                                        environment,
                                        currentDirectory,
                                        startupInfo,
                                        processInformation);
    }

    g_insideCreateProcessHook = true;
    const DWORD hookedCreationFlags = creationFlags | CREATE_SUSPENDED;
    const BOOL result = g_originalCreateProcessW(applicationName,
                                                commandLine,
                                                processAttributes,
                                                threadAttributes,
                                                inheritHandles,
                                                hookedCreationFlags,
                                                environment,
                                                currentDirectory,
                                                startupInfo,
                                                processInformation);
    if (result) {
        FinishChildProcessHook(processInformation, creationFlags);
    }
    g_insideCreateProcessHook = false;
    return result;
}

BOOL WINAPI HookCreateProcessA(LPCSTR applicationName,
                               LPSTR commandLine,
                               LPSECURITY_ATTRIBUTES processAttributes,
                               LPSECURITY_ATTRIBUTES threadAttributes,
                               BOOL inheritHandles,
                               DWORD creationFlags,
                               LPVOID environment,
                               LPCSTR currentDirectory,
                               LPSTARTUPINFOA startupInfo,
                               LPPROCESS_INFORMATION processInformation) {
    if (g_originalCreateProcessA == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    if (!ShouldHookChildProcessCreation() || processInformation == nullptr) {
        return g_originalCreateProcessA(applicationName,
                                        commandLine,
                                        processAttributes,
                                        threadAttributes,
                                        inheritHandles,
                                        creationFlags,
                                        environment,
                                        currentDirectory,
                                        startupInfo,
                                        processInformation);
    }

    g_insideCreateProcessHook = true;
    const DWORD hookedCreationFlags = creationFlags | CREATE_SUSPENDED;
    const BOOL result = g_originalCreateProcessA(applicationName,
                                                commandLine,
                                                processAttributes,
                                                threadAttributes,
                                                inheritHandles,
                                                hookedCreationFlags,
                                                environment,
                                                currentDirectory,
                                                startupInfo,
                                                processInformation);
    if (result) {
        FinishChildProcessHook(processInformation, creationFlags);
    }
    g_insideCreateProcessHook = false;
    return result;
}

bool IsLikelyReadableModule(HMODULE module) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(module, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    return mbi.State == MEM_COMMIT;
}

void PatchModuleImports(HMODULE module) {
    if (module == nullptr || !IsLikelyReadableModule(module)) {
        return;
    }

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }

    const auto& importDirectory =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDirectory.VirtualAddress == 0 || importDirectory.Size == 0) {
        return;
    }

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
            base + importDirectory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        auto* importName = reinterpret_cast<const char*>(base + descriptor->Name);
        const bool importsCom =
                _stricmp(importName, "ole32.dll") == 0 ||
                _stricmp(importName, "combase.dll") == 0;
        const bool importsProcessApi =
                _stricmp(importName, "kernel32.dll") == 0 ||
                _stricmp(importName, "kernelbase.dll") == 0;
        if (!importsCom && !importsProcessApi) {
            continue;
        }

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        auto* originalThunk = descriptor->OriginalFirstThunk != 0
                ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk)
                : thunk;

        for (; originalThunk->u1.AddressOfData != 0; ++originalThunk, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(originalThunk->u1.Ordinal)) {
                continue;
            }
            auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                    base + originalThunk->u1.AddressOfData);
            const char* functionName = reinterpret_cast<const char*>(importByName->Name);
            if (importsCom && std::strcmp(functionName, "CoCreateInstance") == 0) {
                PatchPointer(reinterpret_cast<void**>(&thunk->u1.Function),
                             reinterpret_cast<void*>(&HookCoCreateInstance),
                             reinterpret_cast<void**>(&g_originalCoCreateInstance));
            } else if (importsProcessApi && std::strcmp(functionName, "CreateProcessW") == 0) {
                PatchPointer(reinterpret_cast<void**>(&thunk->u1.Function),
                             reinterpret_cast<void*>(&HookCreateProcessW),
                             reinterpret_cast<void**>(&g_originalCreateProcessW));
            } else if (importsProcessApi && std::strcmp(functionName, "CreateProcessA") == 0) {
                PatchPointer(reinterpret_cast<void**>(&thunk->u1.Function),
                             reinterpret_cast<void*>(&HookCreateProcessA),
                             reinterpret_cast<void**>(&g_originalCreateProcessA));
            }
        }
    }
}

void PatchLoadedModules() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            bool shouldPatch = false;
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                shouldPatch = g_scannedModules.insert(entry.hModule).second;
            }
            if (shouldPatch) {
                PatchModuleImports(entry.hModule);
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
}

void CheckPcmInactive() {
    struct InactiveEvent {
        IAudioRenderClient* renderClient = nullptr;
        IAudioClient* audioClient = nullptr;
        std::uint64_t quietMs = 0;
    };

    std::vector<InactiveEvent> inactiveEvents;
    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        for (auto& item : g_renderClients) {
            auto& state = item.second;
            if (state.pcmActive && state.lastPcmTick != 0 && now - state.lastPcmTick >= 1000) {
                state.pcmActive = false;
                inactiveEvents.push_back({item.first, state.audioClient, now - state.lastPcmTick});
            }
        }
    }

    for (const auto& event : inactiveEvents) {
        Log("PCM inactive. render=%p audio=%p quietMs=%llu",
            event.renderClient,
            event.audioClient,
            static_cast<unsigned long long>(event.quietMs));
    }
}

void OpenControlMapping() {
    if (g_control != nullptr) {
        return;
    }

    g_controlMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kControlMapName);
    if (g_controlMapping == nullptr) {
        return;
    }
    g_control = static_cast<HookControlBlock*>(
            MapViewOfFile(g_controlMapping, FILE_MAP_READ, 0, 0, sizeof(HookControlBlock)));
}

void EnableFakeTimerResolution() {
    if (g_timerResolutionActive.exchange(true)) {
        return;
    }
    if (g_winmm == nullptr) {
        g_winmm = LoadLibraryW(L"winmm.dll");
        if (g_winmm != nullptr) {
            g_timeBeginPeriod =
                    reinterpret_cast<TimePeriodFn>(GetProcAddress(g_winmm, "timeBeginPeriod"));
            g_timeEndPeriod =
                    reinterpret_cast<TimePeriodFn>(GetProcAddress(g_winmm, "timeEndPeriod"));
        }
    }
    if (g_timeBeginPeriod != nullptr) {
        const UINT result = g_timeBeginPeriod(1);
        Log("Fake output timer resolution requested. result=%u", result);
    } else {
        Log("Fake output timer resolution unavailable; event pacing may be coarse.");
    }
}

void DisableFakeTimerResolution() {
    if (!g_timerResolutionActive.exchange(false)) {
        return;
    }
    if (g_timeEndPeriod != nullptr) {
        const UINT result = g_timeEndPeriod(1);
        Log("Fake output timer resolution released. result=%u", result);
    }
}

void SetFakeOutputFromControl(bool fakeOutput) {
    const bool previous = g_fakeOutput.exchange(fakeOutput);
    if (previous != fakeOutput) {
        if (fakeOutput) {
            EnableFakeTimerResolution();
        } else {
            DisableFakeTimerResolution();
        }
        Log("Fake output %s by control block.", fakeOutput ? "enabled" : "disabled");
    }
}

bool FakeOutputEnabled() {
    OpenControlMapping();
    if (g_control != nullptr) {
        SetFakeOutputFromControl(g_control->fakeOutput != 0);
    }
    return g_fakeOutput.load();
}

void PumpFakeEvents() {
    std::vector<HANDLE> events;
    const LONGLONG now = QpcNow();
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        for (auto& item : g_audioClients) {
            auto& state = item.second;
            if (state.fakeOutput &&
                state.fakeStarted &&
                state.fakeEvent != nullptr &&
                (state.fakeNextEventQpc == 0 || now >= state.fakeNextEventQpc)) {
                events.push_back(state.fakeEvent);
                const LONGLONG ticks = QpcTicksForFakePeriod(state);
                state.fakeNextEventQpc =
                        state.fakeNextEventQpc > 0 ? state.fakeNextEventQpc + ticks
                                                    : now + ticks;
                if (state.fakeNextEventQpc <= now) {
                    state.fakeNextEventQpc = now + ticks;
                }
            }
        }
    }

    for (HANDLE eventHandle : events) {
        SetEvent(eventHandle);
    }
}

void ApplyControlBlock() {
    OpenControlMapping();
    if (g_control == nullptr) {
        return;
    }

    const DWORD lockedPid = static_cast<DWORD>(g_control->lockedPid);
    const bool finishCapture = g_control->finish != 0;
    SetFakeOutputFromControl(g_control->fakeOutput != 0);
    g_lockedAudioPid = lockedPid;
    if (finishCapture && !g_finishCapture.exchange(true)) {
        SendPipeMessage(kPipeFinish, nullptr, 0);
        Log("PCM output finalized by control request.");
        if (!g_hooksDisabled.exchange(true)) {
            RestorePatchedPointers();
        }
        g_running = false;
    }
}

void BootstrapWasapiVtables() {
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(coInit);
    if (FAILED(coInit) && coInit != RPC_E_CHANGED_MODE) {
        Log("CoInitializeEx failed during WASAPI bootstrap. hr=0x%08lX", static_cast<unsigned long>(coInit));
        return;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = g_originalCoCreateInstance(__uuidof(MMDeviceEnumerator),
                                            nullptr,
                                            CLSCTX_ALL,
                                            __uuidof(IMMDeviceEnumerator),
                                            reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || enumerator == nullptr) {
        Log("MMDeviceEnumerator bootstrap failed. hr=0x%08lX", static_cast<unsigned long>(hr));
        if (shouldUninitialize) {
            CoUninitialize();
        }
        return;
    }

    PatchEnumerator(enumerator);
    Log("MMDeviceEnumerator bootstrap patched. ptr=%p", enumerator);

    IMMDevice* defaultDevice = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice)) &&
        defaultDevice != nullptr) {
        PatchDevice(defaultDevice);
        BootstrapAudioClientVtables(defaultDevice);
        defaultDevice->Release();
    }

    IMMDeviceCollection* devices = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)) &&
        devices != nullptr) {
        UINT count = 0;
        if (SUCCEEDED(devices->GetCount(&count))) {
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* device = nullptr;
                if (SUCCEEDED(devices->Item(i, &device)) && device != nullptr) {
                    PatchDevice(device);
                    device->Release();
                }
            }
            Log("WASAPI render devices patched from bootstrap. count=%u", count);
        }
        devices->Release();
    }

    enumerator->Release();
    if (shouldUninitialize) {
        CoUninitialize();
    }
}

void BootstrapAudioClientVtables(IMMDevice* device) {
    if (device == nullptr) {
        return;
    }

    g_isBootstrapping = true;
    IAudioClient* audioClient = nullptr;
    HRESULT hr = device->Activate(__uuidof(IAudioClient),
                                  CLSCTX_ALL,
                                  nullptr,
                                  reinterpret_cast<void**>(&audioClient));
    if (FAILED(hr) || audioClient == nullptr) {
        g_isBootstrapping = false;
        Log("IAudioClient bootstrap Activate failed. hr=0x%08lX", static_cast<unsigned long>(hr));
        return;
    }

    PatchAudioClient(audioClient);

    IAudioClient2* audioClient2 = nullptr;
    if (SUCCEEDED(audioClient->QueryInterface(__uuidof(IAudioClient2),
                                             reinterpret_cast<void**>(&audioClient2))) &&
        audioClient2 != nullptr) {
        PatchAudioClient2(audioClient2);
        audioClient2->Release();
    }

    IAudioClient3* audioClient3 = nullptr;
    if (SUCCEEDED(audioClient->QueryInterface(__uuidof(IAudioClient3),
                                             reinterpret_cast<void**>(&audioClient3))) &&
        audioClient3 != nullptr) {
        PatchAudioClient3(audioClient3);
        audioClient3->Release();
    }

    Log("IAudioClient bootstrap patched. audio=%p", audioClient);
    audioClient->Release();
    g_isBootstrapping = false;
}

DWORD WINAPI HookThread(void*) {
    Log("AudioBridge WASAPI hook injected.");
    ApplyControlBlock();

    const HMODULE ole32 = GetModuleHandleW(L"ole32.dll");
    if (ole32 != nullptr) {
        g_originalCoCreateInstance =
                reinterpret_cast<CoCreateInstanceFn>(GetProcAddress(ole32, "CoCreateInstance"));
    }
    if (g_originalCoCreateInstance == nullptr) {
        const HMODULE combase = GetModuleHandleW(L"combase.dll");
        if (combase != nullptr) {
            g_originalCoCreateInstance =
                    reinterpret_cast<CoCreateInstanceFn>(GetProcAddress(combase, "CoCreateInstance"));
        }
    }

    if (g_originalCoCreateInstance == nullptr) {
        Log("CoCreateInstance was not found; hook cannot continue.");
        return 1;
    }

    BootstrapWasapiVtables();
    PatchLoadedModules();

    g_running = true;
    SignalHookReady();
    ULONGLONG lastModulePatchTick = 0;
    ULONGLONG lastInactiveCheckTick = 0;
    while (g_running) {
        ApplyControlBlock();
        if (!g_hooksDisabled.load()) {
            PumpFakeEvents();
            const ULONGLONG now = GetTickCount64();
            if (now - lastModulePatchTick >= 100) {
                PatchLoadedModules();
                lastModulePatchTick = now;
            }
            if (now - lastInactiveCheckTick >= 250) {
                CheckPcmInactive();
                lastInactiveCheckTick = now;
            }
        }
        Sleep(g_fakeOutput.load() ? 1 : 100);
    }
    DisableFakeTimerResolution();
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        g_running = false;
        DisableFakeTimerResolution();
        RestorePatchedPointers();
        Log("AudioBridge WASAPI hook detached.");
        {
            std::lock_guard<std::mutex> lock(g_pipeMutex);
            ClosePipeLocked();
        }
        if (g_control != nullptr) {
            UnmapViewOfFile(g_control);
            g_control = nullptr;
        }
        if (g_controlMapping != nullptr) {
            CloseHandle(g_controlMapping);
            g_controlMapping = nullptr;
        }
    }
    return TRUE;
}

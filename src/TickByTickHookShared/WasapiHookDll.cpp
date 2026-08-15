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

#include "TickByTickHookProtocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
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
    bool fakeHasStarted = false;
    UINT32 fakeBufferFrames = 480;
    std::uint64_t fakeRequestedBufferFrames = 480;
    UINT32 fakePrebufferFrames = 0;
    bool fakeBufferCapped = false;
    UINT32 fakePeriodFrames = 480;
    REFERENCE_TIME fakeDefaultPeriod = 100000;
    REFERENCE_TIME fakeMinPeriod = 30000;
    HANDLE fakeEvent = nullptr;
    std::uint64_t fakeDevicePosition = 0;
    LONGLONG fakeDevicePositionQpc = 0;
    std::uint64_t fakeQueuedFrames = 0;
    std::uint64_t fakeDiscardPendingFrames = 0;
    std::uint64_t fakeReservedFrames = 0;
    std::uint64_t fakeFrameRemainder = 0;
    LONGLONG fakeLastUpdateQpc = 0;
    LONGLONG fakeNextEventQpc = 0;
    std::uint64_t streamId = 0;
    std::uint64_t successfulSubmittedFrames = 0;
    std::uint64_t pendingSubmittedFrames = 0;
    std::uint64_t nextPcmSequence = 0;
    std::uint64_t consumedLogicalFrames = 0;
    std::uint64_t accountedLogicalFrames = 0;
    bool fakeBridgeManaged = false;
    bool fakeAdmissionBlocked = false;
    bool fakeFaulted = false;
    bool submissionInFlight = false;
};

struct RenderClientState {
    IAudioClient* audioClient = nullptr;
    std::uint64_t streamId = 0;
    BYTE* pendingBuffer = nullptr;
    UINT32 pendingFrames = 0;
    bool lateAttached = false;
    bool formatAnnounced = false;
    bool loggedPcm = false;
    bool pcmActive = false;
    ULONGLONG lastPcmTick = 0;
    ULONGLONG lastFlowLogTick = 0;
    ULONGLONG lastReleaseTick = 0;
    std::uint64_t flowNonSilentFrames = 0;
    std::uint64_t flowNonSilentBytes = 0;
    std::uint64_t flowPlayerSilentFrames = 0;
    std::uint64_t flowReleaseCalls = 0;
    std::uint64_t flowMaxReleaseGapMs = 0;
    std::uint64_t successfulSubmittedFrames = 0;
    std::uint64_t nextPcmSequence = 0;
};

HMODULE g_module = nullptr;
#ifndef TICKBYTICK_WASAPI_HOOK_PIPE_NAME
#define TICKBYTICK_WASAPI_HOOK_PIPE_NAME L"\\\\.\\pipe\\LOCAL\\TickByTickWasapiHook"
#endif
#define TICKBYTICK_HOOK_READY_EVENT_PREFIX L"Local\\TickByTickHookReady_"
constexpr wchar_t kPipeName[] = TICKBYTICK_WASAPI_HOOK_PIPE_NAME;
constexpr const wchar_t* kControlMapName = tickbytick::hook_protocol::kControlMapName;
constexpr DWORD kPipeMagic = tickbytick::hook_protocol::kPipeMagic;
constexpr DWORD kPipeText = tickbytick::hook_protocol::kPipeText;
constexpr DWORD kPipeFormat = tickbytick::hook_protocol::kPipeFormat;
constexpr DWORD kPipePcm = tickbytick::hook_protocol::kPipePcm;
constexpr DWORD kPipeFinish = tickbytick::hook_protocol::kPipeFinish;
using PipeMessageHeader = tickbytick::hook_protocol::PipeMessageHeader;
using PipeFormatMessage = tickbytick::hook_protocol::PipeFormatMessage;
using PipePcmMessage = tickbytick::hook_protocol::PipePcmMessage;
using HookControlBlock = tickbytick::hook_protocol::HookControlBlock;
using RendererState = tickbytick::hook_protocol::RendererState;

struct ControlSnapshot {
    bool valid = false;
    DWORD lockedPid = 0;
    bool finish = false;
    bool fakeOutput = false;
    RendererState rendererState = RendererState::Idle;
    LONG streamGeneration = 0;
    std::uint64_t streamId = 0;
    std::uint64_t blockedStreamId = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t prebufferMs = 0;
    std::uint64_t consumedLogicalFrames = 0;
    std::uint64_t consumedLogicalBaseline = 0;
    std::uint64_t consumedLogicalOffset = 0;
};

struct FakeRenderClient {
    void** vtable = nullptr;
    std::atomic<ULONG> refs{1};
    IAudioClient* audioClient = nullptr;
    std::vector<BYTE> buffer;
    bool bufferOutstanding = false;
    UINT32 requestedFrames = 0;
};

struct FakeAudioClock {
    void** vtable = nullptr;
    std::atomic<ULONG> refs{1};
    IAudioClient* audioClient = nullptr;
};

struct FakeAudioStreamVolume {
    void** vtable = nullptr;
    std::atomic<ULONG> refs{1};
    IAudioClient* audioClient = nullptr;
    std::mutex mutex;
    std::vector<float> volumes;
};

struct FakeAudioSessionControl {
    void** vtable = nullptr;
    std::atomic<ULONG> refs{1};
    IAudioClient* audioClient = nullptr;
    std::mutex mutex;
    std::wstring displayName;
    std::wstring iconPath;
    GUID groupingParam{};
    std::vector<IAudioSessionEvents*> notifications;
};

enum class FrequentLogEvent : std::size_t {
    EnumeratorCreated,
    AudioClientCreated,
    AudioClientQueried,
    FakeQueryInterface,
    FakeInitializeFormat,
    FakeInitializeAccepted,
    FakeGetBufferSize,
    FakeRenderAcquired,
    FakeRenderReleased,
    AudioClientReleased,
    FakeGetMixFormat,
    FakeGetDevicePeriod,
    FakeGetBufferUnavailable,
    FakeStop,
    DeviceInvalidated,
    Count
};

struct FrequentLogState {
    std::atomic<ULONGLONG> lastEmissionMs{0};
    std::atomic<std::uint32_t> suppressed{0};
};

std::mutex g_pipeMutex;
std::mutex g_stateMutex;
std::mutex g_submissionMutex;
std::mutex g_controlMutex;
std::unordered_map<void**, void*> g_patchedSlots;
std::unordered_set<HMODULE> g_scannedModules;
std::unordered_map<IAudioClient*, AudioClientState> g_audioClients;
std::unordered_map<IAudioRenderClient*, RenderClientState> g_renderClients;
AudioClientState g_bootstrapAudioState;
std::atomic<bool> g_running{false};
std::atomic<bool> g_hooksDisabled{false};
std::atomic<DWORD> g_lockedAudioPid{0};
std::atomic<bool> g_finishCapture{false};
std::atomic<bool> g_fakeOutput{false};
std::atomic<std::uint32_t> g_audioClientCount{0};
std::atomic<std::uint32_t> g_renderClientCount{0};
std::atomic<std::uint64_t> g_nextStreamId{1};
std::array<FrequentLogState,
           static_cast<std::size_t>(FrequentLogEvent::Count)> g_frequentLogStates{};
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

void OpenControlMapping();

std::uint64_t AllocateStreamId() {
    std::uint64_t streamId = g_nextStreamId.fetch_add(1, std::memory_order_relaxed);
    while (streamId == 0) {
        streamId = g_nextStreamId.fetch_add(1, std::memory_order_relaxed);
    }
    return streamId;
}

std::uint64_t EnsureAudioStreamIdLocked(AudioClientState& state) {
    if (state.streamId == 0) {
        state.streamId = AllocateStreamId();
    }
    return state.streamId;
}

bool ReadControlSnapshot(ControlSnapshot* snapshot) {
    if (snapshot == nullptr) {
        return false;
    }
    *snapshot = {};
    OpenControlMapping();
    const HookControlBlock* control = g_control;
    if (control == nullptr) {
        return false;
    }

    constexpr int kMaxSnapshotAttempts = 16;
    for (int attempt = 0; attempt < kMaxSnapshotAttempts; ++attempt) {
        const LONG configBefore = control->configSequence;
        MemoryBarrier();
        if ((configBefore & 1) != 0) {
            YieldProcessor();
            continue;
        }

        ControlSnapshot candidate{};
        candidate.lockedPid = static_cast<DWORD>(control->lockedPid);
        candidate.finish = control->finish != 0;
        candidate.fakeOutput = control->fakeOutput != 0;
        const LONG protocolVersion = control->protocolVersion;
        const LONG rendererState = control->rendererState;
        candidate.streamGeneration = control->streamGeneration;
        const LONG streamIdLow = control->streamIdLow;
        const LONG streamIdHigh = control->streamIdHigh;
        candidate.streamId = tickbytick::hook_protocol::JoinCounter(
                streamIdLow, streamIdHigh);
        candidate.sampleRate = static_cast<std::uint32_t>(control->sampleRate);
        const LONG prebufferMs = control->prebufferMs;
        const LONG blockedStreamIdLow = control->blockedStreamIdLow;
        const LONG blockedStreamIdHigh = control->blockedStreamIdHigh;
        MemoryBarrier();
        const LONG configAfter = control->configSequence;
        if (configBefore != configAfter || (configAfter & 1) != 0) {
            continue;
        }

        const LONG counterBefore = control->counterSequence;
        MemoryBarrier();
        if ((counterBefore & 1) != 0) {
            YieldProcessor();
            continue;
        }
        const LONG logicalLow = control->consumedLogicalLow;
        const LONG logicalHigh = control->consumedLogicalHigh;
        const LONG baselineLow = control->consumedLogicalBaselineLow;
        const LONG baselineHigh = control->consumedLogicalBaselineHigh;
        const LONG offsetLow = control->consumedLogicalOffsetLow;
        const LONG offsetHigh = control->consumedLogicalOffsetHigh;
        MemoryBarrier();
        const LONG counterAfter = control->counterSequence;
        const LONG configFinal = control->configSequence;
        if (counterBefore != counterAfter || (counterAfter & 1) != 0 ||
            configFinal != configAfter || (configFinal & 1) != 0) {
            continue;
        }

        if (protocolVersion != tickbytick::hook_protocol::kControlProtocolVersion ||
            rendererState < static_cast<LONG>(RendererState::Idle) ||
            rendererState > static_cast<LONG>(RendererState::Faulted)) {
            return false;
        }
        candidate.rendererState = static_cast<RendererState>(rendererState);
        candidate.prebufferMs = prebufferMs > 0
                ? static_cast<std::uint32_t>(prebufferMs)
                : 0;
        candidate.blockedStreamId = tickbytick::hook_protocol::JoinCounter(
                blockedStreamIdLow, blockedStreamIdHigh);
        candidate.consumedLogicalFrames =
                tickbytick::hook_protocol::JoinCounter(logicalLow, logicalHigh);
        candidate.consumedLogicalBaseline =
                tickbytick::hook_protocol::JoinCounter(baselineLow, baselineHigh);
        candidate.consumedLogicalOffset =
                tickbytick::hook_protocol::JoinCounter(offsetLow, offsetHigh);
        candidate.valid = true;
        *snapshot = candidate;
        return true;
    }
    return false;
}

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

bool SendPipeMessage(DWORD type, const void* payload, std::uint64_t payloadBytes) {
    if (payloadBytes > MAXDWORD) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    if (!EnsurePipeConnectedLocked()) {
        return false;
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
    return ok;
}

bool SendPcmMessage(std::uint64_t streamId,
                    std::uint64_t sequence,
                    std::uint64_t submittedFrames,
                    UINT32 frameCount,
                    DWORD flags,
                    const void* pcm,
                    std::uint64_t pcmBytes,
                    DWORD* errorCode) {
    if (errorCode != nullptr) {
        *errorCode = ERROR_SUCCESS;
    }
    const bool playerSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    if (streamId == 0 || sequence == 0 || frameCount == 0 ||
        (!playerSilent && (pcm == nullptr || pcmBytes == 0)) ||
        (playerSilent && pcmBytes != 0) ||
        pcmBytes > static_cast<std::uint64_t>(MAXDWORD) - sizeof(PipePcmMessage)) {
        if (errorCode != nullptr) {
            *errorCode = ERROR_INVALID_DATA;
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(g_pipeMutex);
    if (!EnsurePipeConnectedLocked()) {
        if (errorCode != nullptr) {
            *errorCode = GetLastError();
        }
        return false;
    }

    PipeMessageHeader header{};
    header.type = kPipePcm;
    header.pid = GetCurrentProcessId();
    header.payloadBytes = sizeof(PipePcmMessage) + pcmBytes;
    PipePcmMessage message{};
    message.streamId = streamId;
    message.sequence = sequence;
    message.submittedFrames = submittedFrames;
    message.frameCount = frameCount;
    message.flags = flags;

    const auto writeExact = [&](const void* data, DWORD bytes) {
        DWORD written = 0;
        if (!WriteFile(g_pipe, data, bytes, &written, nullptr)) {
            if (errorCode != nullptr) {
                *errorCode = GetLastError();
            }
            return false;
        }
        if (written != bytes) {
            if (errorCode != nullptr) {
                *errorCode = ERROR_WRITE_FAULT;
            }
            return false;
        }
        return true;
    };
    bool ok = writeExact(&header, sizeof(header));
    if (ok) {
        ok = writeExact(&message, sizeof(message));
    }
    if (ok && pcmBytes > 0) {
        ok = writeExact(pcm, static_cast<DWORD>(pcmBytes));
    }
    if (!ok) {
        ClosePipeLocked();
    }
    return ok;
}

void EmitLogMessage(const char* message) {
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

const char* FrequentLogEventName(FrequentLogEvent event) {
    switch (event) {
    case FrequentLogEvent::EnumeratorCreated:
        return "MMDeviceEnumerator creation";
    case FrequentLogEvent::AudioClientCreated:
        return "IAudioClient creation";
    case FrequentLogEvent::AudioClientQueried:
        return "IAudioClient query";
    case FrequentLogEvent::FakeQueryInterface:
        return "fake IAudioClient QueryInterface";
    case FrequentLogEvent::FakeInitializeFormat:
        return "fake IAudioClient initialization format";
    case FrequentLogEvent::FakeInitializeAccepted:
        return "fake IAudioClient initialization acceptance";
    case FrequentLogEvent::FakeGetBufferSize:
        return "fake GetBufferSize";
    case FrequentLogEvent::FakeRenderAcquired:
        return "fake IAudioRenderClient acquisition";
    case FrequentLogEvent::FakeRenderReleased:
        return "fake IAudioRenderClient release";
    case FrequentLogEvent::AudioClientReleased:
        return "IAudioClient release";
    case FrequentLogEvent::FakeGetMixFormat:
        return "fake GetMixFormat";
    case FrequentLogEvent::FakeGetDevicePeriod:
        return "fake GetDevicePeriod";
    case FrequentLogEvent::FakeGetBufferUnavailable:
        return "fake GetBuffer unavailable";
    case FrequentLogEvent::FakeStop:
        return "fake IAudioClient Stop";
    case FrequentLogEvent::DeviceInvalidated:
        return "AUDCLNT_E_DEVICE_INVALIDATED";
    default:
        return "unknown event";
    }
}

bool TryAcquireFrequentLogSlot(FrequentLogEvent event,
                               std::uint32_t* suppressed) {
    constexpr ULONGLONG kMinimumIntervalMs = 1000;
    const ULONGLONG nowMs = GetTickCount64();
    auto& state = g_frequentLogStates[static_cast<std::size_t>(event)];
    ULONGLONG previousEmission =
            state.lastEmissionMs.load(std::memory_order_relaxed);
    for (;;) {
        if (previousEmission != 0 &&
            (nowMs <= previousEmission ||
             nowMs - previousEmission < kMinimumIntervalMs)) {
            state.suppressed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (state.lastEmissionMs.compare_exchange_weak(
                    previousEmission,
                    nowMs,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
            break;
        }
    }
    const std::uint32_t suppressedCount =
            state.suppressed.exchange(0, std::memory_order_acq_rel);
    if (suppressed != nullptr) {
        *suppressed = suppressedCount;
    }
    return true;
}

void Log(const char* format, ...) {
    char message[2048]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    EmitLogMessage(message);
}

void LogFrequent(FrequentLogEvent event, const char* format, ...) {
    std::uint32_t suppressed = 0;
    if (!TryAcquireFrequentLogSlot(event, &suppressed)) {
        return;
    }

    if (suppressed > 0) {
        char summary[512]{};
        std::snprintf(summary,
                      sizeof(summary),
                      "[log] Suppressed %u high-frequency %s message(s) during the previous interval.",
                      suppressed,
                      FrequentLogEventName(event));
        EmitLogMessage(summary);
    }

    char message[2048]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    EmitLogMessage(message);
}

void LogDeviceInvalidated(const char* operation,
                          const void* audioClient,
                          const AudioClientState& state) {
    ControlSnapshot control{};
    const bool hasControl = ReadControlSnapshot(&control) && control.valid;
    LogFrequent(
        FrequentLogEvent::DeviceInvalidated,
        "%s returned AUDCLNT_E_DEVICE_INVALIDATED. audio=%p stream=%llu managed=%s queued=%llu retired=%llu submitted=%llu logical=%llu controlValid=%s controlPid=%lu controlStream=%llu rendererState=%ld",
        operation,
        audioClient,
        static_cast<unsigned long long>(state.streamId),
        state.fakeBridgeManaged ? "yes" : "no",
        static_cast<unsigned long long>(state.fakeQueuedFrames),
        static_cast<unsigned long long>(state.fakeDiscardPendingFrames),
        static_cast<unsigned long long>(state.successfulSubmittedFrames),
        static_cast<unsigned long long>(state.consumedLogicalFrames),
        hasControl ? "yes" : "no",
        hasControl ? control.lockedPid : 0,
        static_cast<unsigned long long>(hasControl ? control.streamId : 0),
        hasControl ? static_cast<long>(control.rendererState) : -1L);
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

UINT32 MinimumFakeBufferFrames(UINT32 periodFrames) {
    constexpr std::uint64_t kMaxFakeBufferFrames = 16384;
    return static_cast<UINT32>((std::min<std::uint64_t>)(
            kMaxFakeBufferFrames,
            static_cast<std::uint64_t>(periodFrames) * 2U));
}

std::uint64_t FramesFromHns(REFERENCE_TIME hns,
                            UINT32 sampleRate,
                            UINT32 fallbackFrames) {
    if (hns <= 0 || sampleRate == 0) {
        return ClampFakeFrames(fallbackFrames);
    }

    constexpr std::uint64_t kHnsPerSecond = 10000000ull;
    const auto duration = static_cast<std::uint64_t>(hns);
    const auto wholeSeconds = duration / kHnsPerSecond;
    const auto partialHns = duration % kHnsPerSecond;
    if (wholeSeconds >
        ((std::numeric_limits<std::uint64_t>::max)() - sampleRate) /
                sampleRate) {
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    return wholeSeconds * sampleRate +
            (partialHns * sampleRate + kHnsPerSecond - 1) /
                    kHnsPerSecond;
}

UINT32 SelectFakeBufferFrames(std::uint64_t requestedFrames,
                              UINT32 sampleRate,
                              UINT32 minimumFrames,
                              UINT32 prebufferMs,
                              UINT32* prebufferFrames,
                              bool* capped) {
    const std::uint64_t effectiveRate = sampleRate != 0 ? sampleRate : 48000U;
    const std::uint64_t targetFrames64 = prebufferMs > 0
            ? (effectiveRate * prebufferMs + 999U) / 1000U
            : 0;
    const UINT32 targetFrames = static_cast<UINT32>((std::min<std::uint64_t>)(
            targetFrames64,
            (std::numeric_limits<UINT32>::max)()));
    if (prebufferFrames != nullptr) {
        *prebufferFrames = targetFrames;
    }

    // Preserve the application's requested shared-mode ingress capacity while
    // bounding pathological requests to one second. Some renderers use the
    // requested capacity as part of their startup scheduling even after
    // GetBufferSize reports the negotiated result, so shrinking every client
    // to two periods can stall it after its first packet.
    const std::uint64_t maximumFrames = (std::max<std::uint64_t>)(
            minimumFrames,
            effectiveRate);
    const std::uint64_t selectedFrames = (std::min)(
            (std::max<std::uint64_t>)(requestedFrames, minimumFrames),
            maximumFrames);
    if (capped != nullptr) {
        *capped = requestedFrames > maximumFrames;
    }
    return static_cast<UINT32>(selectedFrames);
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

LONGLONG QpcFrequency() {
    static const LONGLONG frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) && value.QuadPart > 0
                ? value.QuadPart
                : 10000000ll;
    }();
    return frequency;
}

std::uint64_t QpcToHns(LONGLONG qpc) {
    if (qpc <= 0) {
        return 0;
    }
    const auto frequency = static_cast<std::uint64_t>(QpcFrequency());
    const auto ticks = static_cast<std::uint64_t>(qpc);
    return (ticks / frequency) * 10000000ull +
            ((ticks % frequency) * 10000000ull) / frequency;
}

bool ApplyAuthoritativeProgressLocked(AudioClientState& state, LONGLONG nowQpc) {
    ControlSnapshot snapshot{};
    const bool hasSnapshot = ReadControlSnapshot(&snapshot);
    const bool selectedProcess = hasSnapshot && snapshot.valid &&
            snapshot.lockedPid == GetCurrentProcessId();
    if (selectedProcess && snapshot.rendererState == RendererState::Faulted &&
        state.fakeOutput) {
        // Faulted describes the selected PID's whole renderer pipeline. It can
        // be published before Core has a usable stream id, so every fake
        // client in that PID must fail closed.
        state.fakeFaulted = true;
        state.fakeAdmissionBlocked = true;
        return true;
    }

    if (selectedProcess && snapshot.blockedStreamId != 0 &&
        snapshot.blockedStreamId == state.streamId && state.fakeOutput) {
        state.fakeBridgeManaged = true;
        state.fakeAdmissionBlocked = true;
        return true;
    }

    if (selectedProcess &&
        snapshot.rendererState == RendererState::Reconfiguring &&
        snapshot.streamId != 0 &&
        state.fakeOutput) {
        // A nonzero route identifies an actual stream handoff. Freeze the
        // selected PID until Core publishes its new timeline generation. The
        // initial streamId=0 discovery state must remain writable so the first
        // PCM packet can select and start ASIO.
        state.fakeBridgeManaged = true;
        state.fakeAdmissionBlocked = true;
        return true;
    }

    const bool matchesStream = selectedProcess &&
            snapshot.streamId != 0 && snapshot.streamId == state.streamId;
    if (!matchesStream) {
        // Once Core has taken custody of this stream, loss of a matching route
        // freezes it rather than falling back to an unrelated wall clock.
        if (state.fakeBridgeManaged) {
            state.fakeAdmissionBlocked = true;
        }
        return state.fakeBridgeManaged;
    }

    state.fakeBridgeManaged = true;
    if (snapshot.rendererState != RendererState::Running) {
        // Reconfiguration and idle periods do not consume virtual WASAPI data.
        state.fakeAdmissionBlocked = true;
        return true;
    }
    state.fakeAdmissionBlocked = false;

    if (snapshot.consumedLogicalFrames < snapshot.consumedLogicalBaseline) {
        state.fakeFaulted = true;
        return true;
    }
    const std::uint64_t generationConsumed =
            snapshot.consumedLogicalFrames - snapshot.consumedLogicalBaseline;
    if (generationConsumed >
        (std::numeric_limits<std::uint64_t>::max)() -
                snapshot.consumedLogicalOffset) {
        state.fakeFaulted = true;
        return true;
    }
    const std::uint64_t consumedLogical =
            snapshot.consumedLogicalOffset + generationConsumed;
    const std::uint64_t submittedCeiling = state.submissionInFlight
            ? state.pendingSubmittedFrames
            : state.successfulSubmittedFrames;
    if (consumedLogical < state.consumedLogicalFrames ||
        consumedLogical > submittedCeiling ||
        state.fakeQueuedFrames > state.fakeBufferFrames ||
        state.fakeDiscardPendingFrames >
                state.fakeBufferFrames - state.fakeQueuedFrames) {
        state.fakeFaulted = true;
        return true;
    }
    state.consumedLogicalFrames = consumedLogical;

    if (consumedLogical < state.accountedLogicalFrames) {
        state.fakeFaulted = true;
        return true;
    }
    const std::uint64_t releasableFrames =
            consumedLogical - state.accountedLogicalFrames;
    const std::uint64_t discardedFrames = (std::min)(
            state.fakeDiscardPendingFrames, releasableFrames);
    state.fakeDiscardPendingFrames -= discardedFrames;
    if (!state.fakeStarted && state.fakeHasStarted) {
        // Stop freezes both padding and IAudioClock. Core may continue retiring
        // its protected timeline, but ordinary player padding is not exposed
        // as consumed until the client starts again.
        state.accountedLogicalFrames += discardedFrames;
        return true;
    }
    state.accountedLogicalFrames = consumedLogical;
    const std::uint64_t queuedReleaseBudget =
            releasableFrames - discardedFrames;
    const std::uint64_t releasedFrames =
            (std::min)(state.fakeQueuedFrames, queuedReleaseBudget);
    state.fakeQueuedFrames -= releasedFrames;
    if (state.fakeStarted &&
        releasedFrames > (std::numeric_limits<std::uint64_t>::max)() -
                                 state.fakeDevicePosition) {
        state.fakeFaulted = true;
        return true;
    }
    if (state.fakeStarted) {
        state.fakeDevicePosition += releasedFrames;
    }
    if (state.fakeStarted && releasedFrames != 0) {
        state.fakeDevicePositionQpc = nowQpc;
    }
    return true;
}

void AdvanceFakePlaybackLocked(AudioClientState& state, LONGLONG nowQpc) {
    if (ApplyAuthoritativeProgressLocked(state, nowQpc)) {
        return;
    }
    if (!state.fakeStarted) {
        state.fakeLastUpdateQpc = 0;
        return;
    }
    if (state.fakeLastUpdateQpc <= 0 || nowQpc <= state.fakeLastUpdateQpc) {
        state.fakeLastUpdateQpc = nowQpc;
        return;
    }

    const auto frequency = static_cast<std::uint64_t>(QpcFrequency());
    const auto sampleRate = static_cast<std::uint64_t>(
            state.format.Format.nSamplesPerSec != 0
                    ? state.format.Format.nSamplesPerSec
                    : 48000);
    const auto elapsedTicks = static_cast<std::uint64_t>(nowQpc - state.fakeLastUpdateQpc);
    const auto wholeSeconds = elapsedTicks / frequency;
    const auto partialTicks = elapsedTicks % frequency;
    const auto partialNumerator =
            partialTicks * sampleRate + state.fakeFrameRemainder;
    const auto advancedFrames =
            wholeSeconds * sampleRate + partialNumerator / frequency;

    state.fakeFrameRemainder = partialNumerator % frequency;
    state.fakeLastUpdateQpc = nowQpc;
    state.fakeDevicePosition += advancedFrames;
    if (advancedFrames > 0) {
        state.fakeDevicePositionQpc = nowQpc;
    }
    state.fakeQueuedFrames -=
            (std::min)(state.fakeQueuedFrames, advancedFrames);
}

UINT32 FakeAvailableFramesLocked(const AudioClientState& state) {
    if (state.fakeAdmissionBlocked) {
        return 0;
    }
    const auto capacity = static_cast<std::uint64_t>(state.fakeBufferFrames);
    std::uint64_t used = (std::min)(state.fakeQueuedFrames, capacity);
    used += (std::min)(state.fakeDiscardPendingFrames, capacity - used);
    used += (std::min)(state.fakeReservedFrames, capacity - used);
    return static_cast<UINT32>(capacity - used);
}

bool FakeEventReadyLocked(const AudioClientState& state) {
    if (state.fakeAdmissionBlocked) {
        return false;
    }
    const UINT32 periodFrames = state.fakePeriodFrames != 0
            ? state.fakePeriodFrames
            : DefaultFramesForRate(
                      state.format.Format.nSamplesPerSec != 0
                              ? state.format.Format.nSamplesPerSec
                              : 48000,
                      100,
                      480);
    const UINT32 requiredFrames =
            (std::min)(state.fakeBufferFrames, periodFrames);
    return FakeAvailableFramesLocked(state) >= requiredFrames;
}

LONGLONG QpcTicksForFakePeriod(const AudioClientState& state) {
    const UINT32 sampleRate = state.format.Format.nSamplesPerSec != 0
            ? state.format.Format.nSamplesPerSec
            : 48000;
    const UINT32 periodFrames = state.fakePeriodFrames != 0
            ? state.fakePeriodFrames
            : DefaultFramesForRate(sampleRate, 100, 480);
    return std::max<LONGLONG>(
            1,
            (static_cast<LONGLONG>(periodFrames) * QpcFrequency() + sampleRate - 1) /
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

bool LogWaveFormat(const char* source,
                   const void* client,
                   const WAVEFORMATEX* format,
                   AUDCLNT_SHAREMODE shareMode,
                   DWORD streamFlags,
                   UINT32 periodInFrames,
                   UINT32 applicationBufferFrames,
                   std::uint64_t streamId) {
    const bool frequentFakeInitialization =
            source != nullptr && std::strncmp(source, "Fake ", 5) == 0;
    if (format == nullptr) {
        char detail[2048]{};
        std::snprintf(detail,
                      sizeof(detail),
                      "%s client=%p format=null shareMode=%u flags=0x%08lX periodFrames=%u",
                      source,
                      client,
                      static_cast<unsigned>(shareMode),
                      streamFlags,
                      periodInFrames);
        if (frequentFakeInitialization) {
            LogFrequent(FrequentLogEvent::FakeInitializeFormat, "%s", detail);
        } else {
            EmitLogMessage(detail);
        }
        return false;
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

    char detail[2048]{};
    std::snprintf(
        detail,
        sizeof(detail),
        "%s client=%p formatTag=0x%04X sampleRate=%u channels=%u bits=%u validBits=%u blockAlign=%u avgBytesPerSec=%u cbSize=%u sampleType=%s channelMask=0x%08lX subFormat=%s shareMode=%u flags=0x%08lX periodFrames=%u",
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
    if (frequentFakeInitialization) {
        LogFrequent(FrequentLogEvent::FakeInitializeFormat, "%s", detail);
    } else {
        EmitLogMessage(detail);
    }

    PipeFormatMessage message{};
    CopyMemory(&message.format,
               format,
               std::min<std::size_t>(sizeof(message.format),
                                     sizeof(WAVEFORMATEX) + format->cbSize));
    message.streamFlags = streamFlags;
    message.shareMode = static_cast<DWORD>(shareMode);
    message.periodFrames = periodInFrames;
    message.applicationBufferFrames = applicationBufferFrames;
    message.streamId = streamId;
    return SendPipeMessage(kPipeFormat, &message, sizeof(message));
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
    const std::uint64_t requestedBufferFrames =
            bufferDuration > 0
                    ? FramesFromHns(bufferDuration, sampleRate, selectedPeriodFrames)
                    : static_cast<std::uint64_t>(ClampFakeFrames(
                              std::max<UINT32>(selectedPeriodFrames,
                                               defaultFrames)));
    const UINT32 minimumBufferFrames =
            MinimumFakeBufferFrames(selectedPeriodFrames);
    ControlSnapshot control{};
    const bool hasControl = ReadControlSnapshot(&control) && control.valid;
    UINT32 prebufferFrames = 0;
    bool bufferCapped = false;
    const UINT32 selectedBufferFrames = SelectFakeBufferFrames(
            requestedBufferFrames,
            sampleRate,
            minimumBufferFrames,
            hasControl ? control.prebufferMs : 0,
            &prebufferFrames,
            &bufferCapped);

    std::lock_guard<std::mutex> lock(g_stateMutex);
    auto& state = g_audioClients[client];
    const bool existingFault = state.fakeFaulted;
    EnsureAudioStreamIdLocked(state);
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
    state.fakeHasStarted = false;
    state.fakeBufferFrames = selectedBufferFrames;
    state.fakeRequestedBufferFrames = requestedBufferFrames;
    state.fakePrebufferFrames = prebufferFrames;
    state.fakeBufferCapped = bufferCapped;
    state.fakePeriodFrames = selectedPeriodFrames;
    state.fakeDefaultPeriod = HnsFromFrames(selectedPeriodFrames, sampleRate, 100000);
    state.fakeMinPeriod = periodicity > 0
            ? periodicity
            : HnsFromFrames(std::max<UINT32>(1, DefaultFramesForRate(sampleRate, 333, 144)),
                            sampleRate,
                            30000);
    state.fakeDevicePosition = 0;
    state.fakeDevicePositionQpc = 0;
    state.fakeQueuedFrames = 0;
    state.fakeDiscardPendingFrames = 0;
    state.fakeReservedFrames = 0;
    state.fakeFrameRemainder = 0;
    state.fakeLastUpdateQpc = 0;
    state.fakeNextEventQpc = 0;
    state.successfulSubmittedFrames = 0;
    state.pendingSubmittedFrames = 0;
    state.nextPcmSequence = 0;
    state.consumedLogicalFrames = 0;
    state.accountedLogicalFrames = 0;
    state.fakeBridgeManaged = false;
    state.fakeAdmissionBlocked = false;
    state.fakeFaulted = existingFault;
    state.submissionInFlight = false;
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
    auto [it, inserted] = g_audioClients.try_emplace(client);
    EnsureAudioStreamIdLocked(it->second);
    return inserted;
}

bool RegisterRenderClient(IAudioRenderClient* renderClient, IAudioClient* audioClient) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    auto [it, inserted] = g_renderClients.try_emplace(renderClient);
    it->second.audioClient = audioClient;
    if (audioClient != nullptr) {
        auto& audioState = g_audioClients[audioClient];
        it->second.streamId = EnsureAudioStreamIdLocked(audioState);
    } else if (it->second.streamId == 0) {
        it->second.streamId = AllocateStreamId();
    }
    return inserted;
}

struct CaptureResult {
    BYTE* data = nullptr;
    std::uint64_t bytes = 0;
    bool fakeDeviceInvalidated = false;
    const char* invalidationReason = "none";
    DWORD pipeError = ERROR_SUCCESS;
};

CaptureResult CaptureReleasedBuffer(IAudioRenderClient* self, UINT32 frameCount, DWORD flags) {
    std::lock_guard<std::mutex> submissionLock(g_submissionMutex);
    RenderClientState renderState{};
    AudioClientState audioState{};
    bool shouldLogPcm = false;
    bool shouldLogFlow = false;
    bool shouldLogResume = false;
    bool shouldAnnounceFallbackFormat = false;
    std::uint64_t flowIntervalMs = 0;
    std::uint64_t flowNonSilentFrames = 0;
    std::uint64_t flowNonSilentBytes = 0;
    std::uint64_t flowPlayerSilentFrames = 0;
    std::uint64_t flowReleaseCalls = 0;
    std::uint64_t flowMaxReleaseGapMs = 0;
    std::uint64_t bytes = 0;
    bool hasReleaseState = false;
    bool fakeOutput = false;
    const bool playerSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    const ULONGLONG now = GetTickCount64();
    const LONGLONG nowQpc = QpcNow();

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        auto renderIt = g_renderClients.find(self);
        if (renderIt != g_renderClients.end()) {
            renderState = renderIt->second;
            hasReleaseState = frameCount <= renderState.pendingFrames;
            if (!renderIt->second.loggedPcm &&
                renderState.pendingBuffer != nullptr &&
                frameCount > 0 &&
                (flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0) {
                shouldLogPcm = true;
                renderIt->second.loggedPcm = true;
            }

            auto audioIt = g_audioClients.find(renderState.audioClient);
            if (audioIt != g_audioClients.end()) {
                auto& state = audioIt->second;
                renderState.streamId = EnsureAudioStreamIdLocked(state);
                renderIt->second.streamId = renderState.streamId;
                fakeOutput = state.fakeOutput;
                if (state.fakeOutput) {
                    AdvanceFakePlaybackLocked(state, nowQpc);
                    state.fakeReservedFrames -=
                            (std::min<std::uint64_t>)(state.fakeReservedFrames,
                                                      renderState.pendingFrames);
                }
                audioState = state;
            } else if (renderState.lateAttached && g_bootstrapAudioState.hasFormat) {
                audioState = g_bootstrapAudioState;
                if (!renderIt->second.formatAnnounced) {
                    renderIt->second.formatAnnounced = true;
                    shouldAnnounceFallbackFormat = true;
                }
            }

            const bool validFlow =
                    renderState.pendingBuffer != nullptr &&
                    hasReleaseState &&
                    frameCount > 0 &&
                    audioState.hasFormat;
            if (validFlow) {
                auto& state = renderIt->second;
                if (state.lastFlowLogTick == 0) {
                    state.lastFlowLogTick = now;
                }
                if (state.lastReleaseTick != 0 && now >= state.lastReleaseTick) {
                    state.flowMaxReleaseGapMs = (std::max)(
                            state.flowMaxReleaseGapMs,
                            static_cast<std::uint64_t>(now - state.lastReleaseTick));
                }
                state.lastReleaseTick = now;
                ++state.flowReleaseCalls;
                if (playerSilent) {
                    state.flowPlayerSilentFrames += frameCount;
                } else {
                    const std::uint64_t flowBytes =
                            static_cast<std::uint64_t>(frameCount) *
                            static_cast<std::uint64_t>(BytesPerFrame(audioState));
                    state.flowNonSilentFrames += frameCount;
                    state.flowNonSilentBytes += flowBytes;
                }
                if (now - state.lastFlowLogTick >= 1000) {
                    shouldLogFlow = true;
                    flowIntervalMs = now - state.lastFlowLogTick;
                    flowNonSilentFrames = state.flowNonSilentFrames;
                    flowNonSilentBytes = state.flowNonSilentBytes;
                    flowPlayerSilentFrames = state.flowPlayerSilentFrames;
                    flowReleaseCalls = state.flowReleaseCalls;
                    flowMaxReleaseGapMs = state.flowMaxReleaseGapMs;
                    state.flowNonSilentFrames = 0;
                    state.flowNonSilentBytes = 0;
                    state.flowPlayerSilentFrames = 0;
                    state.flowReleaseCalls = 0;
                    state.flowMaxReleaseGapMs = 0;
                    state.lastFlowLogTick = now;
                }
            }

            const bool hasPcm = validFlow && !playerSilent;
            if (hasPcm) {
                const std::uint32_t bytesPerFrame = BytesPerFrame(audioState);
                bytes = static_cast<std::uint64_t>(frameCount) *
                        static_cast<std::uint64_t>(bytesPerFrame);

                if (!renderIt->second.pcmActive) {
                    shouldLogResume = true;
                    renderIt->second.pcmActive = true;
                }

                renderIt->second.lastPcmTick = now;
            }
            renderIt->second.pendingBuffer = nullptr;
            renderIt->second.pendingFrames = 0;
        }
    }

    if (shouldAnnounceFallbackFormat) {
        if (!LogWaveFormat("Late-attached IAudioRenderClient fallback format",
                           self,
                           &audioState.format.Format,
                           audioState.shareMode,
                           audioState.streamFlags,
                           0,
                           audioState.fakeOutput
                                   ? audioState.fakeBufferFrames
                                   : 0,
                           renderState.streamId)) {
            Log("Late-attached format message could not be delivered. render=%p stream=%llu",
                self,
                static_cast<unsigned long long>(renderState.streamId));
        }
    }

    const PcmBufferStats stats = InspectPcmBuffer(renderState.pendingBuffer, bytes);
    ControlSnapshot control{};
    const bool hasControl = ReadControlSnapshot(&control);
    const DWORD currentPid = GetCurrentProcessId();
    const bool captureEnabled = hasControl
            ? control.lockedPid == currentPid && !control.finish
            : g_lockedAudioPid.load() == currentPid && !g_finishCapture.load();
    const bool feedbackMatches = hasControl && control.valid &&
            control.lockedPid == currentPid &&
            control.streamId != 0 && control.streamId == renderState.streamId;
    const bool pipelineFaulted = hasControl && control.valid &&
            control.lockedPid == currentPid &&
            control.rendererState == RendererState::Faulted;
    std::uint64_t currentSubmittedFrames = audioState.successfulSubmittedFrames;
    std::uint64_t currentSequence = audioState.nextPcmSequence;
    if (renderState.audioClient == nullptr) {
        currentSubmittedFrames = renderState.successfulSubmittedFrames;
        currentSequence = renderState.nextPcmSequence;
    }
    const bool submittedOverflow = frameCount >
            (std::numeric_limits<std::uint64_t>::max)() - currentSubmittedFrames;
    const std::uint64_t nextSubmittedFrames = submittedOverflow
            ? currentSubmittedFrames
            : currentSubmittedFrames + frameCount;
    const bool sequenceOverflow = currentSequence ==
            (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t nextSequence = sequenceOverflow ? 0 : currentSequence + 1;

    bool sendAttempted = false;
    bool sendSucceeded = false;
    bool queuedBeforeSend = false;
    DWORD pipeError = ERROR_SUCCESS;
    const bool canSend = captureEnabled && frameCount > 0 && hasReleaseState &&
            audioState.hasFormat && renderState.streamId != 0 &&
            !audioState.fakeFaulted && !pipelineFaulted &&
            !submittedOverflow && !sequenceOverflow;
    if (canSend && fakeOutput && renderState.audioClient != nullptr) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        const auto audioIt = g_audioClients.find(renderState.audioClient);
        if (audioIt != g_audioClients.end()) {
            audioIt->second.pendingSubmittedFrames = nextSubmittedFrames;
            audioIt->second.submissionInFlight = true;
            // ReleaseBuffer has transferred ownership to the endpoint. Make
            // the frames visible to the virtual queue before the synchronous
            // pipe write. Confirmed ASIO pages may be published concurrently,
            // and their release budget must see the matching accepted frames.
            // A failed write invalidates the endpoint instead of rolling back
            // data that another thread may already have observed as consumed.
            audioIt->second.fakeQueuedFrames += frameCount;
            queuedBeforeSend = frameCount > 0;
        }
    }
    if (canSend) {
        sendAttempted = true;
        sendSucceeded = SendPcmMessage(renderState.streamId,
                                       nextSequence,
                                       nextSubmittedFrames,
                                       frameCount,
                                       flags,
                                       playerSilent ? nullptr : renderState.pendingBuffer,
                                       playerSilent ? 0 : bytes,
                                       &pipeError);
    }

    bool fakeDeviceInvalidated = false;
    const char* invalidationReason = "none";
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        auto renderIt = g_renderClients.find(self);
        auto audioIt = renderState.audioClient != nullptr
                ? g_audioClients.find(renderState.audioClient)
                : g_audioClients.end();
        if (audioIt != g_audioClients.end()) {
            auto& state = audioIt->second;
            state.submissionInFlight = false;
            const bool authoritativeStream = state.fakeBridgeManaged || feedbackMatches;
            if (feedbackMatches) {
                state.fakeBridgeManaged = true;
            }
            if (state.fakeOutput && pipelineFaulted) {
                invalidationReason = "renderer-fault";
            } else if (state.fakeOutput && submittedOverflow) {
                invalidationReason = "submitted-counter-overflow";
            } else if (state.fakeOutput && sequenceOverflow) {
                invalidationReason = "sequence-counter-overflow";
            } else if (state.fakeOutput &&
                       ((queuedBeforeSend && !sendSucceeded) ||
                        (authoritativeStream && captureEnabled && frameCount > 0 &&
                         sendAttempted && !sendSucceeded))) {
                invalidationReason = "pipe-write";
            } else if (state.fakeOutput && authoritativeStream &&
                       captureEnabled && frameCount > 0 && !sendAttempted) {
                invalidationReason = "submission-unavailable";
            } else if (state.fakeOutput && state.fakeFaulted) {
                invalidationReason = "virtual-accounting";
            }
            if (std::strcmp(invalidationReason, "none") != 0) {
                state.fakeFaulted = true;
                fakeDeviceInvalidated = true;
            } else if (state.fakeOutput && hasReleaseState) {
                if (sendSucceeded) {
                    state.successfulSubmittedFrames = nextSubmittedFrames;
                    state.nextPcmSequence = nextSequence;
                }
                if (state.fakeBridgeManaged) {
                    if (state.consumedLogicalFrames > state.successfulSubmittedFrames) {
                        state.fakeFaulted = true;
                        fakeDeviceInvalidated = true;
                    } else if (sendSucceeded && frameCount > 0 &&
                               !queuedBeforeSend) {
                        state.fakeQueuedFrames += frameCount;
                    }
                } else if (frameCount > 0 && !queuedBeforeSend) {
                    state.fakeQueuedFrames += frameCount;
                }
                if (sendSucceeded && !state.fakeFaulted) {
                    // Re-read logical admission after the pipe write. When Core
                    // has already handled this packet, this immediately frees
                    // the two-period ingress window; otherwise the feedback
                    // thread supplies the same update on its next 1 ms tick.
                    AdvanceFakePlaybackLocked(state, QpcNow());
                    if (state.fakeFaulted) {
                        invalidationReason = "virtual-accounting";
                        fakeDeviceInvalidated = true;
                    }
                }
            } else if (!state.fakeOutput && sendSucceeded) {
                state.successfulSubmittedFrames = nextSubmittedFrames;
                state.nextPcmSequence = nextSequence;
            }
            state.pendingSubmittedFrames = state.successfulSubmittedFrames;
            audioState = state;
        } else if (renderIt != g_renderClients.end()) {
            if (sendSucceeded) {
                renderIt->second.successfulSubmittedFrames = nextSubmittedFrames;
                renderIt->second.nextPcmSequence = nextSequence;
            }
            renderState.successfulSubmittedFrames =
                    renderIt->second.successfulSubmittedFrames;
            renderState.nextPcmSequence = renderIt->second.nextPcmSequence;
        } else if (fakeOutput) {
            invalidationReason = "render-state-missing";
            fakeDeviceInvalidated = true;
        }
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
    if (shouldLogFlow) {
        Log("PCM flow. render=%p audio=%p intervalMs=%llu releaseCalls=%llu nonSilentFrames=%llu nonSilentBytes=%llu playerSilentFrames=%llu totalFrames=%llu maxReleaseGapMs=%llu sampleRate=%u queued=%llu hidden=%llu capacity=%u submitted=%llu logical=%llu accounted=%llu devicePosition=%llu managed=%s blocked=%s started=%s rendererState=%ld routeStream=%llu blockedStream=%llu generation=%ld coreLogical=%llu fakeOutput=%s",
            self,
            renderState.audioClient,
            static_cast<unsigned long long>(flowIntervalMs),
            static_cast<unsigned long long>(flowReleaseCalls),
            static_cast<unsigned long long>(flowNonSilentFrames),
            static_cast<unsigned long long>(flowNonSilentBytes),
            static_cast<unsigned long long>(flowPlayerSilentFrames),
            static_cast<unsigned long long>(
                    flowNonSilentFrames + flowPlayerSilentFrames),
            static_cast<unsigned long long>(flowMaxReleaseGapMs),
            audioState.format.Format.nSamplesPerSec,
            static_cast<unsigned long long>(audioState.fakeQueuedFrames),
            static_cast<unsigned long long>(
                    audioState.fakeDiscardPendingFrames),
            audioState.fakeBufferFrames,
            static_cast<unsigned long long>(audioState.successfulSubmittedFrames),
            static_cast<unsigned long long>(audioState.consumedLogicalFrames),
            static_cast<unsigned long long>(audioState.accountedLogicalFrames),
            static_cast<unsigned long long>(audioState.fakeDevicePosition),
            audioState.fakeBridgeManaged ? "yes" : "no",
            audioState.fakeAdmissionBlocked ? "yes" : "no",
            audioState.fakeStarted ? "yes" : "no",
            control.valid ? static_cast<long>(control.rendererState) : -1L,
            static_cast<unsigned long long>(control.valid ? control.streamId : 0),
            static_cast<unsigned long long>(
                    control.valid ? control.blockedStreamId : 0),
            control.valid ? control.streamGeneration : -1L,
            static_cast<unsigned long long>(
                    control.valid ? control.consumedLogicalFrames : 0),
            audioState.fakeOutput ? "on" : "off");
    }

    return {renderState.pendingBuffer,
            bytes,
            fakeDeviceInvalidated,
            invalidationReason,
            pipeError};
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
            const auto renderIt = g_renderClients.find(self);
            if (renderIt != g_renderClients.end()) {
                const auto audioIt = g_audioClients.find(renderIt->second.audioClient);
                if (audioIt != g_audioClients.end() && audioIt->second.fakeOutput) {
                    auto& audioState = audioIt->second;
                    audioState.fakeReservedFrames -=
                            (std::min<std::uint64_t>)(audioState.fakeReservedFrames,
                                                      renderIt->second.pendingFrames);
                }
                g_renderClients.erase(renderIt);
                removed = true;
            }
        }
        if (removed) {
            const auto count = g_renderClientCount.fetch_sub(1) - 1;
            LogFrequent(
                FrequentLogEvent::FakeRenderReleased,
                "Fake IAudioRenderClient released. active renderClients=%u render=%p audio=%p",
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

    AudioClientState audioState{};
    UINT32 availableFrames = 0;
    bool initialized = false;
    bool faulted = false;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        const auto audioIt = g_audioClients.find(fake->audioClient);
        if (audioIt != g_audioClients.end()) {
            auto& state = audioIt->second;
            initialized = state.fakeInitialized && state.hasFormat;
            if (initialized) {
                AdvanceFakePlaybackLocked(state, QpcNow());
                faulted = state.fakeFaulted;
                availableFrames = faulted ? 0 : FakeAvailableFramesLocked(state);
                if (!faulted && frameCount <= availableFrames) {
                    state.fakeReservedFrames += frameCount;
                }
                audioState = state;
            }
        }
    }

    if (!initialized) {
        Log("Fake output GetBuffer before Initialize. render=%p audio=%p frames=%u",
            self,
            fake->audioClient,
            frameCount);
        return AUDCLNT_E_NOT_INITIALIZED;
    }
    if (faulted) {
        LogDeviceInvalidated("Fake output GetBuffer", fake->audioClient, audioState);
        return AUDCLNT_E_DEVICE_INVALIDATED;
    }
    if (frameCount > availableFrames) {
        LogFrequent(
            FrequentLogEvent::FakeGetBufferUnavailable,
            "Fake output GetBuffer request exceeds available frames. render=%p audio=%p requested=%u available=%u",
            self,
            fake->audioClient,
            frameCount,
            availableFrames);
        return AUDCLNT_E_BUFFER_TOO_LARGE;
    }

    const std::uint32_t bytesPerFrame = BytesPerFrame(audioState);
    const std::uint64_t bytes =
            static_cast<std::uint64_t>(frameCount) * static_cast<std::uint64_t>(bytesPerFrame);
    if (bytes > 64ull * 1024ull * 1024ull) {
        Log("Fake output GetBuffer rejected huge request. render=%p frames=%u bytes=%llu",
            self,
            frameCount,
            static_cast<unsigned long long>(bytes));
        std::lock_guard<std::mutex> lock(g_stateMutex);
        const auto audioIt = g_audioClients.find(fake->audioClient);
        if (audioIt != g_audioClients.end()) {
            auto& state = audioIt->second;
            state.fakeReservedFrames -=
                    (std::min<std::uint64_t>)(state.fakeReservedFrames, frameCount);
        }
        return E_OUTOFMEMORY;
    }

    try {
        fake->buffer.assign(static_cast<std::size_t>(bytes), 0);
    } catch (...) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        const auto audioIt = g_audioClients.find(fake->audioClient);
        if (audioIt != g_audioClients.end()) {
            auto& state = audioIt->second;
            state.fakeReservedFrames -=
                    (std::min<std::uint64_t>)(state.fakeReservedFrames, frameCount);
        }
        return E_OUTOFMEMORY;
    }

    *data = fake->buffer.empty() ? nullptr : fake->buffer.data();
    fake->bufferOutstanding = true;
    fake->requestedFrames = frameCount;

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
    if (frameCount > fake->requestedFrames) {
        Log("Fake output ReleaseBuffer rejected invalid size. render=%p requested=%u released=%u flags=0x%08lX",
            self,
            fake->requestedFrames,
            frameCount,
            flags);
        return AUDCLNT_E_INVALID_SIZE;
    }
    fake->bufferOutstanding = false;
    const CaptureResult capture = CaptureReleasedBuffer(self, frameCount, flags);
    fake->requestedFrames = 0;
    if (capture.fakeDeviceInvalidated) {
        Log("Fake output ReleaseBuffer invalidated. reason=%s pipeError=%lu render=%p audio=%p frames=%u flags=0x%08lX",
            capture.invalidationReason,
            static_cast<unsigned long>(capture.pipeError),
            self,
            fake->audioClient,
            frameCount,
            flags);
        return AUDCLNT_E_DEVICE_INVALIDATED;
    }
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
    const LONGLONG nowQpc = QpcNow();
    LONGLONG positionQpc = nowQpc;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        const auto audioIt = g_audioClients.find(fake->audioClient);
        if (audioIt == g_audioClients.end() || !audioIt->second.fakeInitialized) {
            return AUDCLNT_E_NOT_INITIALIZED;
        }
        AdvanceFakePlaybackLocked(audioIt->second, nowQpc);
        if (audioIt->second.fakeFaulted) {
            return AUDCLNT_E_DEVICE_INVALIDATED;
        }
        *position = audioIt->second.fakeDevicePosition;
        if (audioIt->second.fakeDevicePositionQpc > 0) {
            positionQpc = audioIt->second.fakeDevicePositionQpc;
        }
    }
    if (qpcPosition != nullptr) {
        *qpcPosition = QpcToHns(positionQpc);
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

HRESULT STDMETHODCALLTYPE FakeStreamVolumeQueryInterface(IAudioStreamVolume* self,
                                                          REFIID iid,
                                                          void** out) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioStreamVolume)) {
        *out = self;
        reinterpret_cast<FakeAudioStreamVolume*>(self)->refs.fetch_add(1);
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE FakeStreamVolumeAddRef(IAudioStreamVolume* self) {
    return reinterpret_cast<FakeAudioStreamVolume*>(self)->refs.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE FakeStreamVolumeRelease(IAudioStreamVolume* self) {
    auto* fake = reinterpret_cast<FakeAudioStreamVolume*>(self);
    const ULONG refs = fake->refs.fetch_sub(1) - 1;
    if (refs == 0) {
        Log("Fake IAudioStreamVolume released. volume=%p audio=%p", self, fake->audioClient);
        delete fake;
    }
    return refs;
}

HRESULT STDMETHODCALLTYPE FakeStreamVolumeGetChannelCount(IAudioStreamVolume* self,
                                                           UINT32* channelCount) {
    if (channelCount == nullptr) {
        return E_POINTER;
    }
    auto* fake = reinterpret_cast<FakeAudioStreamVolume*>(self);
    std::lock_guard<std::mutex> lock(fake->mutex);
    *channelCount = static_cast<UINT32>(fake->volumes.size());
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeStreamVolumeSetChannelVolume(IAudioStreamVolume* self,
                                                            UINT32 channelIndex,
                                                            float level) {
    auto* fake = reinterpret_cast<FakeAudioStreamVolume*>(self);
    std::lock_guard<std::mutex> lock(fake->mutex);
    if (channelIndex >= fake->volumes.size() || level < 0.0f || level > 1.0f) {
        return E_INVALIDARG;
    }
    fake->volumes[channelIndex] = level;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeStreamVolumeGetChannelVolume(IAudioStreamVolume* self,
                                                            UINT32 channelIndex,
                                                            float* level) {
    if (level == nullptr) {
        return E_POINTER;
    }
    auto* fake = reinterpret_cast<FakeAudioStreamVolume*>(self);
    std::lock_guard<std::mutex> lock(fake->mutex);
    if (channelIndex >= fake->volumes.size()) {
        return E_INVALIDARG;
    }
    *level = fake->volumes[channelIndex];
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeStreamVolumeSetAllVolumes(IAudioStreamVolume* self,
                                                         UINT32 channelCount,
                                                         const float* levels) {
    if (levels == nullptr) {
        return E_POINTER;
    }
    auto* fake = reinterpret_cast<FakeAudioStreamVolume*>(self);
    std::lock_guard<std::mutex> lock(fake->mutex);
    if (channelCount != fake->volumes.size()) {
        return E_INVALIDARG;
    }
    for (UINT32 index = 0; index < channelCount; ++index) {
        if (levels[index] < 0.0f || levels[index] > 1.0f) {
            return E_INVALIDARG;
        }
    }
    std::copy(levels, levels + channelCount, fake->volumes.begin());
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeStreamVolumeGetAllVolumes(IAudioStreamVolume* self,
                                                         UINT32 channelCount,
                                                         float* levels) {
    if (levels == nullptr) {
        return E_POINTER;
    }
    auto* fake = reinterpret_cast<FakeAudioStreamVolume*>(self);
    std::lock_guard<std::mutex> lock(fake->mutex);
    if (channelCount != fake->volumes.size()) {
        return E_INVALIDARG;
    }
    std::copy(fake->volumes.begin(), fake->volumes.end(), levels);
    return S_OK;
}

HRESULT CopyComString(const std::wstring& value, LPWSTR* out) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    const std::size_t bytes = (value.size() + 1) * sizeof(wchar_t);
    auto* copy = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
    if (copy == nullptr) {
        return E_OUTOFMEMORY;
    }
    std::memcpy(copy, value.c_str(), bytes);
    *out = copy;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeSessionQueryInterface(IAudioSessionControl2* self,
                                                     REFIID iid,
                                                     void** out) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (iid == __uuidof(IUnknown) ||
        iid == __uuidof(IAudioSessionControl) ||
        iid == __uuidof(IAudioSessionControl2)) {
        *out = self;
        reinterpret_cast<FakeAudioSessionControl*>(self)->refs.fetch_add(1);
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE FakeSessionAddRef(IAudioSessionControl2* self) {
    return reinterpret_cast<FakeAudioSessionControl*>(self)->refs.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE FakeSessionRelease(IAudioSessionControl2* self) {
    auto* fake = reinterpret_cast<FakeAudioSessionControl*>(self);
    const ULONG refs = fake->refs.fetch_sub(1) - 1;
    if (refs == 0) {
        for (auto* notification : fake->notifications) {
            notification->Release();
        }
        Log("Fake IAudioSessionControl released. session=%p audio=%p", self, fake->audioClient);
        delete fake;
    }
    return refs;
}

HRESULT STDMETHODCALLTYPE FakeSessionGetState(IAudioSessionControl2* self,
                                               AudioSessionState* state) {
    if (state == nullptr) {
        return E_POINTER;
    }
    const auto* fake = reinterpret_cast<FakeAudioSessionControl*>(self);
    const AudioClientState audioState = SnapshotAudioClient(fake->audioClient);
    *state = audioState.fakeStarted
            ? AudioSessionStateActive
            : AudioSessionStateInactive;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeSessionGetDisplayName(IAudioSessionControl2* self,
                                                     LPWSTR* displayName) {
    auto* fake = reinterpret_cast<FakeAudioSessionControl*>(self);
    std::lock_guard<std::mutex> lock(fake->mutex);
    return CopyComString(fake->displayName, displayName);
}

HRESULT STDMETHODCALLTYPE FakeSessionSetDisplayName(IAudioSessionControl2* self,
                                                     LPCWSTR displayName,
                                                     LPCGUID) {
    if (displayName == nullptr) {
        return E_POINTER;
    }
    auto* fake = reinterpret_cast<FakeAudioSessionControl*>(self);
    std::lock_guard<std::mutex> lock(fake->mutex);
    fake->displayName = displayName;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeSessionGetIconPath(IAudioSessionControl2* self,
                                                  LPWSTR* iconPath) {
    auto* fake = reinterpret_cast<FakeAudioSessionControl*>(self);
    std::lock_guard<std::mutex> lock(fake->mutex);
    return CopyComString(fake->iconPath, iconPath);
}

HRESULT STDMETHODCALLTYPE FakeSessionSetIconPath(IAudioSessionControl2* self,
                                                  LPCWSTR iconPath,
                                                  LPCGUID) {
    if (iconPath == nullptr) {
        return E_POINTER;
    }
    auto* fake = reinterpret_cast<FakeAudioSessionControl*>(self);
    std::lock_guard<std::mutex> lock(fake->mutex);
    fake->iconPath = iconPath;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeSessionGetGroupingParam(IAudioSessionControl2* self,
                                                       GUID* groupingParam) {
    if (groupingParam == nullptr) {
        return E_POINTER;
    }
    auto* fake = reinterpret_cast<FakeAudioSessionControl*>(self);
    std::lock_guard<std::mutex> lock(fake->mutex);
    *groupingParam = fake->groupingParam;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeSessionSetGroupingParam(IAudioSessionControl2* self,
                                                       LPCGUID groupingParam,
                                                       LPCGUID) {
    if (groupingParam == nullptr) {
        return E_POINTER;
    }
    auto* fake = reinterpret_cast<FakeAudioSessionControl*>(self);
    std::lock_guard<std::mutex> lock(fake->mutex);
    fake->groupingParam = *groupingParam;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeSessionRegisterNotification(IAudioSessionControl2* self,
                                                          IAudioSessionEvents* notification) {
    if (notification == nullptr) {
        return E_POINTER;
    }
    auto* fake = reinterpret_cast<FakeAudioSessionControl*>(self);
    std::lock_guard<std::mutex> lock(fake->mutex);
    if (std::find(fake->notifications.begin(), fake->notifications.end(), notification) ==
        fake->notifications.end()) {
        notification->AddRef();
        fake->notifications.push_back(notification);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeSessionUnregisterNotification(IAudioSessionControl2* self,
                                                            IAudioSessionEvents* notification) {
    if (notification == nullptr) {
        return E_POINTER;
    }
    auto* fake = reinterpret_cast<FakeAudioSessionControl*>(self);
    std::lock_guard<std::mutex> lock(fake->mutex);
    const auto it = std::find(fake->notifications.begin(), fake->notifications.end(), notification);
    if (it != fake->notifications.end()) {
        (*it)->Release();
        fake->notifications.erase(it);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeSessionGetIdentifier(IAudioSessionControl2* self,
                                                    LPWSTR* identifier) {
    const auto* fake = reinterpret_cast<FakeAudioSessionControl*>(self);
    return CopyComString(L"TickByTick.Session." +
                         std::to_wstring(GetCurrentProcessId()) + L"." +
                         std::to_wstring(reinterpret_cast<std::uintptr_t>(fake->audioClient)),
                         identifier);
}

HRESULT STDMETHODCALLTYPE FakeSessionGetInstanceIdentifier(IAudioSessionControl2* self,
                                                            LPWSTR* identifier) {
    const auto* fake = reinterpret_cast<FakeAudioSessionControl*>(self);
    return CopyComString(L"TickByTick.Instance." +
                         std::to_wstring(GetCurrentProcessId()) + L"." +
                         std::to_wstring(reinterpret_cast<std::uintptr_t>(fake->audioClient)),
                         identifier);
}

HRESULT STDMETHODCALLTYPE FakeSessionGetProcessId(IAudioSessionControl2*, DWORD* processId) {
    if (processId == nullptr) {
        return E_POINTER;
    }
    *processId = GetCurrentProcessId();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeSessionIsSystemSounds(IAudioSessionControl2*) {
    return S_FALSE;
}

HRESULT STDMETHODCALLTYPE FakeSessionSetDuckingPreference(IAudioSessionControl2*, BOOL) {
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

void* g_fakeStreamVolumeVtable[] = {
        reinterpret_cast<void*>(&FakeStreamVolumeQueryInterface),
        reinterpret_cast<void*>(&FakeStreamVolumeAddRef),
        reinterpret_cast<void*>(&FakeStreamVolumeRelease),
        reinterpret_cast<void*>(&FakeStreamVolumeGetChannelCount),
        reinterpret_cast<void*>(&FakeStreamVolumeSetChannelVolume),
        reinterpret_cast<void*>(&FakeStreamVolumeGetChannelVolume),
        reinterpret_cast<void*>(&FakeStreamVolumeSetAllVolumes),
        reinterpret_cast<void*>(&FakeStreamVolumeGetAllVolumes),
};

void* g_fakeSessionControlVtable[] = {
        reinterpret_cast<void*>(&FakeSessionQueryInterface),
        reinterpret_cast<void*>(&FakeSessionAddRef),
        reinterpret_cast<void*>(&FakeSessionRelease),
        reinterpret_cast<void*>(&FakeSessionGetState),
        reinterpret_cast<void*>(&FakeSessionGetDisplayName),
        reinterpret_cast<void*>(&FakeSessionSetDisplayName),
        reinterpret_cast<void*>(&FakeSessionGetIconPath),
        reinterpret_cast<void*>(&FakeSessionSetIconPath),
        reinterpret_cast<void*>(&FakeSessionGetGroupingParam),
        reinterpret_cast<void*>(&FakeSessionSetGroupingParam),
        reinterpret_cast<void*>(&FakeSessionRegisterNotification),
        reinterpret_cast<void*>(&FakeSessionUnregisterNotification),
        reinterpret_cast<void*>(&FakeSessionGetIdentifier),
        reinterpret_cast<void*>(&FakeSessionGetInstanceIdentifier),
        reinterpret_cast<void*>(&FakeSessionGetProcessId),
        reinterpret_cast<void*>(&FakeSessionIsSystemSounds),
        reinterpret_cast<void*>(&FakeSessionSetDuckingPreference),
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
            LogFrequent(FrequentLogEvent::AudioClientCreated,
                        "IAudioClient created. iid=%s active clients=%u ptr=%p",
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
        LogFrequent(FrequentLogEvent::FakeQueryInterface,
                    "Fake output observed IAudioClient::QueryInterface. self=%p iid=%s name=%s hr=0x%08lX out=%p",
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
                LogFrequent(FrequentLogEvent::AudioClientQueried,
                            "IAudioClient queried. iid=%s active clients=%u ptr=%p",
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
        AudioClientState state = SnapshotAudioClient(self);
        if (state.fakeFaulted) {
            LogDeviceInvalidated("Fake output IAudioClient::Initialize", self, state);
            return AUDCLNT_E_DEVICE_INVALIDATED;
        }
        if (!LogWaveFormat("Fake IAudioClient::Initialize",
                           self,
                           format,
                           shareMode,
                           streamFlags,
                           state.fakePeriodFrames,
                           state.fakeBufferFrames,
                           state.streamId)) {
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_audioClients[self].fakeFaulted = true;
            }
            Log("Fake output Initialize invalidated because the format message could not be delivered. audio=%p stream=%llu",
                self,
                static_cast<unsigned long long>(state.streamId));
            return AUDCLNT_E_DEVICE_INVALIDATED;
        }
        LogFrequent(FrequentLogEvent::FakeInitializeAccepted,
                    "Fake output accepted IAudioClient::Initialize. audio=%p shareMode=%u flags=0x%08lX bufferHns=%lld periodicityHns=%lld requestedBufferFrames=%llu prebufferFrames=%u fakeBufferFrames=%u fakeBufferMs=%llu bufferCapped=%s fakePeriodFrames=%u",
                    self,
                    static_cast<unsigned>(shareMode),
                    streamFlags,
                    static_cast<long long>(bufferDuration),
                    static_cast<long long>(periodicity),
                    static_cast<unsigned long long>(
                            state.fakeRequestedBufferFrames),
                    state.fakePrebufferFrames,
                    state.fakeBufferFrames,
                    static_cast<unsigned long long>(HnsFromFrames(
                            state.fakeBufferFrames,
                            state.format.Format.nSamplesPerSec,
                            0) / 10000),
                    state.fakeBufferCapped ? "yes" : "no",
                    state.fakePeriodFrames);
        return S_OK;
    }

    const HRESULT hr = g_originalInitialize(
            self, shareMode, streamFlags, bufferDuration, periodicity, format, sessionGuid);
    if (SUCCEEDED(hr)) {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto& state = g_audioClients[self];
            EnsureAudioStreamIdLocked(state);
            CopyWaveFormat(format, &state);
            state.shareMode = shareMode;
            state.streamFlags = streamFlags;
        }

        const AudioClientState state = SnapshotAudioClient(self);
        if (!LogWaveFormat("IAudioClient::Initialize",
                           self,
                           format,
                           shareMode,
                           streamFlags,
                           0,
                           0,
                           state.streamId)) {
            Log("IAudioClient format message could not be delivered. audio=%p stream=%llu",
                self,
                static_cast<unsigned long long>(state.streamId));
        }
    }
    return hr;
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
        AudioClientState state = SnapshotAudioClient(audioClient);
        if (state.fakeFaulted) {
            LogDeviceInvalidated(
                "Fake output IAudioClient3::InitializeSharedAudioStream",
                audioClient,
                state);
            return AUDCLNT_E_DEVICE_INVALIDATED;
        }
        if (!LogWaveFormat("Fake IAudioClient3::InitializeSharedAudioStream",
                           audioClient,
                           format,
                           AUDCLNT_SHAREMODE_SHARED,
                           streamFlags,
                           periodInFrames,
                           state.fakeBufferFrames,
                           state.streamId)) {
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_audioClients[audioClient].fakeFaulted = true;
            }
            Log("Fake output InitializeSharedAudioStream invalidated because the format message could not be delivered. audio=%p stream=%llu",
                audioClient,
                static_cast<unsigned long long>(state.streamId));
            return AUDCLNT_E_DEVICE_INVALIDATED;
        }
        LogFrequent(FrequentLogEvent::FakeInitializeAccepted,
                    "Fake output accepted IAudioClient3::InitializeSharedAudioStream. audio=%p flags=0x%08lX requestedPeriodFrames=%u requestedBufferFrames=%llu prebufferFrames=%u fakeBufferFrames=%u fakeBufferMs=%llu bufferCapped=%s fakePeriodFrames=%u",
                    audioClient,
                    streamFlags,
                    periodInFrames,
                    static_cast<unsigned long long>(
                            state.fakeRequestedBufferFrames),
                    state.fakePrebufferFrames,
                    state.fakeBufferFrames,
                    static_cast<unsigned long long>(HnsFromFrames(
                            state.fakeBufferFrames,
                            state.format.Format.nSamplesPerSec,
                            0) / 10000),
                    state.fakeBufferCapped ? "yes" : "no",
                    state.fakePeriodFrames);
        return S_OK;
    }

    const HRESULT hr = g_originalInitializeSharedAudioStream(self,
                                                             streamFlags,
                                                             periodInFrames,
                                                             format,
                                                             audioSessionGuid);
    if (SUCCEEDED(hr)) {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto& state = g_audioClients[audioClient];
            EnsureAudioStreamIdLocked(state);
            CopyWaveFormat(format, &state);
            state.shareMode = AUDCLNT_SHAREMODE_SHARED;
            state.streamFlags = streamFlags;
        }

        const AudioClientState state = SnapshotAudioClient(audioClient);
        if (!LogWaveFormat("IAudioClient3::InitializeSharedAudioStream",
                           audioClient,
                           format,
                           AUDCLNT_SHAREMODE_SHARED,
                           streamFlags,
                           periodInFrames,
                           0,
                           state.streamId)) {
            Log("IAudioClient3 format message could not be delivered. audio=%p stream=%llu",
                audioClient,
                static_cast<unsigned long long>(state.streamId));
        }
    }
    return hr;
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
                LogFrequent(FrequentLogEvent::FakeRenderAcquired,
                            "Fake IAudioRenderClient acquired. active renderClients=%u active audioClients=%u render=%p audio=%p",
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
        if (iid == __uuidof(IAudioStreamVolume)) {
            if (!audioState.fakeInitialized) {
                Log("Fake output GetService(IAudioStreamVolume) before Initialize. audio=%p", self);
                return AUDCLNT_E_NOT_INITIALIZED;
            }
            auto* fake = new (std::nothrow) FakeAudioStreamVolume();
            if (fake == nullptr) {
                return E_OUTOFMEMORY;
            }
            fake->vtable = g_fakeStreamVolumeVtable;
            fake->audioClient = self;
            const UINT32 channelCount = audioState.format.Format.nChannels != 0
                    ? audioState.format.Format.nChannels
                    : 2;
            try {
                fake->volumes.assign(channelCount, 1.0f);
            } catch (...) {
                delete fake;
                return E_OUTOFMEMORY;
            }
            *service = reinterpret_cast<IAudioStreamVolume*>(fake);
            Log("Fake IAudioStreamVolume acquired. volume=%p audio=%p channels=%u",
                *service,
                self,
                channelCount);
            return S_OK;
        }
        if (iid == __uuidof(IAudioSessionControl) ||
            iid == __uuidof(IAudioSessionControl2)) {
            if (!audioState.fakeInitialized) {
                Log("Fake output GetService(IAudioSessionControl) before Initialize. audio=%p", self);
                return AUDCLNT_E_NOT_INITIALIZED;
            }
            auto* fake = new (std::nothrow) FakeAudioSessionControl();
            if (fake == nullptr) {
                return E_OUTOFMEMORY;
            }
            fake->vtable = g_fakeSessionControlVtable;
            fake->audioClient = self;
            *service = reinterpret_cast<IAudioSessionControl2*>(fake);
            Log("Fake IAudioSessionControl acquired. session=%p audio=%p",
                *service,
                self);
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
        LogFrequent(FrequentLogEvent::FakeGetBufferSize,
                    "Fake output GetBufferSize. audio=%p frames=%u",
                    self,
                    *bufferFrames);
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
        const UINT32 latencyFrames = state.fakePrebufferFrames != 0
                ? state.fakePrebufferFrames
                : state.fakeBufferFrames;
        *latency = HnsFromFrames(latencyFrames,
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
        bool initialized = false;
        bool faulted = false;
        AudioClientState diagnosticState{};
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            const auto audioIt = g_audioClients.find(self);
            if (audioIt != g_audioClients.end() && audioIt->second.fakeInitialized) {
                auto& state = audioIt->second;
                AdvanceFakePlaybackLocked(state, QpcNow());
                faulted = state.fakeFaulted;
                diagnosticState = state;
                if (!faulted) {
                    // Managed streams expose only player frames not yet
                    // admitted into Core's protected playout horizon. During
                    // a format boundary the endpoint reports a full ingress
                    // window; probe streams retain the legacy QPC queue.
                    *paddingFrames = state.fakeAdmissionBlocked
                            ? state.fakeBufferFrames
                            : static_cast<UINT32>(state.fakeQueuedFrames);
                }
                initialized = true;
            }
        }
        if (!initialized) {
            Log("Fake output GetCurrentPadding before Initialize. audio=%p", self);
            return AUDCLNT_E_NOT_INITIALIZED;
        }
        if (faulted) {
            LogDeviceInvalidated(
                "Fake output GetCurrentPadding", self, diagnosticState);
            return AUDCLNT_E_DEVICE_INVALIDATED;
        }
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
        LogFrequent(FrequentLogEvent::FakeGetMixFormat,
                    "Fake output GetMixFormat. audio=%p rate=%u channels=%u bits=%u tag=0x%04X",
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
        LogFrequent(FrequentLogEvent::FakeGetDevicePeriod,
                    "Fake output GetDevicePeriod. audio=%p defaultHns=%lld minHns=%lld",
                    self,
                    static_cast<long long>(state.fakeDefaultPeriod),
                    static_cast<long long>(state.fakeMinPeriod));
        return S_OK;
    }
    return g_originalGetDevicePeriod(self, defaultPeriod, minimumPeriod);
}

HRESULT STDMETHODCALLTYPE HookStart(IAudioClient* self) {
    if (!g_isBootstrapping && FakeOutputEnabled()) {
        bool invalidated = false;
        AudioClientState diagnosticState{};
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto& state = g_audioClients[self];
            if (!state.fakeInitialized) {
                Log("Fake output Start before Initialize. audio=%p", self);
                return AUDCLNT_E_NOT_INITIALIZED;
            }
            AdvanceFakePlaybackLocked(state, QpcNow());
            if (state.fakeFaulted) {
                invalidated = true;
                diagnosticState = state;
            } else if (!state.fakeStarted) {
                const LONGLONG nowQpc = QpcNow();
                if (state.fakeHasStarted &&
                    state.consumedLogicalFrames >= state.accountedLogicalFrames) {
                    // Frames retired while Stop was active are discarded from
                    // virtual padding without advancing the paused clock.
                    const std::uint64_t releasableFrames =
                            state.consumedLogicalFrames -
                            state.accountedLogicalFrames;
                    const std::uint64_t discardedFrames = (std::min)(
                            state.fakeDiscardPendingFrames, releasableFrames);
                    state.fakeDiscardPendingFrames -= discardedFrames;
                    const std::uint64_t queuedRelease = (std::min)(
                            state.fakeQueuedFrames,
                            releasableFrames - discardedFrames);
                    state.fakeQueuedFrames -= queuedRelease;
                    state.accountedLogicalFrames = state.consumedLogicalFrames;
                }
                state.fakeStarted = true;
                state.fakeHasStarted = true;
                state.fakeLastUpdateQpc = nowQpc;
                state.fakeDevicePositionQpc = nowQpc;
                state.fakeNextEventQpc = nowQpc + QpcTicksForFakePeriod(state);
                if (state.fakeEvent != nullptr && FakeEventReadyLocked(state)) {
                    SetEvent(state.fakeEvent);
                }
            }
        }
        if (invalidated) {
            LogDeviceInvalidated("Fake output Start", self, diagnosticState);
            return AUDCLNT_E_DEVICE_INVALIDATED;
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
            AdvanceFakePlaybackLocked(state, QpcNow());
            if (state.fakeFaulted) {
                return AUDCLNT_E_DEVICE_INVALIDATED;
            }
            state.fakeStarted = false;
            state.fakeLastUpdateQpc = 0;
            state.fakeNextEventQpc = 0;
        }
        LogFrequent(FrequentLogEvent::FakeStop,
                    "Fake output Stop. audio=%p",
                    self);
        return S_OK;
    }
    return g_originalStop(self);
}

HRESULT STDMETHODCALLTYPE HookReset(IAudioClient* self) {
    if (!g_isBootstrapping && FakeOutputEnabled()) {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto& state = g_audioClients[self];
            const LONGLONG nowQpc = QpcNow();
            AdvanceFakePlaybackLocked(state, nowQpc);
            if (state.fakeFaulted) {
                return AUDCLNT_E_DEVICE_INVALIDATED;
            }
            state.fakeDevicePosition = 0;
            state.fakeDevicePositionQpc = nowQpc;
            state.fakeHasStarted = false;
            if (state.fakeBridgeManaged) {
                if (state.fakeDiscardPendingFrames > state.fakeBufferFrames ||
                    state.fakeQueuedFrames >
                    state.fakeBufferFrames - state.fakeDiscardPendingFrames) {
                    state.fakeFaulted = true;
                    return AUDCLNT_E_DEVICE_INVALIDATED;
                }
                // Reset makes the old padding invisible to the application,
                // but Core may already own those frames. Keep that retired
                // backlog against endpoint capacity until Core admits those
                // player frames into its protected timeline, so repeated
                // Reset/Start cycles cannot stack virtual ingress windows.
                state.fakeDiscardPendingFrames += state.fakeQueuedFrames;
            } else {
                state.fakeDiscardPendingFrames = 0;
                state.accountedLogicalFrames = 0;
            }
            state.fakeQueuedFrames = 0;
            state.fakeReservedFrames = 0;
            state.fakeFrameRemainder = 0;
            state.fakeLastUpdateQpc = state.fakeStarted ? nowQpc : 0;
            state.fakeNextEventQpc = state.fakeStarted
                    ? nowQpc + QpcTicksForFakePeriod(state)
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
    bool lateAttached = false;
    if (SUCCEEDED(hr) && data != nullptr) {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto [it, inserted] = g_renderClients.try_emplace(self);
            auto& state = it->second;
            if (inserted) {
                state.lateAttached = true;
                state.streamId = AllocateStreamId();
                lateAttached = true;
            }
            state.pendingBuffer = *data;
            state.pendingFrames = frameCount;
        }
        if (lateAttached) {
            const auto count = ++g_renderClientCount;
            Log("Late-attached IAudioRenderClient observed. active renderClients=%u render=%p",
                count,
                self);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookReleaseBuffer(IAudioRenderClient* self, UINT32 frameCount, DWORD flags) {
    const CaptureResult capture = CaptureReleasedBuffer(self, frameCount, flags);

#if defined(TICKBYTICK_WASAPI_HOOK_SILENCE_ORIGINAL)
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
            LogFrequent(FrequentLogEvent::AudioClientReleased,
                        "IAudioClient released. active clients=%u ptr=%p",
                        count,
                        self);
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
            LogFrequent(FrequentLogEvent::EnumeratorCreated,
                        "MMDeviceEnumerator created and patched. iid=%s ptr=%p",
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
    return std::wstring(TICKBYTICK_HOOK_READY_EVENT_PREFIX) + std::to_wstring(pid);
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
    std::lock_guard<std::mutex> lock(g_controlMutex);
    if (g_control != nullptr) {
        return;
    }

    g_controlMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kControlMapName);
    if (g_controlMapping == nullptr) {
        return;
    }
    g_control = static_cast<HookControlBlock*>(
            MapViewOfFile(g_controlMapping, FILE_MAP_READ, 0, 0, sizeof(HookControlBlock)));
    if (g_control == nullptr) {
        CloseHandle(g_controlMapping);
        g_controlMapping = nullptr;
    }
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
    ControlSnapshot snapshot{};
    if (ReadControlSnapshot(&snapshot)) {
        SetFakeOutputFromControl(snapshot.fakeOutput);
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
            AdvanceFakePlaybackLocked(state, now);
            if (state.fakeOutput &&
                !state.fakeFaulted &&
                state.fakeStarted &&
                state.fakeEvent != nullptr &&
                FakeEventReadyLocked(state) &&
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
    ControlSnapshot snapshot{};
    if (!ReadControlSnapshot(&snapshot)) {
        return;
    }

    const DWORD lockedPid = snapshot.lockedPid;
    const bool finishCapture = snapshot.finish;
    SetFakeOutputFromControl(snapshot.fakeOutput);
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

    WAVEFORMATEX* mixFormat = nullptr;
    const HRESULT mixFormatResult = audioClient->GetMixFormat(&mixFormat);
    if (SUCCEEDED(mixFormatResult) && mixFormat != nullptr) {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            CopyWaveFormat(mixFormat, &g_bootstrapAudioState);
            g_bootstrapAudioState.shareMode = AUDCLNT_SHAREMODE_SHARED;
            g_bootstrapAudioState.streamFlags = 0;
        }

        const HRESULT initializeResult = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                                                 0,
                                                                 100000,
                                                                 0,
                                                                 mixFormat,
                                                                 nullptr);
        if (SUCCEEDED(initializeResult)) {
            IAudioRenderClient* renderClient = nullptr;
            const HRESULT serviceResult = audioClient->GetService(
                    __uuidof(IAudioRenderClient),
                    reinterpret_cast<void**>(&renderClient));
            if (SUCCEEDED(serviceResult) && renderClient != nullptr) {
                renderClient->Release();
                Log("IAudioRenderClient bootstrap patched for late attachment.");
            } else {
                Log("IAudioRenderClient bootstrap GetService failed. hr=0x%08lX",
                    static_cast<unsigned long>(serviceResult));
            }
        } else {
            Log("IAudioRenderClient bootstrap Initialize failed. hr=0x%08lX",
                static_cast<unsigned long>(initializeResult));
        }
        CoTaskMemFree(mixFormat);
    } else {
        Log("IAudioRenderClient bootstrap GetMixFormat failed. hr=0x%08lX",
            static_cast<unsigned long>(mixFormatResult));
    }

    Log("IAudioClient bootstrap patched. audio=%p", audioClient);
    audioClient->Release();
    g_isBootstrapping = false;
}

DWORD WINAPI HookThread(void*) {
    Log("Tick By Tick WASAPI hook injected.");
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
        Log("Tick By Tick WASAPI hook detached.");
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

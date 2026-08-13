#include "AudioBridgeCore.h"

#include "AsioRenderer.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <ks.h>
#include <ksmedia.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace audiobridge {
namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\AudioBridgeWasapiHook";
constexpr wchar_t kControlMapName[] = L"Local\\AudioBridgeWasapiHookControl";
constexpr wchar_t kHookReadyEventPrefix[] = L"Local\\AudioBridgeHookReady_";
constexpr DWORD kHookReadyTimeoutMs = 5000;
constexpr DWORD kPipeMagic = 0x48504241;  // ABPH
constexpr DWORD kPipeText = 1;
constexpr DWORD kPipeFormat = 2;
constexpr DWORD kPipePcm = 3;
constexpr DWORD kPipeFinish = 4;

constexpr int32_t kOk = 0;
constexpr int32_t kError = -1;
constexpr int32_t kInvalidArgument = -2;
constexpr int32_t kNotRunning = -3;
constexpr int32_t kDefaultPrebufferMs = 300;
constexpr int32_t kDefaultAsioBufferFrames = 0;
#if defined(_WIN64)
constexpr REGSAM kAsioRegistryView = KEY_WOW64_64KEY;
#else
constexpr REGSAM kAsioRegistryView = KEY_WOW64_32KEY;
#endif

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

struct DeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

struct AudioPidState {
    WAVEFORMATEXTENSIBLE format{};
    bool hasFormat = false;
    std::uint32_t bytesPerFrame = 0;
    std::uint64_t lastFormatMs = 0;
    std::uint64_t lastPcmMs = 0;
};

struct ProcessSnapshotEntry {
    DWORD pid = 0;
    DWORD parentPid = 0;
};

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
}

void NativeDiagnostic(const wchar_t* message) {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path))) == 0) {
        return;
    }

    wchar_t* slash = std::wcsrchr(path, L'\\');
    if (slash == nullptr) {
        return;
    }
    *(slash + 1) = L'\0';
    if (std::wcslen(path) + std::wcslen(L"AudioBridgeCore.native.log") + 1 >= std::size(path)) {
        return;
    }
    wcscat_s(path, L"AudioBridgeCore.native.log");

    HANDLE file = CreateFileW(path,
                              FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t line[1600]{};
    std::swprintf(line,
                  std::size(line),
                  L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
                  now.wYear,
                  now.wMonth,
                  now.wDay,
                  now.wHour,
                  now.wMinute,
                  now.wSecond,
                  now.wMilliseconds,
                  message != nullptr ? message : L"(null)");

    char utf8[4096]{};
    const int bytes = WideCharToMultiByte(CP_UTF8,
                                          0,
                                          line,
                                          -1,
                                          utf8,
                                          static_cast<int>(std::size(utf8)),
                                          nullptr,
                                          nullptr);
    if (bytes > 1) {
        DWORD written = 0;
        WriteFile(file, utf8, static_cast<DWORD>(bytes - 1), &written, nullptr);
    }
    CloseHandle(file);
}

std::wstring Win32Message(const wchar_t* action, DWORD error = GetLastError()) {
    wchar_t systemMessage[512]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr,
                   error,
                   0,
                   systemMessage,
                   static_cast<DWORD>(std::size(systemMessage)),
                   nullptr);
    wchar_t buffer[900]{};
    std::swprintf(buffer,
                  std::size(buffer),
                  L"%s failed: %lu %s",
                  action != nullptr ? action : L"Win32 call",
                  error,
                  systemMessage);
    return buffer;
}

std::wstring QuoteCommandArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'"') {
            result += L'\\';
        }
        result += ch;
    }
    result += L"\"";
    return result;
}

bool ReadExact(HANDLE pipe, void* data, DWORD bytes) {
    auto* cursor = static_cast<std::uint8_t*>(data);
    DWORD remaining = bytes;
    while (remaining > 0) {
        DWORD read = 0;
        if (!ReadFile(pipe, cursor, remaining, &read, nullptr) || read == 0) {
            return false;
        }
        cursor += read;
        remaining -= read;
    }
    return true;
}

std::wstring Utf8ToWide(const char* data, std::size_t bytes) {
    if (data == nullptr || bytes == 0) {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8,
                                             MB_ERR_INVALID_CHARS,
                                             data,
                                             static_cast<int>(bytes),
                                             nullptr,
                                             0);
    if (required > 0) {
        std::wstring result(static_cast<std::size_t>(required), L'\0');
        MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            data,
                            static_cast<int>(bytes),
                            result.data(),
                            required);
        return result;
    }

    const int fallbackRequired = MultiByteToWideChar(CP_ACP,
                                                     0,
                                                     data,
                                                     static_cast<int>(bytes),
                                                     nullptr,
                                                     0);
    if (fallbackRequired <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(fallbackRequired), L'\0');
    MultiByteToWideChar(CP_ACP,
                        0,
                        data,
                        static_cast<int>(bytes),
                        result.data(),
                        fallbackRequired);
    return result;
}

int32_t CopyWideString(const std::wstring& value, wchar_t* buffer, int32_t bufferChars) {
    if (buffer == nullptr || bufferChars <= 0) {
        return kInvalidArgument;
    }
    const auto copyChars =
            (std::min)(static_cast<std::size_t>(bufferChars - 1), value.size());
    std::wmemcpy(buffer, value.data(), copyChars);
    buffer[copyChars] = L'\0';
    return static_cast<int32_t>(copyChars);
}

std::uint32_t BytesPerFrame(const WAVEFORMATEX& format) {
    if (format.nBlockAlign != 0) {
        return format.nBlockAlign;
    }
    return format.nChannels * ((format.wBitsPerSample + 7U) / 8U);
}

std::uint8_t SampleFormatValue(const WAVEFORMATEXTENSIBLE& format) {
    if (format.Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return 2;
    }
    if (format.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        IsEqualGUID(format.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
        return 2;
    }
    return 1;
}

bool WaveFormatsEqual(const WAVEFORMATEXTENSIBLE& left,
                      const WAVEFORMATEXTENSIBLE& right) {
    return std::memcmp(&left, &right, sizeof(WAVEFORMATEXTENSIBLE)) == 0;
}

bool QueryProcessMachine(HANDLE process, USHORT* processMachine, USHORT* nativeMachine) {
    auto* isWow64Process2 = reinterpret_cast<decltype(&IsWow64Process2)>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (isWow64Process2 != nullptr) {
        return isWow64Process2(process, processMachine, nativeMachine) != FALSE;
    }

    BOOL isWow64 = FALSE;
    if (!IsWow64Process(process, &isWow64)) {
        return false;
    }
#if defined(_M_X64)
    *nativeMachine = IMAGE_FILE_MACHINE_AMD64;
    *processMachine = isWow64 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_UNKNOWN;
#elif defined(_M_IX86)
    *nativeMachine = IMAGE_FILE_MACHINE_I386;
    *processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
#elif defined(_M_ARM64)
    *nativeMachine = IMAGE_FILE_MACHINE_ARM64;
    *processMachine = isWow64 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_UNKNOWN;
#else
    *nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    *processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
#endif
    return true;
}

bool EffectiveMachine(HANDLE process, USHORT* effectiveMachine) {
    USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    if (!QueryProcessMachine(process, &processMachine, &nativeMachine)) {
        return false;
    }
    *effectiveMachine =
            processMachine == IMAGE_FILE_MACHINE_UNKNOWN ? nativeMachine : processMachine;
    return true;
}

std::int32_t NormalizeAsioBufferFrames(std::int32_t frames) {
    if (frames <= 0) {
        return kDefaultAsioBufferFrames;
    }
    return (std::min<std::int32_t>)((std::max<std::int32_t>)(frames, 1), 32768);
}

std::wstring ReadRegistryString(HKEY key, const wchar_t* valueName) {
    DWORD type = 0;
    DWORD bytes = 0;
    LONG result = RegGetValueW(key,
                               nullptr,
                               valueName,
                               RRF_RT_REG_SZ,
                               &type,
                               nullptr,
                               &bytes);
    if (result != ERROR_SUCCESS || bytes <= sizeof(wchar_t)) {
        return {};
    }

    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    result = RegGetValueW(key,
                          nullptr,
                          valueName,
                          RRF_RT_REG_SZ,
                          &type,
                          value.data(),
                          &bytes);
    if (result != ERROR_SUCCESS) {
        return {};
    }
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

std::wstring Lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::wstring NormalizePathForComparison(const std::filesystem::path& path) {
    std::wstring result;
    try {
        result = std::filesystem::absolute(path).lexically_normal().wstring();
    } catch (...) {
        result = path.wstring();
    }
    if (result.size() >= 4 && result.compare(0, 4, L"\\\\?\\") == 0) {
        result.erase(0, 4);
    }
    return Lowercase(std::move(result));
}

std::wstring QueryProcessImagePath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return {};
    }

    std::wstring path(32768, L'\0');
    DWORD chars = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &chars)) {
        CloseHandle(process);
        return {};
    }
    CloseHandle(process);
    path.resize(chars);
    return path;
}

std::vector<ProcessSnapshotEntry> SnapshotProcesses() {
    std::vector<ProcessSnapshotEntry> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return result;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            ProcessSnapshotEntry item{};
            item.pid = entry.th32ProcessID;
            item.parentPid = entry.th32ParentProcessID;
            result.push_back(std::move(item));
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

std::vector<DWORD> FindProcessesByImagePath(const std::filesystem::path& imagePath) {
    const auto expectedPath = NormalizePathForComparison(imagePath);
    std::vector<DWORD> result;
    for (const auto& process : SnapshotProcesses()) {
        const auto processPath = QueryProcessImagePath(process.pid);
        if (!processPath.empty() && NormalizePathForComparison(processPath) == expectedPath) {
            result.push_back(process.pid);
        }
    }
    return result;
}

bool IsDescendantProcess(DWORD pid,
                         DWORD rootPid,
                         const std::unordered_map<DWORD, DWORD>& parents) {
    std::unordered_set<DWORD> visited;
    DWORD current = pid;
    while (current != 0 && visited.insert(current).second) {
        const auto it = parents.find(current);
        if (it == parents.end()) {
            return false;
        }
        current = it->second;
        if (current == rootPid) {
            return true;
        }
    }
    return false;
}

std::wstring HookReadyEventName(DWORD pid) {
    return std::wstring(kHookReadyEventPrefix) + std::to_wstring(pid);
}

bool IsModuleLoadedInProcess(DWORD pid, const std::filesystem::path& modulePath) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    const auto expectedPath = NormalizePathForComparison(modulePath);
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    bool found = false;
    if (Module32FirstW(snapshot, &module)) {
        do {
            if (NormalizePathForComparison(module.szExePath) == expectedPath) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &module));
    }
    CloseHandle(snapshot);
    return found;
}

std::vector<DeviceInfo> EnumerateAsioDevices() {
    HKEY asioRoot = nullptr;
    const LONG openResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                          L"SOFTWARE\\ASIO",
                                          0,
                                          KEY_READ | kAsioRegistryView,
                                          &asioRoot);
    if (openResult != ERROR_SUCCESS) {
        return {};
    }

    std::vector<DeviceInfo> devices;
    for (DWORD index = 0;; ++index) {
        wchar_t keyName[256]{};
        DWORD keyNameChars = static_cast<DWORD>(std::size(keyName));
        const LONG enumResult =
                RegEnumKeyExW(asioRoot, index, keyName, &keyNameChars, nullptr, nullptr, nullptr, nullptr);
        if (enumResult == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (enumResult != ERROR_SUCCESS) {
            continue;
        }

        HKEY driverKey = nullptr;
        if (RegOpenKeyExW(asioRoot, keyName, 0, KEY_READ, &driverKey) != ERROR_SUCCESS) {
            continue;
        }

        DeviceInfo info{};
        info.id = ReadRegistryString(driverKey, L"CLSID");
        info.name = ReadRegistryString(driverKey, L"description");
        if (info.name.empty()) {
            info.name = keyName;
        }
        RegCloseKey(driverKey);

        CLSID clsid{};
        if (info.id.empty() || FAILED(CLSIDFromString(info.id.c_str(), &clsid))) {
            continue;
        }
        devices.push_back(std::move(info));
    }
    RegCloseKey(asioRoot);

    std::sort(devices.begin(), devices.end(), [](const DeviceInfo& left, const DeviceInfo& right) {
        return Lowercase(left.name) < Lowercase(right.name);
    });
    return devices;
}

int32_t FindDefaultAsioDeviceIndex(const std::vector<DeviceInfo>& devices,
                                   const std::wstring& selectedDeviceId) {
    if (devices.empty()) {
        return -1;
    }
    if (!selectedDeviceId.empty()) {
        for (int32_t i = 0; i < static_cast<int32_t>(devices.size()); ++i) {
            if (devices[static_cast<std::size_t>(i)].id == selectedDeviceId) {
                return i;
            }
        }
    }

    return 0;
}

class AudioBridgeCore {
public:
    AudioBridgeCore();
    ~AudioBridgeCore();

    bool Initialize();
    void Shutdown();
    int32_t StartTarget(const wchar_t* exePath,
                        const wchar_t* hookDllPath,
                        const wchar_t* outputDeviceId,
                        std::int32_t prebufferMs,
                        std::int32_t asioBufferFrames,
                        bool fakeOutput);
    void Stop();
    int32_t SetOutputDevice(const wchar_t* outputDeviceId);
    int32_t SetPrebufferMs(std::int32_t prebufferMs);
    int32_t GetPrebufferMs() const;
    int32_t SelectAudioPid(std::uint32_t pid);
    int32_t RefreshDevices();
    int32_t GetDeviceCount() const;
    int32_t GetDefaultDeviceIndex() const;
    int32_t GetDeviceId(int32_t index, wchar_t* buffer, int32_t bufferChars) const;
    int32_t GetDeviceName(int32_t index, wchar_t* buffer, int32_t bufferChars) const;
    int32_t GetPidCount() const;
    int32_t GetPidInfo(int32_t index, ABC_PidInfo* info) const;
    int32_t GetStatus(ABC_Status* status) const;
    int32_t DrainLog(wchar_t* buffer, int32_t bufferChars);
    int32_t GetLastError(wchar_t* buffer, int32_t bufferChars) const;

private:
    void SetLastErrorLocked(const std::wstring& error);
    void Log(const wchar_t* format, ...);
    void WriteControlState(std::uint32_t audioPid, bool finish);
    bool StartPipeServer(std::wstring* outError);
    void PipeServerThread();
    void HandlePipeClient(HANDLE pipe);
    void HandlePipeMessage(DWORD type, DWORD pid, const std::vector<std::uint8_t>& payload);
    void HandleFormatMessage(DWORD pid, const void* payload, std::size_t payloadBytes);
    void HandlePcmMessage(DWORD pid, const std::uint8_t* data, std::size_t bytes);
    void StartRendererForFormat(std::uint32_t pid, const WAVEFORMATEXTENSIBLE& format);
    bool LaunchAndInjectTarget(const std::filesystem::path& exePath,
                               const std::filesystem::path& hookDllPath,
                               std::wstring* outError);
    void StartProcessMonitor();
    void ProcessMonitorThread();
    bool InjectProcessByPid(DWORD pid, std::wstring* outError);
    bool WaitForHookReady(HANDLE readyEvent, DWORD pid, std::wstring* outError);
    bool InjectDll(HANDLE process, const std::filesystem::path& dllPath, std::wstring* outError);
    bool InjectDllWithHelper(DWORD pid,
                             const std::filesystem::path& helperPath,
                             const std::filesystem::path& dllPath,
                             std::wstring* outError);
    bool CheckArchitectureMatch(HANDLE process, std::wstring* outError);
    void AddActivePipe(HANDLE pipe);
    void RemoveActivePipe(HANDLE pipe);
    void CancelActivePipes();
    std::filesystem::path DefaultHookDllPath(USHORT targetMachine = IMAGE_FILE_MACHINE_AMD64) const;
    std::filesystem::path DefaultInjectHelperPath() const;
    std::vector<ABC_PidInfo> SnapshotPidsLocked() const;

    mutable std::mutex stateMutex_;
    std::wstring lastError_ = L"OK";
    std::wstring logBuffer_;
    std::vector<DeviceInfo> devices_;
    int32_t defaultDeviceIndex_ = -1;
    std::wstring selectedDeviceId_;
    std::int32_t prebufferMs_ = kDefaultPrebufferMs;
    std::int32_t asioBufferFrames_ = kDefaultAsioBufferFrames;
    std::atomic<bool> fakeOutput_{false};
    std::unordered_map<std::uint32_t, AudioPidState> audioPids_;
    bool rendererHasFormat_ = false;
    std::uint32_t rendererPid_ = 0;
    std::wstring rendererDeviceId_;
    WAVEFORMATEXTENSIBLE rendererFormat_{};
    std::int32_t rendererPrebufferMs_ = kDefaultPrebufferMs;
    std::int32_t rendererAsioBufferFrames_ = kDefaultAsioBufferFrames;

    std::atomic<bool> running_{false};
    std::atomic<std::uint32_t> lockedAudioPid_{0};
    std::uint32_t targetPid_ = 0;
    HANDLE targetProcess_ = nullptr;
    std::filesystem::path targetExePath_;
    std::unordered_set<DWORD> injectedPids_;
    std::unordered_map<DWORD, int> injectionAttempts_;
    std::mutex processMonitorMutex_;
    std::thread processMonitorThread_;
    HANDLE controlMapping_ = nullptr;
    HookControlBlock* control_ = nullptr;
    std::thread pipeThread_;

    mutable std::mutex clientThreadsMutex_;
    std::vector<std::thread> clientThreads_;
    std::mutex activePipesMutex_;
    std::vector<HANDLE> activePipes_;

    AsioRenderer renderer_;
};

AudioBridgeCore::AudioBridgeCore() {
    NativeDiagnostic(L"AudioBridgeCore constructor enter.");
    NativeDiagnostic(L"AudioBridgeCore constructor success.");
}

AudioBridgeCore::~AudioBridgeCore() {
    NativeDiagnostic(L"AudioBridgeCore destructor enter.");
    Shutdown();
    NativeDiagnostic(L"AudioBridgeCore destructor success.");
}

bool AudioBridgeCore::Initialize() {
    NativeDiagnostic(L"AudioBridgeCore::Initialize enter.");
    std::lock_guard<std::mutex> lock(stateMutex_);
    NativeDiagnostic(L"AudioBridgeCore::Initialize lock acquired.");
    SetLastErrorLocked(L"OK");
    NativeDiagnostic(L"AudioBridgeCore::Initialize success.");
    return true;
}

void AudioBridgeCore::Shutdown() {
    Stop();
}

int32_t AudioBridgeCore::StartTarget(const wchar_t* exePath,
                                     const wchar_t* hookDllPath,
                                     const wchar_t* outputDeviceId,
                                     std::int32_t prebufferMs,
                                     std::int32_t asioBufferFrames,
                                     bool fakeOutput) {
    if (exePath == nullptr || exePath[0] == L'\0') {
        std::lock_guard<std::mutex> lock(stateMutex_);
        SetLastErrorLocked(L"Target exe path is empty.");
        return kInvalidArgument;
    }

    Stop();

    const std::filesystem::path targetPath(exePath);
    const std::filesystem::path hookPath =
            hookDllPath != nullptr && hookDllPath[0] != L'\0'
                    ? std::filesystem::path(hookDllPath)
                    : std::filesystem::path();

    if (!std::filesystem::exists(targetPath)) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        SetLastErrorLocked(L"Target exe does not exist: " + targetPath.wstring());
        return kInvalidArgument;
    }
    if (!hookPath.empty() && !std::filesystem::exists(hookPath)) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        SetLastErrorLocked(L"Hook DLL does not exist: " + hookPath.wstring());
        return kInvalidArgument;
    }

    const auto existingTargetPids = FindProcessesByImagePath(targetPath);
    if (!existingTargetPids.empty()) {
        std::wstring pidList;
        for (DWORD pid : existingTargetPids) {
            if (!pidList.empty()) {
                pidList += L", ";
            }
            pidList += std::to_wstring(pid);
        }
        std::lock_guard<std::mutex> lock(stateMutex_);
        SetLastErrorLocked(
                L"The selected application is already running (PID " + pidList +
                L"). Exit every instance, then open it again through AudioBridge so the "
                L"audio process can be captured from startup.");
        return kInvalidArgument;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        selectedDeviceId_ = outputDeviceId != nullptr ? outputDeviceId : L"";
        prebufferMs_ = prebufferMs < 0
                ? kDefaultPrebufferMs
                : static_cast<std::int32_t>(
                          (std::min<std::int32_t>)(prebufferMs, 10000));
        asioBufferFrames_ = NormalizeAsioBufferFrames(asioBufferFrames);
        fakeOutput_.store(fakeOutput);
        audioPids_.clear();
        rendererHasFormat_ = false;
        rendererPid_ = 0;
        rendererDeviceId_.clear();
        rendererFormat_ = {};
        rendererPrebufferMs_ = prebufferMs_;
        rendererAsioBufferFrames_ = asioBufferFrames_;
        targetPid_ = 0;
        lockedAudioPid_ = 0;
        logBuffer_.clear();
        SetLastErrorLocked(L"OK");
    }

    controlMapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE,
                                         nullptr,
                                         PAGE_READWRITE,
                                         0,
                                         sizeof(HookControlBlock),
                                         kControlMapName);
    if (controlMapping_ == nullptr) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        SetLastErrorLocked(Win32Message(L"CreateFileMappingW"));
        return kError;
    }

    control_ = static_cast<HookControlBlock*>(
            MapViewOfFile(controlMapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HookControlBlock)));
    if (control_ == nullptr) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        SetLastErrorLocked(Win32Message(L"MapViewOfFile"));
        CloseHandle(controlMapping_);
        controlMapping_ = nullptr;
        return kError;
    }

    WriteControlState(0, false);
    running_ = true;

    std::wstring error;
    if (!StartPipeServer(&error) || !LaunchAndInjectTarget(targetPath, hookPath, &error)) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            SetLastErrorLocked(error.empty() ? L"Failed to start target." : error);
        }
        Stop();
        return kError;
    }

    {
        std::lock_guard<std::mutex> lock(processMonitorMutex_);
        targetExePath_ = std::filesystem::absolute(targetPath);
        injectedPids_.clear();
        injectedPids_.insert(targetPid_);
        injectionAttempts_.clear();
    }
    StartProcessMonitor();

    Log(L"Started target. pid=%u fakeOutput=%s exe=\"%s\"",
        targetPid_,
        fakeOutput_.load() ? L"on" : L"off",
        targetPath.wstring().c_str());
    return kOk;
}

void AudioBridgeCore::Stop() {
    if (!running_.exchange(false)) {
        renderer_.Stop();
        return;
    }

    if (processMonitorThread_.joinable()) {
        processMonitorThread_.join();
    }

    WriteControlState(lockedAudioPid_.load(), true);
    HANDLE wakePipe = CreateFileW(kPipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (wakePipe != INVALID_HANDLE_VALUE) {
        CloseHandle(wakePipe);
    }

    if (pipeThread_.joinable()) {
        pipeThread_.join();
    }

    CancelActivePipes();
    std::vector<std::thread> clients;
    {
        std::lock_guard<std::mutex> lock(clientThreadsMutex_);
        clients.swap(clientThreads_);
    }
    for (auto& client : clients) {
        if (client.joinable()) {
            client.join();
        }
    }

    renderer_.Stop();

    if (targetProcess_ != nullptr) {
        CloseHandle(targetProcess_);
        targetProcess_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(processMonitorMutex_);
        targetExePath_.clear();
        injectedPids_.clear();
        injectionAttempts_.clear();
    }
    if (control_ != nullptr) {
        UnmapViewOfFile(control_);
        control_ = nullptr;
    }
    if (controlMapping_ != nullptr) {
        CloseHandle(controlMapping_);
        controlMapping_ = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        targetPid_ = 0;
        lockedAudioPid_ = 0;
        audioPids_.clear();
        rendererHasFormat_ = false;
        rendererPid_ = 0;
        rendererDeviceId_.clear();
        rendererFormat_ = {};
        rendererPrebufferMs_ = prebufferMs_;
        rendererAsioBufferFrames_ = asioBufferFrames_;
    }
}

int32_t AudioBridgeCore::SetOutputDevice(const wchar_t* outputDeviceId) {
    WAVEFORMATEXTENSIBLE format{};
    std::uint32_t pid = 0;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        selectedDeviceId_ = outputDeviceId != nullptr ? outputDeviceId : L"";
        pid = lockedAudioPid_.load();
        auto it = audioPids_.find(pid);
        if (it != audioPids_.end() && it->second.hasFormat) {
            format = it->second.format;
        }
    }

    if (pid != 0 && format.Format.nSamplesPerSec != 0) {
        StartRendererForFormat(pid, format);
    }
    return kOk;
}

int32_t AudioBridgeCore::SetPrebufferMs(std::int32_t prebufferMs) {
    if (prebufferMs < 0 || prebufferMs > 10000) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        SetLastErrorLocked(L"Prebuffer must be between 0 and 10000 ms.");
        return kInvalidArgument;
    }

    WAVEFORMATEXTENSIBLE format{};
    std::uint32_t pid = 0;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        prebufferMs_ = prebufferMs;
        pid = lockedAudioPid_.load();
        auto it = audioPids_.find(pid);
        if (it != audioPids_.end() && it->second.hasFormat) {
            format = it->second.format;
        }
        SetLastErrorLocked(L"OK");
    }

    if (pid != 0 && format.Format.nSamplesPerSec != 0) {
        StartRendererForFormat(pid, format);
    }
    return kOk;
}

int32_t AudioBridgeCore::GetPrebufferMs() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return prebufferMs_;
}

int32_t AudioBridgeCore::SelectAudioPid(std::uint32_t pid) {
    if (pid == 0) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        SetLastErrorLocked(L"Audio PID must be non-zero.");
        return kInvalidArgument;
    }

    WAVEFORMATEXTENSIBLE format{};
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        auto it = audioPids_.find(pid);
        if (it == audioPids_.end() || !it->second.hasFormat) {
            SetLastErrorLocked(L"Audio PID was not detected or has no captured format.");
            return kInvalidArgument;
        }
        format = it->second.format;
        lockedAudioPid_ = pid;
    }

    WriteControlState(pid, false);
    StartRendererForFormat(pid, format);
    Log(L"Selected audio pid=%u", pid);
    return kOk;
}

int32_t AudioBridgeCore::RefreshDevices() {
    std::vector<DeviceInfo> devices = EnumerateAsioDevices();
    std::lock_guard<std::mutex> lock(stateMutex_);
    const int32_t defaultIndex = FindDefaultAsioDeviceIndex(devices, selectedDeviceId_);
    if (defaultIndex >= 0) {
        devices[static_cast<std::size_t>(defaultIndex)].isDefault = true;
    }
    devices_ = std::move(devices);
    defaultDeviceIndex_ = defaultIndex;
    SetLastErrorLocked(devices_.empty() ? L"No ASIO output drivers were found." : L"OK");
    return kOk;
}

int32_t AudioBridgeCore::GetDeviceCount() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return static_cast<int32_t>(devices_.size());
}

int32_t AudioBridgeCore::GetDefaultDeviceIndex() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return defaultDeviceIndex_;
}

int32_t AudioBridgeCore::GetDeviceId(int32_t index, wchar_t* buffer, int32_t bufferChars) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (index < 0 || index >= static_cast<int32_t>(devices_.size())) {
        return kInvalidArgument;
    }
    return CopyWideString(devices_[static_cast<std::size_t>(index)].id, buffer, bufferChars);
}

int32_t AudioBridgeCore::GetDeviceName(int32_t index, wchar_t* buffer, int32_t bufferChars) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (index < 0 || index >= static_cast<int32_t>(devices_.size())) {
        return kInvalidArgument;
    }
    return CopyWideString(devices_[static_cast<std::size_t>(index)].name, buffer, bufferChars);
}

int32_t AudioBridgeCore::GetPidCount() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return static_cast<int32_t>(audioPids_.size());
}

int32_t AudioBridgeCore::GetPidInfo(int32_t index, ABC_PidInfo* info) const {
    if (info == nullptr) {
        return kInvalidArgument;
    }
    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto pids = SnapshotPidsLocked();
    if (index < 0 || index >= static_cast<int32_t>(pids.size())) {
        return kInvalidArgument;
    }
    *info = pids[static_cast<std::size_t>(index)];
    return kOk;
}

int32_t AudioBridgeCore::GetStatus(ABC_Status* status) const {
    if (status == nullptr) {
        return kInvalidArgument;
    }
    const RendererStats stats = renderer_.GetStats();
    std::lock_guard<std::mutex> lock(stateMutex_);
    status->running = running_.load() ? 1 : 0;
    status->targetPid = targetPid_;
    status->lockedAudioPid = lockedAudioPid_.load();
    status->streamActive = stats.streamActive ? 1 : 0;
    status->prebuffering = stats.prebuffering ? 1 : 0;
    status->totalFramesQueued = stats.totalFramesQueued;
    status->totalFramesPlayed = stats.totalFramesPlayed;
    status->totalFramesDropped = stats.totalFramesDropped;
    status->totalOutputFrames = stats.totalOutputFrames;
    status->totalSilentFrames = stats.totalSilentFrames;
    status->bufferedFrames = stats.bufferedFrames;
    status->bufferedMs = stats.bufferedMs;
    status->bufferCapacityFrames = stats.bufferCapacityFrames;
    status->bufferCapacityMs = stats.bufferCapacityMs;
    status->prebufferTargetFrames = stats.prebufferTargetFrames;
    status->prebufferTargetMs = stats.prebufferTargetMs;
    status->underrunCount = stats.underrunCount;
    status->recentOutputFrames = stats.recentOutputFrames;
    status->recentSilentFrames = stats.recentSilentFrames;
    status->recentSilentPercent = stats.recentSilentPercent;
    status->asioRequestedBufferFrames = stats.asioRequestedBufferFrames;
    status->asioActualBufferFrames = stats.asioActualBufferFrames;
    status->asioMinBufferFrames = stats.asioMinBufferFrames;
    status->asioMaxBufferFrames = stats.asioMaxBufferFrames;
    status->asioPreferredBufferFrames = stats.asioPreferredBufferFrames;
    status->asioBufferGranularity = stats.asioBufferGranularity;
    status->asioOutputSampleType = stats.asioOutputSampleType;
    status->asioSampleRate = stats.asioSampleRate;
    status->asioResetRequests = stats.asioResetRequests;
    status->asioBufferSizeChanges = stats.asioBufferSizeChanges;
    status->asioLatencyChanges = stats.asioLatencyChanges;
    status->asioRebuildCount = stats.asioRebuildCount;
    status->asioLastMessage = stats.asioLastMessage;
    return kOk;
}

int32_t AudioBridgeCore::DrainLog(wchar_t* buffer, int32_t bufferChars) {
    if (buffer == nullptr || bufferChars <= 0) {
        return kInvalidArgument;
    }
    std::lock_guard<std::mutex> lock(stateMutex_);
    const int32_t copied = CopyWideString(logBuffer_, buffer, bufferChars);
    logBuffer_.clear();
    return copied;
}

int32_t AudioBridgeCore::GetLastError(wchar_t* buffer, int32_t bufferChars) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return CopyWideString(lastError_, buffer, bufferChars);
}

void AudioBridgeCore::SetLastErrorLocked(const std::wstring& error) {
    lastError_ = error.empty() ? L"OK" : error;
}

void AudioBridgeCore::Log(const wchar_t* format, ...) {
    wchar_t message[2048]{};
    va_list args;
    va_start(args, format);
    std::vswprintf(message, std::size(message), format, args);
    va_end(args);

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t line[2400]{};
    std::swprintf(line,
                  std::size(line),
                  L"[%02u:%02u:%02u.%03u] %s\r\n",
                  now.wHour,
                  now.wMinute,
                  now.wSecond,
                  now.wMilliseconds,
                  message);

    std::lock_guard<std::mutex> lock(stateMutex_);
    logBuffer_ += line;
    constexpr std::size_t kMaxLogChars = 256U * 1024U;
    if (logBuffer_.size() > kMaxLogChars) {
        logBuffer_.erase(0, logBuffer_.size() - kMaxLogChars);
    }
}

void AudioBridgeCore::WriteControlState(std::uint32_t audioPid, bool finish) {
    if (control_ == nullptr) {
        return;
    }
    InterlockedExchange(&control_->lockedPid, static_cast<LONG>(audioPid));
    InterlockedExchange(&control_->finish, finish ? 1 : 0);
    InterlockedExchange(&control_->fakeOutput, fakeOutput_.load() ? 1 : 0);
}

bool AudioBridgeCore::StartPipeServer(std::wstring* outError) {
    try {
        pipeThread_ = std::thread(&AudioBridgeCore::PipeServerThread, this);
        return true;
    } catch (const std::exception& ex) {
        if (outError != nullptr) {
            *outError = Utf8ToWide(ex.what(), std::strlen(ex.what()));
        }
        return false;
    }
}

void AudioBridgeCore::PipeServerThread() {
    while (running_.load()) {
        HANDLE pipe = CreateNamedPipeW(kPipeName,
                                       PIPE_ACCESS_INBOUND,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES,
                                       1 << 20,
                                       1 << 20,
                                       1000,
                                       nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(100);
            continue;
        }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
                                       ? TRUE
                                       : ::GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected || !running_.load()) {
            CloseHandle(pipe);
            continue;
        }

        AddActivePipe(pipe);
        std::lock_guard<std::mutex> lock(clientThreadsMutex_);
        clientThreads_.emplace_back(&AudioBridgeCore::HandlePipeClient, this, pipe);
    }
}

void AudioBridgeCore::HandlePipeClient(HANDLE pipe) {
    for (;;) {
        PipeMessageHeader header{};
        if (!ReadExact(pipe, &header, sizeof(header)) || header.magic != kPipeMagic) {
            break;
        }
        if (header.payloadBytes > 64ull * 1024ull * 1024ull) {
            break;
        }

        std::vector<std::uint8_t> payload(static_cast<std::size_t>(header.payloadBytes));
        if (!payload.empty() &&
            !ReadExact(pipe, payload.data(), static_cast<DWORD>(payload.size()))) {
            break;
        }
        HandlePipeMessage(header.type, header.pid, payload);
    }

    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    RemoveActivePipe(pipe);
}

void AudioBridgeCore::HandlePipeMessage(DWORD type,
                                        DWORD pid,
                                        const std::vector<std::uint8_t>& payload) {
    if (type == kPipeText) {
        const auto text = Utf8ToWide(reinterpret_cast<const char*>(payload.data()), payload.size());
        std::lock_guard<std::mutex> lock(stateMutex_);
        logBuffer_ += text;
        return;
    }
    if (type == kPipeFormat) {
        HandleFormatMessage(pid, payload.data(), payload.size());
        return;
    }
    if (type == kPipePcm) {
        HandlePcmMessage(pid, payload.data(), payload.size());
        return;
    }
    if (type == kPipeFinish) {
        Log(L"Hook reported PCM finish for pid=%u", pid);
    }
}

void AudioBridgeCore::HandleFormatMessage(DWORD pid,
                                          const void* payload,
                                          std::size_t payloadBytes) {
    if (payload == nullptr || payloadBytes < sizeof(PipeFormatMessage)) {
        return;
    }
    const auto* message = static_cast<const PipeFormatMessage*>(payload);
    const WAVEFORMATEX& format = message->format.Format;
    const std::uint32_t bytesPerFrame = BytesPerFrame(format);
    if (pid == 0 || format.nSamplesPerSec == 0 || format.nChannels == 0 ||
        format.wBitsPerSample == 0 || bytesPerFrame == 0) {
        Log(L"Ignored invalid format from pid=%u", pid);
        return;
    }

    bool shouldSelect = false;
    bool shouldStartRenderer = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        auto& state = audioPids_[pid];
        state.format = message->format;
        state.hasFormat = true;
        state.bytesPerFrame = bytesPerFrame;
        state.lastFormatMs = NowMs();

        std::uint32_t expected = 0;
        shouldSelect = lockedAudioPid_.compare_exchange_strong(expected, pid);
        shouldStartRenderer = lockedAudioPid_.load() == pid;
    }

    if (shouldSelect) {
        WriteControlState(pid, false);
        Log(L"Auto-selected first audio pid=%u", pid);
    } else {
        Log(L"Detected audio pid=%u rate=%u channels=%u bits=%u",
            pid,
            format.nSamplesPerSec,
            format.nChannels,
            format.wBitsPerSample);
    }

    if (shouldStartRenderer) {
        StartRendererForFormat(pid, message->format);
    }
}

void AudioBridgeCore::HandlePcmMessage(DWORD pid, const std::uint8_t* data, std::size_t bytes) {
    if (pid == 0 || data == nullptr || bytes == 0 || pid != lockedAudioPid_.load()) {
        return;
    }

    std::uint32_t bytesPerFrame = 0;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        auto it = audioPids_.find(pid);
        if (it != audioPids_.end()) {
            it->second.lastPcmMs = NowMs();
            bytesPerFrame = it->second.bytesPerFrame;
        }
    }

    if (bytesPerFrame == 0) {
        return;
    }
    const auto frameCount = static_cast<std::uint32_t>(bytes / bytesPerFrame);
    if (frameCount == 0) {
        return;
    }
    std::wstring renderError;
    const auto written = renderer_.PushPcm(data, frameCount, &renderError);
    if (!renderError.empty()) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        SetLastErrorLocked(renderError);
        logBuffer_ += L"[renderer] " + renderError + L"\r\n";
    }
    if (written < frameCount) {
        Log(L"Renderer ring dropped frames. pid=%u submitted=%u written=%u",
            pid,
            frameCount,
            written);
    }
}

void AudioBridgeCore::StartRendererForFormat(std::uint32_t pid,
                                             const WAVEFORMATEXTENSIBLE& format) {
    std::wstring deviceId;
    std::int32_t prebufferMs = kDefaultPrebufferMs;
    std::int32_t asioBufferFrames = kDefaultAsioBufferFrames;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        deviceId = selectedDeviceId_;
        prebufferMs = prebufferMs_;
        asioBufferFrames = asioBufferFrames_;
        if (renderer_.IsRunning() &&
            rendererHasFormat_ &&
            rendererPid_ == pid &&
            rendererDeviceId_ == deviceId &&
            rendererPrebufferMs_ == prebufferMs &&
            rendererAsioBufferFrames_ == asioBufferFrames &&
            WaveFormatsEqual(rendererFormat_, format)) {
            return;
        }
    }

    std::wstring error;
    if (!renderer_.Start(
                deviceId,
                format,
                prebufferMs,
                static_cast<std::uint32_t>(asioBufferFrames),
                &error)) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        rendererHasFormat_ = false;
        rendererPid_ = 0;
        rendererDeviceId_.clear();
        rendererFormat_ = {};
        SetLastErrorLocked(error);
        logBuffer_ += L"[renderer] " + error + L"\r\n";
        return;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        rendererHasFormat_ = true;
        rendererPid_ = pid;
        rendererDeviceId_ = deviceId;
        rendererFormat_ = format;
        rendererPrebufferMs_ = prebufferMs;
        rendererAsioBufferFrames_ = asioBufferFrames;
        SetLastErrorLocked(L"OK");
    }

    Log(L"Renderer started for pid=%u rate=%u channels=%u bits=%u bytesPerFrame=%u",
        pid,
        format.Format.nSamplesPerSec,
        format.Format.nChannels,
        format.Format.wBitsPerSample,
        BytesPerFrame(format.Format));
    Log(L"Renderer prebuffer target=%d ms", prebufferMs);
    Log(L"Renderer ASIO buffer request=%d frames (0=driver preferred)", asioBufferFrames);
}

bool AudioBridgeCore::LaunchAndInjectTarget(const std::filesystem::path& exePath,
                                            const std::filesystem::path& hookDllPath,
                                            std::wstring* outError) {
    const std::wstring targetExe = std::filesystem::absolute(exePath).wstring();
    std::wstring commandLine = QuoteCommandArgument(targetExe);
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION processInfo{};
    const std::wstring workingDirectory =
            std::filesystem::path(targetExe).parent_path().wstring();
    const DWORD creationFlags = CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP;
    const BOOL created = CreateProcessW(targetExe.c_str(),
                                        mutableCommandLine.data(),
                                        nullptr,
                                        nullptr,
                                        FALSE,
                                        creationFlags,
                                        nullptr,
                                        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                                        &startup,
                                        &processInfo);
    if (!created) {
        if (outError != nullptr) {
            *outError = Win32Message(L"CreateProcessW");
        }
        return false;
    }

    targetPid_ = processInfo.dwProcessId;
    targetProcess_ = processInfo.hProcess;

    USHORT targetMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    if (!EffectiveMachine(targetProcess_, &targetMachine)) {
        targetMachine = IMAGE_FILE_MACHINE_AMD64;
    }

    if (targetMachine != IMAGE_FILE_MACHINE_I386) {
        std::wstring architectureError;
        if (!CheckArchitectureMatch(targetProcess_, &architectureError)) {
            if (outError != nullptr) {
                *outError = architectureError +
                            L" Use the win64 package to open a 64-bit target process.";
            }
            TerminateProcess(targetProcess_, 1);
            CloseHandle(processInfo.hThread);
            CloseHandle(targetProcess_);
            targetProcess_ = nullptr;
            targetPid_ = 0;
            return false;
        }
    }

    const std::filesystem::path selectedHookPath =
            hookDllPath.empty() ? DefaultHookDllPath(targetMachine) : hookDllPath;
    if (!std::filesystem::exists(selectedHookPath)) {
        if (outError != nullptr) {
            *outError = L"Hook DLL does not exist: " + selectedHookPath.wstring();
        }
        TerminateProcess(targetProcess_, 1);
        CloseHandle(processInfo.hThread);
        CloseHandle(targetProcess_);
        targetProcess_ = nullptr;
        targetPid_ = 0;
        return false;
    }

    const auto readyEventName = HookReadyEventName(targetPid_);
    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, readyEventName.c_str());
    if (readyEvent == nullptr) {
        if (outError != nullptr) {
            *outError = Win32Message(L"CreateEventW(hook ready)");
        }
        TerminateProcess(targetProcess_, 1);
        CloseHandle(processInfo.hThread);
        CloseHandle(targetProcess_);
        targetProcess_ = nullptr;
        targetPid_ = 0;
        return false;
    }

    bool injected = false;
    if (targetMachine == IMAGE_FILE_MACHINE_I386) {
        const auto helperPath = DefaultInjectHelperPath();
        if (!std::filesystem::exists(helperPath)) {
            if (outError != nullptr) {
                *outError = L"32-bit injector helper does not exist: " + helperPath.wstring();
            }
        } else {
            injected = InjectDllWithHelper(targetPid_, helperPath, selectedHookPath, outError);
        }
    } else {
        injected = InjectDll(targetProcess_, selectedHookPath, outError);
    }

    if (!injected) {
        CloseHandle(readyEvent);
        TerminateProcess(targetProcess_, 1);
        CloseHandle(processInfo.hThread);
        CloseHandle(targetProcess_);
        targetProcess_ = nullptr;
        targetPid_ = 0;
        return false;
    }

    if (!WaitForHookReady(readyEvent, targetPid_, outError)) {
        CloseHandle(readyEvent);
        TerminateProcess(targetProcess_, 1);
        CloseHandle(processInfo.hThread);
        CloseHandle(targetProcess_);
        targetProcess_ = nullptr;
        targetPid_ = 0;
        return false;
    }
    CloseHandle(readyEvent);

    Log(L"Injected hook. targetPid=%u machine=0x%04X dll=\"%s\"",
        targetPid_,
        targetMachine,
        selectedHookPath.wstring().c_str());
    ResumeThread(processInfo.hThread);
    CloseHandle(processInfo.hThread);
    return true;
}

void AudioBridgeCore::StartProcessMonitor() {
    if (processMonitorThread_.joinable()) {
        processMonitorThread_.join();
    }
    processMonitorThread_ = std::thread(&AudioBridgeCore::ProcessMonitorThread, this);
}

void AudioBridgeCore::ProcessMonitorThread() {
    const auto startedAt = std::chrono::steady_clock::now();
    constexpr auto kMonitorDuration = std::chrono::seconds(30);
    constexpr int kMaxInjectionAttempts = 3;

    while (running_.load() && std::chrono::steady_clock::now() - startedAt < kMonitorDuration) {
        const auto processes = SnapshotProcesses();
        std::unordered_map<DWORD, DWORD> parents;
        parents.reserve(processes.size());
        for (const auto& process : processes) {
            parents.emplace(process.pid, process.parentPid);
        }

        DWORD rootPid = 0;
        std::filesystem::path targetPath;
        {
            std::lock_guard<std::mutex> lock(processMonitorMutex_);
            rootPid = targetPid_;
            targetPath = targetExePath_;
        }
        const auto normalizedTargetPath = NormalizePathForComparison(targetPath);

        for (const auto& process : processes) {
            if (process.pid == 0 || process.pid == rootPid ||
                process.pid == GetCurrentProcessId()) {
                continue;
            }

            bool shouldInject = IsDescendantProcess(process.pid, rootPid, parents);
            if (!shouldInject) {
                const auto processPath = QueryProcessImagePath(process.pid);
                shouldInject = !processPath.empty() &&
                               NormalizePathForComparison(processPath) == normalizedTargetPath;
            }
            if (!shouldInject) {
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(processMonitorMutex_);
                if (injectedPids_.find(process.pid) != injectedPids_.end() ||
                    injectionAttempts_[process.pid] >= kMaxInjectionAttempts) {
                    continue;
                }
                ++injectionAttempts_[process.pid];
            }

            std::wstring error;
            if (InjectProcessByPid(process.pid, &error)) {
                {
                    std::lock_guard<std::mutex> lock(processMonitorMutex_);
                    injectedPids_.insert(process.pid);
                }
                Log(L"Fallback process monitor ensured hook for pid=%u.", process.pid);
            } else {
                Log(L"Fallback process monitor could not inject pid=%u: %s",
                    process.pid,
                    error.c_str());
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

bool AudioBridgeCore::InjectProcessByPid(DWORD pid, std::wstring* outError) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
                                         PROCESS_CREATE_THREAD |
                                         PROCESS_VM_OPERATION |
                                         PROCESS_VM_WRITE |
                                         PROCESS_VM_READ |
                                         SYNCHRONIZE,
                                 FALSE,
                                 pid);
    if (process == nullptr) {
        if (outError != nullptr) {
            *outError = Win32Message(L"OpenProcess(process monitor)");
        }
        return false;
    }

    USHORT targetMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    if (!EffectiveMachine(process, &targetMachine)) {
        if (outError != nullptr) {
            *outError = Win32Message(L"IsWow64Process2(process monitor)");
        }
        CloseHandle(process);
        return false;
    }
    const auto hookPath = DefaultHookDllPath(targetMachine);
    if (IsModuleLoadedInProcess(pid, hookPath)) {
        CloseHandle(process);
        return true;
    }

    const auto readyEventName = HookReadyEventName(pid);
    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, readyEventName.c_str());
    if (readyEvent == nullptr) {
        if (outError != nullptr) {
            *outError = Win32Message(L"CreateEventW(process monitor)");
        }
        CloseHandle(process);
        return false;
    }

    bool injected = false;
    if (targetMachine == IMAGE_FILE_MACHINE_I386) {
        const auto helperPath = DefaultInjectHelperPath();
        injected = std::filesystem::exists(helperPath) &&
                   std::filesystem::exists(hookPath) &&
                   InjectDllWithHelper(pid, helperPath, hookPath, outError);
    } else if (std::filesystem::exists(hookPath)) {
        injected = CheckArchitectureMatch(process, outError) &&
                   InjectDll(process, hookPath, outError);
    } else if (outError != nullptr) {
        *outError = L"Hook DLL does not exist: " + hookPath.wstring();
    }

    const bool ready = injected && WaitForHookReady(readyEvent, pid, outError);
    CloseHandle(readyEvent);
    CloseHandle(process);
    return ready;
}

bool AudioBridgeCore::WaitForHookReady(HANDLE readyEvent,
                                       DWORD pid,
                                       std::wstring* outError) {
    const DWORD waitResult = WaitForSingleObject(readyEvent, kHookReadyTimeoutMs);
    if (waitResult == WAIT_OBJECT_0) {
        return true;
    }

    if (outError != nullptr) {
        if (waitResult == WAIT_TIMEOUT) {
            *outError = L"AudioBridge hook initialization timed out for PID " +
                        std::to_wstring(pid) + L".";
        } else {
            *outError = Win32Message(L"WaitForSingleObject(hook ready)");
        }
    }
    return false;
}

bool AudioBridgeCore::InjectDll(HANDLE process,
                                const std::filesystem::path& dllPath,
                                std::wstring* outError) {
    const std::wstring dllPathString = std::filesystem::absolute(dllPath).wstring();
    const SIZE_T bytes = (dllPathString.size() + 1) * sizeof(wchar_t);

    void* remoteString =
            VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remoteString == nullptr) {
        if (outError != nullptr) {
            *outError = Win32Message(L"VirtualAllocEx");
        }
        return false;
    }

    if (!WriteProcessMemory(process, remoteString, dllPathString.c_str(), bytes, nullptr)) {
        if (outError != nullptr) {
            *outError = Win32Message(L"WriteProcessMemory");
        }
        VirtualFreeEx(process, remoteString, 0, MEM_RELEASE);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto* loadLibrary =
            reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));
    if (loadLibrary == nullptr) {
        if (outError != nullptr) {
            *outError = L"LoadLibraryW was not found.";
        }
        VirtualFreeEx(process, remoteString, 0, MEM_RELEASE);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remoteString, 0, nullptr);
    if (thread == nullptr) {
        if (outError != nullptr) {
            *outError = Win32Message(L"CreateRemoteThread");
        }
        VirtualFreeEx(process, remoteString, 0, MEM_RELEASE);
        return false;
    }

    WaitForSingleObject(thread, INFINITE);
    DWORD remoteResult = 0;
    GetExitCodeThread(thread, &remoteResult);
    CloseHandle(thread);
    VirtualFreeEx(process, remoteString, 0, MEM_RELEASE);

    if (remoteResult == 0) {
        if (outError != nullptr) {
            *outError = L"Remote LoadLibraryW failed.";
        }
        return false;
    }
    return true;
}

bool AudioBridgeCore::InjectDllWithHelper(DWORD pid,
                                          const std::filesystem::path& helperPath,
                                          const std::filesystem::path& dllPath,
                                          std::wstring* outError) {
    const std::wstring helper = std::filesystem::absolute(helperPath).wstring();
    const std::wstring dll = std::filesystem::absolute(dllPath).wstring();
    wchar_t pidText[32]{};
    std::swprintf(pidText, std::size(pidText), L"%lu", static_cast<unsigned long>(pid));

    std::wstring commandLine =
            QuoteCommandArgument(helper) + L" " + pidText + L" " + QuoteCommandArgument(dll);
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION processInfo{};
    const std::wstring workingDirectory = std::filesystem::path(helper).parent_path().wstring();
    const BOOL created = CreateProcessW(helper.c_str(),
                                        mutableCommandLine.data(),
                                        nullptr,
                                        nullptr,
                                        FALSE,
                                        CREATE_NO_WINDOW,
                                        nullptr,
                                        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                                        &startup,
                                        &processInfo);
    if (!created) {
        if (outError != nullptr) {
            *outError = Win32Message(L"CreateProcessW(AudioBridgeInject32)");
        }
        return false;
    }

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    if (exitCode != 0) {
        if (outError != nullptr) {
            wchar_t message[256]{};
            std::swprintf(message,
                          std::size(message),
                          L"AudioBridgeInject32 failed with exit code %lu.",
                          static_cast<unsigned long>(exitCode));
            *outError = message;
        }
        return false;
    }

    return true;
}

bool AudioBridgeCore::CheckArchitectureMatch(HANDLE process, std::wstring* outError) {
    USHORT injectorMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT targetMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    if (!EffectiveMachine(GetCurrentProcess(), &injectorMachine) ||
        !EffectiveMachine(process, &targetMachine)) {
        return true;
    }
    if (injectorMachine == targetMachine) {
        return true;
    }
    if (outError != nullptr) {
        *outError = L"Hook DLL architecture does not match target process architecture.";
    }
    return false;
}

void AudioBridgeCore::AddActivePipe(HANDLE pipe) {
    std::lock_guard<std::mutex> lock(activePipesMutex_);
    activePipes_.push_back(pipe);
}

void AudioBridgeCore::RemoveActivePipe(HANDLE pipe) {
    std::lock_guard<std::mutex> lock(activePipesMutex_);
    activePipes_.erase(std::remove(activePipes_.begin(), activePipes_.end(), pipe),
                       activePipes_.end());
}

void AudioBridgeCore::CancelActivePipes() {
    std::lock_guard<std::mutex> lock(activePipesMutex_);
    for (HANDLE pipe : activePipes_) {
        CancelIoEx(pipe, nullptr);
        DisconnectNamedPipe(pipe);
    }
}

std::filesystem::path AudioBridgeCore::DefaultHookDllPath(USHORT targetMachine) const {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ABC_StartTarget),
                       &module);
    wchar_t path[MAX_PATH]{};
    const wchar_t* dllName =
            targetMachine == IMAGE_FILE_MACHINE_I386 ? L"AudioBridgeHook32.dll"
                                                     : L"AudioBridgeHook.dll";
    if (module != nullptr &&
        GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path))) > 0) {
        return std::filesystem::path(path).parent_path() / dllName;
    }
    return dllName;
}

std::filesystem::path AudioBridgeCore::DefaultInjectHelperPath() const {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ABC_StartTarget),
                       &module);
    wchar_t path[MAX_PATH]{};
    if (module != nullptr &&
        GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path))) > 0) {
        return std::filesystem::path(path).parent_path() / L"AudioBridgeInject32.exe";
    }
    return L"AudioBridgeInject32.exe";
}

std::vector<ABC_PidInfo> AudioBridgeCore::SnapshotPidsLocked() const {
    std::vector<ABC_PidInfo> result;
    result.reserve(audioPids_.size());
    const auto lockedPid = lockedAudioPid_.load();
    for (const auto& item : audioPids_) {
        ABC_PidInfo info{};
        info.pid = item.first;
        info.bytesPerFrame = item.second.bytesPerFrame;
        info.lastFormatMs = item.second.lastFormatMs;
        info.lastPcmMs = item.second.lastPcmMs;
        info.isSelected = item.first == lockedPid ? 1 : 0;
        if (item.second.hasFormat) {
            info.sampleRate = item.second.format.Format.nSamplesPerSec;
            info.channels = item.second.format.Format.nChannels;
            info.bitsPerSample = item.second.format.Format.wBitsPerSample;
            info.sampleFormat = SampleFormatValue(item.second.format);
        }
        result.push_back(info);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.isSelected != right.isSelected) {
            return left.isSelected > right.isSelected;
        }
        return left.pid < right.pid;
    });
    return result;
}

AudioBridgeCore* g_core = nullptr;
SRWLOCK g_coreLock = SRWLOCK_INIT;

AudioBridgeCore* Core() noexcept {
    if (g_core != nullptr) {
        return g_core;
    }

    NativeDiagnostic(L"Core() enter.");
    NativeDiagnostic(L"Core() acquiring allocation lock.");
    AcquireSRWLockExclusive(&g_coreLock);
    if (g_core == nullptr) {
        NativeDiagnostic(L"Core() allocating AudioBridgeCore.");
        try {
            g_core = new (std::nothrow) AudioBridgeCore();
        } catch (...) {
            NativeDiagnostic(L"Core() caught C++ exception during allocation.");
            g_core = nullptr;
        }
        NativeDiagnostic(g_core != nullptr ? L"Core() allocation success." : L"Core() allocation returned null.");
    }
    auto* result = g_core;
    ReleaseSRWLockExclusive(&g_coreLock);
    NativeDiagnostic(L"Core() leave.");
    return result;
}

int32_t CopyStartupError(const wchar_t* message, wchar_t* buffer, int32_t bufferChars) {
    return CopyWideString(message != nullptr ? std::wstring(message) : std::wstring(L"AudioBridgeCore is not initialized."),
                          buffer,
                          bufferChars);
}

}  // namespace
}  // namespace audiobridge

extern "C" {

ABC_API int32_t ABC_CALL ABC_Initialize(void) {
    audiobridge::NativeDiagnostic(L"ABC_Initialize enter.");
    __try {
        auto* core = audiobridge::Core();
        if (core == nullptr) {
            audiobridge::NativeDiagnostic(L"ABC_Initialize failed: AudioBridgeCore allocation returned null.");
            return -1;
        }
        const int32_t result = core->Initialize() ? 0 : -1;
        audiobridge::NativeDiagnostic(result == 0 ? L"ABC_Initialize success." : L"ABC_Initialize returned failure.");
        return result;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        audiobridge::NativeDiagnostic(L"ABC_Initialize caught a structured exception.");
        return -1;
    }
}

ABC_API void ABC_CALL ABC_Shutdown(void) {
    if (auto* core = audiobridge::Core()) {
        core->Shutdown();
    }
}

ABC_API int32_t ABC_CALL ABC_StartTarget(const wchar_t* exePath,
                                         const wchar_t* hookDllPath,
                                         const wchar_t* outputDeviceId) {
    auto* core = audiobridge::Core();
    if (core == nullptr) {
        return -1;
    }
    return core->StartTarget(
            exePath,
            hookDllPath,
            outputDeviceId,
            core->GetPrebufferMs(),
            audiobridge::kDefaultAsioBufferFrames,
            false);
}

ABC_API int32_t ABC_CALL ABC_StartTargetWithOptions(const wchar_t* exePath,
                                                    const wchar_t* hookDllPath,
                                                    const wchar_t* outputDeviceId,
                                                    int32_t prebufferMs) {
    auto* core = audiobridge::Core();
    if (core == nullptr) {
        return -1;
    }
    return core->StartTarget(
            exePath,
            hookDllPath,
            outputDeviceId,
            prebufferMs,
            audiobridge::kDefaultAsioBufferFrames,
            false);
}

ABC_API int32_t ABC_CALL ABC_StartTargetWithOptions2(const wchar_t* exePath,
                                                     const wchar_t* hookDllPath,
                                                     const wchar_t* outputDeviceId,
                                                     int32_t prebufferMs,
                                                     int32_t asioBufferFrames) {
    auto* core = audiobridge::Core();
    if (core == nullptr) {
        return -1;
    }
    return core->StartTarget(
            exePath,
            hookDllPath,
            outputDeviceId,
            prebufferMs,
            asioBufferFrames,
            false);
}

ABC_API int32_t ABC_CALL ABC_StartTargetWithOptions3(const wchar_t* exePath,
                                                     const wchar_t* hookDllPath,
                                                     const wchar_t* outputDeviceId,
                                                     int32_t prebufferMs,
                                                     int32_t asioBufferFrames,
                                                     int32_t fakeOutput) {
    auto* core = audiobridge::Core();
    if (core == nullptr) {
        return -1;
    }
    return core->StartTarget(
            exePath,
            hookDllPath,
            outputDeviceId,
            prebufferMs,
            asioBufferFrames,
            fakeOutput != 0);
}

ABC_API void ABC_CALL ABC_Stop(void) {
    if (auto* core = audiobridge::Core()) {
        core->Stop();
    }
}

ABC_API int32_t ABC_CALL ABC_SetOutputDevice(const wchar_t* outputDeviceId) {
    auto* core = audiobridge::Core();
    return core != nullptr ? core->SetOutputDevice(outputDeviceId) : -1;
}

ABC_API int32_t ABC_CALL ABC_SetPrebufferMs(int32_t prebufferMs) {
    auto* core = audiobridge::Core();
    return core != nullptr ? core->SetPrebufferMs(prebufferMs) : -1;
}

ABC_API int32_t ABC_CALL ABC_GetPrebufferMs(void) {
    auto* core = audiobridge::Core();
    return core != nullptr ? core->GetPrebufferMs() : audiobridge::kDefaultPrebufferMs;
}

ABC_API int32_t ABC_CALL ABC_SelectAudioPid(uint32_t pid) {
    auto* core = audiobridge::Core();
    return core != nullptr ? core->SelectAudioPid(pid) : -1;
}

ABC_API int32_t ABC_CALL ABC_RefreshDevices(void) {
    auto* core = audiobridge::Core();
    return core != nullptr ? core->RefreshDevices() : -1;
}

ABC_API int32_t ABC_CALL ABC_GetDeviceCount(void) {
    auto* core = audiobridge::Core();
    return core != nullptr ? core->GetDeviceCount() : 0;
}

ABC_API int32_t ABC_CALL ABC_GetDefaultDeviceIndex(void) {
    auto* core = audiobridge::Core();
    return core != nullptr ? core->GetDefaultDeviceIndex() : -1;
}

ABC_API int32_t ABC_CALL ABC_GetDeviceId(int32_t index, wchar_t* buffer, int32_t bufferChars) {
    auto* core = audiobridge::Core();
    return core != nullptr
            ? core->GetDeviceId(index, buffer, bufferChars)
            : audiobridge::CopyStartupError(L"AudioBridgeCore is not initialized.", buffer, bufferChars);
}

ABC_API int32_t ABC_CALL ABC_GetDeviceName(int32_t index, wchar_t* buffer, int32_t bufferChars) {
    auto* core = audiobridge::Core();
    return core != nullptr
            ? core->GetDeviceName(index, buffer, bufferChars)
            : audiobridge::CopyStartupError(L"AudioBridgeCore is not initialized.", buffer, bufferChars);
}

ABC_API int32_t ABC_CALL ABC_GetPidCount(void) {
    auto* core = audiobridge::Core();
    return core != nullptr ? core->GetPidCount() : 0;
}

ABC_API int32_t ABC_CALL ABC_GetPidInfo(int32_t index, ABC_PidInfo* info) {
    auto* core = audiobridge::Core();
    return core != nullptr ? core->GetPidInfo(index, info) : -1;
}

ABC_API int32_t ABC_CALL ABC_GetStatus(ABC_Status* status) {
    auto* core = audiobridge::Core();
    return core != nullptr ? core->GetStatus(status) : -1;
}

ABC_API int32_t ABC_CALL ABC_DrainLog(wchar_t* buffer, int32_t bufferChars) {
    auto* core = audiobridge::Core();
    return core != nullptr ? core->DrainLog(buffer, bufferChars) : 0;
}

ABC_API int32_t ABC_CALL ABC_GetLastError(wchar_t* buffer, int32_t bufferChars) {
    auto* core = audiobridge::Core();
    return core != nullptr
            ? core->GetLastError(buffer, bufferChars)
            : audiobridge::CopyStartupError(L"AudioBridgeCore is not initialized.", buffer, bufferChars);
}

}  // extern "C"

#include "AsioRenderer.h"
#include "CompensationBridgePolicy.h"
#include "PcmSampleConverter.h"

#include <avrt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>

namespace tickbytick {
namespace {

namespace bridge = compensation_bridge;

std::atomic<AsioRenderer*> g_activeRenderer{nullptr};
std::atomic<bool> g_asioDriverPoisoned{false};
std::atomic<std::uint32_t> g_asioCallbackEntrants{0};
std::mutex g_asioClockProbeMutex;

constexpr DWORD kFirstOutputCallbackTimeoutMs = 2000;
constexpr auto kControlMessagePumpInterval = std::chrono::milliseconds(2);

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "The ASIO callback requires lock-free 64-bit Bridge accounting.");

void EnsureCurrentThreadMessageQueue() noexcept {
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
}

void PumpCurrentThreadMessages() noexcept {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

enum class CallbackRealtimeMode : std::int32_t {
    Unknown = 0,
    MmcssProAudio = 1,
    HighPriorityFallback = 2,
    Failed = -1,
};

class AsioCallbackThreadScheduling final {
public:
    AsioCallbackThreadScheduling() noexcept {
        THREAD_POWER_THROTTLING_STATE powerThrottling{};
        powerThrottling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        powerThrottling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        powerThrottling.StateMask = 0;
        SetThreadInformation(GetCurrentThread(),
                             ThreadPowerThrottling,
                             &powerThrottling,
                             sizeof(powerThrottling));

        DWORD taskIndex = 0;
        mmcssHandle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (mmcssHandle_ != nullptr &&
            AvSetMmThreadPriority(mmcssHandle_, AVRT_PRIORITY_CRITICAL)) {
            mode_ = CallbackRealtimeMode::MmcssProAudio;
            return;
        }

        if (mmcssHandle_ != nullptr) {
            AvRevertMmThreadCharacteristics(mmcssHandle_);
            mmcssHandle_ = nullptr;
        }
        // MMCSS is the supported realtime scheduling path. If it is not
        // available, use the strongest ordinary thread priority without the
        // starvation risk of THREAD_PRIORITY_TIME_CRITICAL.
        mode_ = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)
                ? CallbackRealtimeMode::HighPriorityFallback
                : CallbackRealtimeMode::Failed;
    }

    ~AsioCallbackThreadScheduling() {
        if (mmcssHandle_ != nullptr) {
            AvRevertMmThreadCharacteristics(mmcssHandle_);
        }
    }

    CallbackRealtimeMode Mode() const noexcept {
        return mode_;
    }

private:
    HANDLE mmcssHandle_ = nullptr;
    CallbackRealtimeMode mode_ = CallbackRealtimeMode::Unknown;
};

CallbackRealtimeMode EnsureAsioCallbackRealtimeScheduling() noexcept {
    thread_local AsioCallbackThreadScheduling scheduling;
    return scheduling.Mode();
}

class AsioCallbackIngress final {
public:
    AsioCallbackIngress() {
        g_asioCallbackEntrants.fetch_add(1, std::memory_order_acq_rel);
    }

    ~AsioCallbackIngress() {
        g_asioCallbackEntrants.fetch_sub(1, std::memory_order_acq_rel);
    }
};

void AsioBufferSwitch(long doubleBufferIndex, ASIOBool /*directProcess*/) {
    const AsioCallbackIngress ingress;
    if (auto* renderer = g_activeRenderer.load(std::memory_order_acquire)) {
        renderer->OnAsioBufferSwitch(doubleBufferIndex, nullptr);
    }
}

void AsioSampleRateDidChange(ASIOSampleRate sampleRate) {
    const AsioCallbackIngress ingress;
    if (auto* renderer = g_activeRenderer.load(std::memory_order_acquire)) {
        renderer->OnAsioSampleRateChanged(sampleRate);
    }
}

long AsioMessage(long selector, long value, void* message, double* opt) {
    const AsioCallbackIngress ingress;
    if (auto* renderer = g_activeRenderer.load(std::memory_order_acquire)) {
        return renderer->OnAsioMessage(selector, value, message, opt);
    }
    return 0;
}

ASIOTime* AsioBufferSwitchTimeInfo(ASIOTime* params,
                                   long doubleBufferIndex,
                                   ASIOBool /*directProcess*/) {
    const AsioCallbackIngress ingress;
    if (auto* renderer = g_activeRenderer.load(std::memory_order_acquire)) {
        renderer->OnAsioBufferSwitch(doubleBufferIndex, params);
    }
    return params;
}

std::size_t NextPowerOfTwo(std::size_t value) {
    if (value == 0) {
        return 1;
    }
    --value;
    value |= value >> 1U;
    value |= value >> 2U;
    value |= value >> 4U;
    value |= value >> 8U;
    value |= value >> 16U;
#if defined(_WIN64)
    value |= value >> 32U;
#endif
    return value + 1;
}

std::uint32_t BytesPerFrame(const WAVEFORMATEX& format) {
    if (format.nBlockAlign != 0) {
        return format.nBlockAlign;
    }
    return format.nChannels * ((format.wBitsPerSample + 7U) / 8U);
}

bool IsPcmSubformat(const WAVEFORMATEXTENSIBLE& format) {
    if (format.Format.wFormatTag == WAVE_FORMAT_PCM) {
        return true;
    }
    return format.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
           IsEqualGUID(format.SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
}

bool IsFloatSubformat(const WAVEFORMATEXTENSIBLE& format) {
    if (format.Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    return format.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
           IsEqualGUID(format.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
}

AsioRenderer::SourceSampleKind SourceKindFromWave(const WAVEFORMATEXTENSIBLE& format) {
    const WAVEFORMATEX& wave = format.Format;
    const std::uint32_t bytesPerFrame = BytesPerFrame(wave);
    if (wave.nChannels == 0 || bytesPerFrame == 0 || bytesPerFrame % wave.nChannels != 0) {
        return AsioRenderer::SourceSampleKind::Unknown;
    }

    const std::uint32_t bytesPerSample = bytesPerFrame / wave.nChannels;
    const std::uint16_t validBits =
            wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                    format.Samples.wValidBitsPerSample != 0
            ? format.Samples.wValidBitsPerSample
            : wave.wBitsPerSample;
    if (validBits != wave.wBitsPerSample) {
        return AsioRenderer::SourceSampleKind::Unknown;
    }
    if (IsFloatSubformat(format) && wave.wBitsPerSample == 32 && bytesPerSample == 4) {
        return AsioRenderer::SourceSampleKind::Float32;
    }
    if (!IsPcmSubformat(format)) {
        return AsioRenderer::SourceSampleKind::Unknown;
    }
    if (wave.wBitsPerSample == 16 && bytesPerSample == 2) {
        return AsioRenderer::SourceSampleKind::Pcm16;
    }
    if (wave.wBitsPerSample == 24 && bytesPerSample == 3) {
        return AsioRenderer::SourceSampleKind::Pcm24;
    }
    if (wave.wBitsPerSample == 32 && bytesPerSample == 4) {
        return AsioRenderer::SourceSampleKind::Pcm32;
    }
    return AsioRenderer::SourceSampleKind::Unknown;
}

std::uint32_t NormalizePrebufferMs(std::int32_t prebufferMs) {
    if (prebufferMs < 0) {
        return 300;
    }
    return (std::min<std::uint32_t>)(static_cast<std::uint32_t>(prebufferMs), 10000U);
}

std::uint32_t NormalizeMaxBufferAdvanceMs(std::int32_t maxBufferAdvanceMs) {
    if (maxBufferAdvanceMs < 50) {
        return 100;
    }
    return (std::min<std::uint32_t>)(static_cast<std::uint32_t>(maxBufferAdvanceMs), 10000U);
}

std::uint32_t FramesFromMs(std::uint32_t sampleRate, std::uint32_t ms) {
    if (sampleRate == 0 || ms == 0) {
        return 0;
    }
    return static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(sampleRate) * ms + 999ULL) / 1000ULL);
}

std::int32_t MsFromFrames(std::int64_t frames, std::uint32_t sampleRate) {
    if (sampleRate == 0 || frames <= 0) {
        return 0;
    }
    return static_cast<std::int32_t>(
            (static_cast<std::uint64_t>(frames) * 1000ULL) / sampleRate);
}

std::uint64_t NowSecond() {
    return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
}

bool IsAsioSuccess(ASIOError error) {
    return error == ASE_OK || error == ASE_SUCCESS;
}

std::wstring AnsiToWide(const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return {};
    }

    const int required = MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
    if (required <= 1) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_ACP, 0, value, -1, result.data(), required);
    result.resize(static_cast<std::size_t>(required - 1));
    return result;
}

std::wstring HResultMessage(HRESULT hr, const wchar_t* action) {
    wchar_t systemMessage[512]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr,
                   static_cast<DWORD>(hr),
                   0,
                   systemMessage,
                   static_cast<DWORD>(std::size(systemMessage)),
                   nullptr);

    wchar_t buffer[900]{};
    std::swprintf(buffer,
                  std::size(buffer),
                  L"%s failed: hr=0x%08lX %s",
                  action != nullptr ? action : L"operation",
                  static_cast<unsigned long>(hr),
                  systemMessage);
    return buffer;
}

std::wstring AsioErrorMessage(IASIO* driver, ASIOError error, const wchar_t* action) {
    char asioMessage[128]{};
    if (driver != nullptr) {
        driver->getErrorMessage(asioMessage);
    }

    std::wstring message = action != nullptr ? action : L"ASIO call";
    message += L" failed: ASIOError=";
    wchar_t code[32]{};
    std::swprintf(code, std::size(code), L"%ld", static_cast<long>(error));
    message += code;
    const std::wstring driverMessage = AnsiToWide(asioMessage);
    if (!driverMessage.empty()) {
        message += L" ";
        message += driverMessage;
    }
    return message;
}

bool ReadClockSourcesFromDriver(IASIO* driver,
                                std::vector<AsioClockSourceInfo>* sources,
                                std::wstring* outError) {
    if (driver == nullptr || sources == nullptr) {
        if (outError != nullptr) {
            *outError = L"ASIO clock source query received an invalid argument.";
        }
        return false;
    }

    constexpr long kMaximumClockSources = 64;
    std::array<ASIOClockSource, kMaximumClockSources> asioSources{};
    long sourceCount = kMaximumClockSources;
    const ASIOError result = driver->getClockSources(asioSources.data(), &sourceCount);
    if (!IsAsioSuccess(result)) {
        if (outError != nullptr) {
            *outError = AsioErrorMessage(driver, result, L"IASIO::getClockSources");
        }
        return false;
    }
    if (sourceCount < 0 || sourceCount > kMaximumClockSources) {
        if (outError != nullptr) {
            *outError = L"ASIO driver returned an invalid clock source count.";
        }
        return false;
    }

    std::vector<AsioClockSourceInfo> resultSources;
    resultSources.reserve(static_cast<std::size_t>(sourceCount));
    long currentSourceCount = 0;
    for (long position = 0; position < sourceCount; ++position) {
        const auto& source = asioSources[static_cast<std::size_t>(position)];
        std::array<char, 33> terminatedName{};
        std::memcpy(terminatedName.data(), source.name, sizeof(source.name));

        AsioClockSourceInfo info{};
        info.index = static_cast<std::int32_t>(source.index);
        info.associatedChannel = static_cast<std::int32_t>(source.associatedChannel);
        info.associatedGroup = static_cast<std::int32_t>(source.associatedGroup);
        info.isCurrent = source.isCurrentSource == ASIOTrue;
        if (info.index < 0 ||
            std::any_of(resultSources.begin(),
                        resultSources.end(),
                        [&info](const AsioClockSourceInfo& existing) {
                            return existing.index == info.index;
                        })) {
            if (outError != nullptr) {
                *outError = L"ASIO driver returned an invalid or duplicate clock source index.";
            }
            return false;
        }
        if (info.isCurrent && ++currentSourceCount > 1) {
            if (outError != nullptr) {
                *outError = L"ASIO driver reported more than one current clock source.";
            }
            return false;
        }
        info.name = AnsiToWide(terminatedName.data());
        if (info.name.empty()) {
            info.name = L"Clock source " + std::to_wstring(info.index);
        }
        resultSources.push_back(std::move(info));
    }

    *sources = std::move(resultSources);
    if (outError != nullptr) {
        outError->clear();
    }
    return true;
}

bool QueryAsioClockSourcesOnThread(const std::wstring& deviceId,
                                   std::vector<AsioClockSourceInfo>* sources,
                                   std::wstring* outError) {
    if (deviceId.empty()) {
        if (outError != nullptr) {
            *outError = L"No ASIO output device is selected.";
        }
        return false;
    }

    IASIO* driver = nullptr;
    bool coInitialized = false;
    bool succeeded = false;
    std::wstring error;
    do {
        const HRESULT initializeResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initializeResult) && initializeResult != RPC_E_CHANGED_MODE) {
            error = HResultMessage(initializeResult, L"CoInitializeEx");
            break;
        }
        coInitialized = SUCCEEDED(initializeResult);

        CLSID clsid{};
        HRESULT result = CLSIDFromString(deviceId.c_str(), &clsid);
        if (FAILED(result)) {
            error = L"ASIO device id is not a valid CLSID.";
            break;
        }

        result = CoCreateInstance(clsid,
                                  nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  clsid,
                                  reinterpret_cast<void**>(&driver));
        if (FAILED(result) || driver == nullptr) {
            error = HResultMessage(result, L"CoCreateInstance(ASIO driver)");
            break;
        }
        if (driver->init(GetDesktopWindow()) == ASIOFalse) {
            error = L"IASIO::init failed: " +
                    AsioErrorMessage(driver, ASE_NotPresent, L"IASIO::init");
            break;
        }

        succeeded = ReadClockSourcesFromDriver(driver, sources, &error);
    } while (false);

    if (driver != nullptr) {
        driver->Release();
    }
    if (coInitialized) {
        CoUninitialize();
    }
    if (!succeeded && outError != nullptr) {
        *outError = error.empty() ? L"Failed to query ASIO clock sources." : error;
    }
    return succeeded;
}

const wchar_t* AsioSampleTypeName(ASIOSampleType sampleType) {
    switch (sampleType) {
        case ASIOSTFloat32LSB:
            return L"Float32LSB";
        case ASIOSTInt16LSB:
            return L"Int16LSB";
        case ASIOSTInt24LSB:
            return L"Int24LSB";
        case ASIOSTInt32LSB:
            return L"Int32LSB";
        case ASIOSTInt32LSB16:
            return L"Int32LSB16";
        case ASIOSTInt32LSB18:
            return L"Int32LSB18";
        case ASIOSTInt32LSB20:
            return L"Int32LSB20";
        case ASIOSTInt32LSB24:
            return L"Int32LSB24";
        default:
            return L"Unsupported";
    }
}

std::uint32_t AsioSampleBytes(ASIOSampleType sampleType) {
    switch (sampleType) {
        case ASIOSTFloat32LSB:
            return 4;
        case ASIOSTInt16LSB:
            return 2;
        case ASIOSTInt24LSB:
            return 3;
        case ASIOSTInt32LSB:
            return 4;
        default:
            return 0;
    }
}

bool IsDirectSampleFormat(AsioRenderer::SourceSampleKind sourceKind,
                          ASIOSampleType outputType) {
    switch (sourceKind) {
        case AsioRenderer::SourceSampleKind::Float32:
            return outputType == ASIOSTFloat32LSB;
        case AsioRenderer::SourceSampleKind::Pcm16:
            return outputType == ASIOSTInt16LSB;
        case AsioRenderer::SourceSampleKind::Pcm24:
            return outputType == ASIOSTInt24LSB;
        case AsioRenderer::SourceSampleKind::Pcm32:
            return outputType == ASIOSTInt32LSB;
        default:
            return false;
    }
}

pcm::SampleFormat SourcePcmSampleFormat(
        AsioRenderer::SourceSampleKind sourceKind) {
    switch (sourceKind) {
        case AsioRenderer::SourceSampleKind::Float32:
            return pcm::SampleFormat::Float32;
        case AsioRenderer::SourceSampleKind::Pcm16:
            return pcm::SampleFormat::Int16;
        case AsioRenderer::SourceSampleKind::Pcm24:
            return pcm::SampleFormat::Int24;
        case AsioRenderer::SourceSampleKind::Pcm32:
            return pcm::SampleFormat::Int32;
        default:
            return pcm::SampleFormat::Unknown;
    }
}

pcm::SampleFormat AsioPcmSampleFormat(ASIOSampleType outputType) {
    switch (outputType) {
        case ASIOSTFloat32LSB:
            return pcm::SampleFormat::Float32;
        case ASIOSTInt16LSB:
            return pcm::SampleFormat::Int16;
        case ASIOSTInt24LSB:
            return pcm::SampleFormat::Int24;
        case ASIOSTInt32LSB:
            return pcm::SampleFormat::Int32;
        default:
            return pcm::SampleFormat::Unknown;
    }
}

const wchar_t* SourceSampleKindName(AsioRenderer::SourceSampleKind sourceKind) {
    switch (sourceKind) {
        case AsioRenderer::SourceSampleKind::Float32:
            return L"Float32 interleaved";
        case AsioRenderer::SourceSampleKind::Pcm16:
            return L"PCM16 interleaved";
        case AsioRenderer::SourceSampleKind::Pcm24:
            return L"PCM24 interleaved";
        case AsioRenderer::SourceSampleKind::Pcm32:
            return L"PCM32 interleaved";
        default:
            return L"unsupported";
    }
}

std::uint32_t AlignAsioBufferSize(std::uint32_t requested,
                                  std::uint32_t minimum,
                                  std::uint32_t maximum,
                                  std::uint32_t preferred,
                                  long granularity) {
    if (minimum == 0 || maximum == 0 || minimum > maximum) {
        return requested != 0 ? requested : preferred;
    }

    std::uint32_t value = requested == 0 ? preferred : requested;
    if (value == 0) {
        value = minimum;
    }
    value = (std::min<std::uint32_t>)((std::max<std::uint32_t>)(value, minimum), maximum);

    if (granularity == -1) {
        std::uint32_t power = 1;
        while (power < value && power < maximum) {
            power <<= 1U;
        }
        return (std::min<std::uint32_t>)((std::max<std::uint32_t>)(power, minimum), maximum);
    }
    if (granularity > 1) {
        const auto step = static_cast<std::uint32_t>(granularity);
        const std::uint32_t offset = value > minimum ? value - minimum : 0;
        value = minimum + ((offset + step - 1U) / step) * step;
        if (value > maximum) {
            value = maximum - ((maximum - minimum) % step);
        }
    }
    return (std::min<std::uint32_t>)((std::max<std::uint32_t>)(value, minimum), maximum);
}

template <std::size_t SampleBytes>
void DeinterleaveStereo(const std::uint8_t* interleaved,
                        std::uint8_t* left,
                        std::uint8_t* right,
                        std::uint32_t frameCount) noexcept {
    constexpr std::size_t kFrameBytes = SampleBytes * 2U;
    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        const auto* source = interleaved + static_cast<std::size_t>(frame) * kFrameBytes;
        std::memcpy(left + static_cast<std::size_t>(frame) * SampleBytes,
                    source,
                    SampleBytes);
        std::memcpy(right + static_cast<std::size_t>(frame) * SampleBytes,
                    source + SampleBytes,
                    SampleBytes);
    }
}

}  // namespace

bool QueryAsioClockSources(const std::wstring& deviceId,
                           std::vector<AsioClockSourceInfo>* sources,
                           std::wstring* outError) {
    if (sources == nullptr) {
        if (outError != nullptr) {
            *outError = L"ASIO clock source output is null.";
        }
        return false;
    }

    std::lock_guard<std::mutex> probeLock(g_asioClockProbeMutex);
    if (g_asioDriverPoisoned.load(std::memory_order_acquire)) {
        if (outError != nullptr) {
            *outError = L"The ASIO driver is in an unsafe state; restart Tick By Tick before querying clock sources.";
        }
        return false;
    }
    if (g_activeRenderer.load(std::memory_order_acquire) != nullptr) {
        if (outError != nullptr) {
            *outError = L"Stop the active ASIO stream before querying clock sources.";
        }
        return false;
    }

    std::vector<AsioClockSourceInfo> queriedSources;
    std::wstring queryError;
    bool succeeded = false;
    try {
        std::thread queryThread([&] {
            try {
                succeeded = QueryAsioClockSourcesOnThread(
                        deviceId, &queriedSources, &queryError);
            } catch (const std::exception&) {
                queryError = L"ASIO clock source query failed with a C++ exception.";
            } catch (...) {
                queryError = L"ASIO clock source query failed with an unknown exception.";
            }
        });
        queryThread.join();
    } catch (const std::exception&) {
        queryError = L"Failed to start ASIO clock source query thread.";
    }

    if (!succeeded) {
        if (outError != nullptr) {
            *outError = queryError.empty()
                    ? L"Failed to query ASIO clock sources."
                    : queryError;
        }
        return false;
    }

    *sources = std::move(queriedSources);
    if (outError != nullptr) {
        outError->clear();
    }
    return true;
}

bool RawFrameRingBuffer::Reset(std::uint32_t bytesPerFrame, std::uint32_t capacityFrames) {
    if (bytesPerFrame == 0 || capacityFrames == 0) {
        return false;
    }

    bytesPerFrame_ = bytesPerFrame;
    const std::size_t requestedBytes =
            static_cast<std::size_t>(bytesPerFrame) * static_cast<std::size_t>(capacityFrames);
    const std::size_t capacityBytes = NextPowerOfTwo(requestedBytes);
    bytes_.assign(capacityBytes, 0);
    byteMask_ = capacityBytes - 1;
    frameOwners_.assign(capacityBytes / bytesPerFrame,
                        static_cast<std::uint8_t>(FrameOwner::Bridge));
    confirmedReadIndex_.store(0, std::memory_order_relaxed);
    dispatchReadIndex_.store(0, std::memory_order_relaxed);
    writeIndex_.store(0, std::memory_order_relaxed);
    return true;
}

void RawFrameRingBuffer::Clear() {
    confirmedReadIndex_.store(0, std::memory_order_release);
    dispatchReadIndex_.store(0, std::memory_order_release);
    writeIndex_.store(0, std::memory_order_release);
    std::fill(bytes_.begin(), bytes_.end(), static_cast<std::uint8_t>(0));
    std::fill(frameOwners_.begin(), frameOwners_.end(),
              static_cast<std::uint8_t>(FrameOwner::Bridge));
}

std::uint32_t RawFrameRingBuffer::Push(const std::uint8_t* data,
                                       std::uint32_t frameCount) {
    if (data == nullptr || frameCount == 0 || bytesPerFrame_ == 0 || bytes_.empty()) {
        return 0;
    }

    const std::uint64_t readIndex = confirmedReadIndex_.load(std::memory_order_acquire);
    const std::uint64_t writeIndex = writeIndex_.load(std::memory_order_relaxed);
    const std::size_t requestedBytes =
            static_cast<std::size_t>(frameCount) * static_cast<std::size_t>(bytesPerFrame_);
    if (requestedBytes > AvailableWriteBytes(readIndex, writeIndex)) {
        return 0;
    }

    CopyInto(writeIndex, data, requestedBytes);
    MarkFrames(writeIndex, frameCount, FrameOwner::Player);
    writeIndex_.store(writeIndex + requestedBytes, std::memory_order_release);
    return frameCount;
}

std::uint32_t RawFrameRingBuffer::PushCapturedSilence(std::uint32_t frameCount) {
    return PushSilence(frameCount, FrameOwner::Player);
}

std::uint32_t RawFrameRingBuffer::PushBridgeSilence(std::uint32_t frameCount) {
    return PushSilence(frameCount, FrameOwner::Bridge);
}

RawFrameRingBuffer::WriteBatch RawFrameRingBuffer::BeginWriteBatch() const {
    const std::uint64_t writeIndex =
            writeIndex_.load(std::memory_order_relaxed);
    return {writeIndex, writeIndex, true};
}

std::uint32_t RawFrameRingBuffer::StageWrite(
        WriteBatch* batch,
        const std::uint8_t* data,
        std::uint32_t frameCount,
        FrameOwner owner) {
    if (batch == nullptr) {
        return 0;
    }
    if (!batch->valid || frameCount == 0 || bytesPerFrame_ == 0 ||
        bytes_.empty()) {
        batch->valid = false;
        return 0;
    }
    const std::uint64_t publishedWrite =
            writeIndex_.load(std::memory_order_relaxed);
    if (publishedWrite != batch->startIndex ||
        batch->cursor < batch->startIndex) {
        batch->valid = false;
        return 0;
    }
    const std::uint64_t readIndex =
            confirmedReadIndex_.load(std::memory_order_acquire);
    const std::size_t requestedBytes =
            static_cast<std::size_t>(frameCount) * bytesPerFrame_;
    if (requestedBytes > AvailableWriteBytes(readIndex, batch->cursor)) {
        batch->valid = false;
        return 0;
    }
    if (data == nullptr) {
        ZeroInto(batch->cursor, requestedBytes);
    } else {
        CopyInto(batch->cursor, data, requestedBytes);
    }
    MarkFrames(batch->cursor, frameCount, owner);
    batch->cursor += requestedBytes;
    return frameCount;
}

bool RawFrameRingBuffer::CommitWriteBatch(const WriteBatch& batch) {
    if (!batch.valid || batch.cursor < batch.startIndex ||
        writeIndex_.load(std::memory_order_relaxed) != batch.startIndex) {
        return false;
    }
    // All bytes, conversion results, and owner marks become visible to the
    // callback as one timeline extension. It can observe either the old tail
    // or the complete batch, never a packet prefix.
    writeIndex_.store(batch.cursor, std::memory_order_release);
    return true;
}

std::uint32_t RawFrameRingBuffer::PushSilence(std::uint32_t frameCount,
                                              FrameOwner owner) {
    if (frameCount == 0 || bytesPerFrame_ == 0 || bytes_.empty()) {
        return 0;
    }

    const std::uint64_t readIndex = confirmedReadIndex_.load(std::memory_order_acquire);
    const std::uint64_t writeIndex = writeIndex_.load(std::memory_order_relaxed);
    const std::size_t requestedBytes =
            static_cast<std::size_t>(frameCount) * static_cast<std::size_t>(bytesPerFrame_);
    if (requestedBytes > AvailableWriteBytes(readIndex, writeIndex)) {
        return 0;
    }

    ZeroInto(writeIndex, requestedBytes);
    MarkFrames(writeIndex, frameCount, owner);
    writeIndex_.store(writeIndex + requestedBytes, std::memory_order_release);
    return frameCount;
}

RawFrameRingBuffer::DispatchResult RawFrameRingBuffer::Dispatch(
        std::uint8_t* data,
        std::uint32_t frameCount) {
    DispatchResult result{};
    if (data == nullptr || frameCount == 0 || bytesPerFrame_ == 0 || bytes_.empty()) {
        return result;
    }

    const std::uint64_t writeIndex = writeIndex_.load(std::memory_order_acquire);
    const std::uint64_t readIndex = dispatchReadIndex_.load(std::memory_order_relaxed);
    const std::size_t requestedBytes =
            static_cast<std::size_t>(frameCount) * static_cast<std::size_t>(bytesPerFrame_);
    const std::size_t readableBytes =
            static_cast<std::size_t>((std::min)(
                    static_cast<std::uint64_t>(requestedBytes),
                    AvailableReadBytes(readIndex, writeIndex))) /
            bytesPerFrame_ * bytesPerFrame_;
    result.endIndex = readIndex;
    if (readableBytes == 0) {
        return result;
    }

    CopyOut(readIndex, data, readableBytes);
    result.frames = static_cast<std::uint32_t>(readableBytes / bytesPerFrame_);
    const std::uint64_t beginFrame = readIndex / bytesPerFrame_;
    result.playerFrames = static_cast<std::uint32_t>(
            CountPlayerFrames(beginFrame, beginFrame + result.frames));
    result.bridgeFrames = result.frames - result.playerFrames;
    result.endIndex += readableBytes;
    dispatchReadIndex_.store(result.endIndex, std::memory_order_release);
    return result;
}

bool RawFrameRingBuffer::ConfirmDispatch(std::uint64_t endIndex) {
    const std::uint64_t confirmedIndex =
            confirmedReadIndex_.load(std::memory_order_relaxed);
    const std::uint64_t dispatchedIndex =
            dispatchReadIndex_.load(std::memory_order_acquire);
    if (endIndex < confirmedIndex || endIndex > dispatchedIndex) {
        return false;
    }
    confirmedReadIndex_.store(endIndex, std::memory_order_release);
    return true;
}

void RawFrameRingBuffer::RollbackDispatch() {
    dispatchReadIndex_.store(
            confirmedReadIndex_.load(std::memory_order_acquire),
            std::memory_order_release);
}

std::uint64_t RawFrameRingBuffer::DispatchPosition() const {
    return dispatchReadIndex_.load(std::memory_order_acquire);
}

std::uint64_t RawFrameRingBuffer::ConfirmedPositionFrames() const {
    if (bytesPerFrame_ == 0) {
        return 0;
    }
    return confirmedReadIndex_.load(std::memory_order_acquire) / bytesPerFrame_;
}

std::uint64_t RawFrameRingBuffer::WritePositionFrames() const {
    if (bytesPerFrame_ == 0) {
        return 0;
    }
    return writeIndex_.load(std::memory_order_acquire) / bytesPerFrame_;
}

std::uint64_t RawFrameRingBuffer::CountPlayerFrames(
        std::uint64_t beginFrame,
        std::uint64_t endFrame) const {
    if (endFrame <= beginFrame || frameOwners_.empty()) {
        return 0;
    }
    std::uint64_t playerFrames = 0;
    const std::uint64_t capacity = frameOwners_.size();
    for (std::uint64_t frame = beginFrame; frame < endFrame; ++frame) {
        if (frameOwners_[static_cast<std::size_t>(frame % capacity)] ==
            static_cast<std::uint8_t>(FrameOwner::Player)) {
            ++playerFrames;
        }
    }
    return playerFrames;
}

std::uint32_t RawFrameRingBuffer::AvailableReadFrames() const {
    if (bytesPerFrame_ == 0) {
        return 0;
    }
    const std::uint64_t readIndex = dispatchReadIndex_.load(std::memory_order_acquire);
    const std::uint64_t writeIndex = writeIndex_.load(std::memory_order_acquire);
    return static_cast<std::uint32_t>(
            AvailableReadBytes(readIndex, writeIndex) / bytesPerFrame_);
}

std::uint32_t RawFrameRingBuffer::PendingFrames() const {
    if (bytesPerFrame_ == 0) {
        return 0;
    }
    const std::uint64_t readIndex = confirmedReadIndex_.load(std::memory_order_acquire);
    const std::uint64_t writeIndex = writeIndex_.load(std::memory_order_acquire);
    return static_cast<std::uint32_t>(
            AvailableReadBytes(readIndex, writeIndex) / bytesPerFrame_);
}

std::uint32_t RawFrameRingBuffer::CapacityFrames() const {
    if (bytesPerFrame_ == 0) {
        return 0;
    }
    return static_cast<std::uint32_t>(bytes_.size() / bytesPerFrame_);
}

std::uint64_t RawFrameRingBuffer::AvailableReadBytes(std::uint64_t readIndex,
                                                     std::uint64_t writeIndex) const {
    return writeIndex - readIndex;
}

std::size_t RawFrameRingBuffer::AvailableWriteBytes(std::uint64_t readIndex,
                                                    std::uint64_t writeIndex) const {
    if (bytes_.empty()) {
        return 0;
    }
    const std::uint64_t readableBytes = AvailableReadBytes(readIndex, writeIndex);
    if (readableBytes >= bytes_.size()) {
        return 0;
    }
    return bytes_.size() - static_cast<std::size_t>(readableBytes);
}

void RawFrameRingBuffer::CopyInto(std::uint64_t writeIndex,
                                  const std::uint8_t* data,
                                  std::size_t bytes) {
    const std::size_t offset =
            static_cast<std::size_t>(writeIndex & static_cast<std::uint64_t>(byteMask_));
    const std::size_t firstChunk = (std::min)(bytes, bytes_.size() - offset);
    std::memcpy(bytes_.data() + offset, data, firstChunk);
    if (bytes > firstChunk) {
        std::memcpy(bytes_.data(), data + firstChunk, bytes - firstChunk);
    }
}

void RawFrameRingBuffer::ZeroInto(std::uint64_t writeIndex, std::size_t bytes) {
    const std::size_t offset =
            static_cast<std::size_t>(writeIndex & static_cast<std::uint64_t>(byteMask_));
    const std::size_t firstChunk = (std::min)(bytes, bytes_.size() - offset);
    std::memset(bytes_.data() + offset, 0, firstChunk);
    if (bytes > firstChunk) {
        std::memset(bytes_.data(), 0, bytes - firstChunk);
    }
}

void RawFrameRingBuffer::CopyOut(std::uint64_t readIndex,
                                 std::uint8_t* data,
                                 std::size_t bytes) const {
    const std::size_t offset =
            static_cast<std::size_t>(readIndex & static_cast<std::uint64_t>(byteMask_));
    const std::size_t firstChunk = (std::min)(bytes, bytes_.size() - offset);
    std::memcpy(data, bytes_.data() + offset, firstChunk);
    if (bytes > firstChunk) {
        std::memcpy(data + firstChunk, bytes_.data(), bytes - firstChunk);
    }
}

void RawFrameRingBuffer::MarkFrames(std::uint64_t beginByteIndex,
                                    std::uint32_t frameCount,
                                    FrameOwner owner) {
    if (bytesPerFrame_ == 0 || frameOwners_.empty()) {
        return;
    }
    const std::uint64_t beginFrame = beginByteIndex / bytesPerFrame_;
    const std::uint64_t capacity = frameOwners_.size();
    const auto value = static_cast<std::uint8_t>(owner);
    for (std::uint64_t offset = 0; offset < frameCount; ++offset) {
        frameOwners_[static_cast<std::size_t>((beginFrame + offset) % capacity)] = value;
    }
}

AsioRenderer::~AsioRenderer() {
    Stop();
    if (driverQuiesceFailed_.load(std::memory_order_acquire)) {
        AsioRenderer* expected = this;
        g_activeRenderer.compare_exchange_strong(
                expected, nullptr, std::memory_order_acq_rel);
        while (g_asioCallbackEntrants.load(std::memory_order_acquire) != 0) {
            SwitchToThread();
        }
    }
}

bool AsioRenderer::Start(const std::wstring& deviceId,
                         const WAVEFORMATEXTENSIBLE& format,
                         std::int32_t prebufferMs,
                         std::int32_t maxBufferAdvanceMs,
                         std::uint32_t applicationBufferFrames,
                         std::uint32_t requestedBufferFrames,
                         std::int32_t requestedClockSourceIndex,
                         std::wstring* outError,
                         StartMode startMode) {
    Stop();
    if ((faultRequested_.load(std::memory_order_acquire) &&
         !faulted_.load(std::memory_order_acquire)) ||
        g_asioDriverPoisoned.load(std::memory_order_acquire)) {
        if (outError != nullptr) {
            const std::wstring fault = FaultMessage();
            *outError = !fault.empty()
                    ? fault
                    : L"The previous ASIO stream is still faulting; restart Tick By Tick if it cannot be stopped safely.";
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ResetStats();
        startMode_ = startMode;
        format_ = format;
        sourceBytesPerFrame_ = BytesPerFrame(format_.Format);
        sampleRate_ = format_.Format.nSamplesPerSec;
        asioSampleRate_ = sampleRate_;
        sourceChannels_ = format_.Format.nChannels;
        sourceBytesPerSample_ =
                sourceChannels_ == 0
                ? 0
                : sourceBytesPerFrame_ / sourceChannels_;
        outputBytesPerFrame_ = 0;
        sourceKind_ = SourceKindFromWave(format_);
        prebufferMs_ = static_cast<std::int32_t>(NormalizePrebufferMs(prebufferMs));
        prebufferFrames_ = FramesFromMs(sampleRate_, static_cast<std::uint32_t>(prebufferMs_));
        applicationBufferFrames_ = applicationBufferFrames;
        const std::uint64_t effectiveTimelineFrames =
                static_cast<std::uint64_t>(prebufferFrames_) +
                applicationBufferFrames_;
        if (effectiveTimelineFrames >
            (std::numeric_limits<std::uint32_t>::max)()) {
            if (outError != nullptr) {
                *outError = L"The combined WASAPI buffer and additional prebuffer are too large.";
            }
            return false;
        }
        effectiveTimelineFrames_ =
                static_cast<std::uint32_t>(effectiveTimelineFrames);
        maxBufferAdvanceMs_ =
                static_cast<std::int32_t>(NormalizeMaxBufferAdvanceMs(maxBufferAdvanceMs));
        maxBufferAdvanceFrames_ =
                FramesFromMs(sampleRate_, static_cast<std::uint32_t>(maxBufferAdvanceMs_));
        // The application buffer is already real player-owned timeline. Keep
        // cold seeding, callback compensation, and logical admission based
        // only on the configured additional delay; adding application capacity
        // here would either duplicate it as silence or release a whole startup
        // burst immediately. The combined value is the retention budget used
        // when allocating the ring below.
        minimumTimelineFrames_ = prebufferFrames_ > maxBufferAdvanceFrames_
                ? prebufferFrames_ - maxBufferAdvanceFrames_
                : 0;
        admittedTimelineEndFrame_ = 0;
        requestedBufferFrames_ = requestedBufferFrames;
        minBufferFrames_ = 0;
        maxBufferFrames_ = 0;
        preferredBufferFrames_ = 0;
        bufferGranularity_ = 0;
        asioClockSourceIndex_ = -1;

        if (sourceBytesPerFrame_ == 0 || sampleRate_ == 0 ||
            sourceChannels_ != 2 || sourceBytesPerSample_ == 0 ||
            sourceKind_ == SourceSampleKind::Unknown) {
            if (outError != nullptr) {
                *outError = L"Captured format is not supported by the ASIO renderer; stereo PCM/float is required.";
            }
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        initComplete_ = false;
        initSucceeded_ = false;
        initError_.clear();
        lastStartError_.clear();
        shutdownRequested_ = false;
        startRequestSerial_ = 0;
        startHandledSerial_ = 0;
        lastStartSucceeded_ = false;
        faultStopRequested_.store(false, std::memory_order_release);
    }

    try {
        controlThread_ = std::thread(&AsioRenderer::ControlLoop,
                                     this,
                                     deviceId,
                                     requestedBufferFrames,
                                     requestedClockSourceIndex);
    } catch (const std::exception&) {
        if (outError != nullptr) {
            *outError = L"Failed to start ASIO control thread.";
        }
        return false;
    }

    std::unique_lock<std::mutex> lock(controlMutex_);
    initCv_.wait(lock, [this] { return initComplete_; });
    const std::wstring initMessage = initError_;
    if (!initSucceeded_) {
        lock.unlock();
        Stop();
        if (outError != nullptr) {
            *outError = initMessage.empty()
                    ? L"Failed to initialize ASIO renderer."
                    : initMessage;
        }
        return false;
    }
    lock.unlock();

    if (outError != nullptr) {
        *outError = initMessage;
    }
    return true;
}

void AsioRenderer::Stop() {
    running_.store(false, std::memory_order_release);
    CancelFirstOutputCallbackGate();
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        shutdownRequested_ = true;
    }
    controlCv_.notify_all();
    startCv_.notify_all();
    initCv_.notify_all();

    if (controlThread_.joinable()) {
        controlThread_.join();
    }

    if (driverQuiesceFailed_.load(std::memory_order_acquire)) {
        // The poisoned driver still owns its ASIO pages and may continue to
        // invoke callbacks. Keep all physical-buffer metadata intact so the
        // running=false callback path can overwrite every returned page with
        // silence. A new ASIO generation is process-wide disabled.
        return;
    }

    std::lock_guard<std::mutex> producerLock(producerMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    RollbackOutputPages();
    InterlockedExchange(&callbackExecuting_, FALSE);
    if (callbackWaiterCount_.load(std::memory_order_acquire) != 0) {
        WakeByAddressAll(const_cast<LONG*>(&callbackExecuting_));
    }
    callbackActive_.clear(std::memory_order_release);
    startAccepted_.store(false, std::memory_order_release);
    preparedStartPending_.store(false, std::memory_order_release);
    preparedOutputActive_.store(false, std::memory_order_release);
    startAcceptedAtMs_.store(0, std::memory_order_release);
    deferredSilentStartAtMs_.store(0, std::memory_order_release);
    SetPrebuffering(
            false, PrebufferTransitionReason::Stop, ringBuffer_.AvailableReadFrames());
    ringBuffer_.Clear();
    callbackBuffer_.clear();
    conversionBuffer_.clear();
    bufferFrames_ = 0;
    requestedBufferFrames_ = 0;
    applicationBufferFrames_ = 0;
    effectiveTimelineFrames_ = 0;
    minBufferFrames_ = 0;
    maxBufferFrames_ = 0;
    preferredBufferFrames_ = 0;
    bufferGranularity_ = 0;
    asioSampleRate_ = 0;
    asioClockSourceIndex_ = -1;
    outputAsioSampleType_ = ASIOSTLastEntry;
    outputBytesPerSample_ = 0;
    outputBytesPerFrame_ = 0;
    sampleConversionMode_ = SampleConversionMode::Direct;
    outputReadyState_.store(0, std::memory_order_relaxed);
    startMode_ = StartMode::Normal;
    capturedDrainActive_.store(false, std::memory_order_release);
    minimumTimelineFrames_ = 0;
    admittedTimelineEndFrame_ = 0;
}

std::uint32_t AsioRenderer::PushPcm(const std::uint8_t* data,
                                    std::uint32_t frameCount,
                                    std::wstring* outError) {
    return PushCapturedFrames(data, frameCount, false, false, outError);
}

std::uint32_t AsioRenderer::PushCapturedSilence(std::uint32_t frameCount,
                                                std::wstring* outError) {
    return PushCapturedFrames(nullptr, frameCount, true, false, outError);
}

std::uint32_t AsioRenderer::PushPreparedPcm(const std::uint8_t* data,
                                            std::uint32_t frameCount,
                                            std::wstring* outError) {
    return PushCapturedFrames(data, frameCount, false, true, outError);
}

std::uint32_t AsioRenderer::PushPreparedSilence(std::uint32_t frameCount,
                                                std::wstring* outError) {
    return PushCapturedFrames(nullptr, frameCount, true, true, outError);
}

bool AsioRenderer::PushCapturedBatch(
        const std::vector<CapturedBatchItem>& items,
        bool prepared,
        std::uint64_t* outWrittenFrames,
        std::wstring* outError) {
    if (outWrittenFrames != nullptr) {
        *outWrittenFrames = 0;
    }
    if (items.empty() || !running_.load(std::memory_order_acquire)) {
        if (outError != nullptr) {
            *outError = items.empty()
                    ? L"The captured PCM batch is empty."
                    : L"The ASIO renderer is not running.";
        }
        return false;
    }

    std::uint64_t totalFrames = 0;
    std::uint64_t silentFrames = 0;
    std::uint32_t maximumRealItemFrames = 0;
    {
        std::lock_guard<std::mutex> producerLock(producerMutex_);
        if (!running_.load(std::memory_order_acquire) ||
            prepared !=
                    preparedStartPending_.load(std::memory_order_acquire)) {
            if (outError != nullptr) {
                *outError = L"The renderer changed generation while the captured batch was being prepared.";
            }
            return false;
        }
        if (!prepared &&
            !startAccepted_.load(std::memory_order_acquire)) {
            if (outError != nullptr) {
                *outError = L"An atomic Active-bank append requires an already-started ASIO stream.";
            }
            return false;
        }
        UpdateLogicalAdmissionLocked();

        auto batch = ringBuffer_.BeginWriteBatch();
        for (const auto& item : items) {
            if (item.frameCount == 0 || (!item.silent && item.data == nullptr)) {
                if (outError != nullptr) {
                    *outError = L"The captured PCM batch contains an invalid item.";
                }
                return false;
            }
            const std::uint8_t* retainedData = nullptr;
            if (!item.silent) {
                retainedData = ConvertCapturedFramesLocked(
                        item.data, item.frameCount);
                if (retainedData == nullptr) {
                    if (outError != nullptr) {
                        *outError = L"The captured PCM batch could not be converted to the active ASIO sample format.";
                    }
                    return false;
                }
            }
            const std::uint32_t staged = ringBuffer_.StageWrite(
                    &batch,
                    retainedData,
                    item.frameCount,
                    RawFrameRingBuffer::FrameOwner::Player);
            if (staged != item.frameCount) {
                if (outError != nullptr) {
                    *outError = L"The ASIO Ring does not have room for the complete captured PCM batch.";
                }
                return false;
            }
            totalFrames += staged;
            if (item.silent) {
                silentFrames += staged;
            } else {
                maximumRealItemFrames = (std::max)(
                        maximumRealItemFrames, staged);
            }
        }
        if (!ringBuffer_.CommitWriteBatch(batch)) {
            if (outError != nullptr) {
                *outError = L"The captured PCM batch lost its single-producer commit boundary.";
            }
            return false;
        }
        totalFramesQueued_.fetch_add(
                static_cast<std::int64_t>(totalFrames),
                std::memory_order_relaxed);
        totalPlayerSilentFrames_.fetch_add(
                static_cast<std::int64_t>(silentFrames),
                std::memory_order_relaxed);
        if (maximumRealItemFrames != 0) {
            const std::uint32_t previousMaximum =
                    maximumRealPacketFrames_.load(std::memory_order_relaxed);
            if (maximumRealItemFrames > previousMaximum) {
                maximumRealPacketFrames_.store(
                        maximumRealItemFrames, std::memory_order_release);
            }
        }
        UpdateLogicalAdmissionLocked();
    }

    if (outWrittenFrames != nullptr) {
        *outWrittenFrames = totalFrames;
    }
    // Prepared batches start only at CommitPreparedStart. Non-prepared batch
    // appends were required to be already started before the one-way Ring
    // publication, so no fallible operation remains after CommitWriteBatch.
    return true;
}

std::uint32_t AsioRenderer::PushCapturedFrames(const std::uint8_t* data,
                                               std::uint32_t frameCount,
                                               bool silence,
                                               bool prepared,
                                               std::wstring* outError) {
    if (frameCount == 0) {
        return 0;
    }
    if (!silence && data == nullptr) {
        if (outError != nullptr) {
            *outError = L"Captured PCM data is null.";
        }
        return 0;
    }
    if (!running_.load(std::memory_order_acquire)) {
        if (outError != nullptr && HasFault()) {
            *outError = FaultMessage();
        }
        return 0;
    }
    const bool preparedStartPending =
            preparedStartPending_.load(std::memory_order_acquire);
    if (prepared != preparedStartPending) {
        if (outError != nullptr) {
            *outError = prepared
                    ? L"The renderer is not awaiting a prepared handoff."
                    : L"The prepared handoff must be staged and committed before ordinary PCM can be pushed.";
        }
        return 0;
    }

    std::uint32_t written = 0;
    std::uint32_t pendingFrames = 0;
    std::uint32_t capacityFrames = 0;
    {
        std::lock_guard<std::mutex> producerLock(producerMutex_);
        if (!running_.load(std::memory_order_acquire)) {
            if (outError != nullptr && HasFault()) {
                *outError = FaultMessage();
            }
            return 0;
        }
        if (prepared !=
            preparedStartPending_.load(std::memory_order_acquire)) {
            if (outError != nullptr) {
                *outError = prepared
                        ? L"The prepared handoff was committed before this PCM packet could be staged."
                        : L"The renderer is still staging a prepared handoff.";
            }
            return 0;
        }
        UpdateLogicalAdmissionLocked();
        if (silence) {
            written = ringBuffer_.PushCapturedSilence(frameCount);
        } else {
            const std::uint8_t* retainedData = ConvertCapturedFramesLocked(
                    data, frameCount);
            if (retainedData != nullptr) {
                written = ringBuffer_.Push(retainedData, frameCount);
            }
        }
        totalFramesQueued_.fetch_add(written, std::memory_order_relaxed);
        if (silence) {
            totalPlayerSilentFrames_.fetch_add(written, std::memory_order_relaxed);
        } else if (written == frameCount) {
            const std::uint32_t previousMaximum =
                    maximumRealPacketFrames_.load(std::memory_order_relaxed);
            if (frameCount > previousMaximum) {
                maximumRealPacketFrames_.store(frameCount, std::memory_order_release);
            }
        }
        if (written < frameCount) {
            totalFramesDropped_.fetch_add(frameCount - written, std::memory_order_relaxed);
        }
        UpdateLogicalAdmissionLocked();
        pendingFrames = ringBuffer_.PendingFrames();
        capacityFrames = ringBuffer_.CapacityFrames();
    }
    if (written < frameCount) {
        wchar_t message[512]{};
        std::swprintf(message,
                      std::size(message),
                      L"Captured audio could not be retained; renderer stopped. submitted=%u accepted=%u pending=%u capacity=%u confirmed=%lld",
                      frameCount,
                      written,
                      pendingFrames,
                      capacityFrames,
                      static_cast<long long>(ConfirmedCapturedFrames()));
        LatchFault(message);
        if (outError != nullptr) {
            *outError = FaultMessage();
        }
        return written;
    }
    if (written > 0 && !prepared) {
        if (silence && !startAccepted_.load(std::memory_order_acquire)) {
            std::uint64_t expected = 0;
            deferredSilentStartAtMs_.compare_exchange_strong(
                    expected, GetTickCount64(), std::memory_order_acq_rel);
        } else {
            deferredSilentStartAtMs_.store(0, std::memory_order_release);
            if (!TryStartStreamIfReady(outError)) {
                return written;
            }
        }
    }
    return written;
}

bool AsioRenderer::CommitPreparedStart(std::wstring* outError) {
    if (!running_.load(std::memory_order_acquire)) {
        if (outError != nullptr) {
            const std::wstring fault = FaultMessage();
            *outError = !fault.empty()
                    ? fault
                    : L"The renderer is not running.";
        }
        return false;
    }
    if (!preparedStartPending_.load(std::memory_order_acquire)) {
        if (outError != nullptr) {
            *outError = L"The renderer has no prepared handoff to commit.";
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> producerLock(producerMutex_);
        if (!running_.load(std::memory_order_acquire) ||
            !preparedStartPending_.load(std::memory_order_acquire)) {
            if (outError != nullptr) {
                *outError = L"The prepared handoff stopped or was already committed.";
            }
            return false;
        }
        if (ringBuffer_.PendingFrames() == 0) {
            if (outError != nullptr) {
                *outError = L"The prepared handoff contains no staged PCM frames.";
            }
            return false;
        }

        // Publish the mode transition without appending physical Bridge
        // frames. The staged Standby already represents the one configured T
        // budget; callback-owned virtual compensation begins only after Core
        // opens the prepared output gate.
        preparedStartPending_.store(false, std::memory_order_release);
        deferredSilentStartAtMs_.store(0, std::memory_order_release);
    }

    return TryStartStreamIfReady(outError);
}

bool AsioRenderer::ActivatePreparedOutput(std::wstring* outError) {
    if (!running_.load(std::memory_order_acquire) ||
        !startAccepted_.load(std::memory_order_acquire) ||
        preparedStartPending_.load(std::memory_order_acquire)) {
        if (outError != nullptr) {
            *outError = L"The prepared ASIO stream is not ready for Active-bank output.";
        }
        return false;
    }
    preparedOutputActive_.store(true, std::memory_order_release);
    return true;
}

const std::uint8_t* AsioRenderer::ConvertCapturedFramesLocked(
        const std::uint8_t* data,
        std::uint32_t frameCount) {
    if (sampleConversionMode_ == SampleConversionMode::Direct) {
        return data;
    }
    if (data == nullptr || sourceChannels_ != 2 ||
        sourceBytesPerSample_ == 0 || outputBytesPerSample_ == 0) {
        return nullptr;
    }

    const std::uint64_t sampleCount64 =
            static_cast<std::uint64_t>(frameCount) * sourceChannels_;
    const std::uint64_t requiredBytes64 =
            sampleCount64 * outputBytesPerSample_;
    if (sampleCount64 > (std::numeric_limits<std::size_t>::max)() ||
        requiredBytes64 > conversionBuffer_.size()) {
        return nullptr;
    }
    const std::size_t sampleCount = static_cast<std::size_t>(sampleCount64);
    const pcm::SampleFormat sourceFormat = SourcePcmSampleFormat(sourceKind_);
    const pcm::SampleFormat outputFormat =
            AsioPcmSampleFormat(outputAsioSampleType_);
    if (!pcm::ConvertSamples(data,
                             sourceFormat,
                             conversionBuffer_.data(),
                             outputFormat,
                             sampleCount)) {
        return nullptr;
    }
    return conversionBuffer_.data();
}

RendererStats AsioRenderer::GetStats() const {
    RendererStats stats{};
    std::int64_t bufferedFrames = 0;
    std::int64_t capacityFrames = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t asioSampleRate = 0;
    std::int32_t prebufferMs = 0;
    std::uint32_t prebufferFrames = 0;
    std::uint32_t applicationBufferFrames = 0;
    std::uint32_t effectiveTimelineFrames = 0;
    std::uint32_t requestedBufferFrames = 0;
    std::uint32_t actualBufferFrames = 0;
    std::uint32_t minBufferFrames = 0;
    std::uint32_t maxBufferFrames = 0;
    std::uint32_t preferredBufferFrames = 0;
    long bufferGranularity = 0;
    ASIOSampleType outputSampleType = ASIOSTLastEntry;
    SampleConversionMode conversionMode = SampleConversionMode::Direct;
    std::int32_t clockSourceIndex = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bufferedFrames = static_cast<std::int64_t>(ringBuffer_.PendingFrames());
        capacityFrames = static_cast<std::int64_t>(ringBuffer_.CapacityFrames());
        sampleRate = sampleRate_;
        asioSampleRate = asioSampleRate_;
        prebufferMs = prebufferMs_;
        prebufferFrames = prebufferFrames_;
        applicationBufferFrames = applicationBufferFrames_;
        effectiveTimelineFrames = effectiveTimelineFrames_;
        requestedBufferFrames = requestedBufferFrames_;
        actualBufferFrames = bufferFrames_;
        minBufferFrames = minBufferFrames_;
        maxBufferFrames = maxBufferFrames_;
        preferredBufferFrames = preferredBufferFrames_;
        bufferGranularity = bufferGranularity_;
        outputSampleType = outputAsioSampleType_;
        conversionMode = sampleConversionMode_;
        clockSourceIndex = asioClockSourceIndex_;
    }

    stats.startAccepted = startAccepted_.load(std::memory_order_relaxed);
    stats.preparedStartPending =
            preparedStartPending_.load(std::memory_order_relaxed);
    const LONG callbackState = InterlockedCompareExchange(
            const_cast<LONG*>(&firstOutputCallbackState_),
            static_cast<LONG>(FirstOutputCallbackState::Awaiting),
            static_cast<LONG>(FirstOutputCallbackState::Awaiting));
    stats.streamActive = running_.load(std::memory_order_relaxed) &&
            stats.startAccepted &&
            callbackState ==
                    static_cast<LONG>(FirstOutputCallbackState::Completed);
    const std::uint64_t deferredAt =
            deferredSilentStartAtMs_.load(std::memory_order_relaxed);
    stats.silentStartDeferred = deferredAt != 0 && !stats.startAccepted;
    const std::uint64_t startAcceptedAt =
            startAcceptedAtMs_.load(std::memory_order_relaxed);
    if (startAcceptedAt != 0 && !stats.streamActive) {
        const std::uint64_t elapsed = GetTickCount64() - startAcceptedAt;
        stats.startupWaitMs = static_cast<std::int32_t>((std::min<std::uint64_t>)(
                elapsed,
                static_cast<std::uint64_t>(
                        (std::numeric_limits<std::int32_t>::max)())));
    }
    stats.prebuffering = prebuffering_.load(std::memory_order_relaxed);
    stats.sourceSampleRate = sampleRate;
    stats.totalFramesQueued = totalFramesQueued_.load(std::memory_order_relaxed);
    stats.totalPlayerSilentFrames =
            totalPlayerSilentFrames_.load(std::memory_order_relaxed);
    stats.totalFramesPlayed = totalFramesPlayed_.load(std::memory_order_relaxed);
    stats.totalFramesDropped = totalFramesDropped_.load(std::memory_order_relaxed);
    stats.totalOutputFrames = totalOutputFrames_.load(std::memory_order_relaxed);
    stats.totalSilentFrames = totalSilentFrames_.load(std::memory_order_relaxed);
    stats.totalLogicalFrames = totalLogicalFrames_.load(std::memory_order_relaxed);
    stats.totalBridgeSilentFramesQueued =
            totalBridgeSilentFramesQueued_.load(std::memory_order_relaxed);
    stats.totalBridgeSilentFramesPlayed =
            totalBridgeSilentFramesPlayed_.load(std::memory_order_relaxed);
    stats.totalBridgeSilentFramesReplaced =
            totalBridgeSilentFramesReplaced_.load(std::memory_order_relaxed);
    stats.compensationBridgeFrames = static_cast<std::int64_t>(
            bridge::OutstandingFrames(
                    compensationBridgeState_.load(std::memory_order_relaxed)));
    const auto recent = GetRecentSilenceStats(
            stats.totalOutputFrames, stats.totalSilentFrames);
    stats.recentOutputFrames = recent.outputFrames;
    stats.recentSilentFrames = recent.silentFrames;
    stats.recentSilentPercent = recent.silentPercent;
    stats.bufferedFrames = bufferedFrames;
    stats.bufferedMs = MsFromFrames(bufferedFrames, sampleRate);
    stats.bufferCapacityFrames = capacityFrames;
    stats.bufferCapacityMs = MsFromFrames(capacityFrames, sampleRate);
    stats.prebufferTargetFrames = prebufferFrames;
    stats.prebufferTargetMs = prebufferMs;
    stats.applicationBufferFrames = applicationBufferFrames;
    stats.applicationBufferMs = MsFromFrames(applicationBufferFrames, sampleRate);
    stats.effectiveTimelineFrames = effectiveTimelineFrames;
    stats.effectiveTimelineMs = MsFromFrames(effectiveTimelineFrames, sampleRate);
    stats.maximumRealPacketFrames =
            maximumRealPacketFrames_.load(std::memory_order_acquire);
    stats.underrunCount = underrunCount_.load(std::memory_order_relaxed);
    stats.prebufferEnterCount =
            prebufferEnterCount_.load(std::memory_order_acquire);
    stats.prebufferExitCount =
            prebufferExitCount_.load(std::memory_order_acquire);
    stats.lastPrebufferTransition =
            lastPrebufferTransition_.load(std::memory_order_relaxed);
    stats.lastPrebufferTransitionFrames =
            lastPrebufferTransitionFrames_.load(std::memory_order_relaxed);
    stats.asioRequestedBufferFrames = static_cast<std::int32_t>(requestedBufferFrames);
    stats.asioActualBufferFrames = static_cast<std::int32_t>(actualBufferFrames);
    stats.asioMinBufferFrames = static_cast<std::int32_t>(minBufferFrames);
    stats.asioMaxBufferFrames = static_cast<std::int32_t>(maxBufferFrames);
    stats.asioPreferredBufferFrames = static_cast<std::int32_t>(preferredBufferFrames);
    stats.asioBufferGranularity = static_cast<std::int32_t>(bufferGranularity);
    stats.asioOutputSampleType = static_cast<std::int32_t>(outputSampleType);
    stats.sampleConversionMode = static_cast<std::int32_t>(conversionMode);
    stats.asioSampleRate = asioSampleRate;
    stats.asioResetRequests = asioResetRequests_.load(std::memory_order_relaxed);
    stats.asioBufferSizeChanges = asioBufferSizeChanges_.load(std::memory_order_relaxed);
    stats.asioLatencyChanges = asioLatencyChanges_.load(std::memory_order_relaxed);
    stats.asioRebuildCount = asioRebuildCount_.load(std::memory_order_relaxed);
    stats.asioLastMessage = asioLastMessage_.load(std::memory_order_relaxed);
    stats.asioClockSourceIndex = clockSourceIndex;
    stats.callbackRealtimeMode =
            callbackRealtimeMode_.load(std::memory_order_relaxed);
    stats.asioOutputReadyState =
            outputReadyState_.load(std::memory_order_relaxed);
    return stats;
}

bool AsioRenderer::IsRunning() const {
    return running_.load(std::memory_order_acquire);
}

std::int64_t AsioRenderer::ConfirmedCapturedFrames() const {
    return totalFramesPlayed_.load(std::memory_order_acquire);
}

std::int64_t AsioRenderer::LogicalConsumedFrames() const {
    return totalLogicalFrames_.load(std::memory_order_acquire);
}

std::int64_t AsioRenderer::ConfirmedOutputFrames() const {
    return totalOutputFrames_.load(std::memory_order_acquire);
}

std::int64_t AsioRenderer::PendingCapturedFrames() const {
    const std::int64_t queued = totalFramesQueued_.load(std::memory_order_acquire);
    const std::int64_t confirmed = totalFramesPlayed_.load(std::memory_order_acquire);
    return (std::max<std::int64_t>)(queued - confirmed, 0);
}

std::int64_t AsioRenderer::PendingTimelineFrames() const {
    return ringBuffer_.PendingFrames();
}

std::int64_t AsioRenderer::AvailableTimelineWriteFrames() const {
    const std::int64_t capacity = ringBuffer_.CapacityFrames();
    const std::int64_t pending = ringBuffer_.PendingFrames();
    return (std::max<std::int64_t>)(capacity - pending, 0);
}

std::int64_t AsioRenderer::AdmissionRetentionFrames() {
    std::lock_guard<std::mutex> producerLock(producerMutex_);
    // Stop publishes running=false before it mutates the ring under this
    // producer lock; Start publishes running=true only after the control
    // thread has finished resetting it. Check first so a Core pipe thread
    // never reads non-atomic ring metadata across a renderer generation.
    if (!running_.load(std::memory_order_acquire)) {
        return 0;
    }
    // Admission is the player/Core storage contract. Virtual compensation
    // silence neither owns player PCM nor occupies Ring capacity, including
    // after it has been dispatched to an ASIO page, so it must not consume
    // this budget and suppress the real PCM that is meant to replace it.
    return ringBuffer_.PendingFrames();
}

void AsioRenderer::PrimeTimeline() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> producerLock(producerMutex_);
    UpdateLogicalAdmissionLocked();
    // Normal generations are seeded once in CreateBuffersOnControlThread.
    // Prepared banks already waited for their own T budget. Re-priming here
    // would create a second delay; steady compensation is callback-owned.
}

void AsioRenderer::MaintainTimeline() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> producerLock(producerMutex_);
    MaintainTimelineLocked();
}

void AsioRenderer::UpdateLogicalAdmissionLocked() {
    std::uint64_t confirmedFrame = 0;
    std::uint64_t writeFrame = 0;
    std::uint64_t compensationState = 0;
    std::uint64_t stableSequence = 0;
    bool stableSnapshot = false;
    for (std::uint32_t attempt = 0; attempt < 4; ++attempt) {
        const std::uint64_t sequenceBefore =
                compensationStateSequence_.load(std::memory_order_acquire);
        if ((sequenceBefore & 1U) != 0) {
            YieldProcessor();
            continue;
        }
        confirmedFrame = ringBuffer_.ConfirmedPositionFrames();
        writeFrame = ringBuffer_.WritePositionFrames();
        compensationState =
                compensationBridgeState_.load(std::memory_order_acquire);
        const std::uint64_t sequenceAfter =
                compensationStateSequence_.load(std::memory_order_acquire);
        if (bridge::IsStableSequenceSnapshot(
                    sequenceBefore, sequenceAfter)) {
            stableSnapshot = true;
            stableSequence = sequenceAfter;
            break;
        }
    }
    if (!stableSnapshot) {
        // Logical admission is monotonic. Skipping one producer pass is safer
        // than advancing through a virtual-silence insertion in progress; the
        // next packet/feedback pass will retry.
        return;
    }

    const std::uint64_t targetEnd = bridge::LogicalTargetEnd(
            confirmedFrame,
            writeFrame,
            prebufferFrames_,
            compensationState);
    if (targetEnd <= admittedTimelineEndFrame_) {
        return;
    }

    const std::uint64_t playerFrames = ringBuffer_.CountPlayerFrames(
            admittedTimelineEndFrame_, targetEnd);
    if (compensationStateSequence_.load(std::memory_order_acquire) !=
        stableSequence) {
        return;
    }
    if (playerFrames != 0) {
        totalLogicalFrames_.fetch_add(
                static_cast<std::int64_t>(playerFrames),
                std::memory_order_release);
    }
    admittedTimelineEndFrame_ = targetEnd;
}

void AsioRenderer::MaintainTimelineLocked() {
    UpdateLogicalAdmissionLocked();
    if (!running_.load(std::memory_order_acquire) ||
        preparedStartPending_.load(std::memory_order_acquire) ||
        capturedDrainActive_.load(std::memory_order_acquire)) {
        return;
    }

    const std::uint64_t pendingFrames = ringBuffer_.PendingFrames();
    const std::uint64_t compensationFrames = bridge::OutstandingFrames(
            compensationBridgeState_.load(std::memory_order_acquire));
    if (prebuffering_.load(std::memory_order_acquire) &&
        pendingFrames + compensationFrames >= prebufferFrames_) {
        SetPrebuffering(false,
                        PrebufferTransitionReason::Refilled,
                        static_cast<std::uint32_t>((std::min<std::uint64_t>)(
                                pendingFrames + compensationFrames,
                                (std::numeric_limits<std::uint32_t>::max)())));
    }
}

void AsioRenderer::BeginCapturedDrain() {
    std::lock_guard<std::mutex> producerLock(producerMutex_);
    UpdateLogicalAdmissionLocked();
    capturedDrainActive_.store(true, std::memory_order_release);
    compensationBridgeCancelSerial_.fetch_add(1, std::memory_order_acq_rel);
    CancelQueuedCompensationBridge(false);
    SetPrebuffering(false,
                    PrebufferTransitionReason::DrainBegin,
                    ringBuffer_.AvailableReadFrames());
}

void AsioRenderer::SettleCapturedDrain() {
    // Refresh the final player-owned admission while keeping the no-Bridge
    // drain gate closed. A same-config bank can now be appended directly
    // behind the confirmed old timeline before low-water maintenance resumes.
    std::lock_guard<std::mutex> producerLock(producerMutex_);
    UpdateLogicalAdmissionLocked();
}

void AsioRenderer::EndCapturedDrain() {
    // PendingTimelineFrames reaching zero proves that the DAC has confirmed
    // the old ordered timeline, but the feedback thread may not yet have
    // advanced logical admission through its final player-owned frames. Settle
    // that ownership while drain mode still prevents bridge replenishment, so
    // the caller can take the next stream's baseline without inheriting old
    // player frames.
    std::lock_guard<std::mutex> producerLock(producerMutex_);
    UpdateLogicalAdmissionLocked();
    capturedDrainActive_.store(false, std::memory_order_release);
}

bool AsioRenderer::HasFault() const {
    return faultRequested_.load(std::memory_order_acquire) ||
           faulted_.load(std::memory_order_acquire);
}

std::wstring AsioRenderer::FaultMessage() const {
    std::lock_guard<std::mutex> lock(faultMutex_);
    return faultMessage_;
}

void AsioRenderer::OnAsioBufferSwitch(long doubleBufferIndex,
                                      const ASIOTime* /*timeInfo*/) {
    const auto realtimeMode = static_cast<std::int32_t>(
            EnsureAsioCallbackRealtimeScheduling());
    if (callbackRealtimeMode_.load(std::memory_order_relaxed) ==
        static_cast<std::int32_t>(CallbackRealtimeMode::Unknown)) {
        callbackRealtimeMode_.store(realtimeMode, std::memory_order_release);
    }

    if (callbackActive_.test_and_set(std::memory_order_acquire)) {
        RequestAsioFault(AsioFaultCode::ReenteredCallback);
        return;
    }
    InterlockedExchange(&callbackExecuting_, TRUE);
    bool compensationSequenceOpen = false;
    std::uint64_t compensationSequenceFinal = 0;
    const auto finishCallback = [this,
                                 &compensationSequenceOpen,
                                 &compensationSequenceFinal] {
        if (compensationSequenceOpen) {
            compensationStateSequence_.store(
                    compensationSequenceFinal, std::memory_order_release);
            compensationSequenceOpen = false;
        }
        InterlockedExchange(&callbackExecuting_, FALSE);
        if (callbackWaiterCount_.load(std::memory_order_acquire) != 0) {
            WakeByAddressAll(const_cast<LONG*>(&callbackExecuting_));
        }
        callbackActive_.clear(std::memory_order_release);
    };
    if (doubleBufferIndex < 0 || doubleBufferIndex > 1) {
        RequestAsioFault(AsioFaultCode::InvalidBufferIndex);
        finishCallback();
        return;
    }
    if (!running_.load(std::memory_order_acquire)) {
        FillOutputBufferWithSilence(doubleBufferIndex);
        NotifyOutputReady();
        finishCallback();
        return;
    }

    // Keep Ring confirmation/dispatch and virtual-Bridge insertion in one
    // callback-side seqlock interval. Producers never wait for this interval;
    // they simply defer logical admission if their snapshot overlaps it.
    const std::uint64_t compensationSequenceStart =
            compensationStateSequence_.load(std::memory_order_relaxed);
    compensationSequenceFinal = compensationSequenceStart + 2U;
    // The acquire half prevents Ring/state operations below from moving ahead
    // of the odd publication; producers can therefore never accept an even
    // sequence around a partially updated callback snapshot.
    compensationStateSequence_.exchange(
            compensationSequenceStart + 1U, std::memory_order_acq_rel);
    compensationSequenceOpen = true;

    if (awaitingFirstBufferSwitch_) {
        const std::size_t firstOutputPageIndex =
                static_cast<std::size_t>(1L - doubleBufferIndex);
        auto& firstOutputPage = outputPageLedgers_[firstOutputPageIndex];
        firstOutputPage.valid = true;
        firstOutputPage.sequence = nextDispatchSequence_++;
        firstOutputPage.dispatchEndIndex = ringBuffer_.DispatchPosition();
        firstOutputPage.outputFrames = bufferFrames_;
        firstOutputPage.capturedFrames = 0;
        firstOutputPage.managedSilentFrames = bufferFrames_;
        firstOutputPage.underrunSilentFrames = 0;
        awaitingFirstBufferSwitch_ = false;
    }
    if (!ConfirmOutputPage(doubleBufferIndex)) {
        FillOutputBufferWithSilence(doubleBufferIndex);
        NotifyOutputReady();
        finishCallback();
        return;
    }
    FillOutputBuffer(doubleBufferIndex);
    if (pendingAsioFault_.load(std::memory_order_acquire) == 0 &&
        !faultRequested_.load(std::memory_order_acquire)) {
        const LONG awaiting =
                static_cast<LONG>(FirstOutputCallbackState::Awaiting);
        const LONG completed =
                static_cast<LONG>(FirstOutputCallbackState::Completed);
        if (InterlockedCompareExchange(
                    &firstOutputCallbackState_, completed, awaiting) == awaiting) {
            deferredSilentStartAtMs_.store(0, std::memory_order_release);
            WakeByAddressAll(const_cast<LONG*>(&firstOutputCallbackState_));
        }
    }
    finishCallback();
}

void AsioRenderer::OnAsioSampleRateChanged(ASIOSampleRate sampleRate) {
    if (sampleRate > 0.0) {
        const auto nextSampleRate = static_cast<std::uint32_t>(sampleRate + 0.5);
        if (sampleRate_ != 0 && nextSampleRate != sampleRate_) {
            RequestAsioFault(AsioFaultCode::SampleRateChanged, nextSampleRate);
        }
    }
}

long AsioRenderer::OnAsioMessage(long selector, long value, void* /*message*/, double* /*opt*/) {
    switch (selector) {
        case kAsioSelectorSupported:
            return value == kAsioEngineVersion ||
                           value == kAsioResetRequest ||
                           value == kAsioBufferSizeChange ||
                           value == kAsioResyncRequest ||
                           value == kAsioSupportsTimeInfo ||
                           value == kAsioLatenciesChanged ||
                           value == kAsioOverload
                    ? 1
                    : 0;
        case kAsioEngineVersion:
            return 2;
        case kAsioResetRequest:
            asioResetRequests_.fetch_add(1, std::memory_order_relaxed);
            asioLastMessage_.store(static_cast<std::int32_t>(selector), std::memory_order_relaxed);
            RequestAsioFault(AsioFaultCode::ResetRequested);
            return 1;
        case kAsioBufferSizeChange:
            asioBufferSizeChanges_.fetch_add(1, std::memory_order_relaxed);
            asioLastMessage_.store(static_cast<std::int32_t>(selector), std::memory_order_relaxed);
            RequestAsioFault(AsioFaultCode::BufferSizeChanged);
            return 1;
        case kAsioLatenciesChanged:
            asioLatencyChanges_.fetch_add(1, std::memory_order_relaxed);
            asioLastMessage_.store(static_cast<std::int32_t>(selector), std::memory_order_relaxed);
            return 1;
        case kAsioResyncRequest:
            asioLastMessage_.store(static_cast<std::int32_t>(selector), std::memory_order_relaxed);
            RequestAsioFault(AsioFaultCode::ResyncRequested);
            return 1;
        case kAsioOverload:
            asioLastMessage_.store(static_cast<std::int32_t>(selector), std::memory_order_relaxed);
            RequestAsioFault(AsioFaultCode::Overload);
            return 1;
        case kAsioSupportsTimeInfo:
            return 1;
        default:
            return 0;
    }
}

void AsioRenderer::ControlLoop(std::wstring deviceId,
                               std::uint32_t requestedBufferFrames,
                               std::int32_t requestedClockSourceIndex) {
    // Some Windows ASIO drivers post startup work back to the thread that
    // initialized them. Create that queue before init and keep it pumped while
    // the control thread is otherwise idle.
    EnsureCurrentThreadMessageQueue();
    std::wstring error;
    bool ok = OpenDriverOnControlThread(
            deviceId, requestedBufferFrames, requestedClockSourceIndex, &error);
    if (pendingAsioFault_.load(std::memory_order_acquire) != 0) {
        FinalizePendingAsioFault();
        if (error.empty()) {
            error = FaultMessage();
        }
        ok = false;
    }
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        initComplete_ = true;
        initSucceeded_ = ok;
        initError_ = error;
    }
    initCv_.notify_all();
    if (!ok) {
        CloseDriverOnControlThread();
        return;
    }

    std::uint64_t handledSerial = 0;
    std::unique_lock<std::mutex> lock(controlMutex_);
    while (!shutdownRequested_) {
        const auto controlReady = [this, handledSerial] {
            return shutdownRequested_ ||
                   faultStopRequested_.load(std::memory_order_acquire) ||
                   startRequestSerial_ != handledSerial;
        };
        if (!controlReady()) {
            controlCv_.wait_for(lock, kControlMessagePumpInterval, controlReady);
        }
        lock.unlock();
        PumpCurrentThreadMessages();
        lock.lock();
        if (!controlReady()) {
            continue;
        }
        if (shutdownRequested_) {
            break;
        }
        if (faultStopRequested_.exchange(false, std::memory_order_acq_rel)) {
            lock.unlock();
            FinalizePendingAsioFault();
            lock.lock();
            break;
        }

        const std::uint64_t serial = startRequestSerial_;
        lock.unlock();

        bool started = true;
        std::wstring startError;
        if (faultRequested_.load(std::memory_order_acquire)) {
            started = false;
            startError = L"ASIO renderer entered a faulting state before output could start.";
        } else if (!startAccepted_.load(std::memory_order_acquire)) {
            const ASIOError startResult = asioDriver_->start();
            if (!IsAsioSuccess(startResult)) {
                started = false;
                startError = AsioErrorMessage(asioDriver_, startResult, L"IASIO::start");
                LatchFault(startError);
            } else {
                startAcceptedAtMs_.store(GetTickCount64(), std::memory_order_release);
                startAccepted_.store(true, std::memory_order_release);
                if (faultRequested_.load(std::memory_order_acquire)) {
                    started = false;
                    startError = L"ASIO renderer entered a faulting state while output was starting.";
                }
            }
        }

        lock.lock();
        handledSerial = serial;
        startHandledSerial_ = serial;
        lastStartSucceeded_ = started;
        lastStartError_ = startError;
        startCv_.notify_all();
    }
    lock.unlock();

    bool driverQuiesced = true;
    if (startAccepted_.load(std::memory_order_acquire) && asioDriver_ != nullptr) {
        const ASIOError stopResult = asioDriver_->stop();
        if (IsAsioSuccess(stopResult)) {
            startAccepted_.store(false, std::memory_order_release);
            startAcceptedAtMs_.store(0, std::memory_order_release);
            if (g_activeRenderer.load(std::memory_order_acquire) == this) {
                g_activeRenderer.store(nullptr, std::memory_order_release);
            }
        } else {
            driverQuiesced = false;
            driverQuiesceFailed_.store(true, std::memory_order_release);
            g_asioDriverPoisoned.store(true, std::memory_order_release);
            LatchFault(AsioErrorMessage(asioDriver_, stopResult, L"IASIO::stop"));
        }
    }
    running_.store(false, std::memory_order_release);
    SetPrebuffering(
            false, PrebufferTransitionReason::Stop, ringBuffer_.AvailableReadFrames());
    if (driverQuiesced) {
        RollbackOutputPages();
        CloseDriverOnControlThread();
    }
}

bool AsioRenderer::OpenDriverOnControlThread(const std::wstring& deviceId,
                                             std::uint32_t requestedBufferFrames,
                                             std::int32_t requestedClockSourceIndex,
                                             std::wstring* outError) {
    if (deviceId.empty()) {
        if (outError != nullptr) {
            *outError = L"No ASIO output device is selected.";
        }
        return false;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        if (outError != nullptr) {
            *outError = HResultMessage(hr, L"CoInitializeEx");
        }
        return false;
    }
    coInitialized_ = SUCCEEDED(hr);

    CLSID clsid{};
    hr = CLSIDFromString(deviceId.c_str(), &clsid);
    if (FAILED(hr)) {
        if (outError != nullptr) {
            *outError = L"ASIO device id is not a valid CLSID.";
        }
        return false;
    }

    hr = CoCreateInstance(clsid,
                          nullptr,
                          CLSCTX_INPROC_SERVER,
                          clsid,
                          reinterpret_cast<void**>(&asioDriver_));
    if (FAILED(hr) || asioDriver_ == nullptr) {
        if (outError != nullptr) {
            *outError = HResultMessage(hr, L"CoCreateInstance(ASIO driver)");
        }
        return false;
    }

    if (asioDriver_->init(GetDesktopWindow()) == ASIOFalse) {
        if (outError != nullptr) {
            *outError = L"IASIO::init failed: " + AsioErrorMessage(asioDriver_, ASE_NotPresent, L"IASIO::init");
        }
        return false;
    }

    std::vector<AsioClockSourceInfo> clockSources;
    std::wstring clockSourceError;
    const bool clockSourcesAvailable =
            ReadClockSourcesFromDriver(asioDriver_, &clockSources, &clockSourceError);
    std::int32_t activeClockSourceIndex = -1;
    std::wstring clockSourceWarning;
    if (clockSourcesAvailable) {
        const auto current = std::find_if(
                clockSources.begin(), clockSources.end(), [](const AsioClockSourceInfo& source) {
                    return source.isCurrent;
                });
        if (current != clockSources.end()) {
            activeClockSourceIndex = current->index;
        }
    }

    if (requestedClockSourceIndex >= 0) {
        if (!clockSourcesAvailable) {
            clockSourceWarning = clockSourceError.empty()
                    ? L"The ASIO driver did not expose selectable clock sources."
                    : clockSourceError;
        } else {
            const auto selected = std::find_if(
                    clockSources.begin(),
                    clockSources.end(),
                    [requestedClockSourceIndex](const AsioClockSourceInfo& source) {
                        return source.index == requestedClockSourceIndex;
                    });
            if (selected == clockSources.end()) {
                clockSourceWarning =
                        L"The selected ASIO clock source is no longer available.";
            } else if (selected->isCurrent) {
                activeClockSourceIndex = requestedClockSourceIndex;
            } else {
                const ASIOError setClockResult =
                        asioDriver_->setClockSource(static_cast<long>(requestedClockSourceIndex));
                if (!IsAsioSuccess(setClockResult)) {
                    clockSourceWarning = AsioErrorMessage(
                            asioDriver_, setClockResult, L"IASIO::setClockSource");
                } else {
                    constexpr int kClockSourceConfirmationAttempts = 20;
                    bool confirmed = false;
                    std::wstring confirmationError;
                    for (int attempt = 0;
                         attempt < kClockSourceConfirmationAttempts;
                         ++attempt) {
                        std::vector<AsioClockSourceInfo> refreshedSources;
                        if (ReadClockSourcesFromDriver(
                                    asioDriver_, &refreshedSources, &confirmationError)) {
                            const auto refreshedCurrent = std::find_if(
                                    refreshedSources.begin(),
                                    refreshedSources.end(),
                                    [](const AsioClockSourceInfo& source) {
                                        return source.isCurrent;
                                    });
                            activeClockSourceIndex = refreshedCurrent == refreshedSources.end()
                                    ? -1
                                    : refreshedCurrent->index;
                            if (activeClockSourceIndex == requestedClockSourceIndex) {
                                confirmed = true;
                                break;
                            }
                            confirmationError = L"The ASIO driver did not report the requested clock source as current.";
                        }
                        if (attempt + 1 < kClockSourceConfirmationAttempts) {
                            Sleep(25);
                        }
                    }
                    if (!confirmed) {
                        clockSourceWarning = confirmationError.empty()
                                ? L"The ASIO clock source change could not be confirmed."
                                : confirmationError;
                    }
                }
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        asioClockSourceIndex_ = activeClockSourceIndex;
    }

    ASIOSampleRate currentRate = 0.0;
    ASIOError result = asioDriver_->getSampleRate(&currentRate);
    if (!IsAsioSuccess(result) || currentRate <= 0.0) {
        if (outError != nullptr) {
            *outError = AsioErrorMessage(asioDriver_, result, L"IASIO::getSampleRate");
        }
        return false;
    }

    const ASIOSampleRate requestedRate = static_cast<ASIOSampleRate>(sampleRate_);
    result = asioDriver_->canSampleRate(requestedRate);
    if (!IsAsioSuccess(result)) {
        if (outError != nullptr) {
            wchar_t buffer[256]{};
            std::swprintf(buffer,
                          std::size(buffer),
                          L"ASIO driver does not support %.0f Hz for the captured stream.",
                          requestedRate);
            *outError = buffer;
        }
        return false;
    }
    if (std::fabs(currentRate - requestedRate) >= 0.5) {
        result = asioDriver_->setSampleRate(requestedRate);
        if (!IsAsioSuccess(result)) {
            if (outError != nullptr) {
                *outError = AsioErrorMessage(asioDriver_, result, L"IASIO::setSampleRate");
            }
            return false;
        }
        result = asioDriver_->getSampleRate(&currentRate);
        if (!IsAsioSuccess(result) || std::fabs(currentRate - requestedRate) >= 0.5) {
            if (outError != nullptr) {
                wchar_t buffer[256]{};
                std::swprintf(buffer,
                              std::size(buffer),
                              L"ASIO driver did not synchronize to %.0f Hz after IASIO::setSampleRate.",
                              requestedRate);
                *outError = buffer;
            }
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        asioSampleRate_ = static_cast<std::uint32_t>(currentRate + 0.5);
    }

    if (!CreateBuffersOnControlThread(requestedBufferFrames, true, outError)) {
        return false;
    }
    if (!clockSourceWarning.empty() && outError != nullptr) {
        *outError = L"ASIO clock source selection was skipped; using the driver current source. " +
                clockSourceWarning;
    }
    return true;
}

bool AsioRenderer::CreateBuffersOnControlThread(std::uint32_t requestedBufferFrames,
                                                bool resetRingBuffer,
                                                std::wstring* outError) {
    if (asioDriver_ == nullptr) {
        if (outError != nullptr) {
            *outError = L"ASIO driver is not open.";
        }
        return false;
    }

    long inputChannels = 0;
    long outputChannels = 0;
    ASIOError result = asioDriver_->getChannels(&inputChannels, &outputChannels);
    if (!IsAsioSuccess(result) || outputChannels < 2) {
        if (outError != nullptr) {
            *outError = L"ASIO driver does not expose at least two output channels.";
        }
        return false;
    }

    long minBuffer = 0;
    long maxBuffer = 0;
    long preferredBuffer = 0;
    long granularity = 0;
    result = asioDriver_->getBufferSize(&minBuffer, &maxBuffer, &preferredBuffer, &granularity);
    if (!IsAsioSuccess(result)) {
        if (outError != nullptr) {
            *outError = AsioErrorMessage(asioDriver_, result, L"IASIO::getBufferSize");
        }
        return false;
    }

    const std::uint32_t chosenBufferFrames =
            AlignAsioBufferSize(requestedBufferFrames,
                                static_cast<std::uint32_t>((std::max<long>)(minBuffer, 0)),
                                static_cast<std::uint32_t>((std::max<long>)(maxBuffer, 0)),
                                static_cast<std::uint32_t>((std::max<long>)(preferredBuffer, 0)),
                                granularity);
    if (static_cast<std::uint64_t>(chosenBufferFrames) * 2U >
        (std::numeric_limits<std::uint32_t>::max)()) {
        if (outError != nullptr) {
            *outError = L"The ASIO double buffer is too large for lock-free compensation accounting.";
        }
        return false;
    }

    ASIOChannelInfo firstChannel{};
    firstChannel.channel = 0;
    firstChannel.isInput = ASIOFalse;
    result = asioDriver_->getChannelInfo(&firstChannel);
    if (!IsAsioSuccess(result)) {
        if (outError != nullptr) {
            *outError = AsioErrorMessage(asioDriver_, result, L"IASIO::getChannelInfo(0)");
        }
        return false;
    }

    ASIOChannelInfo secondChannel{};
    secondChannel.channel = 1;
    secondChannel.isInput = ASIOFalse;
    result = asioDriver_->getChannelInfo(&secondChannel);
    if (!IsAsioSuccess(result)) {
        if (outError != nullptr) {
            *outError = AsioErrorMessage(asioDriver_, result, L"IASIO::getChannelInfo(1)");
        }
        return false;
    }
    if (firstChannel.type != secondChannel.type) {
        if (outError != nullptr) {
            *outError = L"ASIO output channel sample types do not match.";
        }
        return false;
    }

    const std::uint32_t outputBytesPerSample = AsioSampleBytes(firstChannel.type);
    if (outputBytesPerSample == 0) {
        if (outError != nullptr) {
            std::wstring message = L"ASIO output sample type is unsupported: ";
            message += AsioSampleTypeName(firstChannel.type);
            *outError = message;
        }
        return false;
    }
    const pcm::SampleFormat sourceSampleFormat =
            SourcePcmSampleFormat(sourceKind_);
    const pcm::SampleFormat outputSampleFormat =
            AsioPcmSampleFormat(firstChannel.type);
    SampleConversionMode conversionMode = SampleConversionMode::Direct;
    if (sourceSampleFormat == pcm::SampleFormat::Unknown ||
        outputSampleFormat == pcm::SampleFormat::Unknown) {
        if (outError != nullptr) {
            std::wstring message = L"ASIO output sample conversion is unsupported. Source=";
            message += SourceSampleKindName(sourceKind_);
            message += L", ASIO=";
            message += AsioSampleTypeName(firstChannel.type);
            message += L". Standard Float32LSB, Int16LSB, Int24LSB, and Int32LSB formats are supported; packed valid-bit ASIO formats are rejected.";
            *outError = message;
        }
        return false;
    }
    if (IsDirectSampleFormat(sourceKind_, firstChannel.type) &&
        outputBytesPerSample == sourceBytesPerSample_) {
        conversionMode = SampleConversionMode::Direct;
    } else if (sourceSampleFormat == pcm::SampleFormat::Float32 &&
               pcm::IsIntegerFormat(outputSampleFormat)) {
        conversionMode = SampleConversionMode::FloatToInteger;
    } else if (pcm::IsIntegerFormat(sourceSampleFormat) &&
               outputSampleFormat == pcm::SampleFormat::Float32) {
        conversionMode = SampleConversionMode::IntegerToFloat;
    } else if (pcm::IsIntegerFormat(sourceSampleFormat) &&
               pcm::IsIntegerFormat(outputSampleFormat)) {
        conversionMode = SampleConversionMode::IntegerBitDepth;
    } else {
        if (outError != nullptr) {
            std::wstring message = L"ASIO output sample conversion is unsupported. Source=";
            message += SourceSampleKindName(sourceKind_);
            message += L", ASIO=";
            message += AsioSampleTypeName(firstChannel.type);
            *outError = message;
        }
        return false;
    }

    bool preparedStart = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        preparedStart = startMode_ == StartMode::PreparedHandoff;
        bufferFrames_ = chosenBufferFrames;
        requestedBufferFrames_ = requestedBufferFrames;
        minBufferFrames_ = static_cast<std::uint32_t>((std::max<long>)(minBuffer, 0));
        maxBufferFrames_ = static_cast<std::uint32_t>((std::max<long>)(maxBuffer, 0));
        preferredBufferFrames_ = static_cast<std::uint32_t>((std::max<long>)(preferredBuffer, 0));
        bufferGranularity_ = granularity;
        outputAsioSampleType_ = firstChannel.type;
        outputBytesPerSample_ = outputBytesPerSample;
        outputBytesPerFrame_ = outputBytesPerSample_ * sourceChannels_;
        sampleConversionMode_ = conversionMode;
        outputReadyState_.store(0, std::memory_order_relaxed);
        channelInfos_[0] = firstChannel;
        channelInfos_[1] = secondChannel;

        if (resetRingBuffer) {
            const std::uint64_t timelineCapacity =
                    static_cast<std::uint64_t>(effectiveTimelineFrames_) * 4U;
            const std::uint64_t callbackCapacity =
                    static_cast<std::uint64_t>(bufferFrames_) * 16U;
            const std::uint64_t ringFrames64 = (std::max<std::uint64_t>)(
                    (std::max<std::uint64_t>)(sampleRate_, timelineCapacity),
                    callbackCapacity);
            if (ringFrames64 == 0 ||
                ringFrames64 > (std::numeric_limits<std::uint32_t>::max)()) {
                if (outError != nullptr) {
                    *outError = L"The combined WASAPI buffer and additional prebuffer exceed the supported timeline capacity.";
                }
                return false;
            }
            const std::uint32_t ringFrames =
                    static_cast<std::uint32_t>(ringFrames64);
            if (!ringBuffer_.Reset(outputBytesPerFrame_, ringFrames)) {
                if (outError != nullptr) {
                    *outError = L"Failed to allocate ASIO PCM ring buffer.";
                }
                return false;
            }
            admittedTimelineEndFrame_ = 0;
            if (!preparedStart && prebufferFrames_ != 0) {
                // A legacy/Normal cold generation begins with one physical T
                // seed. Prepared banks already accumulated that same delay
                // budget and intentionally skip this seed, avoiding 2T.
                // Steady low-water compensation is virtual and callback-owned.
                const std::uint32_t seededFrames =
                        ringBuffer_.PushBridgeSilence(prebufferFrames_);
                if (seededFrames != prebufferFrames_) {
                    if (outError != nullptr) {
                        *outError = L"Failed to seed the fixed-delay ASIO timeline.";
                    }
                    return false;
                }
                totalBridgeSilentFramesQueued_.fetch_add(
                        seededFrames, std::memory_order_relaxed);
                admittedTimelineEndFrame_ = ringBuffer_.WritePositionFrames();
            }
        }
        if (sampleConversionMode_ == SampleConversionMode::Direct) {
            conversionBuffer_.clear();
        } else {
            const std::uint64_t conversionFrameCapacity =
                    (std::max<std::uint64_t>)(sampleRate_, applicationBufferFrames_);
            const std::uint64_t conversionByteCapacity =
                    conversionFrameCapacity * outputBytesPerFrame_;
            if (conversionByteCapacity >
                (std::numeric_limits<std::size_t>::max)()) {
                if (outError != nullptr) {
                    *outError = L"The PCM sample conversion buffer is too large.";
                }
                return false;
            }
            conversionBuffer_.assign(
                    static_cast<std::size_t>(conversionByteCapacity), 0);
        }
        callbackBuffer_.assign(
                static_cast<std::size_t>(bufferFrames_) * outputBytesPerFrame_, 0);
    }

    bufferInfos_[0] = {};
    bufferInfos_[0].isInput = ASIOFalse;
    bufferInfos_[0].channelNum = 0;
    bufferInfos_[1] = {};
    bufferInfos_[1].isInput = ASIOFalse;
    bufferInfos_[1].channelNum = 1;

    callbacks_ = {};
    callbacks_.bufferSwitch = AsioBufferSwitch;
    callbacks_.sampleRateDidChange = AsioSampleRateDidChange;
    callbacks_.asioMessage = AsioMessage;
    callbacks_.bufferSwitchTimeInfo = AsioBufferSwitchTimeInfo;

    g_activeRenderer.store(this, std::memory_order_release);
    result = asioDriver_->createBuffers(bufferInfos_, 2, bufferFrames_, &callbacks_);
    if (!IsAsioSuccess(result)) {
        g_activeRenderer.store(nullptr, std::memory_order_release);
        if (outError != nullptr) {
            *outError = AsioErrorMessage(asioDriver_, result, L"IASIO::createBuffers");
        }
        return false;
    }
    buffersCreated_ = true;
    for (const auto& bufferInfo : bufferInfos_) {
        if (bufferInfo.buffers[0] == nullptr || bufferInfo.buffers[1] == nullptr) {
            if (outError != nullptr) {
                *outError = L"ASIO driver returned a null output buffer.";
            }
            return false;
        }
    }
    RollbackOutputPages();
    FillOutputBufferWithSilence(0);
    FillOutputBufferWithSilence(1);

    if (HasFault()) {
        if (outError != nullptr) {
            *outError = FaultMessage();
        }
        running_.store(false, std::memory_order_release);
        return false;
    }
    preparedStartPending_.store(preparedStart, std::memory_order_release);
    preparedOutputActive_.store(!preparedStart, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    startAccepted_.store(false, std::memory_order_release);
    startAcceptedAtMs_.store(0, std::memory_order_release);
    deferredSilentStartAtMs_.store(0, std::memory_order_release);
    ResetFirstOutputCallbackGate();
    capturedDrainActive_.store(false, std::memory_order_release);
    SetPrebuffering(false,
                    PrebufferTransitionReason::InitialFill,
                    ringBuffer_.PendingFrames());
    return true;
}

void AsioRenderer::DisposeBuffersOnControlThread() {
    if (g_activeRenderer.load(std::memory_order_acquire) == this) {
        g_activeRenderer.store(nullptr, std::memory_order_release);
    }
    if (buffersCreated_ && asioDriver_ != nullptr) {
        asioDriver_->disposeBuffers();
        buffersCreated_ = false;
    }
}

void AsioRenderer::CloseDriverOnControlThread() {
    DisposeBuffersOnControlThread();
    if (asioDriver_ != nullptr) {
        asioDriver_->Release();
        asioDriver_ = nullptr;
    }
    if (coInitialized_) {
        CoUninitialize();
        coInitialized_ = false;
    }
}

bool AsioRenderer::TryStartStreamIfReady(std::wstring* outError) {
    if (faultRequested_.load(std::memory_order_acquire)) {
        if (outError != nullptr) {
            const std::wstring fault = FaultMessage();
            *outError = !fault.empty()
                    ? fault
                    : L"ASIO renderer is entering a faulted state.";
        }
        return false;
    }
    if (preparedStartPending_.load(std::memory_order_acquire)) {
        if (outError != nullptr) {
            *outError = L"The prepared handoff must be committed before the ASIO stream can start.";
        }
        return false;
    }
    if (startAccepted_.load(std::memory_order_acquire)) {
        return true;
    }
    if (!running_.load(std::memory_order_acquire)) {
        if (outError != nullptr) {
            *outError = L"ASIO renderer is not running.";
        }
        return false;
    }

    std::unique_lock<std::mutex> lock(controlMutex_);
    if (shutdownRequested_) {
        if (outError != nullptr) {
            *outError = L"ASIO renderer is stopping.";
        }
        return false;
    }
    if (startAccepted_.load(std::memory_order_acquire)) {
        return true;
    }

    const std::uint64_t serial = ++startRequestSerial_;
    controlCv_.notify_all();
    startCv_.wait(lock, [this, serial] {
        return shutdownRequested_ ||
               faultRequested_.load(std::memory_order_acquire) ||
               startHandledSerial_ >= serial;
    });
    if (faultRequested_.load(std::memory_order_acquire)) {
        if (outError != nullptr) {
            const std::wstring fault = FaultMessage();
            *outError = !fault.empty()
                    ? fault
                    : L"ASIO renderer is entering a faulted state.";
        }
        return false;
    }
    if (shutdownRequested_ && startHandledSerial_ < serial) {
        if (outError != nullptr) {
            *outError = L"ASIO renderer stopped before stream start.";
        }
        return false;
    }
    if (!lastStartSucceeded_) {
        if (outError != nullptr) {
            *outError = lastStartError_.empty() ? L"ASIO stream failed to start." : lastStartError_;
        }
        return false;
    }
    return true;
}

bool AsioRenderer::EnsureStreamStarted(std::wstring* outError) {
    deferredSilentStartAtMs_.store(0, std::memory_order_release);
    return TryStartStreamIfReady(outError);
}

bool AsioRenderer::StartDeferredSilenceIfDue(std::uint32_t delayMs,
                                             std::wstring* outError) {
    if (startAccepted_.load(std::memory_order_acquire)) {
        deferredSilentStartAtMs_.store(0, std::memory_order_release);
        return true;
    }
    const std::uint64_t deferredAt =
            deferredSilentStartAtMs_.load(std::memory_order_acquire);
    if (deferredAt == 0 || GetTickCount64() - deferredAt < delayMs) {
        return true;
    }
    return EnsureStreamStarted(outError);
}

bool AsioRenderer::IsDeferredSilenceStartDue(std::uint32_t delayMs) const {
    if (!running_.load(std::memory_order_acquire) ||
        startAccepted_.load(std::memory_order_acquire)) {
        return false;
    }
    const std::uint64_t deferredAt =
            deferredSilentStartAtMs_.load(std::memory_order_acquire);
    return deferredAt != 0 && GetTickCount64() - deferredAt >= delayMs;
}

bool AsioRenderer::WaitForFirstOutputCallback(std::wstring* outError) {
    if (!startAccepted_.load(std::memory_order_acquire)) {
        if (outError != nullptr) {
            *outError = L"ASIO stream start has not been accepted by the driver.";
        }
        return false;
    }
    const std::uint64_t startAcceptedAt =
            startAcceptedAtMs_.load(std::memory_order_acquire);
    if (startAcceptedAt == 0) {
        if (outError != nullptr) {
            *outError = L"ASIO stream start timestamp is unavailable.";
        }
        return false;
    }
    const ULONGLONG deadline = startAcceptedAt + kFirstOutputCallbackTimeoutMs;
    const LONG awaiting = static_cast<LONG>(FirstOutputCallbackState::Awaiting);
    const LONG completed = static_cast<LONG>(FirstOutputCallbackState::Completed);
    const LONG cancelled = static_cast<LONG>(FirstOutputCallbackState::Cancelled);
    for (;;) {
        if (faultRequested_.load(std::memory_order_acquire)) {
            if (outError != nullptr) {
                const std::wstring fault = FaultMessage();
                *outError = !fault.empty()
                        ? fault
                        : L"ASIO renderer faulted before its first output callback completed.";
            }
            return false;
        }
        if (!running_.load(std::memory_order_acquire)) {
            if (outError != nullptr) {
                *outError = L"ASIO renderer stopped before its first output callback completed.";
            }
            return false;
        }
        const LONG callbackState = InterlockedCompareExchange(
                &firstOutputCallbackState_, awaiting, awaiting);
        if (callbackState == completed) {
            return true;
        }
        if (callbackState == cancelled) {
            if (outError != nullptr) {
                *outError = L"ASIO output stopped before its first callback completed.";
            }
            return false;
        }

        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            if (outError != nullptr) {
                *outError = L"IASIO::start succeeded, but the ASIO driver did not complete its first output callback within 2000 ms; retained audio was not reassigned to another stream.";
            }
            return false;
        }

        const ULONGLONG remaining = deadline - now;
        const DWORD waitMs = static_cast<DWORD>(
                (std::min<ULONGLONG>)(remaining, MAXDWORD));
        LONG waiting = awaiting;
        WaitOnAddress(&firstOutputCallbackState_,
                      &waiting,
                      sizeof(waiting),
                      waitMs);
    }
}

bool AsioRenderer::HasFirstOutputCallbackTimedOut() const {
    if (!running_.load(std::memory_order_acquire) ||
        !startAccepted_.load(std::memory_order_acquire)) {
        return false;
    }
    const LONG awaiting = static_cast<LONG>(FirstOutputCallbackState::Awaiting);
    if (InterlockedCompareExchange(
                const_cast<LONG*>(&firstOutputCallbackState_), awaiting, awaiting) !=
        awaiting) {
        return false;
    }
    const std::uint64_t startAcceptedAt =
            startAcceptedAtMs_.load(std::memory_order_acquire);
    return startAcceptedAt != 0 &&
           GetTickCount64() - startAcceptedAt >= kFirstOutputCallbackTimeoutMs;
}

void AsioRenderer::ResetFirstOutputCallbackGate() noexcept {
    InterlockedExchange(
            &firstOutputCallbackState_,
            static_cast<LONG>(FirstOutputCallbackState::Awaiting));
}

void AsioRenderer::CancelFirstOutputCallbackGate() noexcept {
    const LONG awaiting = static_cast<LONG>(FirstOutputCallbackState::Awaiting);
    const LONG cancelled = static_cast<LONG>(FirstOutputCallbackState::Cancelled);
    if (InterlockedExchange(&firstOutputCallbackState_, cancelled) == awaiting) {
        WakeByAddressAll(const_cast<LONG*>(&firstOutputCallbackState_));
    }
}

std::uint32_t AsioRenderer::CancelQueuedCompensationBridge(
        bool countAsReplaced) noexcept {
    for (std::uint32_t attempt = 0; attempt < 16; ++attempt) {
        std::uint64_t state =
                compensationBridgeState_.load(std::memory_order_acquire);
        const std::uint32_t queued = bridge::UnpackState(state).queuedFrames;
        if (queued == 0) {
            return 0;
        }
        const std::uint64_t desired = bridge::CancelAllQueued(state);
        if (compensationBridgeState_.compare_exchange_strong(
                    state,
                    desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
            if (countAsReplaced) {
                totalBridgeSilentFramesReplaced_.fetch_add(
                        queued, std::memory_order_relaxed);
            }
            return queued;
        }
    }
    // The callback never spins indefinitely. BeginDrain's serial makes a
    // contended cancellation sticky and the next callback retries it.
    return 0;
}

bool AsioRenderer::RetireCompensationBridgeForCallback(
        std::uint32_t frameCount) noexcept {
    if (frameCount == 0) {
        return true;
    }
    for (std::uint32_t attempt = 0; attempt < 16; ++attempt) {
        std::uint64_t state =
                compensationBridgeState_.load(std::memory_order_acquire);
        const std::uint32_t inFlight =
                bridge::UnpackState(state).inFlightFrames;
        if (frameCount > inFlight) {
            return false;
        }
        std::uint64_t desired = 0;
        if (!bridge::TryConfirmInFlight(state, frameCount, &desired)) {
            return false;
        }
        if (compensationBridgeState_.compare_exchange_strong(
                    state,
                    desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

std::uint32_t AsioRenderer::ClaimCompensationBridgeForCallback(
        std::uint32_t maximumFrames) noexcept {
    if (maximumFrames == 0) {
        return 0;
    }
    for (std::uint32_t attempt = 0; attempt < 16; ++attempt) {
        std::uint64_t state =
                compensationBridgeState_.load(std::memory_order_acquire);
        const auto unpacked = bridge::UnpackState(state);
        const std::uint32_t queued = unpacked.queuedFrames;
        const std::uint32_t inFlight = unpacked.inFlightFrames;
        const std::uint32_t claimed = (std::min)(queued, maximumFrames);
        if (claimed == 0 ||
            claimed > (std::numeric_limits<std::uint32_t>::max)() - inFlight) {
            return 0;
        }
        std::uint64_t desired = 0;
        if (!bridge::TryMoveQueuedToInFlight(state, claimed, &desired)) {
            return 0;
        }
        if (compensationBridgeState_.compare_exchange_strong(
                    state,
                    desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
            return claimed;
        }
    }
    return 0;
}

void AsioRenderer::ReconcileCompensationBridgeForCallback(
        bool draining) noexcept {
    const std::uint64_t cancelSerial =
            compensationBridgeCancelSerial_.load(std::memory_order_acquire);
    if (cancelSerial != observedCompensationBridgeCancelSerial_) {
        CancelQueuedCompensationBridge(false);
        if (bridge::UnpackState(
                    compensationBridgeState_.load(
                            std::memory_order_acquire)).queuedFrames != 0) {
            return;
        }
        observedCompensationBridgeCancelSerial_ = cancelSerial;
    }
    if (!bridge::MayPublishQueued(
                running_.load(std::memory_order_acquire),
                preparedOutputActive_.load(std::memory_order_acquire),
                draining,
                minimumTimelineFrames_,
                cancelSerial,
                cancelSerial)) {
        CancelQueuedCompensationBridge(false);
        return;
    }

    std::uint64_t state =
            compensationBridgeState_.load(std::memory_order_acquire);
    const bool compensationGenerationCurrent =
            observedCompensationBridgeCancelSerial_ ==
            compensationBridgeCancelSerial_.load(std::memory_order_acquire);
    std::uint32_t queued = draining || !compensationGenerationCurrent
            ? 0
            : bridge::UnpackState(state).queuedFrames;
    const std::uint64_t barrier =
            compensationBridgeBarrierFrame_.load(std::memory_order_relaxed);
    const std::uint64_t writeFrame = ringBuffer_.WritePositionFrames();
    const std::uint64_t physicalPending = ringBuffer_.PendingFrames();

    if (queued != 0) {
        const std::uint64_t trailingPhysical = writeFrame > barrier
                ? writeFrame - barrier
                : 0;
        // Replace the whole queued placeholder only once. Requiring both a
        // complete trailing block and the post-replacement low-water floor
        // prevents one physical frame from being counted repeatedly and keeps
        // the DAC safety reserve intact.
        if (bridge::CanReplaceWholeQueued(
                    minimumTimelineFrames_,
                    physicalPending,
                    trailingPhysical,
                    state)) {
            CancelQueuedCompensationBridge(true);
            state = compensationBridgeState_.load(std::memory_order_acquire);
            queued = bridge::UnpackState(state).queuedFrames;
        }
    }

    // Every confirmed callback page reduces the future timeline. Extend an
    // existing placeholder at its same barrier, or establish a new barrier if
    // the previous segment has fully moved in-flight. Thus the callback—not a
    // feedback worker—restores the low-water floor after each consumption.
    const std::uint64_t candidateBarrier = queued == 0
            ? ringBuffer_.WritePositionFrames()
            : barrier;
    const std::uint64_t currentPhysicalPending = ringBuffer_.PendingFrames();
    state = compensationBridgeState_.load(std::memory_order_acquire);
    queued = bridge::UnpackState(state).queuedFrames;
    const std::uint32_t needed = bridge::RequiredQueuedFrames(
            minimumTimelineFrames_, currentPhysicalPending, state);
    if (needed == 0) {
        return;
    }

    const std::uint64_t serialBefore =
            compensationBridgeCancelSerial_.load(std::memory_order_acquire);
    if (!bridge::MayPublishQueued(
                running_.load(std::memory_order_acquire),
                preparedOutputActive_.load(std::memory_order_acquire),
                capturedDrainActive_.load(std::memory_order_acquire),
                minimumTimelineFrames_,
                cancelSerial,
                serialBefore)) {
        return;
    }

    if (queued == 0) {
        // Publish the absolute Ring boundary before publishing q. Frames
        // already visible at this point remain ahead of the placeholder;
        // later writes are trailing replacement candidates.
        compensationBridgeBarrierFrame_.store(
                candidateBarrier, std::memory_order_relaxed);
    }
    for (std::uint32_t attempt = 0; attempt < 16; ++attempt) {
        state = compensationBridgeState_.load(std::memory_order_acquire);
        std::uint64_t desired = 0;
        if (!bridge::TryAddQueued(state, needed, &desired)) {
            return;
        }
        if (compensationBridgeState_.compare_exchange_strong(
                    state,
                    desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
            totalBridgeSilentFramesQueued_.fetch_add(
                    needed, std::memory_order_relaxed);
            const std::uint64_t serialAfter =
                    compensationBridgeCancelSerial_.load(
                            std::memory_order_acquire);
            if (!bridge::MayPublishQueued(
                        running_.load(std::memory_order_acquire),
                        preparedOutputActive_.load(
                                std::memory_order_acquire),
                        capturedDrainActive_.load(
                                std::memory_order_acquire),
                        minimumTimelineFrames_,
                        serialBefore,
                        serialAfter)) {
                CancelQueuedCompensationBridge(false);
            }
            return;
        }
    }
}

bool AsioRenderer::ConfirmOutputPage(long doubleBufferIndex) {
    if (doubleBufferIndex < 0 || doubleBufferIndex > 1) {
        return false;
    }

    auto& page = outputPageLedgers_[static_cast<std::size_t>(doubleBufferIndex)];
    if (!page.valid) {
        return true;
    }
    if (page.sequence != nextConfirmSequence_) {
        RequestAsioFault(AsioFaultCode::OutputPageOrder);
        return false;
    }
    if (!ringBuffer_.ConfirmDispatch(page.dispatchEndIndex)) {
        RequestAsioFault(AsioFaultCode::OutputConfirmation);
        return false;
    }
    if (!RetireCompensationBridgeForCallback(
                page.compensationBridgeFrames)) {
        RequestAsioFault(AsioFaultCode::OutputConfirmation);
        return false;
    }

    if (page.capturedFrames > 0) {
        totalFramesPlayed_.fetch_add(page.capturedFrames, std::memory_order_release);
    }
    if (page.bridgeSilentFrames > 0) {
        totalBridgeSilentFramesPlayed_.fetch_add(
                page.bridgeSilentFrames, std::memory_order_relaxed);
    }
    if (page.underrunSilentFrames > 0) {
        underrunCount_.fetch_add(1, std::memory_order_relaxed);
    }
    RecordOutputFrames(page.outputFrames,
                       page.bridgeSilentFrames +
                               page.managedSilentFrames +
                               page.underrunSilentFrames);
    page = {};
    ++nextConfirmSequence_;
    return true;
}

void AsioRenderer::RollbackOutputPages() {
    ringBuffer_.RollbackDispatch();
    outputPageLedgers_.fill({});
    compensationBridgeState_.store(0, std::memory_order_release);
    compensationBridgeBarrierFrame_.store(
            ringBuffer_.DispatchPosition() /
                    (outputBytesPerFrame_ == 0 ? 1U : outputBytesPerFrame_),
            std::memory_order_release);
    observedCompensationBridgeCancelSerial_ =
            compensationBridgeCancelSerial_.load(std::memory_order_acquire);
    nextDispatchSequence_ = 1;
    nextConfirmSequence_ = 1;
    awaitingFirstBufferSwitch_ = true;
}

void AsioRenderer::SetPrebuffering(bool enabled,
                                   PrebufferTransitionReason reason,
                                   std::uint32_t availableFrames) {
    const bool previous = prebuffering_.exchange(enabled, std::memory_order_acq_rel);
    if (previous == enabled) {
        return;
    }

    lastPrebufferTransition_.store(
            static_cast<std::int32_t>(reason), std::memory_order_relaxed);
    lastPrebufferTransitionFrames_.store(
            availableFrames, std::memory_order_relaxed);
    if (enabled) {
        prebufferEnterCount_.fetch_add(1, std::memory_order_release);
    } else {
        prebufferExitCount_.fetch_add(1, std::memory_order_release);
    }
}

void AsioRenderer::RequestAsioFault(AsioFaultCode code,
                                    std::uint32_t detail) noexcept {
    if (code == AsioFaultCode::None) {
        return;
    }

    const std::uint64_t packedFault =
            (static_cast<std::uint64_t>(detail) << 32U) |
            static_cast<std::uint32_t>(code);
    std::uint64_t expected = 0;
    pendingAsioFault_.compare_exchange_strong(
            expected, packedFault, std::memory_order_acq_rel);

    // Driver callbacks only close the output gate and publish a compact fault
    // request. String construction, locks, callback retirement, and driver stop
    // are completed on the control thread after this callback has returned.
    faultRequested_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    CancelFirstOutputCallbackGate();
    faultStopRequested_.store(true, std::memory_order_release);
    controlCv_.notify_all();
    startCv_.notify_all();
}

void AsioRenderer::FinalizePendingAsioFault() {
    const std::uint64_t packedFault =
            pendingAsioFault_.load(std::memory_order_acquire);
    if (packedFault == 0 || faulted_.load(std::memory_order_acquire)) {
        return;
    }

    const auto code = static_cast<AsioFaultCode>(
            static_cast<std::uint32_t>(packedFault));
    const std::uint32_t detail = static_cast<std::uint32_t>(packedFault >> 32U);
    std::wstring message;
    switch (code) {
        case AsioFaultCode::SampleRateChanged: {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                asioSampleRate_ = detail;
            }
            wchar_t buffer[256]{};
            std::swprintf(buffer,
                          std::size(buffer),
                          L"ASIO sample rate changed to %u Hz while captured audio was active.",
                          detail);
            message = buffer;
            break;
        }
        case AsioFaultCode::ResetRequested:
            message = L"ASIO driver requested an output reset; exact page consumption can no longer be proven.";
            break;
        case AsioFaultCode::BufferSizeChanged:
            message = L"ASIO driver changed its buffer size while output was active; exact page consumption can no longer be proven.";
            break;
        case AsioFaultCode::ResyncRequested:
            message = L"ASIO driver reported that its stream position is out of sync.";
            break;
        case AsioFaultCode::Overload:
            message = L"ASIO driver reported an output overload.";
            break;
        case AsioFaultCode::ReenteredCallback:
            message = L"ASIO driver re-entered the output callback.";
            break;
        case AsioFaultCode::InvalidBufferIndex:
            message = L"ASIO driver returned an invalid double-buffer index.";
            break;
        case AsioFaultCode::OutputPageOrder:
            message = L"ASIO output pages were retired out of order.";
            break;
        case AsioFaultCode::OutputConfirmation:
            message = L"ASIO output confirmation did not match the retained PCM queue.";
            break;
        case AsioFaultCode::InvalidRendererBuffers:
            message = L"ASIO requested output while its renderer buffers were invalid.";
            break;
        case AsioFaultCode::None:
        default:
            message = L"ASIO renderer entered a faulted state.";
            break;
    }
    LatchFault(message);
}

bool AsioRenderer::LatchFault(const std::wstring& message) {
    if (faulted_.load(std::memory_order_acquire)) {
        return false;
    }

    faultRequested_.store(true, std::memory_order_release);

    // Closing this gate first guarantees that a callback which starts after
    // the fault request cannot retire another ledger page. A callback already
    // past the gate is allowed to finish so the published counters include its
    // last definitely consumed page.
    running_.store(false, std::memory_order_release);
    CancelFirstOutputCallbackGate();
    SetPrebuffering(false,
                    PrebufferTransitionReason::Fault,
                    ringBuffer_.AvailableReadFrames());

    callbackWaiterCount_.fetch_add(1, std::memory_order_acq_rel);
    while (InterlockedCompareExchange(&callbackExecuting_, FALSE, FALSE) != FALSE) {
        LONG executing = TRUE;
        WaitOnAddress(&callbackExecuting_, &executing, sizeof(executing), INFINITE);
    }
    callbackWaiterCount_.fetch_sub(1, std::memory_order_acq_rel);

    std::lock_guard<std::mutex> producerLock(producerMutex_);
    {
        std::lock_guard<std::mutex> lock(faultMutex_);
        if (faulted_.load(std::memory_order_relaxed)) {
            return false;
        }
        faultMessage_ = message.empty() ? L"ASIO renderer entered a faulted state." : message;
        faulted_.store(true, std::memory_order_release);
    }

    faultStopRequested_.store(true, std::memory_order_release);
    controlCv_.notify_all();
    startCv_.notify_all();
    return true;
}

void AsioRenderer::FillOutputBuffer(long doubleBufferIndex) {
    if (doubleBufferIndex < 0 || doubleBufferIndex > 1 ||
        bufferFrames_ == 0 || outputBytesPerFrame_ == 0 || callbackBuffer_.empty()) {
        FillOutputBufferWithSilence(doubleBufferIndex);
        RequestAsioFault(AsioFaultCode::InvalidRendererBuffers);
        NotifyOutputReady();
        return;
    }

    if (!preparedOutputActive_.load(std::memory_order_acquire)) {
        // IASIO::start is allowed to synchronously enter the callback before
        // returning. Until Core commits startingBank_ as Active, return a
        // normal accounted silence page without advancing the staged ring.
        FillOutputBufferWithSilence(doubleBufferIndex);
        auto& page = outputPageLedgers_[static_cast<std::size_t>(doubleBufferIndex)];
        page.valid = true;
        page.sequence = nextDispatchSequence_++;
        page.dispatchEndIndex = ringBuffer_.DispatchPosition();
        page.outputFrames = bufferFrames_;
        page.capturedFrames = 0;
        page.bridgeSilentFrames = 0;
        page.managedSilentFrames = bufferFrames_;
        page.underrunSilentFrames = 0;
        NotifyOutputReady();
        return;
    }

    const bool draining = capturedDrainActive_.load(std::memory_order_acquire);
    ReconcileCompensationBridgeForCallback(draining);
    const auto shouldKeepTentativeClaim = [this](
                                                     std::uint64_t claimSerial) {
        return bridge::ShouldKeepTentativeClaim(
                running_.load(std::memory_order_acquire),
                preparedOutputActive_.load(std::memory_order_acquire),
                capturedDrainActive_.load(std::memory_order_acquire),
                minimumTimelineFrames_,
                observedCompensationBridgeCancelSerial_,
                claimSerial,
                compensationBridgeCancelSerial_.load(
                        std::memory_order_acquire));
    };
    const auto compensationGenerationIsCurrent = [this,
                                                   &shouldKeepTentativeClaim] {
        const std::uint64_t currentSerial =
                compensationBridgeCancelSerial_.load(
                        std::memory_order_acquire);
        return shouldKeepTentativeClaim(currentSerial);
    };

    std::uint32_t outputCursor = 0;
    std::uint32_t capturedFrames = 0;
    std::uint32_t physicalBridgeFrames = 0;
    std::uint32_t compensationBridgeFrames = 0;
    std::uint64_t dispatchEndIndex = ringBuffer_.DispatchPosition();
    const auto dispatchPhysical = [&](std::uint32_t maximumFrames) {
        if (maximumFrames == 0) {
            return;
        }
        const RawFrameRingBuffer::DispatchResult dispatched =
                ringBuffer_.Dispatch(
                        callbackBuffer_.data() +
                                static_cast<std::size_t>(outputCursor) *
                                        outputBytesPerFrame_,
                        maximumFrames);
        outputCursor += dispatched.frames;
        capturedFrames += dispatched.playerFrames;
        physicalBridgeFrames += dispatched.bridgeFrames;
        dispatchEndIndex = dispatched.endIndex;
    };

    std::uint64_t state =
            compensationBridgeState_.load(std::memory_order_acquire);
    bool compensationGenerationCurrent =
            compensationGenerationIsCurrent();
    std::uint32_t queued = compensationGenerationCurrent
            ? bridge::UnpackState(state).queuedFrames
            : 0;
    if (queued != 0) {
        const std::uint64_t barrier =
                compensationBridgeBarrierFrame_.load(
                        std::memory_order_relaxed);
        const std::uint64_t dispatchFrame =
                outputBytesPerFrame_ == 0
                ? 0
                : ringBuffer_.DispatchPosition() / outputBytesPerFrame_;
        const bridge::PagePlan plan = bridge::PlanPage(
                dispatchFrame,
                ringBuffer_.WritePositionFrames(),
                barrier,
                state,
                bufferFrames_);
        if (plan.valid) {
            dispatchPhysical(plan.physicalPrefixFrames);
        } else {
            // A generation rollback or drain cancellation made this old
            // placeholder unreachable. It is safer to discard it than insert
            // silence in front of PCM that has already crossed the boundary.
            CancelQueuedCompensationBridge(false);
        }
    }

    if (outputCursor < bufferFrames_) {
        state = compensationBridgeState_.load(std::memory_order_acquire);
        compensationGenerationCurrent =
                compensationGenerationIsCurrent();
        queued = !compensationGenerationCurrent
                ? 0
                : bridge::UnpackState(state).queuedFrames;
        const std::uint64_t barrier =
                compensationBridgeBarrierFrame_.load(
                        std::memory_order_relaxed);
        const std::uint64_t dispatchFrame =
                outputBytesPerFrame_ == 0
                ? 0
                : ringBuffer_.DispatchPosition() / outputBytesPerFrame_;
        if (queued != 0 && dispatchFrame == barrier) {
            const std::uint64_t claimSerial =
                    compensationBridgeCancelSerial_.load(
                            std::memory_order_acquire);
            std::uint32_t claimed =
                    shouldKeepTentativeClaim(claimSerial)
                    ? ClaimCompensationBridgeForCallback(
                              bufferFrames_ - outputCursor)
                    : 0;
            if (claimed != 0 &&
                !shouldKeepTentativeClaim(claimSerial)) {
                // BeginDrain linearized between the live precheck and q ->
                // in-flight CAS. The bytes have not been written to the ASIO
                // page yet, so retire this tentative claim instead of letting
                // one last compensation page leak across the drain boundary.
                if (RetireCompensationBridgeForCallback(claimed)) {
                    claimed = 0;
                } else {
                    RequestAsioFault(AsioFaultCode::OutputConfirmation);
                }
            }
            if (claimed != 0) {
                std::memset(callbackBuffer_.data() +
                                    static_cast<std::size_t>(outputCursor) *
                                            outputBytesPerFrame_,
                            0,
                            static_cast<std::size_t>(claimed) *
                                    outputBytesPerFrame_);
                outputCursor += claimed;
                compensationBridgeFrames += claimed;
            }
        }
    }

    // Once the placeholder has been fully dispatched or atomically replaced,
    // any trailing player PCM is now the next ordered timeline segment and may
    // fill the remainder of this same ASIO page.
    compensationGenerationCurrent =
            compensationGenerationIsCurrent();
    if (outputCursor < bufferFrames_ &&
        (!compensationGenerationCurrent || bridge::UnpackState(
                compensationBridgeState_.load(
                        std::memory_order_acquire)).queuedFrames == 0)) {
        dispatchPhysical(bufferFrames_ - outputCursor);
    }

    const std::uint32_t underrunSilentFrames = bufferFrames_ - outputCursor;
    if (underrunSilentFrames != 0) {
        std::memset(callbackBuffer_.data() +
                            static_cast<std::size_t>(outputCursor) *
                                    outputBytesPerFrame_,
                    0,
                    static_cast<std::size_t>(underrunSilentFrames) *
                            outputBytesPerFrame_);
    }

    WriteDirectOutput(callbackBuffer_.data(), bufferFrames_, doubleBufferIndex);
    auto& page = outputPageLedgers_[static_cast<std::size_t>(doubleBufferIndex)];
    page.valid = true;
    page.sequence = nextDispatchSequence_++;
    page.dispatchEndIndex = dispatchEndIndex;
    page.outputFrames = bufferFrames_;
    page.capturedFrames = capturedFrames;
    page.bridgeSilentFrames =
            physicalBridgeFrames + compensationBridgeFrames;
    page.compensationBridgeFrames = compensationBridgeFrames;
    page.managedSilentFrames = draining ? underrunSilentFrames : 0;
    page.underrunSilentFrames = draining ? 0 : underrunSilentFrames;
    NotifyOutputReady();
}

void AsioRenderer::FillOutputBufferWithSilence(long doubleBufferIndex) {
    if (doubleBufferIndex < 0 || doubleBufferIndex > 1) {
        return;
    }
    for (auto& bufferInfo : bufferInfos_) {
        if (bufferInfo.buffers[doubleBufferIndex] != nullptr && bufferFrames_ > 0) {
            const std::uint32_t bytesPerSample = outputBytesPerSample_ != 0
                    ? outputBytesPerSample_
                    : 4U;
            std::memset(bufferInfo.buffers[doubleBufferIndex],
                        0,
                        static_cast<std::size_t>(bufferFrames_) * bytesPerSample);
        }
    }
}

void AsioRenderer::NotifyOutputReady() {
    const std::int32_t state = outputReadyState_.load(std::memory_order_relaxed);
    if (asioDriver_ == nullptr || state < 0) {
        return;
    }

    const ASIOError result = asioDriver_->outputReady();
    if (state == 0) {
        outputReadyState_.store(IsAsioSuccess(result) ? 1 : -1,
                                std::memory_order_release);
    }
}

void AsioRenderer::WriteDirectOutput(const std::uint8_t* interleaved,
                                     std::uint32_t frameCount,
                                     long doubleBufferIndex) {
    if (interleaved == nullptr || doubleBufferIndex < 0 || doubleBufferIndex > 1) {
        return;
    }

    auto* left = static_cast<std::uint8_t*>(bufferInfos_[0].buffers[doubleBufferIndex]);
    auto* right = static_cast<std::uint8_t*>(bufferInfos_[1].buffers[doubleBufferIndex]);
    if (left == nullptr || right == nullptr) {
        return;
    }

    switch (outputBytesPerSample_) {
        case 2:
            DeinterleaveStereo<2>(interleaved, left, right, frameCount);
            break;
        case 3:
            DeinterleaveStereo<3>(interleaved, left, right, frameCount);
            break;
        case 4:
            DeinterleaveStereo<4>(interleaved, left, right, frameCount);
            break;
        default:
            break;
    }
}

void AsioRenderer::ResetStats() {
    {
        std::lock_guard<std::mutex> lock(faultMutex_);
        faultMessage_.clear();
        faulted_.store(false, std::memory_order_release);
    }
    pendingAsioFault_.store(0, std::memory_order_release);
    faultRequested_.store(false, std::memory_order_release);
    faultStopRequested_.store(false, std::memory_order_release);
    preparedStartPending_.store(false, std::memory_order_release);
    preparedOutputActive_.store(false, std::memory_order_release);
    startAcceptedAtMs_.store(0, std::memory_order_release);
    deferredSilentStartAtMs_.store(0, std::memory_order_release);
    ResetFirstOutputCallbackGate();
    totalFramesQueued_.store(0, std::memory_order_relaxed);
    totalPlayerSilentFrames_.store(0, std::memory_order_relaxed);
    totalFramesPlayed_.store(0, std::memory_order_relaxed);
    totalFramesDropped_.store(0, std::memory_order_relaxed);
    totalOutputFrames_.store(0, std::memory_order_relaxed);
    totalSilentFrames_.store(0, std::memory_order_relaxed);
    totalLogicalFrames_.store(0, std::memory_order_relaxed);
    totalBridgeSilentFramesQueued_.store(0, std::memory_order_relaxed);
    totalBridgeSilentFramesPlayed_.store(0, std::memory_order_relaxed);
    totalBridgeSilentFramesReplaced_.store(0, std::memory_order_relaxed);
    compensationBridgeState_.store(0, std::memory_order_relaxed);
    compensationStateSequence_.store(0, std::memory_order_relaxed);
    compensationBridgeCancelSerial_.store(0, std::memory_order_relaxed);
    compensationBridgeBarrierFrame_.store(0, std::memory_order_relaxed);
    observedCompensationBridgeCancelSerial_ = 0;
    maximumRealPacketFrames_.store(0, std::memory_order_relaxed);
    underrunCount_.store(0, std::memory_order_relaxed);
    prebufferEnterCount_.store(0, std::memory_order_relaxed);
    prebufferExitCount_.store(0, std::memory_order_relaxed);
    lastPrebufferTransition_.store(
            static_cast<std::int32_t>(PrebufferTransitionReason::None),
            std::memory_order_relaxed);
    lastPrebufferTransitionFrames_.store(0, std::memory_order_relaxed);
    asioResetRequests_.store(0, std::memory_order_relaxed);
    asioBufferSizeChanges_.store(0, std::memory_order_relaxed);
    asioLatencyChanges_.store(0, std::memory_order_relaxed);
    asioRebuildCount_.store(0, std::memory_order_relaxed);
    asioLastMessage_.store(0, std::memory_order_relaxed);
    callbackRealtimeMode_.store(0, std::memory_order_relaxed);
    outputReadyState_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(silenceStatsMutex_);
        silenceBuckets_.fill({});
        sampledOutputFrames_ = 0;
        sampledSilentFrames_ = 0;
    }
}

void AsioRenderer::RecordOutputFrames(std::uint32_t outputFrames, std::uint32_t silentFrames) {
    if (outputFrames == 0) {
        return;
    }

    totalOutputFrames_.fetch_add(outputFrames, std::memory_order_relaxed);
    totalSilentFrames_.fetch_add(silentFrames, std::memory_order_relaxed);
}

AsioRenderer::SilenceWindowStats AsioRenderer::GetRecentSilenceStats(
        std::int64_t totalOutputFrames,
        std::int64_t totalSilentFrames) const {
    SilenceWindowStats stats{};
    const std::uint64_t second = NowSecond();
    std::lock_guard<std::mutex> lock(silenceStatsMutex_);

    if (totalOutputFrames < sampledOutputFrames_ ||
        totalSilentFrames < sampledSilentFrames_) {
        silenceBuckets_.fill({});
        sampledOutputFrames_ = 0;
        sampledSilentFrames_ = 0;
    }

    auto& current = silenceBuckets_[
            static_cast<std::size_t>(second % silenceBuckets_.size())];
    if (current.second != second) {
        current = {};
        current.second = second;
    }
    current.outputFrames += totalOutputFrames - sampledOutputFrames_;
    current.silentFrames += totalSilentFrames - sampledSilentFrames_;
    sampledOutputFrames_ = totalOutputFrames;
    sampledSilentFrames_ = totalSilentFrames;

    for (const auto& bucket : silenceBuckets_) {
        if (bucket.second != 0 && bucket.second + silenceBuckets_.size() > second) {
            stats.outputFrames += bucket.outputFrames;
            stats.silentFrames += bucket.silentFrames;
        }
    }
    if (stats.outputFrames > 0) {
        stats.silentPercent =
                static_cast<double>(stats.silentFrames) * 100.0 /
                static_cast<double>(stats.outputFrames);
    }
    return stats;
}

}  // namespace tickbytick

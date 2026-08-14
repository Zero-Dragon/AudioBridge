#include "AsioRenderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>

namespace audiobridge {
namespace {

std::atomic<AsioRenderer*> g_activeRenderer{nullptr};
std::atomic<bool> g_asioDriverPoisoned{false};
std::atomic<std::uint32_t> g_asioCallbackEntrants{0};

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
        renderer->OnAsioBufferSwitch(doubleBufferIndex);
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
        renderer->OnAsioBufferSwitch(doubleBufferIndex);
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
    if (IsFloatSubformat(format) && wave.wBitsPerSample == 32 && bytesPerSample == 4) {
        return AsioRenderer::SourceSampleKind::Float32;
    }
    if (!IsPcmSubformat(format)) {
        return AsioRenderer::SourceSampleKind::Unknown;
    }
    if (wave.wBitsPerSample <= 16 && bytesPerSample == 2) {
        return AsioRenderer::SourceSampleKind::Pcm16;
    }
    if (wave.wBitsPerSample <= 24 && bytesPerSample == 3) {
        return AsioRenderer::SourceSampleKind::Pcm24;
    }
    if (wave.wBitsPerSample <= 32 && bytesPerSample == 4) {
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

std::uint32_t NormalizeMaxBufferOffsetMs(std::int32_t maxBufferOffsetMs) {
    if (maxBufferOffsetMs < 50) {
        return 100;
    }
    return (std::min<std::uint32_t>)(static_cast<std::uint32_t>(maxBufferOffsetMs), 10000U);
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

    std::wstring result(static_cast<std::size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, value, -1, result.data(), required);
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

bool OutputKindFromAsio(ASIOSampleType sampleType,
                        AsioRenderer::OutputSampleKind* outKind,
                        std::uint32_t* outShift) {
    if (outKind == nullptr || outShift == nullptr) {
        return false;
    }

    *outShift = 0;
    switch (sampleType) {
        case ASIOSTFloat32LSB:
            *outKind = AsioRenderer::OutputSampleKind::Float32;
            return true;
        case ASIOSTInt16LSB:
            *outKind = AsioRenderer::OutputSampleKind::Int16;
            return true;
        case ASIOSTInt24LSB:
            *outKind = AsioRenderer::OutputSampleKind::Int24;
            return true;
        case ASIOSTInt32LSB:
            *outKind = AsioRenderer::OutputSampleKind::Int32;
            return true;
        case ASIOSTInt32LSB16:
            *outKind = AsioRenderer::OutputSampleKind::Int32;
            *outShift = 16;
            return true;
        case ASIOSTInt32LSB18:
            *outKind = AsioRenderer::OutputSampleKind::Int32;
            *outShift = 14;
            return true;
        case ASIOSTInt32LSB20:
            *outKind = AsioRenderer::OutputSampleKind::Int32;
            *outShift = 12;
            return true;
        case ASIOSTInt32LSB24:
            *outKind = AsioRenderer::OutputSampleKind::Int32;
            *outShift = 8;
            return true;
        default:
            *outKind = AsioRenderer::OutputSampleKind::Unknown;
            return false;
    }
}

std::int32_t FloatSampleToPcm32(float sample) {
    if (sample >= 1.0f) {
        return (std::numeric_limits<std::int32_t>::max)();
    }
    if (sample <= -1.0f) {
        return (std::numeric_limits<std::int32_t>::min)();
    }
    const double scaled = static_cast<double>(sample) * 2147483647.0;
    return static_cast<std::int32_t>(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
}

std::int32_t ReadPcm24Sample(const std::uint8_t* sample) {
    std::int32_t value = static_cast<std::int32_t>(sample[0]) |
                         (static_cast<std::int32_t>(sample[1]) << 8U) |
                         (static_cast<std::int32_t>(sample[2]) << 16U);
    if ((value & 0x00800000) != 0) {
        value |= static_cast<std::int32_t>(0xFF000000);
    }
    return value << 8U;
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

void WriteInt16Sample(std::uint8_t* destination, std::uint32_t frameIndex, std::int32_t value) {
    const auto sample = static_cast<std::int16_t>(value >> 16U);
    std::memcpy(destination + static_cast<std::size_t>(frameIndex) * sizeof(sample),
                &sample,
                sizeof(sample));
}

void WriteInt24Sample(std::uint8_t* destination, std::uint32_t frameIndex, std::int32_t value) {
    const auto sample = static_cast<std::uint32_t>(value >> 8U);
    std::uint8_t* out = destination + static_cast<std::size_t>(frameIndex) * 3U;
    out[0] = static_cast<std::uint8_t>(sample & 0xFFU);
    out[1] = static_cast<std::uint8_t>((sample >> 8U) & 0xFFU);
    out[2] = static_cast<std::uint8_t>((sample >> 16U) & 0xFFU);
}

void WriteInt32Sample(std::uint8_t* destination,
                      std::uint32_t frameIndex,
                      std::int32_t value,
                      std::uint32_t rightShift) {
    const auto sample = static_cast<std::int32_t>(value >> rightShift);
    std::memcpy(destination + static_cast<std::size_t>(frameIndex) * sizeof(sample),
                &sample,
                sizeof(sample));
}

void WriteFloat32Sample(std::uint8_t* destination, std::uint32_t frameIndex, float value) {
    std::memcpy(destination + static_cast<std::size_t>(frameIndex) * sizeof(value),
                &value,
                sizeof(value));
}

}  // namespace

bool RawFrameRingBuffer::Reset(std::uint32_t bytesPerFrame, std::uint32_t capacityFrames) {
    if (bytesPerFrame == 0 || capacityFrames == 0) {
        return false;
    }

    bytesPerFrame_ = bytesPerFrame;
    const std::size_t requestedBytes =
            static_cast<std::size_t>(bytesPerFrame) * static_cast<std::size_t>(capacityFrames);
    const std::size_t capacityBytes = NextPowerOfTwo(requestedBytes);
    bytes_.assign(capacityBytes, 0);
    frameOrigins_.assign(capacityBytes / bytesPerFrame, FrameOrigin::Captured);
    byteMask_ = capacityBytes - 1;
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
    std::fill(frameOrigins_.begin(), frameOrigins_.end(), FrameOrigin::Captured);
}

std::uint32_t RawFrameRingBuffer::Push(const std::uint8_t* data,
                                       std::uint32_t frameCount,
                                       FrameOrigin origin) {
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
    CopyOriginsInto(writeIndex, frameCount, origin);
    writeIndex_.store(writeIndex + requestedBytes, std::memory_order_release);
    return frameCount;
}

std::uint32_t RawFrameRingBuffer::PushSilence(std::uint32_t frameCount) {
    if (frameCount == 0 || bytesPerFrame_ == 0 || bytes_.empty()) {
        return 0;
    }

    const std::uint64_t readIndex = confirmedReadIndex_.load(std::memory_order_acquire);
    const std::uint64_t writeIndex = writeIndex_.load(std::memory_order_relaxed);
    const std::size_t requestedBytes =
            static_cast<std::size_t>(frameCount) * static_cast<std::size_t>(bytesPerFrame_);
    const std::size_t writableBytes =
            (std::min)(requestedBytes, AvailableWriteBytes(readIndex, writeIndex)) /
            bytesPerFrame_ * bytesPerFrame_;
    if (writableBytes == 0) {
        return 0;
    }

    const auto writableFrames = static_cast<std::uint32_t>(writableBytes / bytesPerFrame_);
    ZeroInto(writeIndex, writableBytes);
    CopyOriginsInto(writeIndex, writableFrames, FrameOrigin::PaddingSilence);
    writeIndex_.store(writeIndex + writableBytes, std::memory_order_release);
    return writableFrames;
}

std::uint32_t RawFrameRingBuffer::PushCapturedSilence(std::uint32_t frameCount) {
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
    CopyOriginsInto(writeIndex, frameCount, FrameOrigin::Captured);
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
    result.paddingSilentFrames = CountPaddingOrigins(readIndex, result.frames);
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

void RawFrameRingBuffer::CopyOriginsInto(std::uint64_t writeIndex,
                                         std::uint32_t frameCount,
                                         FrameOrigin origin) {
    if (frameCount == 0 || bytesPerFrame_ == 0 || frameOrigins_.empty()) {
        return;
    }
    const std::uint64_t frameIndex = writeIndex / bytesPerFrame_;
    const std::size_t offset = static_cast<std::size_t>(
            frameIndex % static_cast<std::uint64_t>(frameOrigins_.size()));
    const std::size_t firstChunk =
            (std::min)(static_cast<std::size_t>(frameCount), frameOrigins_.size() - offset);
    std::fill_n(frameOrigins_.begin() + static_cast<std::ptrdiff_t>(offset), firstChunk, origin);
    if (static_cast<std::size_t>(frameCount) > firstChunk) {
        std::fill_n(frameOrigins_.begin(),
                    static_cast<std::size_t>(frameCount) - firstChunk,
                    origin);
    }
}

std::uint32_t RawFrameRingBuffer::CountPaddingOrigins(std::uint64_t readIndex,
                                                       std::uint32_t frameCount) const {
    if (frameCount == 0 || bytesPerFrame_ == 0 || frameOrigins_.empty()) {
        return 0;
    }
    const std::uint64_t frameIndex = readIndex / bytesPerFrame_;
    const std::size_t offset = static_cast<std::size_t>(
            frameIndex % static_cast<std::uint64_t>(frameOrigins_.size()));
    const std::size_t firstChunk =
            (std::min)(static_cast<std::size_t>(frameCount), frameOrigins_.size() - offset);
    std::uint32_t paddingFrames = static_cast<std::uint32_t>(
            std::count(frameOrigins_.begin() + static_cast<std::ptrdiff_t>(offset),
                       frameOrigins_.begin() + static_cast<std::ptrdiff_t>(offset + firstChunk),
                       FrameOrigin::PaddingSilence));
    if (static_cast<std::size_t>(frameCount) > firstChunk) {
        paddingFrames += static_cast<std::uint32_t>(
                std::count(frameOrigins_.begin(),
                           frameOrigins_.begin() +
                                   static_cast<std::ptrdiff_t>(frameCount - firstChunk),
                           FrameOrigin::PaddingSilence));
    }
    return paddingFrames;
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
                         std::int32_t maxBufferOffsetMs,
                         std::uint32_t requestedBufferFrames,
                         std::wstring* outError) {
    Stop();
    if ((faultRequested_.load(std::memory_order_acquire) &&
         !faulted_.load(std::memory_order_acquire)) ||
        g_asioDriverPoisoned.load(std::memory_order_acquire)) {
        if (outError != nullptr) {
            const std::wstring fault = FaultMessage();
            *outError = !fault.empty()
                    ? fault
                    : L"The previous ASIO stream is still faulting; restart AudioBridge if it cannot be stopped safely.";
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ResetStats();
        format_ = format;
        bytesPerFrame_ = BytesPerFrame(format_.Format);
        sampleRate_ = format_.Format.nSamplesPerSec;
        asioSampleRate_ = sampleRate_;
        sourceChannels_ = format_.Format.nChannels;
        sourceBytesPerSample_ =
                sourceChannels_ == 0 ? 0 : bytesPerFrame_ / sourceChannels_;
        sourceKind_ = SourceKindFromWave(format_);
        prebufferMs_ = static_cast<std::int32_t>(NormalizePrebufferMs(prebufferMs));
        prebufferFrames_ = FramesFromMs(sampleRate_, static_cast<std::uint32_t>(prebufferMs_));
        maxBufferOffsetMs_ =
                static_cast<std::int32_t>(NormalizeMaxBufferOffsetMs(maxBufferOffsetMs));
        maxBufferOffsetFrames_ =
                FramesFromMs(sampleRate_, static_cast<std::uint32_t>(maxBufferOffsetMs_));
        requestedBufferFrames_ = requestedBufferFrames;
        minBufferFrames_ = 0;
        maxBufferFrames_ = 0;
        preferredBufferFrames_ = 0;
        bufferGranularity_ = 0;

        if (bytesPerFrame_ == 0 || sampleRate_ == 0 ||
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
        controlThread_ =
                std::thread(&AsioRenderer::ControlLoop, this, deviceId, requestedBufferFrames);
    } catch (const std::exception&) {
        if (outError != nullptr) {
            *outError = L"Failed to start ASIO control thread.";
        }
        return false;
    }

    std::unique_lock<std::mutex> lock(controlMutex_);
    initCv_.wait(lock, [this] { return initComplete_; });
    if (!initSucceeded_) {
        const std::wstring error = initError_;
        lock.unlock();
        Stop();
        if (outError != nullptr) {
            *outError = error.empty() ? L"Failed to initialize ASIO renderer." : error;
        }
        return false;
    }
    lock.unlock();

    try {
        paddingThread_ = std::thread(&AsioRenderer::PaddingLoop, this);
    } catch (const std::exception&) {
        Stop();
        if (outError != nullptr) {
            *outError = L"Failed to start buffer padding thread.";
        }
        return false;
    }

    // Prime only the retained tagged-silence queue. Both physical ASIO pages
    // remain zeroed until the driver requests a page in its first callback.
    MaintainPadding();

    std::wstring startError;
    if (!TryStartStreamIfReady(&startError)) {
        Stop();
        if (outError != nullptr) {
            *outError = startError;
        }
        return false;
    }
    return true;
}

void AsioRenderer::Stop() {
    running_.store(false, std::memory_order_release);
    paddingCv_.notify_all();
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        shutdownRequested_ = true;
    }
    controlCv_.notify_all();
    startCv_.notify_all();
    initCv_.notify_all();

    if (paddingThread_.joinable()) {
        paddingThread_.join();
    }
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
    {
        std::lock_guard<std::mutex> callbackLock(callbackWaitMutex_);
        callbackExecuting_.store(false, std::memory_order_release);
    }
    callbackIdleCv_.notify_all();
    callbackActive_.clear(std::memory_order_release);
    streamActive_.store(false, std::memory_order_release);
    prebuffering_.store(false, std::memory_order_release);
    ringBuffer_.Clear();
    callbackBuffer_.clear();
    bufferFrames_ = 0;
    requestedBufferFrames_ = 0;
    minBufferFrames_ = 0;
    maxBufferFrames_ = 0;
    preferredBufferFrames_ = 0;
    bufferGranularity_ = 0;
    asioSampleRate_ = 0;
    outputKind_ = OutputSampleKind::Unknown;
    outputAsioSampleType_ = ASIOSTLastEntry;
    outputRightShift_ = 0;
    paddingActive_ = false;
}

std::uint32_t AsioRenderer::PushPcm(const std::uint8_t* data,
                                    std::uint32_t frameCount,
                                    std::wstring* outError) {
    return PushCapturedFrames(data, frameCount, false, outError);
}

std::uint32_t AsioRenderer::PushCapturedSilence(std::uint32_t frameCount,
                                                std::wstring* outError) {
    return PushCapturedFrames(nullptr, frameCount, true, outError);
}

std::uint32_t AsioRenderer::PushCapturedFrames(const std::uint8_t* data,
                                               std::uint32_t frameCount,
                                               bool silence,
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
        paddingActive_ = false;
        written = silence
                ? ringBuffer_.PushCapturedSilence(frameCount)
                : ringBuffer_.Push(data, frameCount);
        totalFramesQueued_.fetch_add(written, std::memory_order_relaxed);
        if (written < frameCount) {
            totalFramesDropped_.fetch_add(frameCount - written, std::memory_order_relaxed);
        }
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
    paddingCv_.notify_one();
    if (written > 0 && !TryStartStreamIfReady(outError)) {
        return written;
    }
    return written;
}

RendererStats AsioRenderer::GetStats() const {
    RendererStats stats{};
    const auto recent = GetRecentSilenceStats();
    std::int64_t bufferedFrames = 0;
    std::int64_t capacityFrames = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t asioSampleRate = 0;
    std::int32_t prebufferMs = 0;
    std::uint32_t prebufferFrames = 0;
    std::uint32_t requestedBufferFrames = 0;
    std::uint32_t actualBufferFrames = 0;
    std::uint32_t minBufferFrames = 0;
    std::uint32_t maxBufferFrames = 0;
    std::uint32_t preferredBufferFrames = 0;
    long bufferGranularity = 0;
    ASIOSampleType outputSampleType = ASIOSTLastEntry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bufferedFrames = static_cast<std::int64_t>(ringBuffer_.PendingFrames());
        capacityFrames = static_cast<std::int64_t>(ringBuffer_.CapacityFrames());
        sampleRate = sampleRate_;
        asioSampleRate = asioSampleRate_;
        prebufferMs = prebufferMs_;
        prebufferFrames = prebufferFrames_;
        requestedBufferFrames = requestedBufferFrames_;
        actualBufferFrames = bufferFrames_;
        minBufferFrames = minBufferFrames_;
        maxBufferFrames = maxBufferFrames_;
        preferredBufferFrames = preferredBufferFrames_;
        bufferGranularity = bufferGranularity_;
        outputSampleType = outputAsioSampleType_;
    }

    stats.streamActive = streamActive_.load(std::memory_order_relaxed);
    stats.prebuffering = prebuffering_.load(std::memory_order_relaxed);
    stats.totalFramesQueued = totalFramesQueued_.load(std::memory_order_relaxed);
    stats.totalFramesPlayed = totalFramesPlayed_.load(std::memory_order_relaxed);
    stats.totalFramesDropped = totalFramesDropped_.load(std::memory_order_relaxed);
    stats.totalOutputFrames = totalOutputFrames_.load(std::memory_order_relaxed);
    stats.totalSilentFrames = totalSilentFrames_.load(std::memory_order_relaxed);
    stats.bufferedFrames = bufferedFrames;
    stats.bufferedMs = MsFromFrames(bufferedFrames, sampleRate);
    stats.bufferCapacityFrames = capacityFrames;
    stats.bufferCapacityMs = MsFromFrames(capacityFrames, sampleRate);
    stats.prebufferTargetFrames = prebufferFrames;
    stats.prebufferTargetMs = prebufferMs;
    stats.underrunCount = underrunCount_.load(std::memory_order_relaxed);
    stats.recentOutputFrames = recent.outputFrames;
    stats.recentSilentFrames = recent.silentFrames;
    stats.recentSilentPercent = recent.silentPercent;
    stats.asioRequestedBufferFrames = static_cast<std::int32_t>(requestedBufferFrames);
    stats.asioActualBufferFrames = static_cast<std::int32_t>(actualBufferFrames);
    stats.asioMinBufferFrames = static_cast<std::int32_t>(minBufferFrames);
    stats.asioMaxBufferFrames = static_cast<std::int32_t>(maxBufferFrames);
    stats.asioPreferredBufferFrames = static_cast<std::int32_t>(preferredBufferFrames);
    stats.asioBufferGranularity = static_cast<std::int32_t>(bufferGranularity);
    stats.asioOutputSampleType = static_cast<std::int32_t>(outputSampleType);
    stats.asioSampleRate = asioSampleRate;
    stats.asioResetRequests = asioResetRequests_.load(std::memory_order_relaxed);
    stats.asioBufferSizeChanges = asioBufferSizeChanges_.load(std::memory_order_relaxed);
    stats.asioLatencyChanges = asioLatencyChanges_.load(std::memory_order_relaxed);
    stats.asioRebuildCount = asioRebuildCount_.load(std::memory_order_relaxed);
    stats.asioLastMessage = asioLastMessage_.load(std::memory_order_relaxed);
    return stats;
}

bool AsioRenderer::IsRunning() const {
    return running_.load(std::memory_order_acquire);
}

std::int64_t AsioRenderer::ConfirmedCapturedFrames() const {
    return totalFramesPlayed_.load(std::memory_order_acquire);
}

std::int64_t AsioRenderer::ConfirmedOutputFrames() const {
    return totalOutputFrames_.load(std::memory_order_acquire);
}

std::int64_t AsioRenderer::PendingCapturedFrames() const {
    const std::int64_t queued = totalFramesQueued_.load(std::memory_order_acquire);
    const std::int64_t confirmed = totalFramesPlayed_.load(std::memory_order_acquire);
    return (std::max<std::int64_t>)(queued - confirmed, 0);
}

bool AsioRenderer::HasFault() const {
    return faultRequested_.load(std::memory_order_acquire) ||
           faulted_.load(std::memory_order_acquire);
}

std::wstring AsioRenderer::FaultMessage() const {
    std::lock_guard<std::mutex> lock(faultMutex_);
    return faultMessage_;
}

void AsioRenderer::OnAsioBufferSwitch(long doubleBufferIndex) {
    if (callbackActive_.test_and_set(std::memory_order_acquire)) {
        if (callbackThreadId_.load(std::memory_order_acquire) == GetCurrentThreadId()) {
            running_.store(false, std::memory_order_release);
            prebuffering_.store(false, std::memory_order_release);
            faultRequested_.store(true, std::memory_order_release);
            deferredReentryFault_.store(true, std::memory_order_release);
        } else {
            LatchFault(L"ASIO driver re-entered the output callback.");
        }
        return;
    }
    callbackThreadId_.store(GetCurrentThreadId(), std::memory_order_release);
    callbackExecuting_.store(true, std::memory_order_release);
    const auto finishCallback = [this] {
        if (deferredReentryFault_.exchange(false, std::memory_order_acq_rel)) {
            LatchFault(L"ASIO driver re-entered the output callback.", true);
        }
        callbackThreadId_.store(0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> callbackLock(callbackWaitMutex_);
            callbackExecuting_.store(false, std::memory_order_release);
        }
        callbackIdleCv_.notify_all();
        callbackActive_.clear(std::memory_order_release);
    };
    if (doubleBufferIndex < 0 || doubleBufferIndex > 1) {
        LatchFault(L"ASIO driver returned an invalid double-buffer index.", true);
        finishCallback();
        return;
    }
    if (!running_.load(std::memory_order_acquire)) {
        FillOutputBufferWithSilence(doubleBufferIndex);
        finishCallback();
        return;
    }

    if (awaitingFirstBufferSwitch_) {
        const std::size_t firstOutputPageIndex =
                static_cast<std::size_t>(1L - doubleBufferIndex);
        auto& firstOutputPage = outputPageLedgers_[firstOutputPageIndex];
        firstOutputPage.valid = true;
        firstOutputPage.sequence = nextDispatchSequence_++;
        firstOutputPage.dispatchEndIndex = ringBuffer_.DispatchPosition();
        firstOutputPage.outputFrames = bufferFrames_;
        firstOutputPage.capturedFrames = 0;
        firstOutputPage.paddingSilentFrames = bufferFrames_;
        firstOutputPage.underrunSilentFrames = 0;
        awaitingFirstBufferSwitch_ = false;
    }
    if (!ConfirmOutputPage(doubleBufferIndex)) {
        FillOutputBufferWithSilence(doubleBufferIndex);
        finishCallback();
        return;
    }
    FillOutputBuffer(doubleBufferIndex);
    finishCallback();
}

void AsioRenderer::OnAsioSampleRateChanged(ASIOSampleRate sampleRate) {
    if (sampleRate > 0.0) {
        const auto nextSampleRate = static_cast<std::uint32_t>(sampleRate + 0.5);
        bool changedUnexpectedly = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            asioSampleRate_ = nextSampleRate;
            changedUnexpectedly = sampleRate_ != 0 && nextSampleRate != sampleRate_;
        }
        if (changedUnexpectedly) {
            LatchFault(L"ASIO sample rate changed while captured audio was active.");
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
            LatchFault(L"ASIO driver requested an output reset; exact page consumption can no longer be proven.");
            return 1;
        case kAsioBufferSizeChange:
            asioBufferSizeChanges_.fetch_add(1, std::memory_order_relaxed);
            asioLastMessage_.store(static_cast<std::int32_t>(selector), std::memory_order_relaxed);
            LatchFault(L"ASIO driver changed its buffer size while output was active; exact page consumption can no longer be proven.");
            return 1;
        case kAsioLatenciesChanged:
            asioLatencyChanges_.fetch_add(1, std::memory_order_relaxed);
            asioLastMessage_.store(static_cast<std::int32_t>(selector), std::memory_order_relaxed);
            return 1;
        case kAsioResyncRequest:
            asioLastMessage_.store(static_cast<std::int32_t>(selector), std::memory_order_relaxed);
            LatchFault(L"ASIO driver reported that its stream position is out of sync.");
            return 1;
        case kAsioOverload:
            asioLastMessage_.store(static_cast<std::int32_t>(selector), std::memory_order_relaxed);
            LatchFault(L"ASIO driver reported an output overload.");
            return 1;
        case kAsioSupportsTimeInfo:
            return 1;
        default:
            return 0;
    }
}

void AsioRenderer::ControlLoop(std::wstring deviceId, std::uint32_t requestedBufferFrames) {
    std::wstring error;
    const bool ok = OpenDriverOnControlThread(deviceId, requestedBufferFrames, &error);
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
        controlCv_.wait(lock, [this, handledSerial] {
            return shutdownRequested_ ||
                   faultStopRequested_.load(std::memory_order_acquire) ||
                   startRequestSerial_ != handledSerial;
        });
        if (shutdownRequested_) {
            break;
        }
        if (faultStopRequested_.exchange(false, std::memory_order_acq_rel)) {
            break;
        }

        const std::uint64_t serial = startRequestSerial_;
        lock.unlock();

        bool started = true;
        std::wstring startError;
        if (faultRequested_.load(std::memory_order_acquire)) {
            started = false;
            startError = L"ASIO renderer entered a faulting state before output could start.";
        } else if (!streamActive_.load(std::memory_order_acquire)) {
            const ASIOError startResult = asioDriver_->start();
            if (!IsAsioSuccess(startResult)) {
                started = false;
                startError = AsioErrorMessage(asioDriver_, startResult, L"IASIO::start");
                LatchFault(startError);
            } else {
                streamActive_.store(true, std::memory_order_release);
                prebuffering_.store(false, std::memory_order_release);
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
    if (streamActive_.load(std::memory_order_acquire) && asioDriver_ != nullptr) {
        const ASIOError stopResult = asioDriver_->stop();
        if (IsAsioSuccess(stopResult)) {
            streamActive_.store(false, std::memory_order_release);
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
    prebuffering_.store(false, std::memory_order_release);
    if (driverQuiesced) {
        RollbackOutputPages();
        CloseDriverOnControlThread();
    }
}

bool AsioRenderer::OpenDriverOnControlThread(const std::wstring& deviceId,
                                             std::uint32_t requestedBufferFrames,
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

    ASIOSampleRate currentRate = 0.0;
    asioDriver_->getSampleRate(&currentRate);

    const ASIOSampleRate requestedRate = static_cast<ASIOSampleRate>(sampleRate_);
    ASIOError result = asioDriver_->canSampleRate(requestedRate);
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
        currentRate = requestedRate;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        asioSampleRate_ = static_cast<std::uint32_t>(currentRate + 0.5);
    }

    return CreateBuffersOnControlThread(requestedBufferFrames, true, outError);
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

    OutputSampleKind outputKind = OutputSampleKind::Unknown;
    std::uint32_t outputRightShift = 0;
    if (!OutputKindFromAsio(firstChannel.type, &outputKind, &outputRightShift)) {
        if (outError != nullptr) {
            std::wstring message = L"ASIO output sample type is unsupported: ";
            message += AsioSampleTypeName(firstChannel.type);
            *outError = message;
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        bufferFrames_ = chosenBufferFrames;
        requestedBufferFrames_ = requestedBufferFrames;
        minBufferFrames_ = static_cast<std::uint32_t>((std::max<long>)(minBuffer, 0));
        maxBufferFrames_ = static_cast<std::uint32_t>((std::max<long>)(maxBuffer, 0));
        preferredBufferFrames_ = static_cast<std::uint32_t>((std::max<long>)(preferredBuffer, 0));
        bufferGranularity_ = granularity;
        outputKind_ = outputKind;
        outputAsioSampleType_ = firstChannel.type;
        outputRightShift_ = outputRightShift;
        channelInfos_[0] = firstChannel;
        channelInfos_[1] = secondChannel;

        if (resetRingBuffer) {
            const std::uint32_t ringFrames = (std::max<std::uint32_t>)(
                    (std::max<std::uint32_t>)(sampleRate_, prebufferFrames_ * 4U),
                    bufferFrames_ * 16U);
            if (!ringBuffer_.Reset(bytesPerFrame_, ringFrames)) {
                if (outError != nullptr) {
                    *outError = L"Failed to allocate ASIO PCM ring buffer.";
                }
                return false;
            }
        }
        callbackBuffer_.assign(static_cast<std::size_t>(bufferFrames_) * bytesPerFrame_, 0);
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
    running_.store(true, std::memory_order_release);
    streamActive_.store(false, std::memory_order_release);
    prebuffering_.store(false, std::memory_order_release);
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
    if (streamActive_.load(std::memory_order_acquire)) {
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
    if (streamActive_.load(std::memory_order_acquire)) {
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

void AsioRenderer::PaddingLoop() {
    while (running_.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> waitLock(paddingWaitMutex_);
        paddingCv_.wait_for(waitLock, std::chrono::milliseconds(5), [this] {
            return !running_.load(std::memory_order_acquire);
        });
        waitLock.unlock();
        if (!running_.load(std::memory_order_acquire)) {
            break;
        }
        MaintainPadding();
    }
}

void AsioRenderer::MaintainPadding() {
    if (!running_.load(std::memory_order_acquire) || prebufferFrames_ == 0 ||
        maxBufferOffsetFrames_ >= prebufferFrames_) {
        return;
    }

    std::lock_guard<std::mutex> producerLock(producerMutex_);
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    const std::uint32_t bufferedFrames = ringBuffer_.PendingFrames();
    const std::uint32_t triggerFrames = prebufferFrames_ - maxBufferOffsetFrames_;
    if (!paddingActive_ && bufferedFrames < triggerFrames) {
        paddingActive_ = true;
    }
    if (!paddingActive_ || bufferedFrames >= prebufferFrames_) {
        return;
    }

    ringBuffer_.PushSilence(prebufferFrames_ - bufferedFrames);
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
        wchar_t message[256]{};
        std::swprintf(message,
                      std::size(message),
                      L"ASIO output pages were retired out of order. expected=%llu actual=%llu index=%ld",
                      static_cast<unsigned long long>(nextConfirmSequence_),
                      static_cast<unsigned long long>(page.sequence),
                      doubleBufferIndex);
        LatchFault(message, true);
        return false;
    }
    if (!ringBuffer_.ConfirmDispatch(page.dispatchEndIndex)) {
        LatchFault(L"ASIO output confirmation did not match the retained PCM queue.", true);
        return false;
    }

    if (page.capturedFrames > 0) {
        totalFramesPlayed_.fetch_add(page.capturedFrames, std::memory_order_release);
    }
    if (page.underrunSilentFrames > 0) {
        underrunCount_.fetch_add(1, std::memory_order_relaxed);
    }
    RecordOutputFrames(page.outputFrames,
                       page.paddingSilentFrames + page.underrunSilentFrames);
    page = {};
    ++nextConfirmSequence_;
    paddingCv_.notify_one();
    return true;
}

void AsioRenderer::RollbackOutputPages() {
    ringBuffer_.RollbackDispatch();
    outputPageLedgers_.fill({});
    nextDispatchSequence_ = 1;
    nextConfirmSequence_ = 1;
    awaitingFirstBufferSwitch_ = true;
}

bool AsioRenderer::LatchFault(const std::wstring& message, bool fromOutputCallback) {
    if (faulted_.load(std::memory_order_acquire)) {
        return false;
    }

    faultRequested_.store(true, std::memory_order_release);

    // Closing this gate first guarantees that a callback which starts after
    // the fault request cannot retire another ledger page. A callback already
    // past the gate is allowed to finish so the published counters include its
    // last definitely consumed page.
    running_.store(false, std::memory_order_release);
    prebuffering_.store(false, std::memory_order_release);

    if (!fromOutputCallback) {
        std::unique_lock<std::mutex> callbackLock(callbackWaitMutex_);
        callbackIdleCv_.wait(callbackLock, [this] {
            return !callbackExecuting_.load(std::memory_order_acquire);
        });
    }

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
    paddingCv_.notify_all();
    controlCv_.notify_all();
    startCv_.notify_all();
    return true;
}

void AsioRenderer::FillOutputBuffer(long doubleBufferIndex) {
    if (doubleBufferIndex < 0 || doubleBufferIndex > 1 ||
        bufferFrames_ == 0 || bytesPerFrame_ == 0 || callbackBuffer_.empty()) {
        FillOutputBufferWithSilence(doubleBufferIndex);
        LatchFault(L"ASIO requested output while its renderer buffers were invalid.", true);
        return;
    }

    const RawFrameRingBuffer::DispatchResult dispatched =
            ringBuffer_.Dispatch(callbackBuffer_.data(), bufferFrames_);
    const std::uint32_t underrunSilentFrames = bufferFrames_ - dispatched.frames;
    if (underrunSilentFrames > 0) {
        std::memset(callbackBuffer_.data() +
                            static_cast<std::size_t>(dispatched.frames) * bytesPerFrame_,
                    0,
                    static_cast<std::size_t>(underrunSilentFrames) * bytesPerFrame_);
    }

    WriteConvertedOutput(callbackBuffer_.data(), bufferFrames_, doubleBufferIndex);
    auto& page = outputPageLedgers_[static_cast<std::size_t>(doubleBufferIndex)];
    page.valid = true;
    page.sequence = nextDispatchSequence_++;
    page.dispatchEndIndex = dispatched.endIndex;
    page.outputFrames = bufferFrames_;
    page.paddingSilentFrames = dispatched.paddingSilentFrames;
    page.capturedFrames = dispatched.frames - dispatched.paddingSilentFrames;
    page.underrunSilentFrames = underrunSilentFrames;
}

void AsioRenderer::FillOutputBufferWithSilence(long doubleBufferIndex) {
    if (doubleBufferIndex < 0 || doubleBufferIndex > 1) {
        return;
    }
    for (auto& bufferInfo : bufferInfos_) {
        if (bufferInfo.buffers[doubleBufferIndex] != nullptr && bufferFrames_ > 0) {
            std::uint32_t bytesPerSample = 4;
            switch (outputKind_) {
                case OutputSampleKind::Int16:
                    bytesPerSample = 2;
                    break;
                case OutputSampleKind::Int24:
                    bytesPerSample = 3;
                    break;
                default:
                    bytesPerSample = 4;
                    break;
            }
            std::memset(bufferInfo.buffers[doubleBufferIndex],
                        0,
                        static_cast<std::size_t>(bufferFrames_) * bytesPerSample);
        }
    }
}

void AsioRenderer::WriteConvertedOutput(const std::uint8_t* interleaved,
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

    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        switch (outputKind_) {
            case OutputSampleKind::Float32: {
                WriteFloat32Sample(left, frame, ReadSourceFloat(interleaved, frame, 0));
                WriteFloat32Sample(right, frame, ReadSourceFloat(interleaved, frame, 1));
                break;
            }
            case OutputSampleKind::Int16: {
                WriteInt16Sample(left, frame, ReadSourceInt32(interleaved, frame, 0));
                WriteInt16Sample(right, frame, ReadSourceInt32(interleaved, frame, 1));
                break;
            }
            case OutputSampleKind::Int24: {
                WriteInt24Sample(left, frame, ReadSourceInt32(interleaved, frame, 0));
                WriteInt24Sample(right, frame, ReadSourceInt32(interleaved, frame, 1));
                break;
            }
            case OutputSampleKind::Int32: {
                WriteInt32Sample(left,
                                 frame,
                                 ReadSourceInt32(interleaved, frame, 0),
                                 outputRightShift_);
                WriteInt32Sample(right,
                                  frame,
                                  ReadSourceInt32(interleaved, frame, 1),
                                  outputRightShift_);
                break;
            }
            default:
                break;
        }
    }
}

float AsioRenderer::ReadSourceFloat(const std::uint8_t* interleaved,
                                    std::uint32_t frameIndex,
                                    std::uint32_t channelIndex) const {
    if (sourceKind_ == SourceSampleKind::Float32) {
        channelIndex = (std::min<std::uint32_t>)(channelIndex, sourceChannels_ - 1U);
        const std::uint8_t* sample = interleaved +
                static_cast<std::size_t>(frameIndex) * bytesPerFrame_ +
                static_cast<std::size_t>(channelIndex) * sourceBytesPerSample_;
        float value = 0.0f;
        std::memcpy(&value, sample, sizeof(value));
        return (std::max)(-1.0f, (std::min)(1.0f, value));
    }

    const std::int32_t value = ReadSourceInt32(interleaved, frameIndex, channelIndex);
    return static_cast<float>(static_cast<double>(value) / 2147483648.0);
}

std::int32_t AsioRenderer::ReadSourceInt32(const std::uint8_t* interleaved,
                                           std::uint32_t frameIndex,
                                           std::uint32_t channelIndex) const {
    if (sourceChannels_ == 0 || sourceBytesPerSample_ == 0 || bytesPerFrame_ == 0) {
        return 0;
    }

    channelIndex = (std::min<std::uint32_t>)(channelIndex, sourceChannels_ - 1U);
    const std::uint8_t* sample = interleaved +
            static_cast<std::size_t>(frameIndex) * bytesPerFrame_ +
            static_cast<std::size_t>(channelIndex) * sourceBytesPerSample_;
    switch (sourceKind_) {
        case SourceSampleKind::Float32: {
            float value = 0.0f;
            std::memcpy(&value, sample, sizeof(value));
            return FloatSampleToPcm32(value);
        }
        case SourceSampleKind::Pcm16: {
            std::int16_t value = 0;
            std::memcpy(&value, sample, sizeof(value));
            return static_cast<std::int32_t>(value) << 16U;
        }
        case SourceSampleKind::Pcm24:
            return ReadPcm24Sample(sample);
        case SourceSampleKind::Pcm32: {
            std::int32_t value = 0;
            std::memcpy(&value, sample, sizeof(value));
            return value;
        }
        default:
            return 0;
    }
}

void AsioRenderer::ResetStats() {
    {
        std::lock_guard<std::mutex> lock(faultMutex_);
        faultMessage_.clear();
        faulted_.store(false, std::memory_order_release);
    }
    faultRequested_.store(false, std::memory_order_release);
    faultStopRequested_.store(false, std::memory_order_release);
    totalFramesQueued_.store(0, std::memory_order_relaxed);
    totalFramesPlayed_.store(0, std::memory_order_relaxed);
    totalFramesDropped_.store(0, std::memory_order_relaxed);
    totalOutputFrames_.store(0, std::memory_order_relaxed);
    totalSilentFrames_.store(0, std::memory_order_relaxed);
    underrunCount_.store(0, std::memory_order_relaxed);
    asioResetRequests_.store(0, std::memory_order_relaxed);
    asioBufferSizeChanges_.store(0, std::memory_order_relaxed);
    asioLatencyChanges_.store(0, std::memory_order_relaxed);
    asioRebuildCount_.store(0, std::memory_order_relaxed);
    asioLastMessage_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(silenceStatsMutex_);
        silenceBuckets_.fill({});
    }
}

void AsioRenderer::RecordOutputFrames(std::uint32_t outputFrames, std::uint32_t silentFrames) {
    if (outputFrames == 0) {
        return;
    }

    totalOutputFrames_.fetch_add(outputFrames, std::memory_order_relaxed);
    totalSilentFrames_.fetch_add(silentFrames, std::memory_order_relaxed);

    if (!silenceStatsMutex_.try_lock()) {
        return;
    }
    std::lock_guard<std::mutex> lock(silenceStatsMutex_, std::adopt_lock);
    const std::uint64_t second = NowSecond();
    auto& bucket = silenceBuckets_[static_cast<std::size_t>(second % silenceBuckets_.size())];
    if (bucket.second != second) {
        bucket.second = second;
        bucket.outputFrames = 0;
        bucket.silentFrames = 0;
    }
    bucket.outputFrames += outputFrames;
    bucket.silentFrames += silentFrames;
}

AsioRenderer::SilenceWindowStats AsioRenderer::GetRecentSilenceStats() const {
    SilenceWindowStats stats{};
    const std::uint64_t second = NowSecond();
    std::lock_guard<std::mutex> lock(silenceStatsMutex_);
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

}  // namespace audiobridge

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

void AsioBufferSwitch(long doubleBufferIndex, ASIOBool /*directProcess*/) {
    if (auto* renderer = g_activeRenderer.load(std::memory_order_acquire)) {
        renderer->OnAsioBufferSwitch(doubleBufferIndex);
    }
}

void AsioSampleRateDidChange(ASIOSampleRate sampleRate) {
    if (auto* renderer = g_activeRenderer.load(std::memory_order_acquire)) {
        renderer->OnAsioSampleRateChanged(sampleRate);
    }
}

long AsioMessage(long selector, long value, void* message, double* opt) {
    if (auto* renderer = g_activeRenderer.load(std::memory_order_acquire)) {
        return renderer->OnAsioMessage(selector, value, message, opt);
    }
    return 0;
}

ASIOTime* AsioBufferSwitchTimeInfo(ASIOTime* params,
                                   long doubleBufferIndex,
                                   ASIOBool /*directProcess*/) {
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
    readIndex_.store(0, std::memory_order_relaxed);
    writeIndex_.store(0, std::memory_order_relaxed);
    return true;
}

void RawFrameRingBuffer::Clear() {
    readIndex_.store(0, std::memory_order_release);
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

    const std::size_t readIndex = readIndex_.load(std::memory_order_acquire);
    const std::size_t writeIndex = writeIndex_.load(std::memory_order_relaxed);
    const std::size_t requestedBytes =
            static_cast<std::size_t>(frameCount) * static_cast<std::size_t>(bytesPerFrame_);
    const std::size_t writableBytes =
            (std::min)(requestedBytes, AvailableWriteBytes(readIndex, writeIndex)) /
            bytesPerFrame_ * bytesPerFrame_;
    if (writableBytes == 0) {
        return 0;
    }

    CopyInto(writeIndex, data, writableBytes);
    CopyOriginsInto(writeIndex,
                    static_cast<std::uint32_t>(writableBytes / bytesPerFrame_),
                    origin);
    writeIndex_.store(writeIndex + writableBytes, std::memory_order_release);
    return static_cast<std::uint32_t>(writableBytes / bytesPerFrame_);
}

std::uint32_t RawFrameRingBuffer::PushSilence(std::uint32_t frameCount) {
    if (frameCount == 0 || bytesPerFrame_ == 0 || bytes_.empty()) {
        return 0;
    }

    const std::size_t readIndex = readIndex_.load(std::memory_order_acquire);
    const std::size_t writeIndex = writeIndex_.load(std::memory_order_relaxed);
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

RawFrameRingBuffer::PopResult RawFrameRingBuffer::Pop(std::uint8_t* data,
                                                       std::uint32_t frameCount) {
    PopResult result{};
    if (data == nullptr || frameCount == 0 || bytesPerFrame_ == 0 || bytes_.empty()) {
        return result;
    }

    const std::size_t writeIndex = writeIndex_.load(std::memory_order_acquire);
    const std::size_t readIndex = readIndex_.load(std::memory_order_relaxed);
    const std::size_t requestedBytes =
            static_cast<std::size_t>(frameCount) * static_cast<std::size_t>(bytesPerFrame_);
    const std::size_t readableBytes =
            (std::min)(requestedBytes, AvailableReadBytes(readIndex, writeIndex)) /
            bytesPerFrame_ * bytesPerFrame_;
    if (readableBytes == 0) {
        return result;
    }

    CopyOut(readIndex, data, readableBytes);
    result.frames = static_cast<std::uint32_t>(readableBytes / bytesPerFrame_);
    result.paddingSilentFrames = CountPaddingOrigins(readIndex, result.frames);
    readIndex_.store(readIndex + readableBytes, std::memory_order_release);
    return result;
}

std::uint32_t RawFrameRingBuffer::AvailableReadFrames() const {
    if (bytesPerFrame_ == 0) {
        return 0;
    }
    const std::size_t readIndex = readIndex_.load(std::memory_order_acquire);
    const std::size_t writeIndex = writeIndex_.load(std::memory_order_acquire);
    return static_cast<std::uint32_t>(
            AvailableReadBytes(readIndex, writeIndex) / bytesPerFrame_);
}

std::uint32_t RawFrameRingBuffer::CapacityFrames() const {
    if (bytesPerFrame_ == 0) {
        return 0;
    }
    return static_cast<std::uint32_t>(bytes_.size() / bytesPerFrame_);
}

std::size_t RawFrameRingBuffer::AvailableReadBytes(std::size_t readIndex,
                                                   std::size_t writeIndex) const {
    return writeIndex - readIndex;
}

std::size_t RawFrameRingBuffer::AvailableWriteBytes(std::size_t readIndex,
                                                    std::size_t writeIndex) const {
    if (bytes_.empty()) {
        return 0;
    }
    const std::size_t readableBytes = AvailableReadBytes(readIndex, writeIndex);
    if (readableBytes >= bytes_.size()) {
        return 0;
    }
    return bytes_.size() - readableBytes;
}

void RawFrameRingBuffer::CopyInto(std::size_t writeIndex,
                                  const std::uint8_t* data,
                                  std::size_t bytes) {
    const std::size_t offset = writeIndex & byteMask_;
    const std::size_t firstChunk = (std::min)(bytes, bytes_.size() - offset);
    std::memcpy(bytes_.data() + offset, data, firstChunk);
    if (bytes > firstChunk) {
        std::memcpy(bytes_.data(), data + firstChunk, bytes - firstChunk);
    }
}

void RawFrameRingBuffer::ZeroInto(std::size_t writeIndex, std::size_t bytes) {
    const std::size_t offset = writeIndex & byteMask_;
    const std::size_t firstChunk = (std::min)(bytes, bytes_.size() - offset);
    std::memset(bytes_.data() + offset, 0, firstChunk);
    if (bytes > firstChunk) {
        std::memset(bytes_.data(), 0, bytes - firstChunk);
    }
}

void RawFrameRingBuffer::CopyOut(std::size_t readIndex,
                                 std::uint8_t* data,
                                 std::size_t bytes) const {
    const std::size_t offset = readIndex & byteMask_;
    const std::size_t firstChunk = (std::min)(bytes, bytes_.size() - offset);
    std::memcpy(data, bytes_.data() + offset, firstChunk);
    if (bytes > firstChunk) {
        std::memcpy(data + firstChunk, bytes_.data(), bytes - firstChunk);
    }
}

void RawFrameRingBuffer::CopyOriginsInto(std::size_t writeIndex,
                                         std::uint32_t frameCount,
                                         FrameOrigin origin) {
    if (frameCount == 0 || bytesPerFrame_ == 0 || frameOrigins_.empty()) {
        return;
    }
    const std::size_t frameIndex = writeIndex / bytesPerFrame_;
    const std::size_t offset = frameIndex % frameOrigins_.size();
    const std::size_t firstChunk =
            (std::min)(static_cast<std::size_t>(frameCount), frameOrigins_.size() - offset);
    std::fill_n(frameOrigins_.begin() + static_cast<std::ptrdiff_t>(offset), firstChunk, origin);
    if (static_cast<std::size_t>(frameCount) > firstChunk) {
        std::fill_n(frameOrigins_.begin(),
                    static_cast<std::size_t>(frameCount) - firstChunk,
                    origin);
    }
}

std::uint32_t RawFrameRingBuffer::CountPaddingOrigins(std::size_t readIndex,
                                                       std::uint32_t frameCount) const {
    if (frameCount == 0 || bytesPerFrame_ == 0 || frameOrigins_.empty()) {
        return 0;
    }
    const std::size_t frameIndex = readIndex / bytesPerFrame_;
    const std::size_t offset = frameIndex % frameOrigins_.size();
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
}

bool AsioRenderer::Start(const std::wstring& deviceId,
                         const WAVEFORMATEXTENSIBLE& format,
                         std::int32_t prebufferMs,
                         std::int32_t maxBufferOffsetMs,
                         std::uint32_t requestedBufferFrames,
                         std::wstring* outError) {
    Stop();
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
        pendingResetMask_.store(0, std::memory_order_release);
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

    std::wstring startError;
    if (!TryStartStreamIfReady(&startError)) {
        Stop();
        if (outError != nullptr) {
            *outError = startError;
        }
        return false;
    }
    try {
        paddingThread_ = std::thread(&AsioRenderer::PaddingLoop, this);
    } catch (const std::exception&) {
        Stop();
        if (outError != nullptr) {
            *outError = L"Failed to start buffer padding thread.";
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

    std::lock_guard<std::mutex> producerLock(producerMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
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
    pendingResetMask_.store(0, std::memory_order_release);
}

std::uint32_t AsioRenderer::PushPcm(const std::uint8_t* data,
                                    std::uint32_t frameCount,
                                    std::wstring* outError) {
    if (!running_.load(std::memory_order_acquire)) {
        return 0;
    }

    std::uint32_t written = 0;
    {
        std::lock_guard<std::mutex> producerLock(producerMutex_);
        if (!running_.load(std::memory_order_acquire)) {
            return 0;
        }
        paddingActive_ = false;
        written = ringBuffer_.Push(data, frameCount);
    }
    paddingCv_.notify_one();
    totalFramesQueued_.fetch_add(written, std::memory_order_relaxed);
    if (written < frameCount) {
        totalFramesDropped_.fetch_add(frameCount - written, std::memory_order_relaxed);
    }
    if (written > 0 && !TryStartStreamIfReady(outError)) {
        return written;
    }
    return written;
}

RendererStats AsioRenderer::GetStats() const {
    RendererStats stats{};
    const auto recent = GetRecentSilenceStats();
    const auto bufferedFrames = static_cast<std::int64_t>(ringBuffer_.AvailableReadFrames());
    const auto capacityFrames = static_cast<std::int64_t>(ringBuffer_.CapacityFrames());
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

void AsioRenderer::OnAsioBufferSwitch(long doubleBufferIndex) {
    if (!running_.load(std::memory_order_acquire)) {
        FillOutputBufferWithSilence(doubleBufferIndex);
        return;
    }
    FillOutputBuffer(doubleBufferIndex);
}

void AsioRenderer::OnAsioSampleRateChanged(ASIOSampleRate sampleRate) {
    if (sampleRate > 0.0) {
        std::lock_guard<std::mutex> lock(mutex_);
        asioSampleRate_ = static_cast<std::uint32_t>(sampleRate + 0.5);
    }
}

long AsioRenderer::OnAsioMessage(long selector, long value, void* /*message*/, double* /*opt*/) {
    switch (selector) {
        case kAsioSelectorSupported:
            return value == kAsioEngineVersion ||
                           value == kAsioResetRequest ||
                           value == kAsioBufferSizeChange ||
                           value == kAsioSupportsTimeInfo ||
                           value == kAsioLatenciesChanged
                    ? 1
                    : 0;
        case kAsioEngineVersion:
            return 2;
        case kAsioResetRequest:
            asioResetRequests_.fetch_add(1, std::memory_order_relaxed);
            asioLastMessage_.store(static_cast<std::int32_t>(selector), std::memory_order_relaxed);
            pendingResetMask_.fetch_or(kResetRequest, std::memory_order_acq_rel);
            controlCv_.notify_all();
            return 1;
        case kAsioBufferSizeChange:
            asioBufferSizeChanges_.fetch_add(1, std::memory_order_relaxed);
            asioLastMessage_.store(static_cast<std::int32_t>(selector), std::memory_order_relaxed);
            pendingResetMask_.fetch_or(kBufferSizeChange, std::memory_order_acq_rel);
            controlCv_.notify_all();
            return 1;
        case kAsioLatenciesChanged:
            asioLatencyChanges_.fetch_add(1, std::memory_order_relaxed);
            asioLastMessage_.store(static_cast<std::int32_t>(selector), std::memory_order_relaxed);
            pendingResetMask_.fetch_or(kLatenciesChanged, std::memory_order_acq_rel);
            controlCv_.notify_all();
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
                   startRequestSerial_ != handledSerial ||
                   pendingResetMask_.load(std::memory_order_acquire) != 0;
        });
        if (shutdownRequested_) {
            break;
        }

        const std::uint32_t resetMask =
                pendingResetMask_.exchange(0, std::memory_order_acq_rel);
        if (resetMask != 0) {
            lock.unlock();
            std::wstring resetError;
            if (!RebuildBuffersOnControlThread(requestedBufferFrames, &resetError)) {
                {
                    std::lock_guard<std::mutex> errorLock(controlMutex_);
                    lastStartSucceeded_ = false;
                    lastStartError_ = resetError.empty()
                            ? L"ASIO buffer rebuild failed."
                            : resetError;
                }
                running_.store(false, std::memory_order_release);
                prebuffering_.store(false, std::memory_order_release);
                lock.lock();
                break;
            }
            lock.lock();
            continue;
        }

        const std::uint64_t serial = startRequestSerial_;
        lock.unlock();

        bool started = true;
        std::wstring startError;
        if (!streamActive_.load(std::memory_order_acquire)) {
            FillOutputBuffer(0);
            FillOutputBuffer(1);
            const ASIOError startResult = asioDriver_->start();
            if (!IsAsioSuccess(startResult)) {
                started = false;
                startError = AsioErrorMessage(asioDriver_, startResult, L"IASIO::start");
            } else {
                streamActive_.store(true, std::memory_order_release);
                prebuffering_.store(false, std::memory_order_release);
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

    if (streamActive_.exchange(false, std::memory_order_acq_rel) && asioDriver_ != nullptr) {
        asioDriver_->stop();
    }
    running_.store(false, std::memory_order_release);
    prebuffering_.store(false, std::memory_order_release);
    CloseDriverOnControlThread();
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
    FillOutputBufferWithSilence(0);
    FillOutputBufferWithSilence(1);

    running_.store(true, std::memory_order_release);
    streamActive_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::uint32_t startThreshold =
                prebufferFrames_ > 0 ? (std::max<std::uint32_t>)(prebufferFrames_, bufferFrames_ * 2U) : 0;
        prebuffering_.store(startThreshold > 0, std::memory_order_release);
    }
    return true;
}

bool AsioRenderer::RebuildBuffersOnControlThread(std::uint32_t requestedBufferFrames,
                                                 std::wstring* outError) {
    if (asioDriver_ == nullptr) {
        if (outError != nullptr) {
            *outError = L"ASIO driver is not open.";
        }
        return false;
    }

    const bool wasActive = streamActive_.exchange(false, std::memory_order_acq_rel);
    if (g_activeRenderer.load(std::memory_order_acquire) == this) {
        g_activeRenderer.store(nullptr, std::memory_order_release);
    }
    if (wasActive) {
        asioDriver_->stop();
    }
    DisposeBuffersOnControlThread();

    if (!CreateBuffersOnControlThread(requestedBufferFrames, false, outError)) {
        return false;
    }

    asioRebuildCount_.fetch_add(1, std::memory_order_relaxed);
    if (wasActive) {
        FillOutputBuffer(0);
        FillOutputBuffer(1);
        const ASIOError startResult = asioDriver_->start();
        if (!IsAsioSuccess(startResult)) {
            if (outError != nullptr) {
                *outError = AsioErrorMessage(asioDriver_, startResult, L"IASIO::start after reset");
            }
            return false;
        }
        streamActive_.store(true, std::memory_order_release);
        prebuffering_.store(false, std::memory_order_release);
    }
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
    if (!running_.load(std::memory_order_acquire) ||
        streamActive_.load(std::memory_order_acquire)) {
        return true;
    }

    std::uint32_t startThreshold = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (prebufferFrames_ > 0) {
            startThreshold = (std::max<std::uint32_t>)(prebufferFrames_, bufferFrames_ * 2U);
        }
    }
    if (ringBuffer_.AvailableReadFrames() < startThreshold) {
        return true;
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
        return shutdownRequested_ || startHandledSerial_ >= serial;
    });
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
    if (!streamActive_.load(std::memory_order_acquire) || prebufferFrames_ == 0 ||
        maxBufferOffsetFrames_ >= prebufferFrames_) {
        return;
    }

    std::lock_guard<std::mutex> producerLock(producerMutex_);
    if (!running_.load(std::memory_order_acquire) ||
        !streamActive_.load(std::memory_order_acquire)) {
        return;
    }

    const std::uint32_t bufferedFrames = ringBuffer_.AvailableReadFrames();
    const std::uint32_t triggerFrames = prebufferFrames_ - maxBufferOffsetFrames_;
    if (!paddingActive_ && bufferedFrames < triggerFrames) {
        paddingActive_ = true;
    }
    if (!paddingActive_ || bufferedFrames >= prebufferFrames_) {
        return;
    }

    ringBuffer_.PushSilence(prebufferFrames_ - bufferedFrames);
}

void AsioRenderer::FillOutputBuffer(long doubleBufferIndex) {
    if (doubleBufferIndex < 0 || doubleBufferIndex > 1 ||
        bufferFrames_ == 0 || bytesPerFrame_ == 0 || callbackBuffer_.empty()) {
        FillOutputBufferWithSilence(doubleBufferIndex);
        return;
    }

    const RawFrameRingBuffer::PopResult popped =
            ringBuffer_.Pop(callbackBuffer_.data(), bufferFrames_);
    const std::uint32_t underrunSilentFrames = bufferFrames_ - popped.frames;
    if (underrunSilentFrames > 0) {
        std::memset(callbackBuffer_.data() +
                            static_cast<std::size_t>(popped.frames) * bytesPerFrame_,
                    0,
                    static_cast<std::size_t>(underrunSilentFrames) * bytesPerFrame_);
        underrunCount_.fetch_add(1, std::memory_order_relaxed);
    }

    WriteConvertedOutput(callbackBuffer_.data(), bufferFrames_, doubleBufferIndex);
    const std::uint32_t capturedFrames = popped.frames - popped.paddingSilentFrames;
    if (capturedFrames > 0) {
        totalFramesPlayed_.fetch_add(capturedFrames, std::memory_order_relaxed);
    }
    RecordOutputFrames(bufferFrames_,
                       underrunSilentFrames + popped.paddingSilentFrames);
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

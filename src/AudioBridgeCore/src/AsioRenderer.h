#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmreg.h>

#include "iasiodrv.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace audiobridge {

struct RendererStats {
    bool streamActive = false;
    bool prebuffering = false;
    std::int64_t totalFramesQueued = 0;
    std::int64_t totalFramesPlayed = 0;
    std::int64_t totalFramesDropped = 0;
    std::int64_t totalOutputFrames = 0;
    std::int64_t totalSilentFrames = 0;
    std::int64_t bufferedFrames = 0;
    std::int32_t bufferedMs = 0;
    std::int64_t bufferCapacityFrames = 0;
    std::int32_t bufferCapacityMs = 0;
    std::int64_t prebufferTargetFrames = 0;
    std::int32_t prebufferTargetMs = 0;
    std::int64_t underrunCount = 0;
    std::int64_t recentOutputFrames = 0;
    std::int64_t recentSilentFrames = 0;
    double recentSilentPercent = 0.0;
    std::int32_t asioRequestedBufferFrames = 0;
    std::int32_t asioActualBufferFrames = 0;
    std::int32_t asioMinBufferFrames = 0;
    std::int32_t asioMaxBufferFrames = 0;
    std::int32_t asioPreferredBufferFrames = 0;
    std::int32_t asioBufferGranularity = 0;
    std::int32_t asioOutputSampleType = 0;
    std::uint32_t asioSampleRate = 0;
    std::int64_t asioResetRequests = 0;
    std::int64_t asioBufferSizeChanges = 0;
    std::int64_t asioLatencyChanges = 0;
    std::int64_t asioRebuildCount = 0;
    std::int32_t asioLastMessage = 0;
};

class RawFrameRingBuffer {
public:
    enum class FrameOrigin : std::uint8_t {
        Captured,
        PaddingSilence,
    };

    struct PopResult {
        std::uint32_t frames = 0;
        std::uint32_t paddingSilentFrames = 0;
    };

    bool Reset(std::uint32_t bytesPerFrame, std::uint32_t capacityFrames);
    void Clear();
    std::uint32_t Push(const std::uint8_t* data,
                       std::uint32_t frameCount,
                       FrameOrigin origin = FrameOrigin::Captured);
    std::uint32_t PushSilence(std::uint32_t frameCount);
    PopResult Pop(std::uint8_t* data, std::uint32_t frameCount);
    std::uint32_t AvailableReadFrames() const;
    std::uint32_t CapacityFrames() const;

private:
    // Single serialized producer / single consumer (ASIO callback).
    std::size_t AvailableReadBytes(std::size_t readIndex, std::size_t writeIndex) const;
    std::size_t AvailableWriteBytes(std::size_t readIndex, std::size_t writeIndex) const;
    void CopyInto(std::size_t writeIndex, const std::uint8_t* data, std::size_t bytes);
    void ZeroInto(std::size_t writeIndex, std::size_t bytes);
    void CopyOut(std::size_t readIndex, std::uint8_t* data, std::size_t bytes) const;
    void CopyOriginsInto(std::size_t writeIndex,
                         std::uint32_t frameCount,
                         FrameOrigin origin);
    std::uint32_t CountPaddingOrigins(std::size_t readIndex,
                                      std::uint32_t frameCount) const;

    std::vector<std::uint8_t> bytes_;
    std::vector<FrameOrigin> frameOrigins_;
    std::size_t byteMask_ = 0;
    std::atomic<std::size_t> readIndex_{0};
    std::atomic<std::size_t> writeIndex_{0};
    std::uint32_t bytesPerFrame_ = 0;
};

class AsioRenderer {
public:
    enum class SourceSampleKind {
        Unknown,
        Float32,
        Pcm16,
        Pcm24,
        Pcm32,
    };

    enum class OutputSampleKind {
        Unknown,
        Float32,
        Int16,
        Int24,
        Int32,
    };

    AsioRenderer() = default;
    ~AsioRenderer();

    AsioRenderer(const AsioRenderer&) = delete;
    AsioRenderer& operator=(const AsioRenderer&) = delete;

    bool Start(const std::wstring& deviceId,
               const WAVEFORMATEXTENSIBLE& format,
               std::int32_t prebufferMs,
               std::int32_t maxBufferOffsetMs,
               std::uint32_t requestedBufferFrames,
               std::wstring* outError);
    void Stop();
    std::uint32_t PushPcm(const std::uint8_t* data,
                          std::uint32_t frameCount,
                          std::wstring* outError = nullptr);
    RendererStats GetStats() const;
    bool IsRunning() const;

    void OnAsioBufferSwitch(long doubleBufferIndex);
    void OnAsioSampleRateChanged(ASIOSampleRate sampleRate);
    long OnAsioMessage(long selector, long value, void* message, double* opt);

private:
    struct SilenceBucket {
        std::uint64_t second = 0;
        std::int64_t outputFrames = 0;
        std::int64_t silentFrames = 0;
    };

    struct SilenceWindowStats {
        std::int64_t outputFrames = 0;
        std::int64_t silentFrames = 0;
        double silentPercent = 0.0;
    };

    enum ResetReason : std::uint32_t {
        kResetRequest = 1U << 0U,
        kBufferSizeChange = 1U << 1U,
        kLatenciesChanged = 1U << 2U,
    };

    void ControlLoop(std::wstring deviceId, std::uint32_t requestedBufferFrames);
    bool OpenDriverOnControlThread(const std::wstring& deviceId,
                                   std::uint32_t requestedBufferFrames,
                                   std::wstring* outError);
    bool CreateBuffersOnControlThread(std::uint32_t requestedBufferFrames,
                                      bool resetRingBuffer,
                                      std::wstring* outError);
    bool RebuildBuffersOnControlThread(std::uint32_t requestedBufferFrames,
                                       std::wstring* outError);
    void DisposeBuffersOnControlThread();
    void CloseDriverOnControlThread();
    bool TryStartStreamIfReady(std::wstring* outError);
    void PaddingLoop();
    void MaintainPadding();
    void FillOutputBuffer(long doubleBufferIndex);
    void FillOutputBufferWithSilence(long doubleBufferIndex);
    void WriteConvertedOutput(const std::uint8_t* interleaved,
                              std::uint32_t frameCount,
                              long doubleBufferIndex);
    float ReadSourceFloat(const std::uint8_t* interleaved,
                          std::uint32_t frameIndex,
                          std::uint32_t channelIndex) const;
    std::int32_t ReadSourceInt32(const std::uint8_t* interleaved,
                                 std::uint32_t frameIndex,
                                 std::uint32_t channelIndex) const;
    void ResetStats();
    void RecordOutputFrames(std::uint32_t outputFrames, std::uint32_t silentFrames);
    SilenceWindowStats GetRecentSilenceStats() const;

    mutable std::mutex mutex_;
    std::thread controlThread_;
    std::thread paddingThread_;
    std::mutex controlMutex_;
    std::condition_variable initCv_;
    std::condition_variable controlCv_;
    std::condition_variable startCv_;
    std::mutex paddingWaitMutex_;
    std::condition_variable paddingCv_;
    std::mutex producerMutex_;
    bool initComplete_ = false;
    bool initSucceeded_ = false;
    bool shutdownRequested_ = false;
    std::uint64_t startRequestSerial_ = 0;
    std::uint64_t startHandledSerial_ = 0;
    bool lastStartSucceeded_ = false;
    std::wstring initError_;
    std::wstring lastStartError_;

    IASIO* asioDriver_ = nullptr;
    ASIOBufferInfo bufferInfos_[2]{};
    ASIOChannelInfo channelInfos_[2]{};
    ASIOCallbacks callbacks_{};
    bool buffersCreated_ = false;
    bool coInitialized_ = false;
    std::atomic<std::uint32_t> pendingResetMask_{0};

    std::atomic<bool> running_{false};
    std::atomic<bool> streamActive_{false};
    std::atomic<bool> prebuffering_{false};

    WAVEFORMATEXTENSIBLE format_{};
    SourceSampleKind sourceKind_ = SourceSampleKind::Unknown;
    OutputSampleKind outputKind_ = OutputSampleKind::Unknown;
    ASIOSampleType outputAsioSampleType_ = ASIOSTLastEntry;
    std::uint32_t outputRightShift_ = 0;
    std::uint32_t sourceChannels_ = 0;
    std::uint32_t sourceBytesPerSample_ = 0;
    std::uint32_t bytesPerFrame_ = 0;
    std::uint32_t bufferFrames_ = 0;
    std::uint32_t requestedBufferFrames_ = 0;
    std::uint32_t minBufferFrames_ = 0;
    std::uint32_t maxBufferFrames_ = 0;
    std::uint32_t preferredBufferFrames_ = 0;
    long bufferGranularity_ = 0;
    std::uint32_t sampleRate_ = 0;
    std::uint32_t asioSampleRate_ = 0;
    std::uint32_t prebufferFrames_ = 0;
    std::int32_t prebufferMs_ = 300;
    std::uint32_t maxBufferOffsetFrames_ = 0;
    std::int32_t maxBufferOffsetMs_ = 100;
    bool paddingActive_ = false;
    RawFrameRingBuffer ringBuffer_;
    std::vector<std::uint8_t> callbackBuffer_;

    std::atomic<std::int64_t> totalFramesQueued_{0};
    std::atomic<std::int64_t> totalFramesPlayed_{0};
    std::atomic<std::int64_t> totalFramesDropped_{0};
    std::atomic<std::int64_t> totalOutputFrames_{0};
    std::atomic<std::int64_t> totalSilentFrames_{0};
    std::atomic<std::int64_t> underrunCount_{0};
    std::atomic<std::int64_t> asioResetRequests_{0};
    std::atomic<std::int64_t> asioBufferSizeChanges_{0};
    std::atomic<std::int64_t> asioLatencyChanges_{0};
    std::atomic<std::int64_t> asioRebuildCount_{0};
    std::atomic<std::int32_t> asioLastMessage_{0};
    mutable std::mutex silenceStatsMutex_;
    std::array<SilenceBucket, 60> silenceBuckets_{};
};

}  // namespace audiobridge

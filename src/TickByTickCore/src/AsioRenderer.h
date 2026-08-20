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

namespace tickbytick {

enum class PrebufferTransitionReason : std::int32_t {
    None = 0,
    InitialFill = 1,
    LowWater = 2,
    Refilled = 3,
    DrainBegin = 4,
    DrainEnd = 5,
    Stop = 6,
    Fault = 7,
};

struct RendererStats {
    bool startAccepted = false;
    bool streamActive = false;
    bool preparedStartPending = false;
    bool silentStartDeferred = false;
    std::int32_t startupWaitMs = 0;
    bool prebuffering = false;
    std::uint32_t sourceSampleRate = 0;
    std::int64_t totalFramesQueued = 0;
    std::int64_t totalPlayerSilentFrames = 0;
    std::int64_t totalFramesPlayed = 0;
    std::int64_t totalFramesDropped = 0;
    std::int64_t totalOutputFrames = 0;
    std::int64_t totalSilentFrames = 0;
    std::int64_t totalLogicalFrames = 0;
    std::int64_t totalBridgeSilentFramesQueued = 0;
    std::int64_t totalBridgeSilentFramesPlayed = 0;
    std::int64_t totalBridgeSilentFramesReplaced = 0;
    std::int64_t compensationBridgeFrames = 0;
    std::int64_t bufferedFrames = 0;
    std::int32_t bufferedMs = 0;
    std::int64_t bufferCapacityFrames = 0;
    std::int32_t bufferCapacityMs = 0;
    std::int64_t prebufferTargetFrames = 0;
    std::int32_t prebufferTargetMs = 0;
    std::int64_t applicationBufferFrames = 0;
    std::int32_t applicationBufferMs = 0;
    std::int64_t effectiveTimelineFrames = 0;
    std::int32_t effectiveTimelineMs = 0;
    std::uint32_t maximumRealPacketFrames = 0;
    std::int64_t underrunCount = 0;
    std::int64_t recentOutputFrames = 0;
    std::int64_t recentSilentFrames = 0;
    double recentSilentPercent = 0.0;
    std::uint64_t prebufferEnterCount = 0;
    std::uint64_t prebufferExitCount = 0;
    std::int32_t lastPrebufferTransition = 0;
    std::int64_t lastPrebufferTransitionFrames = 0;
    std::int32_t asioRequestedBufferFrames = 0;
    std::int32_t asioActualBufferFrames = 0;
    std::int32_t asioMinBufferFrames = 0;
    std::int32_t asioMaxBufferFrames = 0;
    std::int32_t asioPreferredBufferFrames = 0;
    std::int32_t asioBufferGranularity = 0;
    std::int32_t asioOutputSampleType = 0;
    std::int32_t sampleConversionMode = 0;
    std::uint32_t asioSampleRate = 0;
    std::int64_t asioResetRequests = 0;
    std::int64_t asioBufferSizeChanges = 0;
    std::int64_t asioLatencyChanges = 0;
    std::int64_t asioRebuildCount = 0;
    std::int32_t asioLastMessage = 0;
    std::int32_t asioClockSourceIndex = -1;
    std::int32_t callbackRealtimeMode = 0;
    std::int32_t asioOutputReadyState = 0;
};

struct AsioClockSourceInfo {
    std::int32_t index = -1;
    std::int32_t associatedChannel = -1;
    std::int32_t associatedGroup = -1;
    bool isCurrent = false;
    std::wstring name;
};

bool QueryAsioClockSources(const std::wstring& deviceId,
                           std::vector<AsioClockSourceInfo>* sources,
                           std::wstring* outError);

class RawFrameRingBuffer {
public:
    enum class FrameOwner : std::uint8_t {
        Bridge = 0,
        Player = 1,
    };

    struct DispatchResult {
        std::uint32_t frames = 0;
        std::uint32_t playerFrames = 0;
        std::uint32_t bridgeFrames = 0;
        std::uint64_t endIndex = 0;
    };

    struct WriteBatch {
        std::uint64_t startIndex = 0;
        std::uint64_t cursor = 0;
        bool valid = false;
    };

    bool Reset(std::uint32_t bytesPerFrame, std::uint32_t capacityFrames);
    void Clear();
    std::uint32_t Push(const std::uint8_t* data, std::uint32_t frameCount);
    std::uint32_t PushCapturedSilence(std::uint32_t frameCount);
    std::uint32_t PushBridgeSilence(std::uint32_t frameCount);
    WriteBatch BeginWriteBatch() const;
    std::uint32_t StageWrite(WriteBatch* batch,
                             const std::uint8_t* data,
                             std::uint32_t frameCount,
                             FrameOwner owner);
    bool CommitWriteBatch(const WriteBatch& batch);
    DispatchResult Dispatch(std::uint8_t* data, std::uint32_t frameCount);
    bool ConfirmDispatch(std::uint64_t endIndex);
    void RollbackDispatch();
    std::uint64_t DispatchPosition() const;
    std::uint64_t ConfirmedPositionFrames() const;
    std::uint64_t WritePositionFrames() const;
    std::uint64_t CountPlayerFrames(std::uint64_t beginFrame,
                                    std::uint64_t endFrame) const;
    std::uint32_t AvailableReadFrames() const;
    std::uint32_t PendingFrames() const;
    std::uint32_t CapacityFrames() const;

private:
    // Single serialized producer / single consumer (ASIO callback). Frames
    // remain owned by the ring after dispatch and become writable again only
    // after the corresponding ASIO page is retired and confirmed.
    std::uint64_t AvailableReadBytes(std::uint64_t readIndex,
                                     std::uint64_t writeIndex) const;
    std::size_t AvailableWriteBytes(std::uint64_t readIndex,
                                    std::uint64_t writeIndex) const;
    void CopyInto(std::uint64_t writeIndex,
                  const std::uint8_t* data,
                  std::size_t bytes);
    void ZeroInto(std::uint64_t writeIndex, std::size_t bytes);
    void CopyOut(std::uint64_t readIndex, std::uint8_t* data, std::size_t bytes) const;
    std::uint32_t PushSilence(std::uint32_t frameCount, FrameOwner owner);
    void MarkFrames(std::uint64_t beginByteIndex,
                    std::uint32_t frameCount,
                    FrameOwner owner);

    std::vector<std::uint8_t> bytes_;
    std::vector<std::uint8_t> frameOwners_;
    std::size_t byteMask_ = 0;
    std::atomic<std::uint64_t> confirmedReadIndex_{0};
    std::atomic<std::uint64_t> dispatchReadIndex_{0};
    std::atomic<std::uint64_t> writeIndex_{0};
    std::uint32_t bytesPerFrame_ = 0;
};

class AsioRenderer {
public:
    enum class StartMode : std::uint8_t {
        Normal = 0,
        PreparedHandoff = 1,
    };

    enum class SourceSampleKind {
        Unknown,
        Float32,
        Pcm16,
        Pcm24,
        Pcm32,
    };

    struct CapturedBatchItem {
        const std::uint8_t* data = nullptr;
        std::uint32_t frameCount = 0;
        bool silent = false;
    };

    AsioRenderer() = default;
    ~AsioRenderer();

    AsioRenderer(const AsioRenderer&) = delete;
    AsioRenderer& operator=(const AsioRenderer&) = delete;

    bool Start(const std::wstring& deviceId,
               const WAVEFORMATEXTENSIBLE& format,
               std::int32_t prebufferMs,
               std::int32_t maxBufferAdvanceMs,
               std::uint32_t applicationBufferFrames,
               std::uint32_t requestedBufferFrames,
               std::int32_t requestedClockSourceIndex,
               std::wstring* outError,
               StartMode startMode = StartMode::Normal);
    void Stop();
    std::uint32_t PushPcm(const std::uint8_t* data,
                          std::uint32_t frameCount,
                          std::wstring* outError = nullptr);
    std::uint32_t PushCapturedSilence(std::uint32_t frameCount,
                                      std::wstring* outError = nullptr);
    std::uint32_t PushPreparedPcm(const std::uint8_t* data,
                                  std::uint32_t frameCount,
                                  std::wstring* outError = nullptr);
    std::uint32_t PushPreparedSilence(std::uint32_t frameCount,
                                      std::wstring* outError = nullptr);
    bool PushCapturedBatch(const std::vector<CapturedBatchItem>& items,
                           bool prepared,
                           std::uint64_t* outWrittenFrames,
                           std::wstring* outError = nullptr);
    bool CommitPreparedStart(std::wstring* outError = nullptr);
    bool ActivatePreparedOutput(std::wstring* outError = nullptr);
    RendererStats GetStats() const;
    bool IsRunning() const;
    std::int64_t ConfirmedCapturedFrames() const;
    std::int64_t LogicalConsumedFrames() const;
    std::int64_t ConfirmedOutputFrames() const;
    std::int64_t PendingCapturedFrames() const;
    std::int64_t PendingTimelineFrames() const;
    std::int64_t AvailableTimelineWriteFrames() const;
    std::int64_t AdmissionRetentionFrames();
    bool EnsureStreamStarted(std::wstring* outError = nullptr);
    bool StartDeferredSilenceIfDue(std::uint32_t delayMs,
                                   std::wstring* outError = nullptr);
    bool IsDeferredSilenceStartDue(std::uint32_t delayMs) const;
    bool WaitForFirstOutputCallback(std::wstring* outError = nullptr);
    bool HasFirstOutputCallbackTimedOut() const;
    void PrimeTimeline();
    void MaintainTimeline();
    void BeginCapturedDrain();
    void SettleCapturedDrain();
    void EndCapturedDrain();
    bool HasFault() const;
    std::wstring FaultMessage() const;

    void OnAsioBufferSwitch(long doubleBufferIndex, const ASIOTime* timeInfo = nullptr);
    void OnAsioSampleRateChanged(ASIOSampleRate sampleRate);
    long OnAsioMessage(long selector, long value, void* message, double* opt);

private:
    enum class SampleConversionMode : std::int32_t {
        Direct = 0,
        FloatToInteger = 1,
        IntegerToFloat = 2,
        IntegerBitDepth = 3,
    };

    enum class AsioFaultCode : std::uint32_t {
        None = 0,
        SampleRateChanged = 1,
        ResetRequested = 2,
        BufferSizeChanged = 3,
        ResyncRequested = 4,
        Overload = 5,
        ReenteredCallback = 6,
        InvalidBufferIndex = 7,
        OutputPageOrder = 8,
        OutputConfirmation = 9,
        InvalidRendererBuffers = 10,
    };

    enum class FirstOutputCallbackState : LONG {
        Awaiting = 0,
        Completed = 1,
        Cancelled = 2,
    };

    struct OutputPageLedger {
        bool valid = false;
        std::uint64_t sequence = 0;
        std::uint64_t dispatchEndIndex = 0;
        std::uint32_t outputFrames = 0;
        std::uint32_t capturedFrames = 0;
        std::uint32_t bridgeSilentFrames = 0;
        std::uint32_t compensationBridgeFrames = 0;
        std::uint32_t managedSilentFrames = 0;
        std::uint32_t underrunSilentFrames = 0;
    };

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

    void ControlLoop(std::wstring deviceId,
                     std::uint32_t requestedBufferFrames,
                     std::int32_t requestedClockSourceIndex);
    bool OpenDriverOnControlThread(const std::wstring& deviceId,
                                   std::uint32_t requestedBufferFrames,
                                   std::int32_t requestedClockSourceIndex,
                                   std::wstring* outError);
    bool CreateBuffersOnControlThread(std::uint32_t requestedBufferFrames,
                                      bool resetRingBuffer,
                                      std::wstring* outError);
    void DisposeBuffersOnControlThread();
    void CloseDriverOnControlThread();
    bool TryStartStreamIfReady(std::wstring* outError);
    void ResetFirstOutputCallbackGate() noexcept;
    void CancelFirstOutputCallbackGate() noexcept;
    std::uint32_t PushCapturedFrames(const std::uint8_t* data,
                                     std::uint32_t frameCount,
                                     bool silence,
                                     bool prepared,
                                     std::wstring* outError);
    const std::uint8_t* ConvertCapturedFramesLocked(const std::uint8_t* data,
                                                    std::uint32_t frameCount);
    void MaintainTimelineLocked();
    void UpdateLogicalAdmissionLocked();
    bool ConfirmOutputPage(long doubleBufferIndex);
    void RollbackOutputPages();
    void ReconcileCompensationBridgeForCallback(bool draining) noexcept;
    std::uint32_t ClaimCompensationBridgeForCallback(
            std::uint32_t maximumFrames) noexcept;
    bool RetireCompensationBridgeForCallback(
            std::uint32_t frameCount) noexcept;
    std::uint32_t CancelQueuedCompensationBridge(
            bool countAsReplaced) noexcept;
    void RequestAsioFault(AsioFaultCode code, std::uint32_t detail = 0) noexcept;
    void FinalizePendingAsioFault();
    bool LatchFault(const std::wstring& message);
    void FillOutputBuffer(long doubleBufferIndex);
    void FillOutputBufferWithSilence(long doubleBufferIndex);
    void NotifyOutputReady();
    void SetPrebuffering(bool enabled,
                         PrebufferTransitionReason reason,
                         std::uint32_t availableFrames);
    void WriteDirectOutput(const std::uint8_t* interleaved,
                           std::uint32_t frameCount,
                           long doubleBufferIndex);
    void ResetStats();
    void RecordOutputFrames(std::uint32_t outputFrames, std::uint32_t silentFrames);
    SilenceWindowStats GetRecentSilenceStats(
            std::int64_t totalOutputFrames,
            std::int64_t totalSilentFrames) const;

    mutable std::mutex mutex_;
    std::thread controlThread_;
    std::mutex controlMutex_;
    std::condition_variable initCv_;
    std::condition_variable controlCv_;
    std::condition_variable startCv_;
    std::mutex producerMutex_;
    std::atomic<std::uint32_t> callbackWaiterCount_{0};
    bool initComplete_ = false;
    bool initSucceeded_ = false;
    bool shutdownRequested_ = false;
    std::uint64_t startRequestSerial_ = 0;
    std::uint64_t startHandledSerial_ = 0;
    bool lastStartSucceeded_ = false;
    std::wstring initError_;
    std::wstring lastStartError_;
    std::atomic<bool> faultStopRequested_{false};

    IASIO* asioDriver_ = nullptr;
    ASIOBufferInfo bufferInfos_[2]{};
    ASIOChannelInfo channelInfos_[2]{};
    ASIOCallbacks callbacks_{};
    bool buffersCreated_ = false;
    bool coInitialized_ = false;
    std::atomic<bool> driverQuiesceFailed_{false};
    std::atomic<bool> running_{false};
    // IASIO::start() returning success does not prove that the driver has
    // entered its output callback loop.
    std::atomic<bool> startAccepted_{false};
    std::atomic<bool> preparedStartPending_{false};
    // A prepared driver may synchronously callback from inside IASIO::start.
    // Keep staged PCM undispatchable until Core has atomically published the
    // matching bank as Active.
    std::atomic<bool> preparedOutputActive_{true};
    std::atomic<std::uint64_t> startAcceptedAtMs_{0};
    std::atomic<std::uint64_t> deferredSilentStartAtMs_{0};
    // Tri-state WaitOnAddress gate. Cancellation changes the observed value
    // before waking waiters, so a Stop/Fault wake cannot be lost.
    volatile LONG firstOutputCallbackState_ =
            static_cast<LONG>(FirstOutputCallbackState::Awaiting);
    std::atomic<bool> prebuffering_{false};
    std::atomic_flag callbackActive_ = ATOMIC_FLAG_INIT;
    volatile LONG callbackExecuting_ = FALSE;
    std::atomic<std::int32_t> callbackRealtimeMode_{0};

    WAVEFORMATEXTENSIBLE format_{};
    StartMode startMode_ = StartMode::Normal;
    SourceSampleKind sourceKind_ = SourceSampleKind::Unknown;
    SampleConversionMode sampleConversionMode_ = SampleConversionMode::Direct;
    ASIOSampleType outputAsioSampleType_ = ASIOSTLastEntry;
    std::uint32_t outputBytesPerSample_ = 0;
    std::atomic<std::int32_t> outputReadyState_{0};
    std::uint32_t sourceChannels_ = 0;
    std::uint32_t sourceBytesPerSample_ = 0;
    std::uint32_t sourceBytesPerFrame_ = 0;
    std::uint32_t outputBytesPerFrame_ = 0;
    std::uint32_t bufferFrames_ = 0;
    std::uint32_t requestedBufferFrames_ = 0;
    std::uint32_t minBufferFrames_ = 0;
    std::uint32_t maxBufferFrames_ = 0;
    std::uint32_t preferredBufferFrames_ = 0;
    long bufferGranularity_ = 0;
    std::uint32_t sampleRate_ = 0;
    std::uint32_t asioSampleRate_ = 0;
    std::int32_t asioClockSourceIndex_ = -1;
    std::uint32_t prebufferFrames_ = 0;
    std::int32_t prebufferMs_ = 300;
    std::uint32_t applicationBufferFrames_ = 0;
    std::uint32_t effectiveTimelineFrames_ = 0;
    std::uint32_t maxBufferAdvanceFrames_ = 0;
    std::int32_t maxBufferAdvanceMs_ = 200;
    std::uint32_t minimumTimelineFrames_ = 0;
    std::uint64_t admittedTimelineEndFrame_ = 0;
    RawFrameRingBuffer ringBuffer_;
    std::vector<std::uint8_t> callbackBuffer_;
    std::vector<std::uint8_t> conversionBuffer_;
    std::array<OutputPageLedger, 2> outputPageLedgers_{};
    std::uint64_t nextDispatchSequence_ = 1;
    std::uint64_t nextConfirmSequence_ = 1;
    bool awaitingFirstBufferSwitch_ = true;
    std::atomic<std::int64_t> totalFramesQueued_{0};
    std::atomic<std::int64_t> totalPlayerSilentFrames_{0};
    std::atomic<std::int64_t> totalFramesPlayed_{0};
    std::atomic<std::int64_t> totalFramesDropped_{0};
    std::atomic<std::int64_t> totalOutputFrames_{0};
    std::atomic<std::int64_t> totalSilentFrames_{0};
    std::atomic<std::int64_t> totalLogicalFrames_{0};
    std::atomic<std::int64_t> totalBridgeSilentFramesQueued_{0};
    std::atomic<std::int64_t> totalBridgeSilentFramesPlayed_{0};
    std::atomic<std::int64_t> totalBridgeSilentFramesReplaced_{0};
    // High 32 bits: queued, replaceable virtual Bridge frames. Low 32 bits:
    // frames already dispatched in ASIO pages. A single packed CAS keeps the
    // transfer queued -> in-flight and drain cancellation indivisible on x86
    // and x64; the callback remains the only ordinary state writer.
    alignas(8) std::atomic<std::uint64_t> compensationBridgeState_{0};
    // Callback-side seqlock. Producers advance logical admission only from a
    // stable Ring-position + virtual-Bridge snapshot.
    std::atomic<std::uint64_t> compensationStateSequence_{0};
    std::atomic<std::uint64_t> compensationBridgeCancelSerial_{0};
    std::atomic<std::uint64_t> compensationBridgeBarrierFrame_{0};
    std::uint64_t observedCompensationBridgeCancelSerial_ = 0;
    std::atomic<std::uint32_t> maximumRealPacketFrames_{0};
    std::atomic<std::int64_t> underrunCount_{0};
    std::atomic<std::uint64_t> prebufferEnterCount_{0};
    std::atomic<std::uint64_t> prebufferExitCount_{0};
    std::atomic<std::int32_t> lastPrebufferTransition_{0};
    std::atomic<std::int64_t> lastPrebufferTransitionFrames_{0};
    std::atomic<bool> capturedDrainActive_{false};
    std::atomic<std::int64_t> asioResetRequests_{0};
    std::atomic<std::int64_t> asioBufferSizeChanges_{0};
    std::atomic<std::int64_t> asioLatencyChanges_{0};
    std::atomic<std::int64_t> asioRebuildCount_{0};
    std::atomic<std::int32_t> asioLastMessage_{0};
    std::atomic<std::uint64_t> pendingAsioFault_{0};
    std::atomic<bool> faultRequested_{false};
    std::atomic<bool> faulted_{false};
    mutable std::mutex faultMutex_;
    std::wstring faultMessage_;
    mutable std::mutex silenceStatsMutex_;
    mutable std::array<SilenceBucket, 60> silenceBuckets_{};
    mutable std::int64_t sampledOutputFrames_ = 0;
    mutable std::int64_t sampledSilentFrames_ = 0;
};

}  // namespace tickbytick

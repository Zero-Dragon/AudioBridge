#pragma once

#include <cstdint>
#include <limits>

namespace tickbytick::compensation_bridge {

// queued frames are still replaceable. in-flight frames have already been
// handed to an ASIO page and are therefore irrevocable until that page is
// confirmed. Keeping both counters in one 64-bit value lets the realtime path
// move queued -> in-flight with one compare/exchange.
struct PackedState {
    std::uint32_t queuedFrames = 0;
    std::uint32_t inFlightFrames = 0;
};

constexpr std::uint64_t PackState(std::uint32_t queuedFrames,
                                  std::uint32_t inFlightFrames) noexcept {
    return (static_cast<std::uint64_t>(queuedFrames) << 32U) |
           inFlightFrames;
}

constexpr PackedState UnpackState(std::uint64_t packed) noexcept {
    return {
            static_cast<std::uint32_t>(packed >> 32U),
            static_cast<std::uint32_t>(packed)};
}

constexpr std::uint64_t OutstandingFrames(std::uint64_t packed) noexcept {
    const PackedState state = UnpackState(packed);
    return static_cast<std::uint64_t>(state.queuedFrames) +
           state.inFlightFrames;
}

constexpr std::uint64_t SaturatingAdd(std::uint64_t left,
                                      std::uint64_t right) noexcept {
    return right > (std::numeric_limits<std::uint64_t>::max)() - left
            ? (std::numeric_limits<std::uint64_t>::max)()
            : left + right;
}

inline bool TryAddQueued(std::uint64_t packed,
                         std::uint32_t frames,
                         std::uint64_t* next) noexcept {
    if (next == nullptr) {
        return false;
    }
    const PackedState state = UnpackState(packed);
    if (frames > (std::numeric_limits<std::uint32_t>::max)() -
                         state.queuedFrames) {
        return false;
    }
    *next = PackState(state.queuedFrames + frames, state.inFlightFrames);
    return true;
}

inline bool TryMoveQueuedToInFlight(std::uint64_t packed,
                                    std::uint32_t frames,
                                    std::uint64_t* next) noexcept {
    if (next == nullptr) {
        return false;
    }
    const PackedState state = UnpackState(packed);
    if (frames > state.queuedFrames ||
        frames > (std::numeric_limits<std::uint32_t>::max)() -
                         state.inFlightFrames) {
        return false;
    }
    *next = PackState(
            state.queuedFrames - frames,
            state.inFlightFrames + frames);
    return true;
}

inline bool TryConfirmInFlight(std::uint64_t packed,
                               std::uint32_t frames,
                               std::uint64_t* next) noexcept {
    if (next == nullptr) {
        return false;
    }
    const PackedState state = UnpackState(packed);
    if (frames > state.inFlightFrames) {
        return false;
    }
    *next = PackState(
            state.queuedFrames,
            state.inFlightFrames - frames);
    return true;
}

constexpr std::uint64_t CancelAllQueued(std::uint64_t packed) noexcept {
    return PackState(0, UnpackState(packed).inFlightFrames);
}

constexpr std::uint32_t RequiredQueuedFrames(
        std::uint32_t minimumTimelineFrames,
        std::uint64_t physicalPendingFrames,
        std::uint64_t packed) noexcept {
    const std::uint64_t retained = SaturatingAdd(
            physicalPendingFrames, OutstandingFrames(packed));
    return retained >= minimumTimelineFrames
            ? 0
            : static_cast<std::uint32_t>(minimumTimelineFrames - retained);
}

// Replacement is deliberately all-or-nothing for one queued virtual segment.
// trailingPhysicalFrames proves that a complete replacement exists behind its
// barrier. The second condition proves that removing the segment still leaves
// the protected low-water floor intact; trailing >= queued alone is not enough
// after older physical frames have been confirmed.
constexpr bool CanReplaceWholeQueued(
        std::uint32_t minimumTimelineFrames,
        std::uint64_t physicalPendingFrames,
        std::uint64_t trailingPhysicalFrames,
        std::uint64_t packed) noexcept {
    const PackedState state = UnpackState(packed);
    if (state.queuedFrames == 0 ||
        trailingPhysicalFrames < state.queuedFrames) {
        return false;
    }
    return SaturatingAdd(physicalPendingFrames, state.inFlightFrames) >=
           minimumTimelineFrames;
}

constexpr std::uint64_t PhysicalHorizonBudget(
        std::uint64_t prebufferFrames,
        std::uint64_t packed) noexcept {
    const std::uint64_t compensation = OutstandingFrames(packed);
    return compensation >= prebufferFrames
            ? 0
            : prebufferFrames - compensation;
}

constexpr std::uint64_t LogicalTargetEnd(
        std::uint64_t confirmedPhysicalFrame,
        std::uint64_t writePhysicalFrame,
        std::uint64_t prebufferFrames,
        std::uint64_t packed) noexcept {
    const std::uint64_t horizonEnd = SaturatingAdd(
            confirmedPhysicalFrame,
            PhysicalHorizonBudget(prebufferFrames, packed));
    return writePhysicalFrame < horizonEnd ? writePhysicalFrame : horizonEnd;
}

constexpr bool IsStableSequenceSnapshot(std::uint64_t before,
                                        std::uint64_t after) noexcept {
    return (before & 1U) == 0U && before == after;
}

// A q -> in-flight move is only tentative until the callback has checked the
// same cancellation generation again.  Keeping this decision in the shared
// policy prevents the realtime integration and its tests from implementing
// subtly different prepared/drain/Stop gates.
constexpr bool ShouldKeepTentativeClaim(
        bool rendererRunning,
        bool preparedOutputActive,
        bool draining,
        std::uint32_t minimumTimelineFrames,
        std::uint64_t observedCancelSerial,
        std::uint64_t claimCancelSerial,
        std::uint64_t currentCancelSerial) noexcept {
    return rendererRunning && preparedOutputActive && !draining &&
           minimumTimelineFrames != 0 &&
           observedCancelSerial == claimCancelSerial &&
           claimCancelSerial == currentCancelSerial;
}

constexpr bool MayPublishQueued(bool rendererRunning,
                                bool preparedOutputActive,
                                bool draining,
                                std::uint32_t minimumTimelineFrames,
                                std::uint64_t cancelSerialBefore,
                                std::uint64_t cancelSerialAfter) noexcept {
    return ShouldKeepTentativeClaim(rendererRunning,
                                    preparedOutputActive,
                                    draining,
                                    minimumTimelineFrames,
                                    cancelSerialBefore,
                                    cancelSerialBefore,
                                    cancelSerialAfter);
}

struct PagePlan {
    bool valid = false;
    std::uint32_t physicalPrefixFrames = 0;
    std::uint32_t compensationFrames = 0;
    std::uint32_t physicalSuffixFrames = 0;
    std::uint32_t unfilledFrames = 0;
    std::uint64_t dispatchEndFrame = 0;
};

// A queued segment is anchored at an absolute physical Ring position. Already
// accepted physical frames before the barrier are always dispatched first.
// Physical frames behind the barrier become eligible only after the complete
// queued segment has either been claimed into this page or replaced.
constexpr PagePlan PlanPage(std::uint64_t dispatchPhysicalFrame,
                            std::uint64_t writePhysicalFrame,
                            std::uint64_t barrierPhysicalFrame,
                            std::uint64_t packed,
                            std::uint32_t pageFrames) noexcept {
    PagePlan plan{};
    plan.dispatchEndFrame = dispatchPhysicalFrame;
    if (dispatchPhysicalFrame > writePhysicalFrame) {
        return plan;
    }

    const PackedState state = UnpackState(packed);
    std::uint64_t cursor = dispatchPhysicalFrame;
    std::uint32_t remaining = pageFrames;
    if (state.queuedFrames == 0) {
        const std::uint64_t available = writePhysicalFrame - cursor;
        plan.physicalPrefixFrames = static_cast<std::uint32_t>(
                available < remaining ? available : remaining);
        cursor += plan.physicalPrefixFrames;
        remaining -= plan.physicalPrefixFrames;
        plan.unfilledFrames = remaining;
        plan.dispatchEndFrame = cursor;
        plan.valid = true;
        return plan;
    }

    if (cursor > barrierPhysicalFrame ||
        barrierPhysicalFrame > writePhysicalFrame) {
        return plan;
    }

    const std::uint64_t beforeBarrier = barrierPhysicalFrame - cursor;
    plan.physicalPrefixFrames = static_cast<std::uint32_t>(
            beforeBarrier < remaining ? beforeBarrier : remaining);
    cursor += plan.physicalPrefixFrames;
    remaining -= plan.physicalPrefixFrames;

    if (remaining != 0 && cursor == barrierPhysicalFrame) {
        plan.compensationFrames = state.queuedFrames < remaining
                ? state.queuedFrames
                : remaining;
        remaining -= plan.compensationFrames;

        // Only a complete claim opens the barrier. A partial claim must leave
        // all trailing physical frames for a later callback page.
        if (plan.compensationFrames == state.queuedFrames && remaining != 0) {
            const std::uint64_t afterBarrier = writePhysicalFrame - cursor;
            plan.physicalSuffixFrames = static_cast<std::uint32_t>(
                    afterBarrier < remaining ? afterBarrier : remaining);
            cursor += plan.physicalSuffixFrames;
            remaining -= plan.physicalSuffixFrames;
        }
    }

    plan.unfilledFrames = remaining;
    plan.dispatchEndFrame = cursor;
    plan.valid = true;
    return plan;
}

}  // namespace tickbytick::compensation_bridge

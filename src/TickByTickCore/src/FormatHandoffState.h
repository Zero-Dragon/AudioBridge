#pragma once

#include <cstdint>
#include <limits>

namespace tickbytick::format_handoff {

enum class Phase : std::uint8_t {
    Active = 0,
    Preparing = 1,
    Draining = 2,
    Reconfiguring = 3,
    Arming = 4,
    Faulted = 5,
};

struct Target {
    std::uint32_t pid = 0;
    std::uint32_t streamEpoch = 0;
    std::uint64_t streamId = 0;
    std::uint64_t formatKey = 0;

    constexpr bool IsValid() const noexcept {
        return pid != 0 && streamEpoch != 0 && streamId != 0;
    }
};

constexpr bool operator==(const Target& left, const Target& right) noexcept {
    return left.pid == right.pid &&
           left.streamEpoch == right.streamEpoch &&
           left.streamId == right.streamId &&
           left.formatKey == right.formatKey;
}

constexpr bool operator!=(const Target& left, const Target& right) noexcept {
    return !(left == right);
}

enum class RequestDisposition : std::uint8_t {
    Rejected = 0,
    NoChange = 1,
    Created = 2,
    Replaced = 3,
    CancelledBackToActive = 4,
};

struct RequestResult {
    RequestDisposition disposition = RequestDisposition::Rejected;
    std::uint64_t epoch = 0;

    constexpr bool Accepted() const noexcept {
        return disposition != RequestDisposition::Rejected;
    }

    constexpr bool Changed() const noexcept {
        return disposition == RequestDisposition::Created ||
               disposition == RequestDisposition::Replaced ||
               disposition == RequestDisposition::CancelledBackToActive;
    }
};

struct ReconfigureRequest {
    bool valid = false;
    std::uint64_t epoch = 0;
    Target target{};
};

enum class ReconfigureCompletion : std::uint8_t {
    Unexpected = 0,
    ReadyToArm = 1,
    Stale = 2,
    FailedCurrent = 3,
};

// Pure control-plane model for a two-bank format handoff. It deliberately
// knows nothing about ASIO, PCM storage, locks, or threads. Callers retain one
// committed Active bank and at most one replaceable latest-wins Standby bank.
class FormatHandoffState final {
public:
    FormatHandoffState() = default;

    explicit FormatHandoffState(Target initialActive) noexcept {
        InitializeActive(initialActive);
    }

    bool InitializeActive(Target target) noexcept {
        if (!target.IsValid() || phase_ == Phase::Faulted || activeValid_ ||
            desiredValid_ || inFlightEpoch_ != 0 || armedEpoch_ != 0) {
            return false;
        }

        const std::uint64_t epoch = AllocateEpoch();
        if (epoch == 0) {
            return false;
        }
        active_ = target;
        activeEpoch_ = epoch;
        activeValid_ = true;
        activeDrained_ = false;
        phase_ = Phase::Active;
        return true;
    }

    RequestResult Request(Target target) noexcept {
        if (!target.IsValid() || phase_ == Phase::Faulted) {
            return {};
        }

        if (phase_ == Phase::Active && activeValid_ && target == active_) {
            return {RequestDisposition::NoChange, activeEpoch_};
        }
        if (desiredValid_ && target == desired_) {
            return {RequestDisposition::NoChange, desiredEpoch_};
        }

        // Before driver reconfiguration begins, returning to the committed
        // Active target cancels the uncommitted standby and resumes ordinary
        // low-water maintenance. Once reconfiguration has begun, even the old
        // target must be requested as a new epoch because the driver may no
        // longer be configured for it.
        if (activeValid_ && target == active_ &&
            (phase_ == Phase::Preparing || phase_ == Phase::Draining)) {
            ClearPending();
            activeDrained_ = false;
            phase_ = Phase::Active;
            return {RequestDisposition::CancelledBackToActive, activeEpoch_};
        }

        const bool replacing = desiredValid_ ||
                phase_ == Phase::Reconfiguring || phase_ == Phase::Arming;
        const std::uint64_t epoch = AllocateEpoch();
        if (epoch == 0) {
            return {};
        }

        desired_ = target;
        desiredEpoch_ = epoch;
        desiredValid_ = true;
        standby_ = target;
        standbyEpoch_ = epoch;
        standbyValid_ = true;
        standbyReady_ = false;
        armedEpoch_ = 0;

        if (!activeValid_) {
            activeDrained_ = true;
        }

        // A driver call already in flight cannot be cancelled. Keep the phase
        // until its completion is reported; the epoch check will make its
        // result stale. An armed-but-uncommitted target is freely replaceable.
        if (phase_ != Phase::Reconfiguring) {
            phase_ = Phase::Preparing;
            inFlightEpoch_ = 0;
        }

        return {replacing ? RequestDisposition::Replaced
                          : RequestDisposition::Created,
                epoch};
    }

    bool MarkStandbyReady(std::uint64_t epoch) noexcept {
        if (phase_ == Phase::Faulted || !desiredValid_ || !standbyValid_ ||
            epoch == 0 || epoch != desiredEpoch_ || epoch != standbyEpoch_) {
            return false;
        }

        standbyReady_ = true;
        if (phase_ != Phase::Reconfiguring) {
            phase_ = Phase::Draining;
        }
        return true;
    }

    bool MarkActiveDrained() noexcept {
        if (phase_ == Phase::Faulted || phase_ == Phase::Active) {
            return false;
        }

        activeDrained_ = true;
        if (standbyReady_ && phase_ != Phase::Reconfiguring) {
            phase_ = Phase::Draining;
        }
        return true;
    }

    ReconfigureRequest TryBeginReconfigure() noexcept {
        if (phase_ == Phase::Faulted || phase_ == Phase::Reconfiguring ||
            phase_ == Phase::Arming || !desiredValid_ || !standbyValid_ ||
            !standbyReady_ || !activeDrained_ ||
            desiredEpoch_ == 0 || desiredEpoch_ != standbyEpoch_) {
            return {};
        }

        phase_ = Phase::Reconfiguring;
        inFlightEpoch_ = desiredEpoch_;
        return {true, inFlightEpoch_, desired_};
    }

    ReconfigureCompletion CompleteReconfigure(std::uint64_t epoch,
                                               bool succeeded) noexcept {
        if (phase_ != Phase::Reconfiguring || epoch == 0 ||
            epoch != inFlightEpoch_) {
            return ReconfigureCompletion::Unexpected;
        }

        inFlightEpoch_ = 0;
        const bool isCurrent = desiredValid_ && standbyValid_ &&
                epoch == desiredEpoch_ && epoch == standbyEpoch_;
        if (!isCurrent) {
            phase_ = standbyReady_ ? Phase::Draining : Phase::Preparing;
            return ReconfigureCompletion::Stale;
        }

        if (!succeeded) {
            Fault();
            return ReconfigureCompletion::FailedCurrent;
        }

        armedEpoch_ = epoch;
        phase_ = Phase::Arming;
        return ReconfigureCompletion::ReadyToArm;
    }

    bool CommitArmed(std::uint64_t epoch) noexcept {
        if (phase_ != Phase::Arming || epoch == 0 || epoch != armedEpoch_ ||
            !desiredValid_ || !standbyValid_ || !standbyReady_ ||
            epoch != desiredEpoch_ || epoch != standbyEpoch_) {
            return false;
        }

        active_ = desired_;
        activeEpoch_ = epoch;
        activeValid_ = true;
        ClearPending();
        activeDrained_ = false;
        phase_ = Phase::Active;
        return true;
    }

    void Fault() noexcept {
        ClearPending();
        activeDrained_ = false;
        phase_ = Phase::Faulted;
    }

    constexpr Phase CurrentPhase() const noexcept {
        return phase_;
    }

    constexpr bool HasActive() const noexcept {
        return activeValid_;
    }

    constexpr Target ActiveTarget() const noexcept {
        return active_;
    }

    constexpr std::uint64_t ActiveEpoch() const noexcept {
        return activeEpoch_;
    }

    constexpr bool HasDesired() const noexcept {
        return desiredValid_;
    }

    constexpr Target DesiredTarget() const noexcept {
        return desired_;
    }

    constexpr std::uint64_t DesiredEpoch() const noexcept {
        return desiredEpoch_;
    }

    constexpr bool StandbyReady() const noexcept {
        return standbyReady_;
    }

    constexpr bool ActiveDrained() const noexcept {
        return activeDrained_;
    }

    constexpr std::uint64_t InFlightEpoch() const noexcept {
        return inFlightEpoch_;
    }

    constexpr std::uint64_t ArmedEpoch() const noexcept {
        return armedEpoch_;
    }

    // Bridge silence maintains an ordinary running Active bank only. The first
    // replacement request disables it and it remains disabled through standby
    // preparation, old-bank drain, reconfiguration, and arming.
    constexpr bool ShouldMaintainBridge() const noexcept {
        return phase_ == Phase::Active && activeValid_;
    }

    constexpr bool IsInternallyConsistent() const noexcept {
        if (desiredValid_ != standbyValid_) {
            return false;
        }
        if (desiredValid_ &&
            (desired_ != standby_ || desiredEpoch_ == 0 ||
             desiredEpoch_ != standbyEpoch_)) {
            return false;
        }
        if (!desiredValid_ &&
            (desiredEpoch_ != 0 || standbyEpoch_ != 0 || standbyReady_)) {
            return false;
        }
        if (activeValid_ != (activeEpoch_ != 0)) {
            return false;
        }

        switch (phase_) {
            case Phase::Active:
                return !desiredValid_ && inFlightEpoch_ == 0 &&
                       armedEpoch_ == 0 && !activeDrained_;
            case Phase::Preparing:
                return desiredValid_ && !standbyReady_ &&
                       inFlightEpoch_ == 0 && armedEpoch_ == 0;
            case Phase::Draining:
                return desiredValid_ && standbyReady_ &&
                       inFlightEpoch_ == 0 && armedEpoch_ == 0;
            case Phase::Reconfiguring:
                return desiredValid_ && inFlightEpoch_ != 0 &&
                       armedEpoch_ == 0;
            case Phase::Arming:
                return desiredValid_ && standbyReady_ &&
                       inFlightEpoch_ == 0 && armedEpoch_ == desiredEpoch_;
            case Phase::Faulted:
                return !desiredValid_ && inFlightEpoch_ == 0 &&
                       armedEpoch_ == 0 && !standbyReady_;
            default:
                return false;
        }
    }

private:
    std::uint64_t AllocateEpoch() noexcept {
        if (nextEpoch_ == (std::numeric_limits<std::uint64_t>::max)()) {
            Fault();
            return 0;
        }
        return ++nextEpoch_;
    }

    void ClearPending() noexcept {
        desired_ = {};
        desiredEpoch_ = 0;
        desiredValid_ = false;
        standby_ = {};
        standbyEpoch_ = 0;
        standbyValid_ = false;
        standbyReady_ = false;
        inFlightEpoch_ = 0;
        armedEpoch_ = 0;
    }

    Phase phase_ = Phase::Active;
    Target active_{};
    std::uint64_t activeEpoch_ = 0;
    bool activeValid_ = false;
    Target desired_{};
    std::uint64_t desiredEpoch_ = 0;
    bool desiredValid_ = false;
    Target standby_{};
    std::uint64_t standbyEpoch_ = 0;
    bool standbyValid_ = false;
    bool standbyReady_ = false;
    bool activeDrained_ = false;
    std::uint64_t inFlightEpoch_ = 0;
    std::uint64_t armedEpoch_ = 0;
    std::uint64_t nextEpoch_ = 0;
};

}  // namespace tickbytick::format_handoff

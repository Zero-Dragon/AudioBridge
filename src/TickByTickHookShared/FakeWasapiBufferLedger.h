#pragma once

#include <algorithm>
#include <cstdint>
#include <initializer_list>

namespace tickbytick::fake_wasapi {

struct IngressLedger {
    std::uint32_t capacityFrames = 0;
    std::uint64_t queuedFrames = 0;
    std::uint64_t reservedFrames = 0;
    std::uint64_t eventGrantedFrames = 0;
};

inline bool IsWithinCapacity(const IngressLedger& ledger) noexcept {
    std::uint64_t remaining = ledger.capacityFrames;
    for (const std::uint64_t frames : {
                 ledger.queuedFrames,
                 ledger.reservedFrames,
                 ledger.eventGrantedFrames}) {
        if (frames > remaining) {
            return false;
        }
        remaining -= frames;
    }
    return true;
}

inline std::uint32_t UncommittedAvailableFrames(
        const IngressLedger& ledger) noexcept {
    const auto capacity = static_cast<std::uint64_t>(ledger.capacityFrames);
    std::uint64_t used = (std::min)(ledger.queuedFrames, capacity);
    used += (std::min)(ledger.reservedFrames, capacity - used);
    used += (std::min)(ledger.eventGrantedFrames, capacity - used);
    return static_cast<std::uint32_t>(capacity - used);
}

inline std::uint32_t ClaimableGetBufferFrames(
        const IngressLedger& ledger) noexcept {
    const std::uint64_t available = UncommittedAvailableFrames(ledger);
    const std::uint64_t granted = (std::min)(
            ledger.eventGrantedFrames,
            static_cast<std::uint64_t>(ledger.capacityFrames));
    return static_cast<std::uint32_t>((std::min)(
            static_cast<std::uint64_t>(ledger.capacityFrames),
            available + granted));
}

inline std::uint32_t GrantableEventFrames(
        const IngressLedger& ledger,
        std::uint32_t periodFrames) noexcept {
    if (ledger.eventGrantedFrames != 0 || ledger.capacityFrames == 0 ||
        periodFrames == 0) {
        return 0;
    }

    const std::uint32_t requiredFrames =
            (std::min)(ledger.capacityFrames, periodFrames);
    return UncommittedAvailableFrames(ledger) >= requiredFrames
            ? requiredFrames
            : 0;
}

// The Win32 event and the logical grant are one promise. Clear both sides even
// when the ledger already says zero: a signal can still be pending after an
// opportunistic GetBuffer call consumed the grant without first waiting.
template <typename ResetSignal>
inline bool ClearEventGrantAndSignal(
        std::uint64_t& eventGrantedFrames,
        ResetSignal resetSignal) {
    const bool hadGrant = eventGrantedFrames != 0;
    eventGrantedFrames = 0;
    resetSignal();
    return hadGrant;
}

}  // namespace tickbytick::fake_wasapi

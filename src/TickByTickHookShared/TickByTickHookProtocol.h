#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmreg.h>

#include <cstddef>
#include <cstdint>

namespace tickbytick::hook_protocol {

constexpr LONG kControlProtocolVersion = 8;
constexpr wchar_t kControlMapName[] = L"Local\\TickByTickWasapiHookControlV8";
constexpr wchar_t kPipeName[] =
        L"\\\\.\\pipe\\LOCAL\\TickByTickWasapiHookV8";
constexpr DWORD kPipeProtocolVersion =
        static_cast<DWORD>(kControlProtocolVersion);

constexpr DWORD kPipeMagic = 0x48504241;  // ABPH
constexpr DWORD kPipeText = 1;
constexpr DWORD kPipeFormat = 2;
constexpr DWORD kPipePcm = 3;
constexpr DWORD kPipeFinish = 4;
constexpr DWORD kPipeStreamLifecycle = 5;

struct PipeMessageHeader {
    DWORD magic = kPipeMagic;
    DWORD type = 0;
    DWORD pid = 0;
    // The versioned name isolates major revisions. This field additionally
    // rejects a stale peer before either side interprets its payload layout.
    DWORD reserved = kPipeProtocolVersion;
    std::uint64_t payloadBytes = 0;
};

struct PipeFormatMessage {
    DWORD formatBytes = sizeof(WAVEFORMATEXTENSIBLE);
    WAVEFORMATEXTENSIBLE format{};
    DWORD streamFlags = 0;
    DWORD shareMode = 0;
    DWORD periodFrames = 0;
    // Capacity exposed by the fake WASAPI endpoint. Core combines this with
    // the user-configured additional prebuffer when sizing the protected
    // timeline and its hard retention boundary.
    DWORD applicationBufferFrames = 0;
    // Reset starts a new accounting epoch without changing the IAudioClient
    // identity or its fixed source format.
    DWORD streamEpoch = 1;
    std::uint64_t streamId = 0;
};

// sequence is one-based and advances only after the complete pipe write.
// submittedFrames includes player-owned silent frames.
struct PipePcmMessage {
    std::uint64_t streamId = 0;
    std::uint64_t sequence = 0;
    std::uint64_t submittedFrames = 0;
    DWORD streamEpoch = 0;
    DWORD frameCount = 0;
    DWORD flags = 0;
    DWORD reserved = 0;
};

enum class StreamLifecycleAction : DWORD {
    Reset = 1,
    Close = 2,
};

struct PipeStreamLifecycleMessage {
    std::uint64_t streamId = 0;
    DWORD streamEpoch = 0;
    DWORD action = 0;
};

enum class RendererState : LONG {
    Idle = 0,
    Reconfiguring = 1,
    Running = 2,
    Faulted = 3,
};

// Admission is the hand-off from the fake endpoint waiting queue into a Core
// prebuffer bank. It is deliberately independent of DAC/ASIO consumption.
enum class AdmissionState : LONG {
    Empty = 0,
    Pending = 1,
    Managed = 2,
    Bypassed = 3,
    Faulted = 4,
};

struct HookAdmissionSlot {
    volatile LONG sequence = 0;
    volatile LONG pid = 0;
    volatile LONG state = static_cast<LONG>(AdmissionState::Empty);
    volatile LONG streamEpoch = 0;
    volatile LONG streamIdLow = 0;
    volatile LONG streamIdHigh = 0;
    volatile LONG admittedLow = 0;
    volatile LONG admittedHigh = 0;
};

constexpr std::size_t kAdmissionSlotCount = 32;

// The mapping is shared by 32-bit hooks, 64-bit hooks, and the matching Core.
// Every field is 32-bit so a read-only Win32 hook can take tear-free 64-bit
// snapshots through the two seqlocks without writing to the mapping.
struct HookControlBlock {
    volatile LONG lockedPid = 0;
    volatile LONG finish = 0;
    volatile LONG fakeOutput = 0;
    volatile LONG protocolVersion = kControlProtocolVersion;
    volatile LONG configSequence = 0;
    volatile LONG rendererState = static_cast<LONG>(RendererState::Idle);
    volatile LONG streamGeneration = 0;
    volatile LONG streamIdLow = 0;
    volatile LONG streamIdHigh = 0;
    volatile LONG sampleRate = 0;
    volatile LONG counterSequence = 0;
    // Logical player progress advances when player-owned frames enter Core's
    // protected playout horizon. Bridge-owned silence never advances it.
    volatile LONG consumedLogicalLow = 0;
    volatile LONG consumedLogicalHigh = 0;
    volatile LONG consumedLogicalBaselineLow = 0;
    volatile LONG consumedLogicalBaselineHigh = 0;
    volatile LONG consumedLogicalOffsetLow = 0;
    volatile LONG consumedLogicalOffsetHigh = 0;
    volatile LONG prebufferMs = 0;
    HookAdmissionSlot admissionSlots[kAdmissionSlotCount]{};
};

static_assert(sizeof(PipeMessageHeader) == 24);
static_assert(offsetof(PipeMessageHeader, reserved) == 12);
static_assert(offsetof(PipeMessageHeader, payloadBytes) == 16);
static_assert(sizeof(PipeFormatMessage) == 72);
static_assert(offsetof(PipeFormatMessage, applicationBufferFrames) == 56);
static_assert(offsetof(PipeFormatMessage, streamEpoch) == 60);
static_assert(offsetof(PipeFormatMessage, streamId) == 64);
static_assert(sizeof(PipePcmMessage) == 40);
static_assert(offsetof(PipePcmMessage, streamId) == 0);
static_assert(offsetof(PipePcmMessage, sequence) == 8);
static_assert(offsetof(PipePcmMessage, submittedFrames) == 16);
static_assert(offsetof(PipePcmMessage, streamEpoch) == 24);
static_assert(offsetof(PipePcmMessage, frameCount) == 28);
static_assert(offsetof(PipePcmMessage, flags) == 32);
static_assert(sizeof(PipeStreamLifecycleMessage) == 16);
static_assert(sizeof(HookAdmissionSlot) == 32);
static_assert(sizeof(HookControlBlock) == 1096);
static_assert(alignof(HookControlBlock) == alignof(LONG));
static_assert(offsetof(HookControlBlock, configSequence) == 16);
static_assert(offsetof(HookControlBlock, counterSequence) == 40);
static_assert(offsetof(HookControlBlock, consumedLogicalOffsetHigh) == 64);
static_assert(offsetof(HookControlBlock, prebufferMs) == 68);
static_assert(offsetof(HookControlBlock, admissionSlots) == 72);

inline std::uint64_t ReadStreamId(const HookControlBlock& control) noexcept {
    const auto low = static_cast<std::uint32_t>(control.streamIdLow);
    const auto high = static_cast<std::uint32_t>(control.streamIdHigh);
    return (static_cast<std::uint64_t>(high) << 32U) | low;
}

inline LONG StreamIdLow(std::uint64_t streamId) noexcept {
    return static_cast<LONG>(static_cast<std::uint32_t>(streamId));
}

inline LONG StreamIdHigh(std::uint64_t streamId) noexcept {
    return static_cast<LONG>(static_cast<std::uint32_t>(streamId >> 32U));
}

inline std::uint64_t JoinCounter(LONG low, LONG high) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(high)) << 32U) |
            static_cast<std::uint32_t>(low);
}

inline LONG CounterLow(std::uint64_t value) noexcept {
    return static_cast<LONG>(static_cast<std::uint32_t>(value));
}

inline LONG CounterHigh(std::uint64_t value) noexcept {
    return static_cast<LONG>(static_cast<std::uint32_t>(value >> 32U));
}

}  // namespace tickbytick::hook_protocol

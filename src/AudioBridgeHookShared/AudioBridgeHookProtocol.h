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

namespace audiobridge::hook_protocol {

constexpr LONG kControlProtocolVersion = 3;
constexpr wchar_t kControlMapName[] = L"Local\\AudioBridgeWasapiHookControlV3";

constexpr DWORD kPipeMagic = 0x48504241;  // ABPH
constexpr DWORD kPipeText = 1;
constexpr DWORD kPipeFormat = 2;
constexpr DWORD kPipePcm = 3;
constexpr DWORD kPipeFinish = 4;

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
    std::uint64_t streamId = 0;
};

// sequence is one-based and advances only after the complete pipe write.
// submittedFrames includes player-owned silent frames.
struct PipePcmMessage {
    std::uint64_t streamId = 0;
    std::uint64_t sequence = 0;
    std::uint64_t submittedFrames = 0;
    DWORD frameCount = 0;
    DWORD flags = 0;
};

enum class RendererState : LONG {
    Idle = 0,
    Reconfiguring = 1,
    Running = 2,
    Faulted = 3,
};

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
    volatile LONG consumedCapturedLow = 0;
    volatile LONG consumedCapturedHigh = 0;
    volatile LONG consumedOutputLow = 0;
    volatile LONG consumedOutputHigh = 0;
    volatile LONG consumedCapturedBaselineLow = 0;
    volatile LONG consumedCapturedBaselineHigh = 0;
    volatile LONG consumedCapturedOffsetLow = 0;
    volatile LONG consumedCapturedOffsetHigh = 0;
    volatile LONG dacPositionLow = 0;
    volatile LONG dacPositionHigh = 0;
    volatile LONG dacAnchorQpcLow = 0;
    volatile LONG dacAnchorQpcHigh = 0;
    volatile LONG dacBufferFrames = 0;
    volatile LONG dacClockValid = 0;
};

static_assert(sizeof(PipeMessageHeader) == 24);
static_assert(offsetof(PipeMessageHeader, payloadBytes) == 16);
static_assert(sizeof(PipeFormatMessage) == 64);
static_assert(offsetof(PipeFormatMessage, streamId) == 56);
static_assert(sizeof(PipePcmMessage) == 32);
static_assert(offsetof(PipePcmMessage, streamId) == 0);
static_assert(offsetof(PipePcmMessage, sequence) == 8);
static_assert(offsetof(PipePcmMessage, submittedFrames) == 16);
static_assert(offsetof(PipePcmMessage, frameCount) == 24);
static_assert(offsetof(PipePcmMessage, flags) == 28);
static_assert(sizeof(HookControlBlock) == 100);
static_assert(alignof(HookControlBlock) == alignof(LONG));
static_assert(offsetof(HookControlBlock, configSequence) == 16);
static_assert(offsetof(HookControlBlock, counterSequence) == 40);
static_assert(offsetof(HookControlBlock, consumedCapturedOffsetHigh) == 72);
static_assert(offsetof(HookControlBlock, dacPositionLow) == 76);
static_assert(offsetof(HookControlBlock, dacClockValid) == 96);

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

}  // namespace audiobridge::hook_protocol

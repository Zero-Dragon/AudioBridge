#pragma once

#include <stdint.h>

#if defined(_WIN32)
#if defined(ABC_BUILD_DLL)
#define ABC_API __declspec(dllexport)
#else
#define ABC_API __declspec(dllimport)
#endif
#define ABC_CALL __cdecl
#else
#define ABC_API
#define ABC_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ABC_Status {
    int32_t running;
    uint32_t targetPid;
    uint32_t lockedAudioPid;
    int32_t streamActive;
    int32_t prebuffering;
    int64_t totalFramesQueued;
    int64_t totalFramesPlayed;
    int64_t totalFramesDropped;
    int64_t totalOutputFrames;
    int64_t totalSilentFrames;
    int64_t bufferedFrames;
    int32_t bufferedMs;
    int64_t bufferCapacityFrames;
    int32_t bufferCapacityMs;
    int64_t prebufferTargetFrames;
    int32_t prebufferTargetMs;
    int64_t underrunCount;
    int64_t recentOutputFrames;
    int64_t recentSilentFrames;
    double recentSilentPercent;
    int32_t asioRequestedBufferFrames;
    int32_t asioActualBufferFrames;
    int32_t asioMinBufferFrames;
    int32_t asioMaxBufferFrames;
    int32_t asioPreferredBufferFrames;
    int32_t asioBufferGranularity;
    int32_t asioOutputSampleType;
    uint32_t asioSampleRate;
    int64_t asioResetRequests;
    int64_t asioBufferSizeChanges;
    int64_t asioLatencyChanges;
    int64_t asioRebuildCount;
    int32_t asioLastMessage;
} ABC_Status;

typedef struct ABC_PidInfo {
    uint32_t pid;
    uint32_t sampleRate;
    uint16_t channels;
    uint16_t bitsPerSample;
    uint32_t bytesPerFrame;
    uint8_t sampleFormat;
    uint64_t lastFormatMs;
    uint64_t lastPcmMs;
    int32_t isSelected;
} ABC_PidInfo;

ABC_API int32_t ABC_CALL ABC_Initialize(void);
ABC_API void ABC_CALL ABC_Shutdown(void);
ABC_API int32_t ABC_CALL ABC_StartTarget(const wchar_t* exePath,
                                         const wchar_t* hookDllPath,
                                         const wchar_t* outputDeviceId);
ABC_API int32_t ABC_CALL ABC_StartTargetWithOptions(const wchar_t* exePath,
                                                    const wchar_t* hookDllPath,
                                                    const wchar_t* outputDeviceId,
                                                    int32_t prebufferMs);
ABC_API int32_t ABC_CALL ABC_StartTargetWithOptions2(const wchar_t* exePath,
                                                     const wchar_t* hookDllPath,
                                                     const wchar_t* outputDeviceId,
                                                     int32_t prebufferMs,
                                                     int32_t asioBufferFrames);
ABC_API int32_t ABC_CALL ABC_StartTargetWithOptions3(const wchar_t* exePath,
                                                     const wchar_t* hookDllPath,
                                                     const wchar_t* outputDeviceId,
                                                     int32_t prebufferMs,
                                                     int32_t asioBufferFrames,
                                                     int32_t fakeOutput);
ABC_API void ABC_CALL ABC_Stop(void);
ABC_API int32_t ABC_CALL ABC_SetOutputDevice(const wchar_t* outputDeviceId);
ABC_API int32_t ABC_CALL ABC_SetPrebufferMs(int32_t prebufferMs);
ABC_API int32_t ABC_CALL ABC_GetPrebufferMs(void);
ABC_API int32_t ABC_CALL ABC_SetMaxBufferAdvanceMs(int32_t maxBufferAdvanceMs);
ABC_API int32_t ABC_CALL ABC_GetMaxBufferAdvanceMs(void);
ABC_API int32_t ABC_CALL ABC_SelectAudioPid(uint32_t pid);

ABC_API int32_t ABC_CALL ABC_RefreshDevices(void);
ABC_API int32_t ABC_CALL ABC_GetDeviceCount(void);
ABC_API int32_t ABC_CALL ABC_GetDefaultDeviceIndex(void);
ABC_API int32_t ABC_CALL ABC_GetDeviceId(int32_t index, wchar_t* buffer, int32_t bufferChars);
ABC_API int32_t ABC_CALL ABC_GetDeviceName(int32_t index, wchar_t* buffer, int32_t bufferChars);

ABC_API int32_t ABC_CALL ABC_GetPidCount(void);
ABC_API int32_t ABC_CALL ABC_GetPidInfo(int32_t index, ABC_PidInfo* info);
ABC_API int32_t ABC_CALL ABC_GetStatus(ABC_Status* status);
ABC_API int32_t ABC_CALL ABC_DrainLog(wchar_t* buffer, int32_t bufferChars);
ABC_API int32_t ABC_CALL ABC_GetLastError(wchar_t* buffer, int32_t bufferChars);

#ifdef __cplusplus
}
#endif

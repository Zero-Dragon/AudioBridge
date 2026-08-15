#pragma once

#include <stdint.h>

#if defined(_WIN32)
#if defined(TBT_BUILD_DLL)
#define TBT_API __declspec(dllexport)
#else
#define TBT_API __declspec(dllimport)
#endif
#define TBT_CALL __cdecl
#else
#define TBT_API
#define TBT_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TBT_Status {
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
    int32_t asioClockSourceIndex;
    int64_t wasapiBufferFrames;
    int32_t wasapiBufferMs;
    int64_t effectiveTimelineFrames;
    int32_t effectiveTimelineMs;
} TBT_Status;

typedef struct TBT_ClockSourceInfo {
    int32_t index;
    int32_t associatedChannel;
    int32_t associatedGroup;
    int32_t isCurrent;
} TBT_ClockSourceInfo;

typedef struct TBT_PidInfo {
    uint32_t pid;
    uint32_t sampleRate;
    uint16_t channels;
    uint16_t bitsPerSample;
    uint32_t bytesPerFrame;
    uint8_t sampleFormat;
    uint64_t lastFormatMs;
    uint64_t lastPcmMs;
    int32_t isSelected;
} TBT_PidInfo;

TBT_API int32_t TBT_CALL TBT_Initialize(void);
TBT_API void TBT_CALL TBT_Shutdown(void);
TBT_API int32_t TBT_CALL TBT_StartTarget(const wchar_t* exePath,
                                         const wchar_t* hookDllPath,
                                         const wchar_t* outputDeviceId);
TBT_API int32_t TBT_CALL TBT_StartTargetWithOptions(const wchar_t* exePath,
                                                    const wchar_t* hookDllPath,
                                                    const wchar_t* outputDeviceId,
                                                    int32_t prebufferMs);
TBT_API int32_t TBT_CALL TBT_StartTargetWithOptions2(const wchar_t* exePath,
                                                     const wchar_t* hookDllPath,
                                                     const wchar_t* outputDeviceId,
                                                     int32_t prebufferMs,
                                                     int32_t asioBufferFrames);
TBT_API int32_t TBT_CALL TBT_StartTargetWithOptions3(const wchar_t* exePath,
                                                     const wchar_t* hookDllPath,
                                                     const wchar_t* outputDeviceId,
                                                     int32_t prebufferMs,
                                                     int32_t asioBufferFrames,
                                                     int32_t fakeOutput);
TBT_API void TBT_CALL TBT_Stop(void);
TBT_API int32_t TBT_CALL TBT_SetOutputDevice(const wchar_t* outputDeviceId);
TBT_API int32_t TBT_CALL TBT_SetPrebufferMs(int32_t prebufferMs);
TBT_API int32_t TBT_CALL TBT_GetPrebufferMs(void);
TBT_API int32_t TBT_CALL TBT_SetMaxBufferAdvanceMs(int32_t maxBufferAdvanceMs);
TBT_API int32_t TBT_CALL TBT_GetMaxBufferAdvanceMs(void);
TBT_API int32_t TBT_CALL TBT_SelectAudioPid(uint32_t pid);

TBT_API int32_t TBT_CALL TBT_RefreshDevices(void);
TBT_API int32_t TBT_CALL TBT_GetDeviceCount(void);
TBT_API int32_t TBT_CALL TBT_GetDefaultDeviceIndex(void);
TBT_API int32_t TBT_CALL TBT_GetDeviceId(int32_t index, wchar_t* buffer, int32_t bufferChars);
TBT_API int32_t TBT_CALL TBT_GetDeviceName(int32_t index, wchar_t* buffer, int32_t bufferChars);

TBT_API int32_t TBT_CALL TBT_RefreshClockSources(const wchar_t* outputDeviceId);
TBT_API int32_t TBT_CALL TBT_GetClockSourceCount(void);
TBT_API int32_t TBT_CALL TBT_GetClockSourceInfo(int32_t position, TBT_ClockSourceInfo* info);
TBT_API int32_t TBT_CALL TBT_GetClockSourceName(int32_t position,
                                               wchar_t* buffer,
                                               int32_t bufferChars);
TBT_API int32_t TBT_CALL TBT_SetClockSource(int32_t clockSourceIndex);
TBT_API int32_t TBT_CALL TBT_GetClockSource(void);

TBT_API int32_t TBT_CALL TBT_GetPidCount(void);
TBT_API int32_t TBT_CALL TBT_GetPidInfo(int32_t index, TBT_PidInfo* info);
TBT_API int32_t TBT_CALL TBT_GetStatus(TBT_Status* status);
TBT_API int32_t TBT_CALL TBT_DrainLog(wchar_t* buffer, int32_t bufferChars);
TBT_API int32_t TBT_CALL TBT_GetLastError(wchar_t* buffer, int32_t bufferChars);

#ifdef __cplusplus
}
#endif

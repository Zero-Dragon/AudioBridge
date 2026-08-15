using System.Runtime.InteropServices;
using System.Text;

namespace TickByTick;

internal static partial class TickByTickNative
{
    private const string DllName = "TickByTickCore.dll";

    [StructLayout(LayoutKind.Sequential)]
    internal struct Status
    {
        public int Running;
        public uint TargetPid;
        public uint LockedAudioPid;
        public int StreamActive;
        public int Prebuffering;
        public long TotalFramesQueued;
        public long TotalFramesPlayed;
        public long TotalFramesDropped;
        public long TotalOutputFrames;
        public long TotalSilentFrames;
        public long BufferedFrames;
        public int BufferedMs;
        public long BufferCapacityFrames;
        public int BufferCapacityMs;
        public long PrebufferTargetFrames;
        public int PrebufferTargetMs;
        public long UnderrunCount;
        public long RecentOutputFrames;
        public long RecentSilentFrames;
        public double RecentSilentPercent;
        public int AsioRequestedBufferFrames;
        public int AsioActualBufferFrames;
        public int AsioMinBufferFrames;
        public int AsioMaxBufferFrames;
        public int AsioPreferredBufferFrames;
        public int AsioBufferGranularity;
        public int AsioOutputSampleType;
        public uint AsioSampleRate;
        public long AsioResetRequests;
        public long AsioBufferSizeChanges;
        public long AsioLatencyChanges;
        public long AsioRebuildCount;
        public int AsioLastMessage;
        public int AsioClockSourceIndex;
        public long WasapiBufferFrames;
        public int WasapiBufferMs;
        public long EffectiveTimelineFrames;
        public int EffectiveTimelineMs;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct ClockSourceInfo
    {
        public int Index;
        public int AssociatedChannel;
        public int AssociatedGroup;
        public int IsCurrent;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct PidInfo
    {
        public uint Pid;
        public uint SampleRate;
        public ushort Channels;
        public ushort BitsPerSample;
        public uint BytesPerFrame;
        public byte SampleFormat;
        public ulong LastFormatMs;
        public ulong LastPcmMs;
        public int IsSelected;
    }

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int TBT_Initialize();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void TBT_Shutdown();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int TBT_StartTargetWithOptions(
        string exePath,
        string? hookDllPath,
        string? outputDeviceId,
        int prebufferMs);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int TBT_StartTargetWithOptions2(
        string exePath,
        string? hookDllPath,
        string? outputDeviceId,
        int prebufferMs,
        int asioBufferFrames);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int TBT_StartTargetWithOptions3(
        string exePath,
        string? hookDllPath,
        string? outputDeviceId,
        int prebufferMs,
        int asioBufferFrames,
        int fakeOutput);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void TBT_Stop();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int TBT_SetOutputDevice(string? outputDeviceId);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int TBT_SetPrebufferMs(int prebufferMs);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int TBT_SetMaxBufferAdvanceMs(int maxBufferAdvanceMs);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int TBT_SelectAudioPid(uint pid);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int TBT_RefreshDevices();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int TBT_GetDeviceCount();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int TBT_GetDefaultDeviceIndex();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int TBT_GetDeviceId(int index, StringBuilder buffer, int bufferChars);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int TBT_GetDeviceName(int index, StringBuilder buffer, int bufferChars);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int TBT_RefreshClockSources(string? outputDeviceId);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int TBT_GetClockSourceCount();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int TBT_GetClockSourceInfo(int position, out ClockSourceInfo info);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int TBT_GetClockSourceName(
        int position,
        StringBuilder buffer,
        int bufferChars);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int TBT_SetClockSource(int clockSourceIndex);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int TBT_GetClockSource();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int TBT_GetPidCount();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int TBT_GetPidInfo(int index, out PidInfo info);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int TBT_GetStatus(out Status status);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int TBT_DrainLog(StringBuilder buffer, int bufferChars);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int TBT_GetLastError(StringBuilder buffer, int bufferChars);

    internal static string LastError()
    {
        var buffer = new StringBuilder(4096);
        var result = TBT_GetLastError(buffer, buffer.Capacity);
        return result >= 0 ? buffer.ToString() : $"Native error {result}";
    }

    internal static string ReadString(Func<StringBuilder, int, int> read)
    {
        var buffer = new StringBuilder(4096);
        var result = read(buffer, buffer.Capacity);
        return result >= 0 ? buffer.ToString() : string.Empty;
    }
}

using System.Runtime.InteropServices;
using System.Text;

namespace AudioBridge;

internal static partial class AudioBridgeNative
{
    private const string DllName = "AudioBridgeCore.dll";

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
    internal static extern int ABC_Initialize();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void ABC_Shutdown();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int ABC_StartTargetWithOptions(
        string exePath,
        string? hookDllPath,
        string? outputDeviceId,
        int prebufferMs);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int ABC_StartTargetWithOptions2(
        string exePath,
        string? hookDllPath,
        string? outputDeviceId,
        int prebufferMs,
        int asioBufferFrames);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int ABC_StartTargetWithOptions3(
        string exePath,
        string? hookDllPath,
        string? outputDeviceId,
        int prebufferMs,
        int asioBufferFrames,
        int fakeOutput);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void ABC_Stop();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int ABC_SetOutputDevice(string? outputDeviceId);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ABC_SetPrebufferMs(int prebufferMs);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ABC_SetMaxBufferAdvanceMs(int maxBufferAdvanceMs);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ABC_SelectAudioPid(uint pid);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ABC_RefreshDevices();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ABC_GetDeviceCount();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ABC_GetDefaultDeviceIndex();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int ABC_GetDeviceId(int index, StringBuilder buffer, int bufferChars);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int ABC_GetDeviceName(int index, StringBuilder buffer, int bufferChars);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int ABC_RefreshClockSources(string? outputDeviceId);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ABC_GetClockSourceCount();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ABC_GetClockSourceInfo(int position, out ClockSourceInfo info);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int ABC_GetClockSourceName(
        int position,
        StringBuilder buffer,
        int bufferChars);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ABC_SetClockSource(int clockSourceIndex);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ABC_GetClockSource();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ABC_GetPidCount();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ABC_GetPidInfo(int index, out PidInfo info);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ABC_GetStatus(out Status status);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int ABC_DrainLog(StringBuilder buffer, int bufferChars);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern int ABC_GetLastError(StringBuilder buffer, int bufferChars);

    internal static string LastError()
    {
        var buffer = new StringBuilder(4096);
        var result = ABC_GetLastError(buffer, buffer.Capacity);
        return result >= 0 ? buffer.ToString() : $"Native error {result}";
    }

    internal static string ReadString(Func<StringBuilder, int, int> read)
    {
        var buffer = new StringBuilder(4096);
        var result = read(buffer, buffer.Capacity);
        return result >= 0 ? buffer.ToString() : string.Empty;
    }
}

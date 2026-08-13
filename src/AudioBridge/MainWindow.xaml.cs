using System.Collections.ObjectModel;
using System.IO;
using System.Text;
using AudioBridge.Services;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Storage.Pickers;
using WinRT.Interop;

namespace AudioBridge;

public sealed partial class MainWindow : Window
{
    private readonly ObservableCollection<AudioDeviceView> devices = new();
    private readonly ObservableCollection<AudioPidView> pids = new();
    private readonly DispatcherQueueTimer pollTimer;
    private readonly StringBuilder logBuffer = new();
    private bool suppressDeviceSelection;
    private bool suppressPidSelection;
    private bool nativeReady;

    public MainWindow()
    {
        InitializeComponent();
        SetWindowIcon();

        DeviceComboBox.ItemsSource = devices;
        PidListView.ItemsSource = pids;
        ExtendsContentIntoTitleBar = false;

        var lastStartedExePath = AppSettingsService.LoadLastStartedExePath();
        if (!string.IsNullOrWhiteSpace(lastStartedExePath))
        {
            ExePathBox.Text = lastStartedExePath;
        }

        pollTimer = DispatcherQueue.CreateTimer();
        pollTimer.Interval = TimeSpan.FromMilliseconds(200);
        pollTimer.Tick += (_, _) => PollNativeState();

        Closed += MainWindow_Closed;
        InitializeNative();
    }

    private void SetWindowIcon()
    {
        var iconPath = Path.Combine(AppContext.BaseDirectory, "Assets", "AudioBridge.ico");
        if (File.Exists(iconPath))
        {
            AppWindow.SetIcon(iconPath);
        }
    }

    private void InitializeNative()
    {
        try
        {
            var result = AudioBridgeNative.ABC_Initialize();
            nativeReady = result == 0;
            if (!nativeReady)
            {
                AppendLog($"Native init failed: {AudioBridgeNative.LastError()}");
                return;
            }

            RefreshDevices();
            AudioBridgeNative.ABC_SetPrebufferMs((int)PrebufferBox.Value);
            pollTimer.Start();
            AppendLog("AudioBridge ready.");
        }
        catch (DllNotFoundException ex)
        {
            AppendLog($"AudioBridgeCore.dll not found: {ex.Message}");
        }
        catch (Exception ex)
        {
            AppendLog($"Native init exception: {ex.Message}");
        }
    }

    private void RefreshDevices()
    {
        if (!nativeReady)
        {
            return;
        }

        devices.Clear();
        var result = AudioBridgeNative.ABC_RefreshDevices();
        if (result != 0)
        {
            AppendLog($"Device refresh failed: {AudioBridgeNative.LastError()}");
            return;
        }

        var count = AudioBridgeNative.ABC_GetDeviceCount();
        var defaultIndex = AudioBridgeNative.ABC_GetDefaultDeviceIndex();
        for (var i = 0; i < count; i++)
        {
            var id = AudioBridgeNative.ReadString((buffer, chars) =>
                AudioBridgeNative.ABC_GetDeviceId(i, buffer, chars));
            var name = AudioBridgeNative.ReadString((buffer, chars) =>
                AudioBridgeNative.ABC_GetDeviceName(i, buffer, chars));
            if (string.IsNullOrWhiteSpace(name))
            {
                name = string.IsNullOrWhiteSpace(id) ? $"Device {i}" : id;
            }
            if (i == defaultIndex)
            {
                name += " (Default)";
            }

            devices.Add(new AudioDeviceView
            {
                Index = i,
                Id = id,
                Name = name
            });
        }

        suppressDeviceSelection = true;
        DeviceComboBox.SelectedIndex = defaultIndex >= 0 && defaultIndex < devices.Count ? defaultIndex : 0;
        suppressDeviceSelection = false;
    }

    private async void BrowseButton_Click(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker
        {
            SuggestedStartLocation = PickerLocationId.Desktop
        };
        picker.FileTypeFilter.Add(".exe");

        var hwnd = WindowNative.GetWindowHandle(this);
        InitializeWithWindow.Initialize(picker, hwnd);

        var file = await picker.PickSingleFileAsync();
        if (file != null)
        {
            ExePathBox.Text = file.Path;
        }
    }

    private void OpenButton_Click(object sender, RoutedEventArgs e)
    {
        if (!nativeReady)
        {
            AppendLog("Native bridge is not ready.");
            return;
        }

        var exePath = ExePathBox.Text.Trim();
        if (string.IsNullOrWhiteSpace(exePath) || !File.Exists(exePath))
        {
            AppendLog("Select a valid target exe.");
            return;
        }

        var deviceId = (DeviceComboBox.SelectedItem as AudioDeviceView)?.Id;
        var prebufferMs = ReadPrebufferMs();
        var asioBufferFrames = ReadAsioBufferFrames();
        var fakeOutput = FakeOutputCheckBox.IsChecked == true;
        var result = AudioBridgeNative.ABC_StartTargetWithOptions3(
            exePath,
            null,
            deviceId,
            prebufferMs,
            asioBufferFrames,
            fakeOutput ? 1 : 0);
        if (result != 0)
        {
            var error = AudioBridgeNative.LastError();
            LastErrorText.Text = error;
            AppendLog($"Open failed: {error}");
            return;
        }

        OpenButton.IsEnabled = false;
        StopButton.IsEnabled = true;
        LastErrorText.Text = string.Empty;
        try
        {
            AppSettingsService.SaveLastStartedExePath(exePath);
        }
        catch (Exception ex)
        {
            AppendLog($"Could not remember the target path: {ex.Message}");
        }
        AppendLog($"Opened target: {exePath} (Fake Output {(fakeOutput ? "on" : "off")})");
    }

    private void StopButton_Click(object sender, RoutedEventArgs e)
    {
        StopBridge();
    }

    private void DeviceComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressDeviceSelection || !nativeReady)
        {
            return;
        }

        var deviceId = (DeviceComboBox.SelectedItem as AudioDeviceView)?.Id;
        var result = AudioBridgeNative.ABC_SetOutputDevice(deviceId);
        if (result != 0)
        {
            AppendLog($"ASIO output switch failed: {AudioBridgeNative.LastError()}");
        }
    }

    private void PrebufferBox_ValueChanged(NumberBox sender, NumberBoxValueChangedEventArgs args)
    {
        if (!nativeReady || double.IsNaN(args.NewValue))
        {
            return;
        }

        var result = AudioBridgeNative.ABC_SetPrebufferMs(ReadPrebufferMs());
        if (result != 0)
        {
            AppendLog($"Prebuffer update failed: {AudioBridgeNative.LastError()}");
        }
    }

    private void PidListView_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressPidSelection || !nativeReady)
        {
            return;
        }

        if (PidListView.SelectedItem is not AudioPidView selected)
        {
            return;
        }

        var result = AudioBridgeNative.ABC_SelectAudioPid(selected.Pid);
        if (result != 0)
        {
            AppendLog($"PID selection failed: {AudioBridgeNative.LastError()}");
        }
    }

    private void PollNativeState()
    {
        if (!nativeReady)
        {
            return;
        }

        if (AudioBridgeNative.ABC_GetStatus(out var status) == 0)
        {
            UpdateStatus(status);
            RefreshPids(status.LockedAudioPid);
        }

        DrainNativeLog();
    }

    private void UpdateStatus(AudioBridgeNative.Status status)
    {
        TargetPidText.Text = status.TargetPid == 0 ? "-" : status.TargetPid.ToString();
        SelectedPidText.Text = status.LockedAudioPid == 0 ? "-" : status.LockedAudioPid.ToString();
        BridgeStateText.Text = StateText(status);
        BufferedText.Text = $"{status.BufferedMs} ms";
        CapacityText.Text = $"{status.BufferCapacityMs} ms";
        SilentPercentText.Text = $"{status.RecentSilentPercent:F2}%";
        UnderrunText.Text = status.UnderrunCount.ToString("N0");
        DroppedText.Text = status.TotalFramesDropped.ToString("N0");
        FramesText.Text = $"{status.TotalFramesQueued:N0} / {status.TotalFramesPlayed:N0}";
        AsioBufferText.Text = status.AsioActualBufferFrames > 0
            ? $"{status.AsioActualBufferFrames:N0} frames (req {AsioRequestedText(status.AsioRequestedBufferFrames)})"
            : "-";
        AsioPreferredText.Text = status.AsioPreferredBufferFrames > 0
            ? $"{status.AsioPreferredBufferFrames:N0} frames"
            : "-";
        AsioSampleText.Text = status.AsioSampleRate > 0
            ? $"{status.AsioSampleRate:N0} Hz, {AsioSampleTypeName(status.AsioOutputSampleType)}"
            : "-";
        AsioRangeText.Text = status.AsioMinBufferFrames > 0 || status.AsioMaxBufferFrames > 0
            ? $"{status.AsioMinBufferFrames:N0}-{status.AsioMaxBufferFrames:N0} frames, gran {AsioGranularityText(status.AsioBufferGranularity)}"
            : "-";
        AsioEventsText.Text = status.AsioResetRequests > 0 ||
                              status.AsioBufferSizeChanges > 0 ||
                              status.AsioLatencyChanges > 0 ||
                              status.AsioRebuildCount > 0
            ? $"reset {status.AsioResetRequests:N0}, buffer {status.AsioBufferSizeChanges:N0}, rebuild {status.AsioRebuildCount:N0}"
            : "-";

        var bufferPercent = status.BufferCapacityFrames > 0
            ? Math.Clamp(status.BufferedFrames * 100.0 / status.BufferCapacityFrames, 0.0, 100.0)
            : 0.0;
        BufferProgress.Value = bufferPercent;
        SilentProgress.Value = Math.Clamp(status.RecentSilentPercent, 0.0, 100.0);

        if (status.Running == 0)
        {
            OpenButton.IsEnabled = true;
            StopButton.IsEnabled = false;
        }
    }

    private static string StateText(AudioBridgeNative.Status status)
    {
        if (status.Running == 0)
        {
            return "Idle";
        }
        if (status.Prebuffering != 0)
        {
            return $"Prebuffering {status.BufferedMs}/{status.PrebufferTargetMs} ms";
        }
        return status.StreamActive != 0 ? "Output Active" : "Waiting For Audio";
    }

    private void RefreshPids(uint selectedPid)
    {
        var count = AudioBridgeNative.ABC_GetPidCount();
        var nextPids = new List<(uint Pid, string Format, string LastPcm, bool IsSelected)>(count);
        for (var i = 0; i < count; i++)
        {
            if (AudioBridgeNative.ABC_GetPidInfo(i, out var info) != 0)
            {
                continue;
            }

            nextPids.Add((
                info.Pid,
                $"{info.SampleRate} Hz, {info.Channels} ch, {info.BitsPerSample} bit, {SampleFormatName(info.SampleFormat)}",
                info.LastPcmMs == 0 ? "No PCM yet" : $"PCM tick {info.LastPcmMs}",
                info.IsSelected != 0));
        }

        suppressPidSelection = true;
        for (var i = 0; i < nextPids.Count; i++)
        {
            var next = nextPids[i];
            if (i >= pids.Count)
            {
                pids.Add(new AudioPidView
                {
                    Pid = next.Pid,
                    Format = next.Format,
                    LastPcm = next.LastPcm,
                    IsSelected = next.IsSelected
                });
            }
            else if (pids[i].Pid == next.Pid)
            {
                pids[i].Update(next.Format, next.LastPcm, next.IsSelected);
            }
            else
            {
                var existingIndex = -1;
                for (var j = i + 1; j < pids.Count; j++)
                {
                    if (pids[j].Pid == next.Pid)
                    {
                        existingIndex = j;
                        break;
                    }
                }

                if (existingIndex >= 0)
                {
                    pids.Move(existingIndex, i);
                    pids[i].Update(next.Format, next.LastPcm, next.IsSelected);
                }
                else
                {
                    pids.Insert(i, new AudioPidView
                    {
                        Pid = next.Pid,
                        Format = next.Format,
                        LastPcm = next.LastPcm,
                        IsSelected = next.IsSelected
                    });
                }
            }
        }
        while (pids.Count > nextPids.Count)
        {
            pids.RemoveAt(pids.Count - 1);
        }

        var selected = pids.FirstOrDefault(pid => pid.Pid == selectedPid);
        if (!ReferenceEquals(PidListView.SelectedItem, selected))
        {
            PidListView.SelectedItem = selected;
        }
        suppressPidSelection = false;
    }

    private void DrainNativeLog()
    {
        var buffer = new StringBuilder(32768);
        var result = AudioBridgeNative.ABC_DrainLog(buffer, buffer.Capacity);
        if (result <= 0)
        {
            return;
        }

        AppendRawLog(buffer.ToString());
    }

    private int ReadPrebufferMs()
    {
        if (double.IsNaN(PrebufferBox.Value))
        {
            return 300;
        }

        return (int)Math.Clamp(Math.Round(PrebufferBox.Value), 0, 10000);
    }

    private int ReadAsioBufferFrames()
    {
        if (double.IsNaN(AsioBufferBox.Value))
        {
            return 0;
        }

        return (int)Math.Clamp(Math.Round(AsioBufferBox.Value), 0, 8192);
    }

    private static string SampleFormatName(byte sampleFormat)
    {
        return sampleFormat switch
        {
            2 => "float",
            1 => "int",
            _ => "unknown"
        };
    }

    private static string AsioRequestedText(int frames)
    {
        return frames <= 0 ? "preferred" : frames.ToString("N0");
    }

    private static string AsioGranularityText(int granularity)
    {
        return granularity switch
        {
            -1 => "pow2",
            0 => "free",
            1 => "1",
            _ => granularity.ToString("N0")
        };
    }

    private static string AsioSampleTypeName(int sampleType)
    {
        return sampleType switch
        {
            16 => "Int16LSB",
            17 => "Int24LSB",
            18 => "Int32LSB",
            19 => "Float32LSB",
            24 => "Int32LSB16",
            25 => "Int32LSB18",
            26 => "Int32LSB20",
            27 => "Int32LSB24",
            _ => sampleType == 0 ? "unknown" : $"type {sampleType}"
        };
    }

    private void AppendLog(string message)
    {
        AppendRawLog($"[{DateTime.Now:HH:mm:ss.fff}] {message}\r\n");
    }

    private void AppendRawLog(string text)
    {
        logBuffer.Append(text);
        const int maxChars = 160_000;
        if (logBuffer.Length > maxChars)
        {
            logBuffer.Remove(0, logBuffer.Length - maxChars);
        }

        LogBox.Text = logBuffer.ToString();
        LogBox.SelectionStart = LogBox.Text.Length;
    }

    private void StopBridge()
    {
        if (!nativeReady)
        {
            return;
        }

        AudioBridgeNative.ABC_Stop();
        OpenButton.IsEnabled = true;
        StopButton.IsEnabled = false;
        AppendLog("Bridge stopped.");
    }

    private void MainWindow_Closed(object sender, WindowEventArgs args)
    {
        pollTimer.Stop();
        if (!nativeReady)
        {
            return;
        }

        AudioBridgeNative.ABC_Stop();
        AudioBridgeNative.ABC_Shutdown();
        nativeReady = false;
    }
}

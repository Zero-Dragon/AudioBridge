using Microsoft.UI.Xaml;
using System;
using System.IO;

namespace AudioBridge;

public partial class App : Application
{
    private Window? window;

    public App()
    {
        InitializeComponent();
        UnhandledException += (_, args) =>
        {
            WriteStartupCrashLog(args.Exception);
        };
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        try
        {
            window = new MainWindow();
            window.Activate();
        }
        catch (Exception ex)
        {
            WriteStartupCrashLog(ex);
            throw;
        }
    }

    private static void WriteStartupCrashLog(Exception ex)
    {
        try
        {
            var path = Path.Combine(AppContext.BaseDirectory, "AudioBridge.startup.log");
            File.AppendAllText(path, $"[{DateTime.Now:O}]\r\n{ex}\r\n\r\n");
        }
        catch
        {
        }
    }
}

using Microsoft.UI.Xaml;
using AudioBridge.Services;

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
        SessionLogWriter.WriteCrashLog(ex);
    }
}

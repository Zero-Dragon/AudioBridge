using System.Text.Json;

namespace AudioBridge.Services;

internal static class AppSettingsService
{
    private const string SettingsFileName = "settings.json";

    private sealed class AppSettings
    {
        public string? LastStartedExePath { get; set; }
    }

    public static string? LoadLastStartedExePath()
    {
        try
        {
            var settingsPath = GetSettingsPath();
            if (!File.Exists(settingsPath))
            {
                return null;
            }

            var settings = JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(settingsPath));
            var path = settings?.LastStartedExePath?.Trim();
            return string.IsNullOrWhiteSpace(path) ? null : path;
        }
        catch
        {
            return null;
        }
    }

    public static void SaveLastStartedExePath(string exePath)
    {
        var settingsPath = GetSettingsPath();
        var settings = new AppSettings
        {
            LastStartedExePath = Path.GetFullPath(exePath),
        };
        var json = JsonSerializer.Serialize(settings, new JsonSerializerOptions
        {
            WriteIndented = true,
        });
        var temporaryPath = settingsPath + ".tmp";
        try
        {
            File.WriteAllText(temporaryPath, json);
            File.Move(temporaryPath, settingsPath, true);
        }
        finally
        {
            File.Delete(temporaryPath);
        }
    }

    private static string GetSettingsPath()
    {
        return Path.Combine(AppContext.BaseDirectory, SettingsFileName);
    }
}

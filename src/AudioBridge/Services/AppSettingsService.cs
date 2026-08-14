using System.Text.Json;
using System.Text.Json.Serialization;

namespace AudioBridge.Services;

internal sealed class AppSettingsState
{
    public string? SelectedAsioDeviceId { get; set; }
    public int PrebufferMs { get; set; } = 300;
    public int MaxBufferOffsetMs { get; set; } = 100;
    public int AsioBufferFrames { get; set; }
    public int AsioClockSourceIndex { get; set; } = -1;
    public List<RecentTargetSetting> RecentTargets { get; set; } = [];
}

internal sealed class RecentTargetSetting
{
    public RecentTargetSetting()
    {
    }

    public string Path { get; set; } = string.Empty;
    public DateTimeOffset LastOpenedUtc { get; set; }
}

internal static class AppSettingsService
{
    public const int MaxRecentTargets = 5;

    public static string? LastLoadError { get; private set; }

    private const string SettingsFileName = "settings.json";

    private static readonly JsonSerializerOptions WriteOptions = new()
    {
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
    };

    private sealed class PersistedAppSettings
    {
        public PersistedAppSettings()
        {
        }

        public string? SelectedAsioDeviceId { get; set; }
        public int PrebufferMs { get; set; } = 300;
        public int MaxBufferOffsetMs { get; set; } = 100;
        public int AsioBufferFrames { get; set; }
        public int AsioClockSourceIndex { get; set; } = -1;
        public List<RecentTargetSetting>? RecentTargets { get; set; }

        // The first settings schema stored only this value. Reading it once
        // preserves the user's most recent target while the next save upgrades
        // the file to the recent-target list.
        public string? LastStartedExePath { get; set; }
    }

    public static AppSettingsState Load()
    {
        LastLoadError = null;
        try
        {
            var settingsPath = GetSettingsPath();
            if (!File.Exists(settingsPath))
            {
                return new AppSettingsState();
            }

            using var document = JsonDocument.Parse(File.ReadAllText(settingsPath));
            var root = document.RootElement;
            var recentTargets = ReadRecentTargets(root);
            var legacyPath = NormalizePath(ReadString(root, "lastStartedExePath"));
            if (recentTargets.Count == 0 && legacyPath is not null)
            {
                recentTargets.Add(new RecentTargetSetting
                {
                    Path = legacyPath,
                    LastOpenedUtc = File.GetLastWriteTimeUtc(settingsPath),
                });
            }

            var selectedDeviceId = NormalizeDeviceId(ReadString(root, "selectedAsioDeviceId"));
            return new AppSettingsState
            {
                SelectedAsioDeviceId = selectedDeviceId,
                PrebufferMs = Math.Clamp(ReadInt32(root, "prebufferMs", 300), 0, 10000),
                MaxBufferOffsetMs = Math.Clamp(
                    ReadInt32(root, "maxBufferOffsetMs", 100),
                    50,
                    10000),
                AsioBufferFrames = selectedDeviceId is null
                    ? 0
                    : Math.Clamp(ReadInt32(root, "asioBufferFrames", 0), 0, 8192),
                AsioClockSourceIndex = selectedDeviceId is null
                    ? -1
                    : Math.Max(ReadInt32(root, "asioClockSourceIndex", -1), -1),
                RecentTargets = recentTargets,
            };
        }
        catch (Exception ex)
        {
            LastLoadError = ex.Message;
            return new AppSettingsState();
        }
    }

    public static void Save(AppSettingsState settings)
    {
        ArgumentNullException.ThrowIfNull(settings);

        var selectedDeviceId = NormalizeDeviceId(settings.SelectedAsioDeviceId);
        var persisted = new PersistedAppSettings
        {
            SelectedAsioDeviceId = selectedDeviceId,
            PrebufferMs = Math.Clamp(settings.PrebufferMs, 0, 10000),
            MaxBufferOffsetMs = Math.Clamp(settings.MaxBufferOffsetMs, 50, 10000),
            AsioBufferFrames = selectedDeviceId is null
                ? 0
                : Math.Clamp(settings.AsioBufferFrames, 0, 8192),
            AsioClockSourceIndex = selectedDeviceId is null
                ? -1
                : Math.Max(settings.AsioClockSourceIndex, -1),
            RecentTargets = NormalizeRecentTargets(settings.RecentTargets),
        };

        var settingsPath = GetSettingsPath();
        var json = JsonSerializer.Serialize(persisted, WriteOptions);
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

    private static List<RecentTargetSetting> NormalizeRecentTargets(
        IEnumerable<RecentTargetSetting> targets)
    {
        var uniqueTargets = new Dictionary<string, RecentTargetSetting>(
            StringComparer.OrdinalIgnoreCase);
        foreach (var target in targets)
        {
            var path = NormalizePath(target.Path);
            if (path is null)
            {
                continue;
            }

            var normalized = new RecentTargetSetting
            {
                Path = path,
                LastOpenedUtc = target.LastOpenedUtc,
            };
            if (!uniqueTargets.TryGetValue(path, out var existing) ||
                normalized.LastOpenedUtc > existing.LastOpenedUtc)
            {
                uniqueTargets[path] = normalized;
            }
        }

        return uniqueTargets.Values
            .OrderByDescending(target => target.LastOpenedUtc)
            .ThenBy(target => target.Path, StringComparer.OrdinalIgnoreCase)
            .Take(MaxRecentTargets)
            .ToList();
    }

    private static List<RecentTargetSetting> ReadRecentTargets(JsonElement root)
    {
        if (!TryGetProperty(root, "recentTargets", out var recentTargetsElement) ||
            recentTargetsElement.ValueKind != JsonValueKind.Array)
        {
            return [];
        }

        var recentTargets = new List<RecentTargetSetting>();
        foreach (var item in recentTargetsElement.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.Object)
            {
                continue;
            }

            var path = ReadString(item, "path");
            if (string.IsNullOrWhiteSpace(path))
            {
                continue;
            }

            var lastOpenedUtc = DateTimeOffset.MinValue;
            if (TryGetProperty(item, "lastOpenedUtc", out var timestampElement) &&
                timestampElement.ValueKind == JsonValueKind.String)
            {
                timestampElement.TryGetDateTimeOffset(out lastOpenedUtc);
            }

            recentTargets.Add(new RecentTargetSetting
            {
                Path = path,
                LastOpenedUtc = lastOpenedUtc,
            });
        }

        return NormalizeRecentTargets(recentTargets);
    }

    private static string? ReadString(JsonElement element, string propertyName)
    {
        return TryGetProperty(element, propertyName, out var value) &&
               value.ValueKind == JsonValueKind.String
            ? value.GetString()
            : null;
    }

    private static int ReadInt32(JsonElement element, string propertyName, int fallback)
    {
        return TryGetProperty(element, propertyName, out var value) &&
               value.ValueKind == JsonValueKind.Number &&
               value.TryGetInt32(out var result)
            ? result
            : fallback;
    }

    private static bool TryGetProperty(
        JsonElement element, string propertyName, out JsonElement value)
    {
        if (element.ValueKind == JsonValueKind.Object)
        {
            foreach (var property in element.EnumerateObject())
            {
                if (string.Equals(property.Name, propertyName, StringComparison.OrdinalIgnoreCase))
                {
                    value = property.Value;
                    return true;
                }
            }
        }

        value = default;
        return false;
    }

    private static string? NormalizePath(string? path)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            return null;
        }

        try
        {
            return Path.GetFullPath(path.Trim());
        }
        catch
        {
            return null;
        }
    }

    private static string? NormalizeDeviceId(string? deviceId)
    {
        var normalized = deviceId?.Trim();
        return string.IsNullOrWhiteSpace(normalized) ? null : normalized;
    }

    private static string GetSettingsPath()
    {
        return Path.Combine(AppContext.BaseDirectory, SettingsFileName);
    }
}

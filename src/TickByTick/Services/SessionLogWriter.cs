using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;

namespace TickByTick.Services;

internal sealed class SessionLogWriter : IDisposable
{
    private const long MaxFileBytes = 4L * 1024L * 1024L;
    private const int RetainedFileBytes = 3 * 1024 * 1024;
    private static readonly UTF8Encoding Utf8NoBom = new(false);

    private readonly BlockingCollection<string>? pendingWrites;
    private readonly Thread? writerThread;
    private string? firstCriticalLine;
    private int disposed;

    private SessionLogWriter(string? filePath)
    {
        FilePath = filePath;
        if (filePath is null)
        {
            return;
        }

        File.WriteAllText(
            filePath,
            $"[{DateTime.Now:HH:mm:ss.fff}] Tick By Tick session log. " +
            $"Maximum size={MaxFileBytes / (1024 * 1024)} MiB; " +
            $"latest {RetainedFileBytes / (1024 * 1024)} MiB retained after trimming.\r\n",
            Utf8NoBom);

        pendingWrites = new BlockingCollection<string>();
        writerThread = new Thread(WriteLoop)
        {
            IsBackground = true,
            Name = "Tick By Tick session log writer"
        };
        writerThread.Start();
    }

    public string? FilePath { get; }

    public static SessionLogWriter Create()
    {
        foreach (var directory in CandidateDirectories())
        {
            try
            {
                Directory.CreateDirectory(directory);
                var timestamp = DateTime.Now.ToString("yyyyMMdd-HHmmss-fff");
                var fileName = $"TickByTick-{timestamp}-p{Environment.ProcessId}.log";
                return new SessionLogWriter(Path.Combine(directory, fileName));
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"TickByTick log path unavailable: {directory}: {ex.Message}");
            }
        }

        return new SessionLogWriter(null);
    }

    public static void WriteCrashLog(Exception exception)
    {
        foreach (var directory in CandidateDirectories())
        {
            try
            {
                Directory.CreateDirectory(directory);
                var timestamp = DateTime.Now.ToString("yyyyMMdd-HHmmss-fff");
                var fileName = $"TickByTick-crash-{timestamp}-p{Environment.ProcessId}.log";
                File.WriteAllText(
                    Path.Combine(directory, fileName),
                    $"[{DateTime.Now:O}]\r\n{exception}\r\n",
                    Utf8NoBom);
                return;
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"TickByTick crash log path unavailable: {directory}: {ex.Message}");
            }
        }
    }

    public void Write(string text)
    {
        if (string.IsNullOrEmpty(text) ||
            pendingWrites is null ||
            Volatile.Read(ref disposed) != 0)
        {
            return;
        }

        try
        {
            pendingWrites.Add(text);
        }
        catch (InvalidOperationException)
        {
            // The window is closing and the writer has already completed.
        }
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref disposed, 1) != 0 || pendingWrites is null)
        {
            return;
        }

        pendingWrites.CompleteAdding();
        writerThread?.Join(TimeSpan.FromSeconds(3));
    }

    private void WriteLoop()
    {
        if (pendingWrites is null || FilePath is null)
        {
            return;
        }

        var batch = new StringBuilder(64 * 1024);
        while (!pendingWrites.IsCompleted)
        {
            if (!pendingWrites.TryTake(out var text, 500))
            {
                continue;
            }

            batch.Append(text);
            while (pendingWrites.TryTake(out text))
            {
                batch.Append(text);
            }

            var output = batch.ToString();
            batch.Clear();
            CaptureFirstCriticalLine(output);

            try
            {
                File.AppendAllText(FilePath, output, Utf8NoBom);
                TrimIfNeeded();
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"TickByTick log write failed: {ex.Message}");
            }
        }
    }

    private static IEnumerable<string> CandidateDirectories()
    {
        var localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        return new[]
        {
            Path.Combine(AppContext.BaseDirectory, "logs"),
            string.IsNullOrWhiteSpace(localAppData)
                ? null
                : Path.Combine(localAppData, "TickByTick", "logs"),
            Path.Combine(Path.GetTempPath(), "TickByTick", "logs")
        }
        .Where(path => !string.IsNullOrWhiteSpace(path))
        .Select(path => path!)
        .Distinct(StringComparer.OrdinalIgnoreCase);
    }

    private void CaptureFirstCriticalLine(string text)
    {
        if (firstCriticalLine is not null)
        {
            return;
        }

        foreach (var line in text.Split(new[] { "\r\n", "\n" }, StringSplitOptions.RemoveEmptyEntries))
        {
            if (!IsCriticalLine(line))
            {
                continue;
            }

            firstCriticalLine = line.Length <= 2048 ? line : line[..2048];
            return;
        }
    }

    private static bool IsCriticalLine(string line)
    {
        return line.Contains("[fault]", StringComparison.OrdinalIgnoreCase) ||
               line.Contains(" failed", StringComparison.OrdinalIgnoreCase) ||
               line.Contains(" failure", StringComparison.OrdinalIgnoreCase) ||
               line.Contains(" exception", StringComparison.OrdinalIgnoreCase) ||
               line.Contains("timed out", StringComparison.OrdinalIgnoreCase) ||
               line.Contains("invalidated", StringComparison.OrdinalIgnoreCase);
    }

    private void TrimIfNeeded()
    {
        if (FilePath is null || new FileInfo(FilePath).Length <= MaxFileBytes)
        {
            return;
        }

        var bytes = File.ReadAllBytes(FilePath);
        var start = Math.Max(0, bytes.Length - RetainedFileBytes);
        while (start < bytes.Length && bytes[start] != (byte)'\n')
        {
            ++start;
        }
        if (start < bytes.Length)
        {
            ++start;
        }

        var retained = Utf8NoBom.GetString(bytes, start, bytes.Length - start);
        var marker = $"[{DateTime.Now:HH:mm:ss.fff}] [log] Earlier log content was trimmed; latest events retained.\r\n";
        if (firstCriticalLine is not null)
        {
            marker += $"[{DateTime.Now:HH:mm:ss.fff}] [log] First critical event retained: {firstCriticalLine}\r\n";
        }

        var temporaryPath = FilePath + ".trim";
        File.WriteAllText(temporaryPath, marker + retained, Utf8NoBom);
        File.Move(temporaryPath, FilePath, true);
    }
}

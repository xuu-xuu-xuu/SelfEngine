using System.Diagnostics;
using System.Text;

internal sealed class BootstrapDiagnostics : IDisposable
{
    private readonly StreamWriter writer;
    private readonly object gate = new();
    private readonly Stopwatch elapsed = Stopwatch.StartNew();
    public BootstrapDiagnostics(string projectRoot)
    {
        var directory = Environment.GetEnvironmentVariable("SELFENGINE_LOG_DIRECTORY");
        if (string.IsNullOrEmpty(directory))
        {
            directory = Path.Combine(projectRoot, "out", "Logs");
            Environment.SetEnvironmentVariable("SELFENGINE_LOG_DIRECTORY", directory);
        }
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, $"managed-{DateTime.UtcNow:yyyyMMdd-HHmmssfff}-{Environment.ProcessId}.log");
        writer = new StreamWriter(new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read), new UTF8Encoding(false)) { AutoFlush = true };
        Write("Managed.Start", $"runtime={System.Runtime.InteropServices.RuntimeInformation.FrameworkDescription}");
        Console.WriteLine($"[Info] Diagnostics.ManagedLog: {path}"); // 语言表加载前的启动诊断回退。
    }
    public void Write(string stage, string detail = "")
    {
        try { lock (gate) writer.WriteLine($"{DateTime.UtcNow:O} elapsed_ms={elapsed.ElapsedMilliseconds} pid={Environment.ProcessId} stage={stage} {detail}"); }
        catch (IOException) { } // 磁盘写入失败不应使引擎退出。
    }
    public void Dispose() { Write("Managed.Exit"); writer.Dispose(); }
}

using System.IO.Pipelines;
using System.Reflection;
using System.Text.Json;

// 只有显式预检才进入本类型；缺失依赖的 JIT/加载异常由入口统一记录。
internal static class RuntimePreflight
{
    public static void Verify(string reportPath)
    {
        var pipe = new Pipe();
        try
        {
            var payload = JsonSerializer.SerializeToUtf8Bytes(new Probe(37, "SelfEngine"));
            pipe.Writer.WriteAsync(payload).GetAwaiter().GetResult();
            pipe.Writer.Complete();
            var value = JsonSerializer.DeserializeAsync<Probe>(pipe.Reader).GetAwaiter().GetResult();
            if (value is not { Number: 37, Name: "SelfEngine" })
                throw new InvalidDataException("Bootstrap.RuntimeCheckFailed");
        }
        finally { pipe.Reader.Complete(); }
        var assemblies = new[] { typeof(object).Assembly, typeof(Pipe).Assembly, typeof(JsonSerializer).Assembly };
        bool IsPrivate(Assembly assembly) => string.Equals(Path.GetDirectoryName(assembly.Location),
            Path.TrimEndingDirectorySeparator(AppContext.BaseDirectory), StringComparison.OrdinalIgnoreCase);
        var report = new
        {
            schemaVersion = 1, success = true, privateRuntime = assemblies.All(IsPrivate),
            framework = System.Runtime.InteropServices.RuntimeInformation.FrameworkDescription,
            assemblies = assemblies.Select(a => new { name = a.GetName().Name, version = a.GetName().Version?.ToString(), path = a.Location })
        };
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(reportPath))!);
        File.WriteAllText(reportPath, JsonSerializer.Serialize(report, new JsonSerializerOptions { WriteIndented = true }));
    }
    internal sealed record Probe(int Number, string Name);
}

// 父工具锁第 0 字节负责构建串行化，子进程锁第 1 字节保护已加载的预览。
// 父工具意外退出时子锁仍有效；后来的构建同时申请两个字节。
internal sealed class PreviewLease : IDisposable
{
    private readonly FileStream? stream;
    private PreviewLease(FileStream? stream)=>this.stream=stream;
    internal static PreviewLease Acquire(string? path)
    {
        if(path is null)return new(null);
        if(!OperatingSystem.IsWindows())throw new PlatformNotSupportedException("Preview.WindowsRequired");
        FileStream? stream=null;
        try
        {
            stream=new FileStream(Path.GetFullPath(path),FileMode.OpenOrCreate,FileAccess.ReadWrite,FileShare.ReadWrite);
            stream.Lock(1,1);
            return new(stream);
        }
        catch {stream?.Dispose();throw new InvalidOperationException("Preview.Busy");}
    }
    public void Dispose()=>stream?.Dispose();
}

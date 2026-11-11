using System.Runtime.InteropServices;
using System.Text;

namespace SelfEngine;

public sealed partial class EngineSession
{
    /// <summary>下一次 Player 窗口的有界诊断采集；已有输出拒绝覆盖。null 关闭，最多 6000 帧。</summary>
    public void SetWindowProfile(string? directory,uint width=1280,uint height=720)
    {
        string? target=string.IsNullOrEmpty(directory)?null:Path.GetFullPath(directory);
        Invoke(value=>{Check(NativeMethods.SetWindowProfile(value,target,target is null?0:checked((uint)Encoding.UTF8.GetByteCount(target)),target is null?0:width,target is null?0:height));return 0;});
    }
}

internal static partial class NativeMethods
{
    [LibraryImport(Library,EntryPoint="se_session_set_window_profile",StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int SetWindowProfile(ulong session,string? path,uint length,uint width,uint height);
}

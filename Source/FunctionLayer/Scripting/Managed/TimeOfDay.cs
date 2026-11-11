using System.Runtime.InteropServices;
using System.Text.Json;

namespace SelfEngine;

public readonly record struct EnvironmentVector(float X,float Y,float Z);
public sealed record EnvironmentSnapshot(bool Active,ulong FrameIndex,long Day,double Time,double SimulationSeconds,double Rate,
    bool Daylight,EnvironmentVector SunDirection,EnvironmentVector SunColor,float SunIntensity,EnvironmentVector SkyColor,EnvironmentVector FogColor,float FogDensity);
public readonly record struct TimeChanged(long Day,double Time,double SimulationSeconds);
public readonly record struct Sunrise(long Day,double Time,double SimulationSeconds);
public readonly record struct Sunset(long Day,double Time,double SimulationSeconds);

// 读取当前 Environment 的自有值；昼夜 Profile 仍由主场景配置选择。
public sealed unsafe class TimeOfDayService(EngineSession session)
{
    public EnvironmentSnapshot Snapshot=>session.Invoke(handle=>JsonSerializer.Deserialize<EnvironmentSnapshot>(
        EngineSession.ReadString((byte* bytes,uint capacity,out uint required)=>NativeMethods.TODSnapshot(handle,bytes,capacity,out required)))
        ??throw new InvalidDataException("TOD.InvalidTime"));
    public void SetTime(long day,double time,bool replayBoundaries=false)=>session.Invoke(handle=>
    {EngineSession.Check(NativeMethods.TODSetTime(handle,day,time,replayBoundaries?1:0));return 0;});
    public void SetRate(double rate)=>session.Invoke(handle=>{EngineSession.Check(NativeMethods.TODSetRate(handle,rate));return 0;});
}
internal static unsafe partial class NativeMethods
{
    [LibraryImport(Library,EntryPoint="se_tod_snapshot")]
    internal static partial int TODSnapshot(ulong session,byte* buffer,uint capacity,out uint required);
    [LibraryImport(Library,EntryPoint="se_tod_set_time")]
    internal static partial int TODSetTime(ulong session,long day,double time,int replayBoundaries);
    [LibraryImport(Library,EntryPoint="se_tod_set_rate")]
    internal static partial int TODSetRate(ulong session,double rate);
}

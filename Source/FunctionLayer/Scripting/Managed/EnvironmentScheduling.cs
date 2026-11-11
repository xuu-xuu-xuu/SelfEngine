using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace SelfEngine;

public enum TickPhase:uint {FrameBegin,PreTick,FixedPreTick,FixedPostTick,Tick,PostTick}
public readonly record struct TickDependency(Behaviour Behaviour,TickPhase Phase,bool RequireFresh=false);
public readonly record struct SimulationClockState(ulong Steps,double SimulationSeconds,double DroppedSeconds);
internal enum EnvironmentPhase:uint {Prepare,FrameBegin,PreTick,FixedPreTick,BeforePhysics,AfterPhysics,FixedPostTick,Tick,AfterLogic,PostTick,FrameEnd,FixedEnd,PrepareFinished}

public sealed unsafe partial class EngineSession
{
    internal ScriptSystem? EnvironmentOwner;
    internal event Action<EnvironmentPhase,double>? EnvironmentPhaseChanged;
    internal event Action<ulong,TickPhase,double>? ComponentTick;
    public SimulationClockState SimulationClock=>Invoke(value=>{Check(NativeMethods.EnvironmentStats(value,out var steps,out var simulation,out var dropped));return new SimulationClockState(steps,simulation,dropped);});
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    private static int NativeEnvironmentTick(ulong _,ulong component,uint phase,double seconds,ulong frame,ulong fixedStep,nint context)
    {
        EngineSession? owner=null;
        ulong previousComponent=0;
        try
        {
            owner=GCHandle.FromIntPtr(context).Target as EngineSession;if(owner is null)return 1;++owner.callbackDepth;
            previousComponent=owner.NativeTickComponent;owner.NativeTickComponent=component;
            owner.environmentFrame=frame;
            if(component==0)owner.EnvironmentPhaseChanged?.Invoke((EnvironmentPhase)(phase&0xff),seconds);
            else owner.ComponentTick?.Invoke(component,(TickPhase)phase,seconds);
            if(component==0&&(EnvironmentPhase)(phase&0xff)==EnvironmentPhase.FrameEnd)owner.FinishSceneRetirements(frame);
            return 0;
        }
        catch(Exception exception)
        {
            if(owner is not null){owner.LastCallbackException=exception;try{owner.Diagnostic?.Invoke("Bridge.ManagedCallbackFailed",exception);}catch{}}
            return 1;
        }
        finally
        {
            if(owner is not null){owner.NativeTickComponent=previousComponent;if(owner.disposeRequested&&owner.IsWindowRunning)try{owner.RequestWindowClose();}catch{}--owner.callbackDepth;}
        }
    }
}

internal static unsafe partial class NativeMethods
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct TickPrerequisite {internal ulong Task;internal uint Mode,Reserved;}
    [LibraryImport(Library,EntryPoint="se_environment_set_callback")]
    internal static partial int SetEnvironmentCallback(ulong session,delegate* unmanaged[Cdecl]<ulong,ulong,uint,double,ulong,ulong,nint,int> callback,nint context);
    [LibraryImport(Library,EntryPoint="se_environment_configure")]
    internal static partial int EnvironmentConfigure(ulong session,double seconds,uint maximum,int automatic,int physics);
    [LibraryImport(Library,EntryPoint="se_environment_set_paused")]
    internal static partial int EnvironmentPaused(ulong session,int paused);
    [LibraryImport(Library,EntryPoint="se_environment_release")]
    internal static partial int EnvironmentRelease(ulong session);
    [LibraryImport(Library,EntryPoint="se_environment_stats")]
    internal static partial int EnvironmentStats(ulong session,out ulong steps,out double simulation,out double dropped);
    [LibraryImport(Library,EntryPoint="se_environment_fixed_step")]
    internal static partial int EnvironmentFixedStep(ulong session,double seconds);
    [LibraryImport(Library,EntryPoint="se_component_tick_register")]
    internal static partial int ComponentTickRegister(ulong session,ulong component,uint stage,int simulation,int paused,out ulong task);
    [LibraryImport(Library,EntryPoint="se_component_tick_remove")]
    internal static partial int ComponentTickRemove(ulong session,ulong task);
    [LibraryImport(Library,EntryPoint="se_component_tick_prerequisites")]
    internal static partial int ComponentTickPrerequisites(ulong session,ulong task,TickPrerequisite* values,uint count);
    [LibraryImport(Library,EntryPoint="se_entity_is_pending")]
    internal static partial int EntityPending(ulong session,ulong entity,out int pending);
    [LibraryImport(Library,EntryPoint="se_script_component_pending")]
    internal static partial int ScriptComponentPending(ulong session,ulong component,out int pending);
}

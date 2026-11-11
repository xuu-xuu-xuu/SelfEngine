using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace SelfEngine;

public readonly record struct SceneHandle(ulong Value);
public enum SceneState:uint {Unloaded,Preparing,ReadyToActivate,Active,Unloading,Failed,Cancelled}
public enum SceneLoadMode:uint {Replace,Additive}
public sealed record SceneSnapshot(ulong Id,SceneState State,bool Active,ulong Request,ulong Revision,ulong Scope,string Name,string Source,string RequestedSource,string Error,ulong[] Members);
public sealed record SceneStatistics(ulong Requested,ulong Activated,ulong Unloaded,ulong Failed,ulong Cancelled,ulong StaleResults,ulong CallbackFaults,ulong NotificationFailures,ulong PendingJobs,ulong Records,ulong Members);
public readonly record struct SceneActivated(SceneHandle Scene,ulong Request);
public readonly record struct SceneUnloaded(SceneHandle Scene,ulong Request);
public readonly record struct SceneLoadFailed(SceneHandle Scene,ulong Request,string Error);

public sealed unsafe class Scene
{
    public EngineSession Session{get;}
    public SceneHandle Handle{get;}
    public ulong Scope=>Snapshot.Scope;
    internal Scene(EngineSession session,SceneHandle handle){Session=session;Handle=handle;}
    public SceneSnapshot Snapshot=>Session.Invoke(value=>JsonSerializer.Deserialize<SceneSnapshot>(EngineSession.ReadString((byte* buffer,uint capacity,out uint required)=>NativeMethods.SceneGet(value,Handle.Value,buffer,capacity,out required)))??throw new InvalidDataException("Scene.InvalidData"));
    public ulong RequestLoad(string path,SceneLoadMode mode=SceneLoadMode.Replace,SceneHandle replacing=default)
    {
        ArgumentNullException.ThrowIfNull(path);path=Path.GetFullPath(path);
        return Session.Invoke(value=>{EngineSession.Check(NativeMethods.SceneRequestFile(value,Handle.Value,path,checked((uint)Encoding.UTF8.GetByteCount(path)),(uint)mode,replacing.Value,out var request));return request;});
    }
    public ulong RequestJson(string json,SceneLoadMode mode=SceneLoadMode.Replace,SceneHandle replacing=default)
    {
        ArgumentNullException.ThrowIfNull(json);
        return Session.Invoke(value=>{EngineSession.Check(NativeMethods.SceneRequestJson(value,Handle.Value,json,checked((uint)Encoding.UTF8.GetByteCount(json)),(uint)mode,replacing.Value,out var request));return request;});
    }
    public void Cancel()=>Session.Invoke(value=>{EngineSession.Check(NativeMethods.SceneCancel(value,Handle.Value));return 0;});
    public void Unload()=>Session.Invoke(value=>{EngineSession.Check(NativeMethods.SceneUnload(value,Handle.Value));return 0;});
    public Entity CreateEntity(string name)
    {
        ArgumentNullException.ThrowIfNull(name);
        return Session.Invoke(value=>{EngineSession.Check(NativeMethods.SceneCreateEntity(value,Handle.Value,name,checked((uint)Encoding.UTF8.GetByteCount(name)),out var entity));return new Entity(Session,entity);});
    }
}
public sealed unsafe class SceneService
{
    private readonly EngineSession session;
    internal SceneService(EngineSession session)=>this.session=session;
    public Scene? Active=>session.Invoke(value=>{EngineSession.Check(NativeMethods.SceneActive(value,out var scene));return scene==0?null:new Scene(session,new(scene));});
    public Scene Create(string name)
    {
        ArgumentNullException.ThrowIfNull(name);
        return session.Invoke(value=>{EngineSession.Check(NativeMethods.SceneCreate(value,name,checked((uint)Encoding.UTF8.GetByteCount(name)),out var scene));return new Scene(session,new(scene));});
    }
    public Scene Get(SceneHandle handle){var scene=new Scene(session,handle);_=scene.Snapshot;return scene;}
    public Scene? Of(Entity entity)
    {
        ArgumentNullException.ThrowIfNull(entity);if(entity.Session!=session)throw new ArgumentException("Scene.ContextMismatch");
        return session.Invoke(value=>{EngineSession.Check(NativeMethods.SceneOfEntity(value,entity.Handle,out var scene));return scene==0?null:new Scene(session,new(scene));});
    }
    public SceneStatistics Statistics=>session.Invoke(value=>JsonSerializer.Deserialize<SceneStatistics>(EngineSession.ReadString((byte* buffer,uint capacity,out uint required)=>NativeMethods.SceneStats(value,buffer,capacity,out required)))??throw new InvalidDataException("Scene.InvalidData"));
}
internal sealed record SceneRetirement(SceneHandle Scene,HashSet<ulong> Members,HashSet<ulong> Invalidated);
public sealed unsafe partial class EngineSession
{
    internal event Action<SceneRetirement>? SceneRetiring;
    private readonly Dictionary<ulong,ulong> retiringEntities=[];
    private ulong environmentFrame;
    internal ulong NativeTickComponent;
    private ulong sceneCallbackScope;
    internal readonly struct SceneScope(EngineSession? session,ulong previous):IDisposable
    {
        public void Dispose(){var owner=session;var prior=previous;if(owner is not null&&!owner.IsDisposed)owner.Invoke(value=>{Check(NativeMethods.SceneCallbackScope(value,prior,out _));owner.sceneCallbackScope=prior;return 0;});}
    }
    internal SceneScope EnterSceneCallbackScope(SceneHandle scene,ulong component=0)
    {
        if(IsDisposed||sceneCallbackScope==scene.Value||(component!=0&&sceneCallbackScope==0&&NativeTickComponent==component))return default;
        var previous=Invoke(value=>{Check(NativeMethods.SceneCallbackScope(value,scene.Value,out var prior));return prior;});sceneCallbackScope=scene.Value;return new(this,previous);
    }
    internal bool IsSceneRetiring(ulong entity)=>retiringEntities.ContainsKey(entity);
    private void FinishSceneRetirements(ulong frame)
    {foreach(var pair in retiringEntities.Where(pair=>pair.Value<=frame).ToArray())retiringEntities.Remove(pair.Key);}
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    private static int NativeSceneRetiring(ulong _,ulong scene,ulong* members,uint count,ulong* invalidated,uint invalidatedCount,nint context)
    {
        EngineSession? owner=null;
        try
        {
            owner=GCHandle.FromIntPtr(context).Target as EngineSession;if(owner is null)return 1;++owner.callbackDepth;
            if(count>100000||invalidatedCount>100000||(count!=0&&members==null)||(invalidatedCount!=0&&invalidated==null))return 1;
            var entities=new ReadOnlySpan<ulong>(members,(int)count).ToArray().ToHashSet();var ids=new ReadOnlySpan<ulong>(invalidated,(int)invalidatedCount).ToArray().ToHashSet();
            foreach(var entity in entities)owner.retiringEntities[entity]=owner.environmentFrame==ulong.MaxValue?ulong.MaxValue:owner.environmentFrame+1;
            owner.SceneRetiring?.Invoke(new(new(scene),entities,ids));return 0;
        }
        catch(Exception exception){if(owner is not null){owner.LastCallbackException=exception;try{owner.Diagnostic?.Invoke("Scene.CallbackFailed",exception);}catch{}}return 1;}
        finally{if(owner is not null){if(owner.disposeRequested&&owner.IsWindowRunning)try{owner.RequestWindowClose();}catch{}--owner.callbackDepth;}}
    }
}
internal static unsafe partial class NativeMethods
{
    [LibraryImport(Library,EntryPoint="se_scene_set_callback")]
    internal static partial int SetSceneCallback(ulong session,delegate* unmanaged[Cdecl]<ulong,ulong,ulong*,uint,ulong*,uint,nint,int> callback,nint context);
    [LibraryImport(Library,EntryPoint="se_scene_create",StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int SceneCreate(ulong session,string name,uint length,out ulong scene);
    [LibraryImport(Library,EntryPoint="se_scene_request_file",StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int SceneRequestFile(ulong session,ulong scene,string path,uint length,uint mode,ulong replacing,out ulong request);
    [LibraryImport(Library,EntryPoint="se_scene_request_json",StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int SceneRequestJson(ulong session,ulong scene,string text,uint length,uint mode,ulong replacing,out ulong request);
    [LibraryImport(Library,EntryPoint="se_scene_cancel")]
    internal static partial int SceneCancel(ulong session,ulong scene);
    [LibraryImport(Library,EntryPoint="se_scene_unload")]
    internal static partial int SceneUnload(ulong session,ulong scene);
    [LibraryImport(Library,EntryPoint="se_scene_active")]
    internal static partial int SceneActive(ulong session,out ulong scene);
    [LibraryImport(Library,EntryPoint="se_scene_of_entity")]
    internal static partial int SceneOfEntity(ulong session,ulong entity,out ulong scene);
    [LibraryImport(Library,EntryPoint="se_scene_get")]
    internal static partial int SceneGet(ulong session,ulong scene,byte* buffer,uint capacity,out uint required);
    [LibraryImport(Library,EntryPoint="se_scene_stats")]
    internal static partial int SceneStats(ulong session,byte* buffer,uint capacity,out uint required);
    [LibraryImport(Library,EntryPoint="se_scene_create_entity",StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int SceneCreateEntity(ulong session,ulong scene,string name,uint length,out ulong entity);
    [LibraryImport(Library,EntryPoint="se_scene_callback_scope")]
    internal static partial int SceneCallbackScope(ulong session,ulong scene,out ulong previous);
}

using System.Buffers.Binary;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace SelfEngine;

public enum EventPhase:uint {NextFrame,AfterPhysics,AfterLogic}
public enum EventPostResult:uint {Accepted,Closed,UnknownType,UnsupportedVersion,InvalidScope,InvalidTarget,Full,TooLarge,SequenceExhausted}
[AttributeUsage(AttributeTargets.Struct,Inherited=false)]
public sealed class EventIdAttribute(string key,uint version=1):Attribute {public string Key{get;}=key;public uint Version{get;}=version;}
public readonly record struct SceneStateChanged(bool Active,uint FormatVersion);
[StructLayout(LayoutKind.Sequential)]
public readonly struct EventHeader
{
    public readonly ulong World,Channel,Type,Source,SourceStableId,Target,Scene,Frame,FixedStep,Sequence;
    public readonly uint Version;
    public readonly EventPhase Phase;
}
[StructLayout(LayoutKind.Sequential)]
public struct EventStatistics
{
    public ulong Accepted,Delivered,Rejected,DroppedTargets,NoReceiver,CallbackFailures,InvalidPayload,Pending,Queued,PendingBytes,PeakPending,PeakBytes;
    public uint Closed;
    private uint reserved;
}
internal sealed class NativeEventPublisher(ulong value):SafeHandle((nint)value,true)
{
    public override bool IsInvalid=>handle==0;
    protected override bool ReleaseHandle()=>NativeMethods.EventPublisherClose((ulong)handle)==0;
    internal unsafe EventPostResult Post(ulong type,uint version,byte[] bytes,EventPhase phase,ulong source,ulong stable,ulong target,ulong scene)
    {
        bool added=false;
        try
        {
            DangerousAddRef(ref added);
            fixed(byte* data=bytes){EngineSession.Check(NativeMethods.EventPublish((ulong)DangerousGetHandle(),type,version,(uint)phase,data,checked((uint)bytes.Length),source,stable,target,scene,out var result));return (EventPostResult)result;}
        }
        catch(ObjectDisposedException){return EventPostResult.Closed;}
        finally{if(added)DangerousRelease();}
    }
}
// 只持有弱原生发布端和数值编解码器，后台任务不能借此进入 World/Session。
public sealed class ScriptEventPublisher<T>
{
    private readonly NativeEventPublisher publisher;
    private readonly Func<T,byte[]> encode;
    private readonly ulong type;
    private readonly uint version;
    internal ScriptEventPublisher(NativeEventPublisher publisher,ulong type,uint version,Func<T,byte[]> encode)
    {this.publisher=publisher;this.type=type;this.version=version;this.encode=encode;}
    public EventPostResult Publish(T value,EventPhase phase=EventPhase.AfterLogic,ulong source=0,ulong sourceStableId=0,ulong target=0,ulong scene=0)
        =>publisher.IsClosed?EventPostResult.Closed:publisher.Post(type,version,encode(value),phase,source,sourceStableId,target,scene);
}
public sealed unsafe class ScriptEvents
{
    private sealed record Codec<T>(ulong Id,string Key,uint Version,uint Maximum,Func<T,byte[]> Encode,Func<byte[],T> Decode);
    private sealed record Subscription(Behaviour Owner);
    private sealed class Token(ScriptEvents registry,ulong id):IDisposable
    {
        private readonly WeakReference<ScriptEvents> registry=new(registry);
        private ulong id=id;
        public void Dispose(){if(id==0)return;if(registry.TryGetTarget(out var owner))owner.Release(id);id=0;}
    }
    private sealed record UiWire(UiEventKind Kind,ulong Target,double Value,string Text,UiBinding? Binding,uint? SourceEpoch);
    private readonly ScriptSystem system;
    private readonly Dictionary<Type,object> codecs=[];
    private readonly Dictionary<ulong,Subscription> subscriptions=[];
    private NativeEventPublisher? publisher;
    private bool closed;
    internal ScriptEvents(ScriptSystem system)=>this.system=system;
    public int Count=>subscriptions.Count;
    public EventStatistics Statistics=>system.Session.Invoke(handle=>{EngineSession.Check(NativeMethods.EventStats(handle,out var value));return value;});
    public string TraceJson=>system.Session.Invoke(handle=>EngineSession.ReadString((byte* buffer,uint size,out uint required)=>NativeMethods.EventTrace(handle,buffer,size,out required)));
    private NativeEventPublisher Publisher=>publisher??=system.Session.Invoke(handle=>{EngineSession.Check(NativeMethods.EventPublisherCreate(handle,out var value));return new NativeEventPublisher(value);});
    internal void Open()=>closed=false;
    public ScriptEventPublisher<T> CreatePublisher<T>()
    {
        if(closed)throw new InvalidOperationException("Event.Closed");var codec=GetCodec<T>();return new(Publisher,codec.Id,codec.Version,codec.Encode);
    }
    public EventPostResult Publish<T>(T value,EventPhase phase=EventPhase.AfterLogic,ulong source=0,ulong sourceStableId=0,ulong target=0,ulong scene=0) where T:notnull
        =>closed?EventPostResult.Closed:CreatePublisher<T>().Publish(value,phase,source,sourceStableId,target,scene);
    public IDisposable Subscribe<T>(Behaviour owner,Action<T> handler)=>Subscribe<T>(owner,(_,value)=>handler(value));
    public IDisposable Subscribe<T>(Behaviour owner,Action<EventHeader,T> handler,ulong target=0,ulong scene=0)
    {
        ArgumentNullException.ThrowIfNull(handler);
        if(closed||owner.Owner!=system)throw new ArgumentException("Script.NotAttached");
        var codec=GetCodec<T>();var channel=Publisher;
        var id=system.Session.Invoke(handle=>{EngineSession.Check(NativeMethods.EventSubscribe(handle,(ulong)channel.DangerousGetHandle(),system.ComponentHandle(owner),codec.Id,target,scene,out var token));return token;});
        subscriptions.Add(id,new(owner));
        system.Session.EventHandlers.Add(id,(header,bytes)=>
        {
            if(!system.CanRun(owner))return 2;
            T value;
            try{if(header.Type!=codec.Id||header.Version!=codec.Version)throw new InvalidDataException();value=codec.Decode(bytes);}
            catch(Exception exception)
            {system.Report(owner,"Event.InvalidPayload",exception.Message,"EventDecode");return 3;}
            var success=system.InvokeEvent(owner,()=>handler(header,value));system.FlushMutations();return success?0:1;
        });
        return new Token(this,id);
    }
    private void Release(ulong id)
    {
        if(!subscriptions.Remove(id))return;system.Session.EventHandlers.Remove(id);
        if(!system.Session.IsDisposed)system.Session.Invoke(handle=>{EngineSession.Check(NativeMethods.EventUnsubscribe(handle,id));return 0;});
    }
    internal void Remove(Behaviour owner){foreach(var id in subscriptions.Where(pair=>pair.Value.Owner==owner).Select(pair=>pair.Key).ToArray())Release(id);}
    internal void Clear()
    {
        closed=true;publisher?.Dispose();publisher=null;
        foreach(var id in subscriptions.Keys.ToArray())Release(id);
        // 项目 Type 和 JsonSerializerOptions 只属于本次运行，不能存入进程静态缓存。
        codecs.Clear();
    }
    private Codec<T> GetCodec<T>()
    {
        if(codecs.TryGetValue(typeof(T),out var prior))return (Codec<T>)prior;
        Codec<T> codec;
        if(typeof(T)==typeof(int))codec=(Codec<T>)(object)new Codec<int>(1,"engine.int32",1,4,value=>{var bytes=new byte[4];BinaryPrimitives.WriteInt32LittleEndian(bytes,value);return bytes;},bytes=>{Length(bytes,4);return BinaryPrimitives.ReadInt32LittleEndian(bytes);});
        else if(typeof(T)==typeof(string))
        {var utf8=new UTF8Encoding(false,true);codec=(Codec<T>)(object)new Codec<string>(5,"engine.utf8",1,65536,utf8.GetBytes,utf8.GetString);}
        else if(typeof(T)==typeof(PhysicsContact))codec=(Codec<T>)(object)new Codec<PhysicsContact>(2,"engine.physics_contact",1,24,value=>{
            var bytes=new byte[24];BinaryPrimitives.WriteUInt64LittleEndian(bytes,value.EntityA.StableId);BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(8),value.EntityB.StableId);BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(16),value.Began?1:0);BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(20),value.Ended?1:0);return bytes;
        },bytes=>{Length(bytes,24);var began=BinaryPrimitives.ReadInt32LittleEndian(bytes.AsSpan(16));var ended=BinaryPrimitives.ReadInt32LittleEndian(bytes.AsSpan(20));if(began is <0 or >1||ended is <0 or >1)throw new InvalidDataException();return new(new(BinaryPrimitives.ReadUInt64LittleEndian(bytes)),new(BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(8))),began!=0,ended!=0);});
        else if(typeof(T)==typeof(SceneStateChanged))codec=(Codec<T>)(object)new Codec<SceneStateChanged>(4,"engine.scene_state",1,8,value=>{var bytes=new byte[8];BinaryPrimitives.WriteUInt32LittleEndian(bytes,value.Active?1u:0u);BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4),value.FormatVersion);return bytes;},bytes=>{Length(bytes,8);var active=BinaryPrimitives.ReadUInt32LittleEndian(bytes);if(active>1)throw new InvalidDataException();return new(active!=0,BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(4)));});
        else if(typeof(T)==typeof(SceneActivated))codec=(Codec<T>)(object)new Codec<SceneActivated>(16,"engine.scene_activated",1,4096,value=>EncodeScene(value.Scene,value.Request,SceneState.Active,""),bytes=>{var value=DecodeScene(bytes,SceneState.Active);return new(value.Scene,value.Request);});
        else if(typeof(T)==typeof(SceneUnloaded))codec=(Codec<T>)(object)new Codec<SceneUnloaded>(17,"engine.scene_unloaded",1,4096,value=>EncodeScene(value.Scene,value.Request,SceneState.Unloaded,""),bytes=>{var value=DecodeScene(bytes,SceneState.Unloaded);return new(value.Scene,value.Request);});
        else if(typeof(T)==typeof(SceneLoadFailed))codec=(Codec<T>)(object)new Codec<SceneLoadFailed>(18,"engine.scene_load_failed",1,4096,value=>EncodeScene(value.Scene,value.Request,SceneState.Failed,value.Error),bytes=>{var value=DecodeScene(bytes,SceneState.Failed);return new(value.Scene,value.Request,value.Error);});
        else if(typeof(T)==typeof(TimeChanged))codec=(Codec<T>)(object)new Codec<TimeChanged>(19,"engine.tod_time_changed",1,24,value=>EncodeTime(value.Day,value.Time,value.SimulationSeconds),bytes=>{var v=DecodeTime(bytes);return new(v.Day,v.Time,v.Seconds);});
        else if(typeof(T)==typeof(Sunrise))codec=(Codec<T>)(object)new Codec<Sunrise>(20,"engine.tod_sunrise",1,24,value=>EncodeTime(value.Day,value.Time,value.SimulationSeconds),bytes=>{var v=DecodeTime(bytes);return new(v.Day,v.Time,v.Seconds);});
        else if(typeof(T)==typeof(Sunset))codec=(Codec<T>)(object)new Codec<Sunset>(21,"engine.tod_sunset",1,24,value=>EncodeTime(value.Day,value.Time,value.SimulationSeconds),bytes=>{var v=DecodeTime(bytes);return new(v.Day,v.Time,v.Seconds);});
        else if(typeof(T)==typeof(UiEvent))
        {
            var options=Options();codec=(Codec<T>)(object)new Codec<UiEvent>(3,"engine.ui_event",1,65536,value=>JsonSerializer.SerializeToUtf8Bytes(new UiWire(value.Kind,value.Target,value.Value,value.Text,value.Binding,value.SourceEpoch),options),bytes=>{
                var value=JsonSerializer.Deserialize<UiWire>(bytes,options)??throw new InvalidDataException();if(!Enum.IsDefined(value.Kind)||value.Text is null||!double.IsFinite(value.Value))throw new InvalidDataException();return new(value.Kind,value.Target,value.Value,value.Text){Binding=value.Binding,SourceEpoch=value.SourceEpoch};});
        }
        else
        {
            var type=typeof(T);var attribute=type.GetCustomAttribute<EventIdAttribute>();
            var properties=type.GetProperties(BindingFlags.Instance|BindingFlags.Public).OrderBy(property=>property.Name,StringComparer.Ordinal).ToArray();
            if(attribute is null||!type.IsValueType||!UiBinding.ValidName(attribute.Key)||attribute.Version==0||type.GetFields(BindingFlags.Instance|BindingFlags.Public).Length!=0
                ||properties.Any(property=>!Scalar(property.PropertyType)||property.GetIndexParameters().Length!=0||property.GetMethod is null||property.SetMethod is null))throw new ArgumentException("Event.UnsupportedPayload");
            // 不把可卸载项目 Type 交给 System.Text.Json 的共享元数据缓存；仅编解码明确允许的标量。
            codec=new(Hash(attribute.Key),attribute.Key,attribute.Version,65536,value=>EncodeFlat(value!,properties),bytes=>(T)DecodeFlat(bytes,type,properties));
        }
        system.Session.Invoke(handle=>{EngineSession.Check(NativeMethods.EventRegisterType(handle,codec.Id,codec.Version,codec.Key,checked((uint)Encoding.UTF8.GetByteCount(codec.Key)),codec.Maximum));return 0;});
        codecs.Add(typeof(T),codec);return codec;
    }
    private static JsonSerializerOptions Options()=>new(){UnmappedMemberHandling=JsonUnmappedMemberHandling.Disallow,MaxDepth=8};
    private static void ValidateTime(long day,double time,double seconds)
    {if(day is <-1000000000 or >1000000000||!double.IsFinite(time)||time<0||time>=1||!double.IsFinite(seconds)||seconds<0)throw new InvalidDataException("TOD.InvalidTime");}
    private static byte[] EncodeTime(long day,double time,double seconds)
    {
        ValidateTime(day,time,seconds);var bytes=new byte[24];BinaryPrimitives.WriteInt64LittleEndian(bytes,day);
        BinaryPrimitives.WriteDoubleLittleEndian(bytes.AsSpan(8),time);BinaryPrimitives.WriteDoubleLittleEndian(bytes.AsSpan(16),seconds);return bytes;
    }
    private static (long Day,double Time,double Seconds) DecodeTime(byte[] bytes)
    {
        Length(bytes,24);var day=BinaryPrimitives.ReadInt64LittleEndian(bytes);var time=BinaryPrimitives.ReadDoubleLittleEndian(bytes.AsSpan(8));var seconds=BinaryPrimitives.ReadDoubleLittleEndian(bytes.AsSpan(16));
        ValidateTime(day,time,seconds);return(day,time,seconds);
    }
    private static byte[] EncodeScene(SceneHandle scene,ulong request,SceneState state,string error)
    {
        var text=new UTF8Encoding(false,true).GetBytes(error);var bytes=new byte[24+text.Length];BinaryPrimitives.WriteUInt64LittleEndian(bytes,scene.Value);BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(8),request);BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(16),(uint)state);BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(20),checked((uint)text.Length));text.CopyTo(bytes,24);return bytes;
    }
    private static (SceneHandle Scene,ulong Request,string Error) DecodeScene(byte[] bytes,SceneState expected)
    {
        if(bytes.Length<24||bytes.Length>4096||BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(16))!=(uint)expected||BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(20))!=bytes.Length-24||BinaryPrimitives.ReadUInt64LittleEndian(bytes)==0)throw new InvalidDataException();
        return(new(BinaryPrimitives.ReadUInt64LittleEndian(bytes)),BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(8)),new UTF8Encoding(false,true).GetString(bytes,24,bytes.Length-24));
    }
    private static bool Scalar(Type type)=>type.IsEnum||type==typeof(bool)||type==typeof(int)||type==typeof(uint)||type==typeof(long)||type==typeof(ulong)||type==typeof(float)||type==typeof(double)||type==typeof(string)||type==typeof(EntityReference);
    private static byte[] EncodeFlat(object value,PropertyInfo[] properties)
    {
        using var stream=new MemoryStream();using(var writer=new Utf8JsonWriter(stream))
        {
            writer.WriteStartObject();foreach(var property in properties)
            {
                writer.WritePropertyName(property.Name);var field=property.GetValue(value);
                switch(field)
                {
                    case null:writer.WriteNullValue();break;
                    case string text:writer.WriteStringValue(text);break;
                    case bool boolean:writer.WriteBooleanValue(boolean);break;
                    case int number:writer.WriteNumberValue(number);break;
                    case uint number:writer.WriteNumberValue(number);break;
                    case long number:writer.WriteNumberValue(number);break;
                    case ulong number:writer.WriteNumberValue(number);break;
                    case float number:writer.WriteNumberValue(number);break;
                    case double number:writer.WriteNumberValue(number);break;
                    case EntityReference reference:writer.WriteStartObject();writer.WriteNumber("StableId",reference.StableId);writer.WriteEndObject();break;
                    case Enum enumeration:if(Enum.GetUnderlyingType(property.PropertyType)==typeof(ulong))writer.WriteNumberValue(Convert.ToUInt64(enumeration));else writer.WriteNumberValue(Convert.ToInt64(enumeration));break;
                    default:throw new InvalidDataException("Event.UnsupportedPayload");
                }
            }
            writer.WriteEndObject();
        }
        return stream.ToArray();
    }
    private static object DecodeFlat(byte[] bytes,Type type,PropertyInfo[] properties)
    {
        using var document=JsonDocument.Parse(bytes,new(){MaxDepth=8});var root=document.RootElement;
        if(root.ValueKind!=JsonValueKind.Object||root.EnumerateObject().Count()!=properties.Length)throw new InvalidDataException();
        var value=Activator.CreateInstance(type)??throw new InvalidDataException();
        foreach(var property in properties)
        {
            if(!root.TryGetProperty(property.Name,out var field))throw new InvalidDataException();var scalar=property.PropertyType;object? decoded;
            if(scalar==typeof(string))decoded=field.GetString();
            else if(scalar==typeof(bool))decoded=field.GetBoolean();
            else if(scalar==typeof(int))decoded=field.GetInt32();
            else if(scalar==typeof(uint))decoded=field.GetUInt32();
            else if(scalar==typeof(long))decoded=field.GetInt64();
            else if(scalar==typeof(ulong))decoded=field.GetUInt64();
            else if(scalar==typeof(float)){var number=field.GetSingle();if(!float.IsFinite(number))throw new InvalidDataException();decoded=number;}
            else if(scalar==typeof(double)){var number=field.GetDouble();if(!double.IsFinite(number))throw new InvalidDataException();decoded=number;}
            else if(scalar==typeof(EntityReference)){if(field.ValueKind!=JsonValueKind.Object||field.EnumerateObject().Count()!=1||!field.TryGetProperty("StableId",out var stable))throw new InvalidDataException();decoded=new EntityReference(stable.GetUInt64());}
            else if(scalar.IsEnum){decoded=Enum.GetUnderlyingType(scalar)==typeof(ulong)?Enum.ToObject(scalar,field.GetUInt64()):Enum.ToObject(scalar,field.GetInt64());if(!Enum.IsDefined(scalar,decoded))throw new InvalidDataException();}
            else throw new InvalidDataException();
            property.SetValue(value,decoded);
        }
        return value;
    }
    private static void Length(byte[] bytes,int expected){if(bytes.Length!=expected)throw new InvalidDataException();}
    private static ulong Hash(string key){ulong hash=14695981039346656037;foreach(var value in Encoding.UTF8.GetBytes(key))hash=unchecked((hash^value)*1099511628211);return hash|0x8000000000000000;}
}

public sealed unsafe partial class EngineSession
{
    internal readonly Dictionary<ulong,Func<EventHeader,byte[],int>> EventHandlers=[];
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    private static int NativeEvent(ulong _,ulong token,EventHeader* header,byte* data,uint length,nint context)
    {
        EngineSession? owner=null;
        try
        {
            owner=GCHandle.FromIntPtr(context).Target as EngineSession;if(owner is null)return 2;++owner.callbackDepth;
            if(header==null||length>65536||(length!=0&&data==null))return 3;
            return owner.EventHandlers.TryGetValue(token,out var callback)?callback(*header,new ReadOnlySpan<byte>(data,(int)length).ToArray()):2;
        }
        catch(Exception exception){if(owner is not null){owner.LastCallbackException=exception;try{owner.Diagnostic?.Invoke("Bridge.ManagedCallbackFailed",exception);}catch{}}return 1;}
        finally{if(owner is not null){if(owner.disposeRequested&&owner.IsWindowRunning)try{owner.RequestWindowClose();}catch{}--owner.callbackDepth;}}
    }
}
internal static unsafe partial class NativeMethods
{
    [LibraryImport(Library,EntryPoint="se_event_set_callback")]
    internal static partial int SetEventCallback(ulong session,delegate* unmanaged[Cdecl]<ulong,ulong,EventHeader*,byte*,uint,nint,int> callback,nint context);
    [LibraryImport(Library,EntryPoint="se_event_register_type",StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int EventRegisterType(ulong session,ulong type,uint version,string key,uint length,uint maximum);
    [LibraryImport(Library,EntryPoint="se_event_publisher_create")]
    internal static partial int EventPublisherCreate(ulong session,out ulong publisher);
    [LibraryImport(Library,EntryPoint="se_event_publisher_close")]
    internal static partial int EventPublisherClose(ulong publisher);
    [LibraryImport(Library,EntryPoint="se_event_publish")]
    internal static partial int EventPublish(ulong publisher,ulong type,uint version,uint phase,byte* bytes,uint length,ulong source,ulong stable,ulong target,ulong scene,out uint result);
    [LibraryImport(Library,EntryPoint="se_event_subscribe")]
    internal static partial int EventSubscribe(ulong session,ulong publisher,ulong component,ulong type,ulong target,ulong scene,out ulong subscription);
    [LibraryImport(Library,EntryPoint="se_event_unsubscribe")]
    internal static partial int EventUnsubscribe(ulong session,ulong subscription);
    [LibraryImport(Library,EntryPoint="se_event_stats")]
    internal static partial int EventStats(ulong session,out EventStatistics stats);
    [LibraryImport(Library,EntryPoint="se_event_trace")]
    internal static partial int EventTrace(ulong session,byte* buffer,uint capacity,out uint required);
}

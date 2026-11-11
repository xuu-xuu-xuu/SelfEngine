using SelfEngine;
using System.Runtime.InteropServices;
using System.Text.Json;

internal static class EventContractTests
{
    private static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    public static void Run()
    {
        Check(Marshal.SizeOf<EventHeader>()==88&&Marshal.SizeOf<EventStatistics>()==104,"fixed event ABI");
        using var first=new EngineSession();using var other=new EngineSession();
        using var scripts=new ScriptSystem(first,new ScriptRegistry(),automaticFixedUpdates:false,simulatePhysics:false);
        using var isolated=new ScriptSystem(other,new ScriptRegistry(),automaticFixedUpdates:false,simulatePhysics:false);
        var a=scripts.Attach<EventContractProbe>(first.CreateEntity("a"));var b=isolated.Attach<EventContractProbe>(other.CreateEntity("b"));scripts.Start();isolated.Start();
        var mainThread=Environment.CurrentManagedThreadId;var values=new List<int>();var headers=new List<EventHeader>();
        using var messages=scripts.Events.Subscribe<EventContractValue>(a,(header,value)=>{Check(Environment.CurrentManagedThreadId==mainThread,"worker cannot execute receiver");values.Add(value.Value);headers.Add(header);Check(value.Label=="自有值"&&value.Source.StableId==17,"flat record owns string and stable reference");});
        var typed=scripts.Events.CreatePublisher<EventContractValue>();
        Task.Run(()=>{for(int i=0;i<32;++i)Check(typed.Publish(new(i,"自有值",new(17)))==EventPostResult.Accepted,"worker accepted");}).GetAwaiter().GetResult();
        Check(values.Count==0,"workers only enqueue");first.Step(0);other.Step(0);
        Check(values.SequenceEqual(Enumerable.Range(0,32))&&b.Values.Count==0&&headers.All(header=>header.World==headers[0].World&&header.Version==1)&&headers.Select(header=>header.Sequence).SequenceEqual(headers.Select(header=>header.Sequence).Order()),"FIFO, owned schema and world isolation");
        a.Handler=(header,value)=>{if(value==1)scripts.Events.Publish(2);if(value==3)scripts.Events.Publish(4,EventPhase.AfterLogic);};
        scripts.Events.Publish(1);Check(a.Values.Count==0,"normal publication is deferred");first.Step(0);Check(a.Values.SequenceEqual(new[]{1}),"same-phase publication waits for next batch");first.Step(0);Check(a.Values.SequenceEqual(new[]{1,2}),"reentrant message arrives once");
        scripts.Events.Publish(3,EventPhase.NextFrame);first.Step(0);Check(a.Values.SequenceEqual(new[]{1,2,3,4}),"NextFrame callback can post to later same-frame AfterLogic");
        var target=first.CreateEntity("doomed");var stable=target.StableId;
        scripts.Events.Publish(8,target:target.Handle);target.Destroy();first.Step(0);Check(!a.Values.Contains(8)&&scripts.Events.Statistics.DroppedTargets==1,"target is resolved again at delivery");
        EventHeader historical=default;using var history=scripts.Events.Subscribe<int>(a,(header,value)=>{if(value==9)historical=header;});
        scripts.Events.Publish(9,source:target.Handle,sourceStableId:stable);first.Step(0);Check(historical.Source==target.Handle&&historical.SourceStableId==stable,"historical source is information, not a borrowed object");
        var noReceiver=scripts.Events.Statistics.NoReceiver;Check(scripts.Events.Publish(42,scene:ulong.MaxValue)==EventPostResult.Accepted,"available scene resolver checks live membership at delivery");first.Step(0);Check(!a.Values.Contains(42)&&scripts.Events.Statistics.NoReceiver==noReceiver+1,"unknown scene never reaches a receiver");
        a.Enabled=false;scripts.Events.Publish(10);first.Step(0);Check(!a.Values.Contains(10),"disabled receiver skipped at dispatch");a.Enabled=true;
        var commandCount=0;using(var command=scripts.Commands.Register<int>(a,"test.increment",value=>{commandCount+=value;return value>0;}))
        {Check(scripts.Commands.Execute("test.increment",2)&&commandCount==2&&!scripts.Commands.Execute("missing",1),"explicit command result");scripts.SimulationPaused=true;Check(!scripts.Commands.Execute("test.increment",1)&&commandCount==2,"paused command owner rejected");scripts.SimulationPaused=false;}
        Check(!scripts.Commands.Execute("test.increment",1),"command token removes its single owner");
        var trace=JsonDocument.Parse(scripts.Events.TraceJson);Check(trace.RootElement.EnumerateArray().Any(row=>row.GetProperty("deliveryFrame").GetUInt64()>0&&row.GetProperty("type").GetUInt64()!=0),"trace exposes source and delivery frame");
        var oldEvents=scripts.Events;scripts.Events.Publish(99);scripts.Stop();
        Check(scripts.Events.Count==0&&scripts.Commands.Count==0&&typed.Publish(new(99,"自有值",new(17)))==EventPostResult.Closed&&oldEvents.Publish(1)==EventPostResult.Closed,"Stop closes retained publishers and clears callbacks");
        var fresh=scripts.Attach<EventContractProbe>(first.CreateEntity("fresh"));scripts.Start();first.Step(0);Check(fresh.Values.Count==0,"old queued channel cannot deliver to restarted scripts");
        var flood=scripts.Events.CreatePublisher<int>();int accepted=0;while(flood.Publish(21)==EventPostResult.Accepted)++accepted;
        Check(accepted>0&&accepted<=4096&&scripts.Events.Statistics.Rejected>0&&scripts.Events.Statistics.Pending==(ulong)accepted,"bounded queue returns full with visible statistics");
        first.Step(0);Check(fresh.Values.Count==accepted&&scripts.Events.Statistics.Pending==0,"drain releases queued and in-flight budget");
        var late=scripts.Events.CreatePublisher<int>();using var go=new ManualResetEventSlim();EventPostResult lateResult=EventPostResult.Accepted;
        var lateTask=Task.Run(()=>{go.Wait();lateResult=late.Publish(55);});scripts.Stop();go.Set();lateTask.GetAwaiter().GetResult();Check(lateResult==EventPostResult.Closed,"background completion after Stop cannot resurrect owner");
        var scene=first.ExportScene();first.ImportScene(scene);var sceneProbe=scripts.Attach<EventContractProbe>(first.CreateEntity("scene observer"));scripts.Start();int sceneEvents=0;
        using var sceneSubscription=scripts.Events.Subscribe<SceneStateChanged>(sceneProbe,value=>{Check(value.Active&&value.FormatVersion==2,"scene committed value");++sceneEvents;});
        first.Step(0);Check(sceneEvents==1,"successful scene replacement posts minimum state notification");scripts.Stop();
        var proof=Path.Combine(AppContext.BaseDirectory,"Validation","E04",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(proof);
        File.WriteAllText(Path.Combine(proof,"managed-events.json"),JsonSerializer.Serialize(new{success=true,workerMessages=32,ownedFlatSchema=true,mainThreadCallbacks=true,worldIsolation=true,samePhaseDeferred=true,laterPhaseSameFrame=true,staleTarget=true,historicalSource=true,queueCapacity=accepted,lateClosed=true,restartIsolated=true,commands=true,sceneNotification=true}));
        Console.WriteLine("EventContractTests passed (phase ABI, owned schemas, workers, commands, scope, budget and close).");
    }
}
[EventId("test.event_value")] public readonly record struct EventContractValue(int Value,string Label,EntityReference Source);
[ScriptId("test.event_probe")] public sealed class EventContractProbe:Behaviour
{
    public readonly List<int> Values=[];
    public Action<EventHeader,int>? Handler;
    public override void OnCreate()=>Scripts.Events.Subscribe<int>(this,(header,value)=>{Values.Add(value);Handler?.Invoke(header,value);});
}

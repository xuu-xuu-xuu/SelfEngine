using SelfEngine;
using System.Runtime.CompilerServices;
using System.Text.Json;

internal static class SchedulingContractTests
{
    private static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static WeakReference StopWithDispose(EngineSession session)
    {
        var scripts=new ScriptSystem(session,new ScriptRegistry());var probe=scripts.Attach<DisposeOnDestroyProbe>(session.CreateEntity("dispose from destroy"));scripts.Start();scripts.Stop();
        Check(probe.Destroyed==1&&scripts.Count==0&&scripts.Events.Count==0,"stop cleanup occurs once");
        session.Physics.Step(1.0/60);bool disposed=false;try{scripts.Attach<InputEdgesProbe>(session.CreateEntity("after disposal"));}catch(ObjectDisposedException){disposed=true;}
        Check(disposed,"Dispose requested by OnDestroy survives Stop queue clearing");return new WeakReference(scripts);
    }
    public static void Run()
    {
        var records=new List<object>();
        foreach(var fps in new[]{30,60,120,144})
        {
            using var session=new EngineSession();using var scripts=new ScriptSystem(session,new ScriptRegistry(),simulatePhysics:false);var probe=scripts.Attach<InputEdgesProbe>(session.CreateEntity("input edges"));scripts.Start();
            for(int frame=0;frame<fps;++frame)
            {
                scripts.SetInput(new ScriptInput(frame==0?[87]:[],frame==0?[87]:[],frame==1?[87]:[],false,new(new(8,9),frame==0?1u:0u,frame==0?1u:0u,frame==1?1u:0u,frame==0?2:0,false)));
                session.Step(1.0/fps);
            }
            Check(probe.Updates==fps&&probe.FixedCalls==60&&probe.UpdatePresses==1&&probe.UpdateReleases==1&&probe.FixedPresses==1&&probe.FixedReleases==1&&probe.UpdateClicks==1&&probe.UpdateUps==1&&probe.FixedClicks==1&&probe.FixedUps==1&&probe.FixedWheel==2&&scripts.DroppedFixedSteps==0,"display rates preserve fixed timing and consume each key/pointer edge exactly once");
            records.Add(new{kind="input",fps,probe.Updates,probe.FixedCalls,probe.UpdatePresses,probe.UpdateReleases,probe.FixedPresses,probe.FixedReleases,probe.FixedClicks,probe.FixedUps,probe.FixedWheel});
        }
        foreach(bool inEvent in new[]{false,true})
        {
            for(int cycle=0;cycle<32;++cycle)
            {
                using var session=new EngineSession();using var scripts=new ScriptSystem(session,new ScriptRegistry(),automaticFixedUpdates:false,simulatePhysics:false);
                var actor=scripts.Attach<MutationProbe>(session.CreateEntity("actor"),value=>{value.Act=true;value.InEvent=inEvent;});
                var victim=scripts.Attach<MutationProbe>(session.CreateEntity("victim"));var healthy=scripts.Attach<MutationProbe>(session.CreateEntity("healthy"));actor.Victim=EntityReference.From(victim.Entity);scripts.Start();
                if(inEvent){scripts.Events.Publish(1);Check(actor.Received==0,"event mutation is deferred to AfterLogic");session.Step(0);}else scripts.FixedUpdate(1.0/60);
                Check(actor.Spawned is null,"callback spawn waits until the next FrameBegin");session.Step(0);
                Check(actor.Destroyed==1&&victim.Destroyed==1&&!actor.Entity.IsAlive&&!victim.Entity.IsAlive&&actor.Spawned is { IsAlive:true }&&scripts.Count==1&&scripts.Events.Count==1,"callback self/other destruction is queued once and removes subscriptions");
                Check(victim.FixedCalls==0&&victim.Received==0&&(inEvent?healthy.Received==1:healthy.FixedCalls==1),"destroying target is skipped later in same dispatch snapshot");
                var oldActor=actor.Received;scripts.Events.Publish(3);session.Step(0);Check(actor.Received==oldActor&&healthy.Received==(inEvent?4:3),"removed owner never receives later events");
                var replacement=session.CreateEntity("slot reuse");bool stale=false;try{_ = actor.Entity.Transform;}catch(EngineException exception){stale=exception.Key=="Bridge.InvalidEntity";}
                Check(stale&&replacement.Handle!=actor.Entity.Handle,"callback stale proxy cannot access reused slot");scripts.Stop();Check(healthy.Destroyed==1&&scripts.Count==0&&scripts.Events.Count==0,"all remaining callbacks and tokens cleanly stop");
            }
            records.Add(new{kind="mutations",phase=inEvent?"Event":"OnFixedUpdate",cycles=32,selfAndOtherDestroyed=64,remainingSubscriptions=0});
        }
        using(var session=new EngineSession())
        {
            var weak=StopWithDispose(session);for(int attempt=0;weak.IsAlive&&attempt<10;++attempt){GC.Collect();GC.WaitForPendingFinalizers();GC.Collect();}
            Check(!weak.IsAlive,"disposed script system is not rooted by live session callbacks or physics clock");records.Add(new{kind="dispose_from_destroy",clockReleased=true,collected=true});
        }
        var directory=Path.Combine(AppContext.BaseDirectory,"Validation","T48",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(directory);
        File.WriteAllText(Path.Combine(directory,"scheduling.json"),JsonSerializer.Serialize(new{schema=1,success=true,records},new JsonSerializerOptions{WriteIndented=true}));Console.WriteLine("Scheduling contract passed; evidence: "+directory);
    }
}

[ScriptId("tests.input_edges")]
public sealed class InputEdgesProbe:Behaviour
{
    public int Updates,FixedCalls,UpdatePresses,UpdateReleases,FixedPresses,FixedReleases,UpdateClicks,UpdateUps,FixedClicks,FixedUps;public float FixedWheel;
    public override void OnUpdate(double seconds){++Updates;if(Input.Pressed(87))++UpdatePresses;if(Input.Released(87))++UpdateReleases;if(Input.Pointer.Pressed(MouseButton.Left))++UpdateClicks;if(Input.Pointer.Released(MouseButton.Left))++UpdateUps;}
    public override void OnFixedUpdate(double seconds){++FixedCalls;if(Input.Pressed(87))++FixedPresses;if(Input.Released(87))++FixedReleases;if(Input.Pointer.Pressed(MouseButton.Left))++FixedClicks;if(Input.Pointer.Released(MouseButton.Left))++FixedUps;FixedWheel+=Input.Pointer.Wheel;}
}
[ScriptId("tests.mutation")]
public sealed class MutationProbe:Behaviour
{
    public bool Act,InEvent;public EntityReference Victim;public Entity? Spawned;public int Destroyed,Received,FixedCalls;
    public override void OnCreate()=>Subscribe<int>(value=>{Received+=value;if(Act&&InEvent)Change();});
    private void Change(){Act=false;var system=Scripts;if(Victim.Resolve(Entity.Session) is {} victim)system.Destroy(victim);DestroySelf();system.Spawn("spawn requested before destruction",entity=>Spawned=entity);}
    public override void OnFixedUpdate(double seconds){++FixedCalls;if(Act&&!InEvent)Change();}
    public override void OnDestroy()=>++Destroyed;
}
[ScriptId("tests.dispose_from_destroy")]
public sealed class DisposeOnDestroyProbe:Behaviour
{
    public int Destroyed;
    public override void OnCreate()=>Subscribe<int>(_=>{});
    public override void OnDestroy(){++Destroyed;Scripts.Dispose();}
}

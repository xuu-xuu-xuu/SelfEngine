using SelfEngine;
using System.Numerics;
using System.Text.Json;

internal static class LifecycleContractTests
{
    private static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    public static void Run()
    {
        var records=new List<object>();
        var legacy=new ScriptDiagnostic("key",42,"type","detail");var (key,handle,type,detail)=legacy;
        Check(key=="key"&&handle==42&&type=="type"&&detail=="detail"&&legacy.Phase.Length==0,"four-field diagnostic construction and deconstruction remain compatible");
        using(var session=new EngineSession())
        using(var scripts=new ScriptSystem(session,new ScriptRegistry(),automaticFixedUpdates:false,simulatePhysics:false))
        {
            var owner=session.CreateEntity("lifecycle owner");var victim=session.CreateEntity("lifecycle victim");
            var probe=scripts.Attach<LifecycleSequenceProbe>(owner,value=>{value.Enabled=false;value.Victim=EntityReference.From(victim);});
            scripts.Start();Check(probe.Trace.SequenceEqual(["OnCreate"]),"initially disabled behaviour is created without enabling");
            probe.Enabled=false;probe.Enabled=true;scripts.FixedUpdate(1.0/60);session.Step(1.0/30);
            Check(!victim.IsAlive&&probe.Spawned is null&&owner.Transform.Position.X==1,"destroy is immediate, while callback spawn configuration waits for the next FrameBegin");
            probe.Enabled=false;probe.Enabled=false;scripts.FixedUpdate(1.0/60);session.Step(1.0/30);
            Check(probe.Spawned is {IsAlive:true}&&owner.Transform.Position.X==1,"next FrameBegin activates and configures the queued spawn without re-running its disabled owner");
            probe.Enabled=true;scripts.Stop();scripts.Stop();
            string[] expected=["OnCreate","OnEnable","OnFixedUpdate","OnUpdate","OnDisable","OnEnable","OnDisable","OnDestroy"];
            Check(probe.Trace.SequenceEqual(expected),"lifecycle order and repeated enable/disable/stop semantics");
            records.Add(new{kind="normal",entity=owner.Handle,stableId=owner.StableId,trace=probe.Trace.ToArray(),spawned=probe.Spawned!.StableId});
            var selfEntity=session.CreateEntity("self destroy");var self=scripts.Attach<LifecycleSequenceProbe>(selfEntity,value=>value.SelfDestruct=true);
            scripts.Start();session.Step(1.0/60);var replacement=session.CreateEntity("slot replacement");bool rejected=false;
            try{_ = selfEntity.Transform;}catch(EngineException exception){rejected=exception.Key=="Bridge.InvalidEntity";}
            Check(rejected&&!selfEntity.IsAlive&&replacement.Handle!=selfEntity.Handle&&self.Trace.Count(value=>value=="OnDestroy")==1,"old proxy cannot access replacement after callback self destruction");
            records.Add(new{kind="self_destroy",entity=selfEntity.Handle,replacement=replacement.Handle,trace=self.Trace.ToArray()});
        }
        foreach(var phase in new[]{"OnCreate","OnEnable","OnFixedUpdate","OnUpdate","OnDisable","OnDestroy","Event"})
        {
            using var session=new EngineSession();using var scripts=new ScriptSystem(session,new ScriptRegistry(),automaticFixedUpdates:false,simulatePhysics:false);
            var diagnostics=new List<ScriptDiagnostic>();scripts.Diagnostic+=diagnostics.Add;
            var healthy=scripts.Attach<MoveBehaviour>(session.CreateEntity("healthy"));
            var fault=scripts.Attach<LifecycleFaultProbe>(session.CreateEntity("fault"),value=>value.ThrowPhase=phase);
            scripts.Start();
            if(phase=="OnFixedUpdate")scripts.FixedUpdate(1.0/60);
            if(phase=="OnUpdate")session.Step(1.0/60);
            if(phase=="OnDisable")fault.Enabled=false;
            if(phase=="Event"){scripts.Events.Publish(7);session.Step(0);}
            if(phase!="OnDestroy")
            {
                fault.Enabled=true;
                Check(!fault.Enabled&&diagnostics.Any(value=>value.Key=="Script.FaultedCannotEnable"&&value.EntityHandle==fault.Entity.Handle&&value.Phase=="OnEnable"),"faulted instance stays disabled and explains how to restart");
            }
            session.Step(1.0/60);scripts.Stop();scripts.Stop();
            Check(diagnostics.Any(value=>value.Key=="Script.CallbackFailed"&&value.EntityHandle==fault.Entity.Handle&&value.ScriptType=="tests.lifecycle_fault"&&value.Phase==phase&&value.Detail.Contains("InvalidOperationException: "+phase,StringComparison.Ordinal)),"callback failure identifies entity, ScriptId, phase and exception");
            Check(healthy.Updates>0&&healthy.Entity.Transform.Position.X>0&&fault.Destroyed==1&&scripts.Count==0&&scripts.Events.Count==0&&session.LastCallbackException is null,"fault remains isolated, healthy native updates continue and cleanup occurs once");
            records.Add(new{kind="failure",phase,trace=fault.Trace.ToArray(),healthyUpdates=healthy.Updates,diagnostics});
        }
        var directory=Path.Combine(AppContext.BaseDirectory,"Validation","T46",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(directory);
        File.WriteAllText(Path.Combine(directory,"lifecycle-trace.json"),JsonSerializer.Serialize(new{schema=1,records},new JsonSerializerOptions{WriteIndented=true}),new System.Text.UTF8Encoding(false));
        Console.WriteLine("Lifecycle contract passed; evidence: "+directory);
    }
}

[ScriptId("tests.lifecycle_sequence")]
public sealed class LifecycleSequenceProbe:Behaviour
{
    public readonly List<string> Trace=[];
    public EntityReference Victim;
    public Entity? Spawned;
    public bool SelfDestruct;
    public override void OnCreate()=>Trace.Add("OnCreate");
    public override void OnEnable()=>Trace.Add("OnEnable");
    public override void OnFixedUpdate(double seconds)
    {Trace.Add("OnFixedUpdate");if(Victim.Resolve(Entity.Session) is {} victim)Scripts.Destroy(victim);}
    public override void OnUpdate(double seconds)
    {
        Trace.Add("OnUpdate");if(SelfDestruct){DestroySelf();return;}
        var transform=Entity.Transform;transform.Position.X+=1;Entity.Transform=transform;Scripts.Spawn("spawned from update",entity=>Spawned=entity);
    }
    public override void OnDisable()=>Trace.Add("OnDisable");
    public override void OnDestroy()=>Trace.Add("OnDestroy");
}

[ScriptId("tests.lifecycle_fault")]
public sealed class LifecycleFaultProbe:Behaviour
{
    public string ThrowPhase="";
    public readonly List<string> Trace=[];
    public int Destroyed;
    private void Visit(string phase){Trace.Add(phase);if(phase==ThrowPhase)throw new InvalidOperationException(phase);}
    public override void OnCreate(){if(ThrowPhase=="Event")Subscribe<int>(_=>Visit("Event"));Visit("OnCreate");}
    public override void OnEnable()=>Visit("OnEnable");
    public override void OnFixedUpdate(double seconds)=>Visit("OnFixedUpdate");
    public override void OnUpdate(double seconds)=>Visit("OnUpdate");
    public override void OnDisable()=>Visit("OnDisable");
    public override void OnDestroy(){++Destroyed;Visit("OnDestroy");}
}

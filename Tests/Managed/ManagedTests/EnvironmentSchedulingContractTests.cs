using SelfEngine;
using System.Text.Json;

internal static class EnvironmentSchedulingContractTests
{
    internal static void Check(bool condition,string message){if(!condition)throw new InvalidOperationException(message);}
    public static void Run()
    {
        var trace=new List<string>();
        using(var session=new EngineSession())
        using(var scripts=new ScriptSystem(session,new ScriptRegistry()))
        {
            var a=scripts.Attach<PhaseSequenceProbe>(session.CreateEntity("phase a"),value=>{value.Name="a";value.Trace=trace;value.Producer=true;});
            var b=scripts.Attach<PhaseSequenceProbe>(session.CreateEntity("phase b"),value=>{value.Name="b";value.Trace=trace;value.Other=EntityReference.From(a.Entity);});
            var old=scripts.Attach<OldDataProbe>(session.CreateEntity("old state reader"));scripts.Start();
            foreach(var phase in new[]{TickPhase.PreTick,TickPhase.FixedPreTick,TickPhase.Tick,TickPhase.PostTick})
                scripts.SetTickPrerequisites(b,phase,new TickDependency(a,phase,true));
            scripts.SetTickPrerequisites(old,TickPhase.PostTick,new TickDependency(a,TickPhase.FixedPreTick));
            bool cycle=false,future=false;
            try{scripts.SetTickPrerequisites(a,TickPhase.PreTick,new TickDependency(b,TickPhase.PreTick));}catch(EngineException exception){cycle=exception.Key=="Tick.DependencyCycle";}
            try{scripts.SetTickPrerequisites(a,TickPhase.PreTick,new TickDependency(b,TickPhase.Tick));}catch(EngineException exception){future=exception.Key=="Tick.FutureDependency";}
            Check(cycle&&future,"managed dependency graph rejects cycle and future stage");
            var diagnostics=new List<ScriptDiagnostic>();scripts.Diagnostic+=diagnostics.Add;
            session.Step(1.0/30);
            Check(trace.SequenceEqual(new[]{"a.pre","b.pre","a.fixed","b.fixed","a.fixed","b.fixed","a.tick","b.tick","a.post","b.post"}),"all component stages execute globally in declared order");
            Check(a.FixedCalls==2&&b.FixedCalls==2&&session.SimulationClock.Steps==2&&a.Entity.Transform.Position.X==115&&diagnostics.Count==0,"single physical clock and committed final transform");
            scripts.SetTickPrerequisites(b,TickPhase.PostTick,new TickDependency(a,TickPhase.PostTick,true),new TickDependency(a,TickPhase.FixedPreTick,true));
            session.Step(0);Check(a.FixedCalls==2&&b.FixedCalls==2&&b.PostCalls==1&&old.Reads==2,"zero fixed step keeps old state reader but skips new-data reader");
            a.Enabled=false;session.Step(1.0/60);Check(b.FixedCalls==2&&b.PostCalls==1&&old.Reads==3,"disabled producer cannot fabricate fresh data");
            var control=scripts.Attach<PausedControlProbe>(session.CreateEntity("frame control"));scripts.SimulationPaused=true;
            var before=session.SimulationClock.Steps;session.Step(1);Check(control.Updates==1&&session.SimulationClock.Steps==before,"paused control runs once before simulation");
            scripts.SimulationPaused=false;a.Enabled=true;session.Step(1.0/60);Check(session.SimulationClock.Steps==before+1,"resume has no paused debt");
            bool extra=false;try{session.Physics.Step(1.0/60);}catch(InvalidOperationException exception){extra=exception.Message=="Script.PhysicsClockOwned";}
            Check(extra,"managed public physics rejects second driver");
            scripts.Stop();Check(scripts.Count==0&&scripts.Events.Count==0&&session.NativeScriptComponentCount==0,"stage registrations and native associations stop together");
        }
        using(var session=new EngineSession())
        using(var scripts=new ScriptSystem(session,new ScriptRegistry()))
        {
            var probe=scripts.Attach<BudgetPhaseProbe>(session.CreateEntity("budget"));scripts.Start();session.Step(1);
            Check(probe.FixedCalls==8&&session.SimulationClock.Steps==8&&scripts.DroppedFixedSteps==52,"managed callbacks use the native eight-step budget");
            session.Step(0);Check(probe.FixedCalls==8,"zero delta never replays dropped steps");session.Step(1.0/60);Check(probe.FixedCalls==9,"budget recovery resumes one step");
        }
        using(var session=new EngineSession())
        using(var scripts=new ScriptSystem(session,new ScriptRegistry(),simulatePhysics:false))
        {
            var maker=scripts.Attach<BirthPhaseProbe>(session.CreateEntity("creator"));var victim=scripts.Attach<BudgetPhaseProbe>(session.CreateEntity("victim"));maker.Victim=EntityReference.From(victim.Entity);scripts.Start();
            session.Step(1.0/60);Check(!victim.Entity.IsAlive&&victim.FixedCalls==0&&maker.Born is null,"PreTick destruction skips later fixed work and birth waits");
            session.Step(0);Check(maker.Born is {Created:1,Updates:1}&&maker.Born.Entity.IsAlive,"next FrameBegin configures and initializes the new component before this frame Tick");
            scripts.Stop();Check(session.NativeScriptComponentCount==0,"pending births and tasks cleanly stop");
        }
        using(var session=new EngineSession())
        using(var scripts=new ScriptSystem(session,new ScriptRegistry(),simulatePhysics:false))
        {
            var bad=scripts.Attach<FreshFaultProbe>(session.CreateEntity("failed producer"));var dependent=scripts.Attach<BudgetPhaseProbe>(session.CreateEntity("fresh dependent"));var healthy=scripts.Attach<BudgetPhaseProbe>(session.CreateEntity("healthy"));
            scripts.Start();scripts.SetTickPrerequisites(dependent,TickPhase.Tick,new TickDependency(bad,TickPhase.Tick,true));session.Step(0);
            Check(!bad.Enabled&&dependent.Updates==0&&healthy.Updates==1,"caught managed callback failure never produces a fresh native stamp");
        }
        using(var session=new EngineSession())
        using(var scripts=new ScriptSystem(session,new ScriptRegistry(),simulatePhysics:false))
        {
            var maker=scripts.Attach<BirthPhaseProbe>(session.CreateEntity("cancel pending creator"));scripts.Start();session.Step(0);
            Check(maker.Born is null,"spawn is pending before Stop");scripts.Stop();scripts.Start();session.Step(0);
            Check(session.NativeScriptComponentCount==0&&scripts.Count==0&&session.ExportScene().Length>0,"Stop cancels pending activation and restart has no stale commands");
        }
        var root=Path.Combine(AppContext.BaseDirectory,"Validation","E03",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);
        File.WriteAllText(Path.Combine(root,"managed-scheduling.json"),JsonSerializer.Serialize(new{success=true,globalStages=true,cycleRejected=true,futureRejected=true,zeroFixed=true,oldDataAllowed=true,freshDataSkipped=true,physicsClockSingle=true,budgetSteps=8,droppedSteps=52,pauseNoDebt=true,controlOnce=true,deferredBirth=true,birthReadyInNextFrame=true,canceledBirthNoStaleCommand=true,destroyBeforeFixed=true,faultNoFreshStamp=true,stopZero=true,trace}));
        Console.WriteLine("EnvironmentSchedulingContractTests passed (native phase tasks, dependencies, single clock, deferred births and faults).");
    }
}
[ScriptId("test.e03.sequence")]
public sealed class PhaseSequenceProbe:Behaviour
{
    public string Name="";public bool Producer;public EntityReference Other;public List<string> Trace=[];public int FixedCalls,PostCalls;
    private void Add(float value){var transform=Entity.Transform;transform.Position.X+=value;Entity.Transform=transform;}
    public override void OnPreTick(double seconds)
    {Trace.Add(Name+".pre");if(Producer){var transform=Entity.Transform;transform.Position.X=1;Entity.Transform=transform;}else EnvironmentSchedulingContractTests.Check(Other.Resolve(Entity.Session)!.Transform.Position.X==1,"fresh pre state");}
    public override void OnFixedUpdate(double seconds){Trace.Add(Name+".fixed");++FixedCalls;if(Producer)Add(2);}
    public override void OnUpdate(double seconds){Trace.Add(Name+".tick");if(Producer)Add(10);}
    public override void OnPostTick(double seconds){Trace.Add(Name+".post");++PostCalls;if(Producer)Add(100);else EnvironmentSchedulingContractTests.Check(Other.Resolve(Entity.Session)!.Transform.Position.X>=111,"post reads final logic");}
}
[ScriptId("test.e03.old")]
public sealed class OldDataProbe:Behaviour{public int Reads;public override void OnPostTick(double seconds)=>++Reads;}
[ScriptId("test.e03.control")]
public sealed class PausedControlProbe:Behaviour{public int Updates;public override bool UpdateWhenPaused=>true;public override void OnUpdate(double seconds)=>++Updates;}
[ScriptId("test.e03.budget")]
public sealed class BudgetPhaseProbe:Behaviour
{public int FixedCalls,Updates,Created;public override void OnCreate()=>++Created;public override void OnFixedUpdate(double seconds)=>++FixedCalls;public override void OnUpdate(double seconds)=>++Updates;}
[ScriptId("test.e03.birth")]
public sealed class BirthPhaseProbe:Behaviour
{
    public EntityReference Victim;public BudgetPhaseProbe? Born;private bool requested;
    public override void OnPreTick(double seconds)
    {if(requested)return;requested=true;if(Victim.Resolve(Entity.Session) is {} victim)Scripts.Destroy(victim);Scripts.Spawn("next frame actor",entity=>Born=Scripts.Attach<BudgetPhaseProbe>(entity));}
}
[ScriptId("test.e03.fresh_fault")]
public sealed class FreshFaultProbe:Behaviour{public override void OnUpdate(double seconds)=>throw new InvalidOperationException("intentional producer fault");}

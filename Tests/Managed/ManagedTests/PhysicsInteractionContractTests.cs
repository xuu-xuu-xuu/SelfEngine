using System.Numerics;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using SelfEngine;

internal static class PhysicsInteractionContractTests
{
    private static string Scene=>File.ReadAllText(Path.Combine(AppContext.BaseDirectory,"PhysicsSample","PhysicsPlayground.scene.json"));
    private static string Bindings=>File.ReadAllText(Path.Combine(AppContext.BaseDirectory,"PhysicsSample","PhysicsPlayground.bindings.json"));
    private static string AssemblyPath=>Path.Combine(AppContext.BaseDirectory,"ProjectScripts","SelfEngine.ProjectScripts.dll");
    private static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    private static EngineSession Session(){var session=new EngineSession();session.LoadLocale("en-US");session.ImportScene(Scene);return session;}
    private static ScriptProject Start(EngineSession session,List<ScriptDiagnostic> diagnostics,bool destroy=false)
    {
        var document=JsonNode.Parse(Bindings)!;
        document["Scripts"]![0]!["Fields"]!["DestroyCrateOnContact"]=new JsonObject{["Kind"]="bool",["Value"]=destroy};
        var project=new ScriptProject(session);project.Diagnostic+=diagnostics.Add;project.Start(AssemblyPath,["sample.physics_playground"],document.ToJsonString());return project;
    }
    private static RayHit Look(EngineSession session)
    {
        var player=session.FindEntity(1)!;var eye=session.FindEntity(2)!;
        var hit=session.Physics.Raycast(player.Transform.Position+eye.Transform.Position,Vector3.Transform(-Vector3.UnitZ,eye.Transform.Rotation),4);
        Check(hit is { } value&&value.Entity.StableId==4&&value.Body.State.Mass==1,"actual project ray selects dynamic crate");return hit!.Value;
    }
    private static bool Missing(RigidBody body){try{_=body.State;return false;}catch(EngineException error){return error.Key=="Bridge.InvalidBody";}}
    private static void Collect(WeakReference weak)
    {for(int attempt=0;weak.IsAlive&&attempt<12;++attempt){GC.Collect();GC.WaitForPendingFinalizers();GC.Collect();}Check(!weak.IsAlive,"physical project context unloads after teardown");}
    private sealed record Episode(Vector3 Position,Vector3 Velocity,WeakReference Context);
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static Episode Rate(int fps,List<object> records)
    {
        using var session=Session();List<ScriptDiagnostic> diagnostics=[];using var project=Start(session,diagnostics);var scripts=project.Scripts;
        var hit=Look(session);var observer=scripts.Attach<PhysicsContactObserver>(session.CreateEntity("observer"),value=>{value.Victim=new(4);value.Body=hit.Body;});
        for(int frame=0;frame<fps;++frame){scripts.SetInput(new ScriptInput([],frame==0?['E']:[],[]));session.Step(1.0/fps);}
        var state=hit.Body.State;Console.WriteLine($"T55 rate={fps} position={state.Transform.Position} velocity={state.LinearVelocity}");Check(Vector3.Distance(state.Transform.Position,new(0,.5f,0))>.01f,"one interaction edge changes real native body pose above numeric tolerance");
        Check(observer.Begins>0&&observer.AliveObservations>0&&observer.Synchronized&&observer.ClockRejected&&observer.DrainWasEmpty,"contacts dispatch after native pose synchronization and prohibit a second physics clock");
        project.Stop();Check(diagnostics.Count==0&&scripts.Events.Count==0&&Missing(hit.Body),"sample stop destroys bodies and subscriptions without diagnostics");
        Check(session.Physics.SetDebugDrawing(false),"sample stop restores previous debug state");session.Physics.SetDebugDrawing(true);
        records.Add(new{kind="rate",fps,position=state.Transform.Position,velocity=state.LinearVelocity,observer.Begins,observer.AliveObservations,observer.ClockRejected});
        return new(state.Transform.Position,state.LinearVelocity,project.LastUnloadedContext!);
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static Episode Edge(char key,bool manual,List<object> records)
    {
        using var session=Session();List<ScriptDiagnostic> diagnostics=[];using var project=Start(session,diagnostics);var hit=Look(session);
        if(manual)
        {
            var direction=Vector3.Transform(-Vector3.UnitZ,session.FindEntity(2)!.Transform.Rotation);
            if(key=='E')hit.Body.ApplyImpulse(direction,hit.Point);else hit.Body.ApplyForce(direction*12,hit.Point);
        }
        project.Scripts.SetInput(new ScriptInput([],manual?[]:[key],[]));session.Step(1.0/30);var state=hit.Body.State;
        Check(diagnostics.Count==0,"edge invocation healthy");project.Stop();records.Add(new{kind="edge",key=key.ToString(),manual,position=state.Transform.Position,velocity=state.LinearVelocity});
        return new(state.Transform.Position,state.LinearVelocity,project.LastUnloadedContext!);
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static WeakReference Push(List<object> records)
    {
        using var session=Session();List<ScriptDiagnostic> diagnostics=[];using var project=Start(session,diagnostics);var body=Look(session).Body;float maximum=0;
        for(int step=0;step<180;++step){project.Scripts.SetInput(new ScriptInput(['W'],[],[]));session.Step(1.0/60);maximum=Math.Max(maximum,body.State.LinearVelocity.Length());}
        var moved=body.State.Transform.Position;Check(moved.Z< -.5f&&maximum<8,"sustained capsule push moves crate with bounded target speed");
        project.Scripts.SetInput(new ScriptInput([],['R'],[]));session.Step(1.0/60);var reset=body.State;
        Check(Vector3.Distance(reset.Transform.Position,new(0,.5f,0))<.005f&&reset.LinearVelocity.Length()<.01f&&Vector3.Distance(session.FindEntity(1)!.Transform.Position,new(0,.9f,3))<.003f,"explicit R teleport restores body and character with cleared motion");
        Check(diagnostics.Count==0,"push/reset has no script failures");project.Stop();records.Add(new{kind="push",moved,maximum_speed=maximum,reset=reset.Transform.Position});return project.LastUnloadedContext!;
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static WeakReference Callback(int cycle,List<object> records)
    {
        using var session=Session();List<ScriptDiagnostic> diagnostics=[];using var project=Start(session,diagnostics,true);var body=Look(session).Body;var scripts=project.Scripts;
        var observer=scripts.Attach<PhysicsContactObserver>(session.CreateEntity("healthy observer"),value=>{value.Victim=new(4);value.Body=body;});
        session.Step(1.0/60);session.Step(1.0/60);
        Check(session.FindEntity(4) is null&&Missing(body)&&observer.Begins>0&&observer.ClockRejected&&observer.Synchronized&&observer.DrainWasEmpty,"contact callback safely destroys its body/entity while a healthy observer continues");
        project.Stop();Check(diagnostics.Count==0&&scripts.Events.Count==0,"callback cleanup removes all owned subscriptions");
        records.Add(new{kind="callback",cycle,observer.Begins,observer.AliveObservations,observer.MissingObservations,observer.ClockRejected});return project.LastUnloadedContext!;
    }
    private static void ProxyLifetime(List<object> records)
    {
        using var session=new EngineSession();var entity=session.CreateEntity("controller lifetime");var first=new CharacterController(entity,CharacterDescription.Default);first.Destroy();
        var second=new CharacterController(entity,CharacterDescription.Default);bool rejected=false;try{first.Move(Vector3.UnitX,false,1.0/60);}catch(EngineException error){rejected=error.Key=="Bridge.InvalidCharacter"&&error.Result==2;}
        first.Destroy();second.Move(Vector3.Zero,false,1.0/60);Check(rejected,"destroyed proxy cannot move or destroy a replacement controller on the same entity");second.Destroy();records.Add(new{kind="proxy_lifetime",rejected});
    }
    public static void Run(bool window)
    {
        List<object> records=[];Episode? baseline=null;
        foreach(int fps in new[]{30,60,120,144}){var result=Rate(fps,records);if(baseline is not null)Check(Vector3.Distance(result.Position,baseline.Position)<.0001f&&Vector3.Distance(result.Velocity,baseline.Velocity)<.0001f,"display rates produce same fixed-step interaction state");baseline=result;Collect(result.Context);}
        foreach(char key in new[]{'E','F'}){var automatic=Edge(key,false,records);var manual=Edge(key,true,records);Check(Vector3.Distance(automatic.Position,manual.Position)<.0001f&&Vector3.Distance(automatic.Velocity,manual.Velocity)<.0001f,"one edge in a two-step frame equals one manual impulse/force");Collect(automatic.Context);Collect(manual.Context);}
        Collect(Push(records));for(int cycle=0;cycle<16;++cycle)Collect(Callback(cycle,records));ProxyLifetime(records);
        if(window)
        {using var session=Session();List<ScriptDiagnostic> diagnostics=[];using var project=Start(session,diagnostics);session.RunWindow("en-US",false,6);project.Stop();Check(diagnostics.Count==0,"actual managed first person window renders and shuts down cleanly");records.Add(new{kind="window",frames=6});}
        var output=Path.Combine(AppContext.BaseDirectory,"Validation","T55",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(output);
        File.WriteAllText(Path.Combine(output,"physics-interaction.json"),JsonSerializer.Serialize(new{schema=1,success=true,callback_cycles=16,records},new JsonSerializerOptions{IncludeFields=true,WriteIndented=true}));
        Console.WriteLine("T55 physics interaction passed: "+output);
    }
}

[ScriptId("tests.physics_contact_observer")]
public sealed class PhysicsContactObserver:Behaviour
{
    public EntityReference Victim;public RigidBody? Body;
    public int Begins,AliveObservations,MissingObservations;public bool Synchronized=true,ClockRejected=true,DrainWasEmpty=true;
    public override void OnCreate()=>Subscribe<PhysicsContact>(contact=>{
        if(!contact.Began||(contact.EntityA!=Victim&&contact.EntityB!=Victim))return;++Begins;
        if(Victim.Resolve(Entity.Session) is { } entity){++AliveObservations;Synchronized&=entity.Transform.Position==Body!.State.Transform.Position;}else ++MissingObservations;
        DrainWasEmpty&=Entity.Session.Physics.DrainContacts().Count==0;
        bool rejected=false;try{Entity.Session.Physics.Step(1.0/60);}catch(InvalidOperationException error){rejected=error.Message=="Script.PhysicsClockOwned";}ClockRejected&=rejected;
    });
}

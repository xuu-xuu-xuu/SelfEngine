using System.Numerics;
using System.Text.Json;
using System.Text.Json.Nodes;
using ProjectScripts;
using SelfEngine;

internal static class EnvironmentIntegrationTests
{
    private const string Profile="c7e70000000040008000000000000001";
    internal static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    internal static string Document(ulong id,bool preserve=false)=>JsonSerializer.Serialize(new
    {
        format="SelfEngine.Scene",version=2,environment=new{todProfile=Profile,preserveTime=preserve},
        entities=new[]{new{id=id.ToString(),name="switched",enabled=true,parent=(string?)null,
            components=new[]{new{type="1",version=1,enabled=true,properties=new{position=new[]{0,0,0},rotation=new[]{0,0,0,1},scale=new[]{1,1,1}}}}}}
    });
    public static void Run()
    {
        var root=Path.Combine(AppContext.BaseDirectory,"Validation","E08",Guid.NewGuid().ToString("N"));var assets=Path.Combine(root,"Assets");Directory.CreateDirectory(assets);
        var profile=JsonNode.Parse(File.ReadAllText("Resources/Samples/TOD/DayNight.tod"))!;profile["dayLengthSeconds"]=1;profile["initialTime"]=.24;
        File.WriteAllText(Path.Combine(assets,"day.tod"),profile.ToJsonString());File.Copy("Resources/Samples/TOD/DayNight.tod.asset.json",Path.Combine(assets,"day.tod.asset.json"));
        using var session=new EngineSession();session.SetAssetRoot(assets,Path.Combine(root,"Cache"));session.ImportScene(Document(99001));session.Step(0);
        using var scripts=new ScriptSystem(session,new ScriptRegistry());var diagnostics=new List<ScriptDiagnostic>();scripts.Diagnostic+=diagnostics.Add;
        Entity Make(string name,Vector3 position){var entity=session.CreateEntity(name);var t=Transform.Identity;t.Position=position;entity.Transform=t;return entity;}
        var floor=Make("floor",new(0,-.5f,0));var box=Make("box",new(0,.5f,0));var actor=Make("character",new(0,.9001f,2));var view=Make("view",new(0,2,2));
        var probe=scripts.Attach<IntegratedEnvironmentProbe>(actor,p=>{p.Floor=floor;p.Box=box;});
        var menu=scripts.Attach<MenuFlow>(Make("menu",Vector3.Zero),p=>p.ViewEntity=EntityReference.From(view));scripts.Start();
        var first=session.TimeOfDay.Snapshot;Check(first.Active&&first.Time==.24&&first.SimulationSeconds==0,"scene chooses TOD before scripts start");
        for(int i=0;i<8;++i)session.Step(1.0/60);Check(session.TimeOfDay.Snapshot.Time==first.Time&&probe.FixedSteps==0,"real main menu freezes character, physics and TOD together");
        void Click(string name){session.Ui.Layout(800,600);var v=session.Ui.Visuals().Single(v=>v.Id==menu.Controls[name]);var p=new Vector2(v.X+v.Width/2,v.Y+v.Height/2);Check(session.Ui.Pointer(UiPointerOperation.Down,p)&&session.Ui.Pointer(UiPointerOperation.Up,p),"menu input "+name);session.Step(1.0/60);}
        Click("start");Check(!scripts.SimulationPaused,"menu starts unique simulation clock");
        for(int i=0;i<120;++i){session.Step(1.0/60);Check(probe.LastPost==session.TimeOfDay.Snapshot,"PostTick snapshot remains identical at frame return");}
        Check(probe.Contacts>0&&probe.FixedSteps==121&&probe.Sunrises>=2&&probe.Sunsets>=2&&box.Transform.Position.Z<-.1f,"one integrated path advances contacts, character push and TOD boundaries");
        Check(probe.Spawned is {IsAlive:false},"deferred creation and callback destruction retire one object");
        var held=session.TimeOfDay.Snapshot;Check(menu.Pause(),"pause request");session.Step(.5);Check(session.TimeOfDay.Snapshot.Time==held.Time&&session.TimeOfDay.Snapshot.SimulationSeconds==held.SimulationSeconds,"pause has no time debt");
        int sunrise=probe.Sunrises,sunset=probe.Sunsets,changed=probe.TimeChanges;
        session.TimeOfDay.SetTime(-1,.1);session.Step(0);Check(probe.TimeChanges==changed+1&&probe.Sunrises==sunrise&&probe.Sunsets==sunset,"ordinary backward jump only reports TimeChanged");
        session.TimeOfDay.SetTime(0,.9,true);session.Step(0);Check(probe.Sunrises==sunrise+2&&probe.Sunsets==sunset+2,"explicit forward replay reports bounded boundaries through managed codecs");
        session.TimeOfDay.SetRate(2);bool rejected=false;try{session.TimeOfDay.SetRate(double.PositiveInfinity);}catch(EngineException){rejected=true;}Check(rejected&&session.TimeOfDay.Snapshot.Rate==2,"bad rate preserves old value");
        Check(held.Time!=session.TimeOfDay.Snapshot.Time&&held.Rate==1,"held snapshot is owned immutable data");
        Click("resume");Check(!scripts.SimulationPaused,"menu resumes");Check(Math.Abs(session.TimeOfDay.Snapshot.SimulationSeconds-held.SimulationSeconds-1.0/60)<1e-8,"resume executes one fixed step");
        Check(menu.Pause(),"pause before scene transaction");var priorScene=session.Scenes.Active!;var replacement=session.Scenes.Create("replacement");
        replacement.RequestJson(Document(99002,true),SceneLoadMode.Replace,priorScene.Handle);var before=session.TimeOfDay.Snapshot;
        session.Step(0);Check(!actor.IsAlive&&probe.Destroyed==1&&probe.BodyRetired&&replacement.Snapshot.Active&&session.FindEntity(99002) is not null,"paused scene replacement destroys scripts and proxies exactly once");
        Check(session.TimeOfDay.Snapshot.Time==before.Time&&scripts.Count==0&&session.Ui.Visuals().Count==0,"scene retirement preserves selected time and removes menu/behaviors");
        scripts.Stop();Check(session.NativeScriptComponentCount==0&&scripts.Events.Count==0&&diagnostics.Count==0,"CPU integration stops without callbacks or scripts left behind");
        Check(probe.Late!.Publish(1)==EventPostResult.Closed,"stopped publisher cannot call old receiver");
        File.WriteAllText(Path.Combine(root,"verification.json"),JsonSerializer.Serialize(new{success=true,probe.FixedSteps,probe.Contacts,probe.Sunrises,probe.Sunsets,probe.TimeChanges,probe.Destroyed,postTickSnapshot=true,menuPause=true,characterPush=true,sceneSwitch=true,trace=probe.Trace}));
        Console.WriteLine("EnvironmentIntegrationTests passed (menu, character/box, spawn/destroy, scene switch, TOD and PostTick snapshot): "+root);
    }
}
[ScriptId("test.e08.integrated")]
public sealed class IntegratedEnvironmentProbe:Behaviour
{
    public override bool UpdateWhenPaused=>true;
    public Entity Floor=null!,Box=null!;private CharacterController? character;private RigidBody? floorBody,boxBody;
    public int Contacts,FixedSteps,Sunrises,Sunsets,TimeChanges,Destroyed;public bool BodyRetired;public Entity? Spawned;
    public EnvironmentSnapshot? LastPost;public List<object> Trace=[];public ScriptEventPublisher<int>? Late;
    public override void OnCreate()
    {
        var ground=BoxBodyDescription.Static;ground.HalfExtents=new(100,.5f,100);floorBody=Entity.Session.Physics.CreateBox(Floor,ground);
        boxBody=Entity.Session.Physics.CreateBox(Box,BoxBodyDescription.Dynamic);character=new(Entity,CharacterDescription.Default);
        Scripts.Events.Subscribe<PhysicsContact>(this,v=>{if(v.Began)++Contacts;});
        Scripts.Events.Subscribe<Sunrise>(this,v=>{++Sunrises;Trace.Add(new{phase="sunrise",v.Day,v.Time,v.SimulationSeconds});});
        Scripts.Events.Subscribe<Sunset>(this,v=>++Sunsets);Scripts.Events.Subscribe<TimeChanged>(this,v=>++TimeChanges);
        Scripts.Events.Subscribe<int>(this,_=>{if(Spawned is not null)Scripts.Destroy(Spawned);});Late=Scripts.Events.CreatePublisher<int>();
    }
    public override void OnFixedUpdate(double seconds){++FixedSteps;character!.Move(-Vector3.UnitZ,false,seconds);}
    public override void OnUpdate(double seconds)
    {
        if(FixedSteps==1)Scripts.Spawn("ephemeral",e=>Spawned=e);
        if(FixedSteps==3)Scripts.Events.Publish(1);
    }
    public override void OnPostTick(double seconds)
    {
        LastPost=Entity.Session.TimeOfDay.Snapshot;Trace.Add(new{phase="postTick",LastPost.FrameIndex,fixedStep=FixedSteps,LastPost.Time,LastPost.SimulationSeconds});
    }
    public override void OnDestroy()
    {
        ++Destroyed;character?.Destroy();boxBody?.Destroy();floorBody?.Destroy();
        try{_=boxBody!.State;}catch(EngineException){BodyRetired=true;}
    }
}

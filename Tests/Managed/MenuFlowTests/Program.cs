using SelfEngine;
using ProjectScripts;
using System.Numerics;

static class Test
{
    static void Check(bool condition,string text){if(!condition)throw new InvalidOperationException(text);}
    static int Main(string[] args)
    {
        try
        {
            EnvironmentIntegrationTests.Run();
            using var session=new EngineSession();session.LoadLocale("zh-CN");
            var registry=new ScriptRegistry();using var scripts=new ScriptSystem(session,registry);
            var diagnostics=new List<ScriptDiagnostic>();scripts.Diagnostic+=value=>{diagnostics.Add(value);Console.Error.WriteLine(value.Key+": "+value.Detail);};
            var menuEntity=session.CreateEntity("menu");var view=session.CreateEntity("view");var transform=Transform.Identity;
            transform.Position=new(0,2,0);view.Transform=transform;
            var target=session.CreateEntity("target");transform.Position=new(0,2,-3);target.Transform=transform;
            session.Physics.CreateBox(target,BoxBodyDescription.Static);
            var falling=session.CreateEntity("falling");transform.Position=new(5,10,0);falling.Transform=transform;
            session.Physics.CreateBox(falling,BoxBodyDescription.Dynamic);
            var actor=session.CreateEntity("actor");var game=scripts.Attach<ProbeGame>(actor,value=>value.ProvideInteraction=true);
            var menu=scripts.Attach<MenuFlow>(menuEntity,value=>value.ViewEntity=EntityReference.From(view));
            scripts.Start();session.Ui.Layout(800,600);
            Check(menu.State==MenuFlowState.MainMenu&&scripts.SimulationPaused,"main menu pauses simulation");
            var initialY=falling.Transform.Position.Y;session.Step(1.0/60);
            Check(game.Updates==0&&falling.Transform.Position.Y==initialY,"game scripts and physics initially paused");
            void Click(string id)
            {
                session.Ui.Layout(800,600);var visual=session.Ui.Visuals().Single(value=>value.Id==menu.Controls[id]);
                var point=new Vector2(visual.X+visual.Width*0.5f,visual.Y+visual.Height*0.5f);
                Check(session.Ui.Pointer(UiPointerOperation.Down,point)&&session.Ui.Pointer(UiPointerOperation.Up,point),"UI click "+id);
                session.Step(1.0/60);
            }
            Click("main-settings");Check(menu.State==MenuFlowState.Settings&&scripts.SimulationPaused,"settings from main menu");
            Click("back");Check(menu.State==MenuFlowState.MainMenu,"settings return to origin");
            Click("start");Check(menu.State==MenuFlowState.Playing&&!scripts.SimulationPaused&&game.Starts==1,"start via real UI event");
            Check(game.Updates>0&&falling.Transform.Position.Y<initialY,"start advances actual game and physics");
            Check(menu.Target.StableId==target.StableId,"crosshair ray targets nearest physics object");
            scripts.SetInput(new ScriptInput([69],[69],[]));session.Step(1.0/60);
            Check(game.Interactions==1&&game.Target==target.StableId,"interaction publishes stable target");
            scripts.SetInput(new ScriptInput([27],[27],[]));session.Step(1.0/60);
            Check(menu.State==MenuFlowState.Paused&&scripts.SimulationPaused,"escape pauses");
            var pausedY=falling.Transform.Position.Y;var pausedUpdates=game.Updates;
            for(int i=0;i<20;++i)session.Step(1.0/60);
            Check(falling.Transform.Position.Y==pausedY&&game.Updates==pausedUpdates,"pause freezes simulation without frame debt");
            Click("pause-settings");Check(menu.State==MenuFlowState.Settings,"paused UI continues receiving events");
            session.Ui.Find(menu.Controls["speed"])!.SetValue(7);session.Step(1.0/60);
            Check(menu.MoveSpeed==7,"slider edits settings");
            Check(!menu.SetSettings(float.NaN,0.003f)&&menu.MoveSpeed==7,"bad settings rejected without partial mutation");
            Click("back");Check(menu.State==MenuFlowState.Paused,"settings returns to pause");
            Click("resume");Check(menu.State==MenuFlowState.Playing&&game.Speed==7,"resume republishes settings to gameplay owner");
            Check(pausedY-falling.Transform.Position.Y<0.02f,"resume does not catch up all paused frames");
            Check(game.Interactions==1,"resume does not leak prior interaction edge");
            menu.Pause();Click("main-menu");Check(menu.State==MenuFlowState.MainMenu&&scripts.SimulationPaused,"return to main menu");
            scripts.Remove(menu);Check(!scripts.SimulationPaused&&session.Ui.Visuals().Count==0,"menu teardown removes UI and restores prior pause policy");
            session.Step(1.0/60);Check(scripts.Events.Count==3,"menu subscriptions released; only gameplay subscriptions remain");
            Check(diagnostics.Count==0,"no swallowed script errors");
            CheckPlayerSettings();
            if(args.Contains("--window-smoke"))MenuFlowWindowTests.Run();
            Console.WriteLine("MenuFlowTests passed (UI states, pause, settings, ray interaction, teardown)");return 0;
        }
        catch(Exception error){Console.Error.WriteLine("MenuFlowTests failed: "+error);return 1;}
    }
    static void CheckPlayerSettings()
    {
        using var session=new EngineSession();session.LoadLocale("zh-CN");
        using var scripts=new ScriptSystem(session,new ScriptRegistry());var diagnostics=new List<ScriptDiagnostic>();scripts.Diagnostic+=diagnostics.Add;
        var floor=session.CreateEntity("floor");var transform=Transform.Identity;transform.Position=new(0,-0.5f,0);floor.Transform=transform;
        var ground=BoxBodyDescription.Static;ground.HalfExtents=new(100,0.5f,100);session.Physics.CreateBox(floor,ground);
        var body=session.CreateEntity("player");transform.Position=new(0,0.9001f,0);body.Transform=transform;
        var camera=session.CreateEntity("view");transform.Position=new(0,1.5001f,0);camera.Transform=transform;
        var player=scripts.Attach<PlayerCharacter>(body,value=>value.ViewEntity=EntityReference.From(camera));
        var menu=scripts.Attach<MenuFlow>(session.CreateEntity("menu"),value=>value.ViewEntity=EntityReference.From(camera));
        scripts.Start();menu.StartGame();
        void Tick(int[] down,int[] pressed,Vector2 mouse)
        {scripts.SetInput(new ScriptInput(down,pressed,[],pointer:new(mouse,0,0,0,0,false)));session.Step(1.0/60);}
        var pointer=new Vector2(100,100);for(int i=0;i<5;++i)Tick([],[],pointer);
        Check(player.ViewAvailable&&player.CharacterState.Grounded,"configured first-person view follows grounded character");
        menu.SetSettings(1,0.003f);session.Step(0);var start=body.Transform.Position;
        for(int i=0;i<30;++i)Tick([87],[],pointer);
        var slow=Vector3.Distance(start,body.Transform.Position);
        var beforeChange=player.CharacterState;menu.SetSettings(8,0.003f);
        Check(player.CharacterState==beforeChange,"changing speed preserves existing controller state");
        session.Step(0);
        start=body.Transform.Position;for(int i=0;i<30;++i)Tick([87],[],pointer);
        var fast=Vector3.Distance(start,body.Transform.Position);
        Check(Math.Abs(slow-0.5f)<0.01f&&Math.Abs(fast-4)<0.02f,"menu speed changes actual capsule movement");
        menu.SetSettings(8,0.002f);session.Step(0);Tick([],[],pointer);var yaw=player.Yaw;pointer.X+=10;Tick([],[],pointer);
        var low=Math.Abs(player.Yaw-yaw);
        menu.SetSettings(8,0.01f);session.Step(0);Tick([],[],pointer);yaw=player.Yaw;pointer.X+=10;Tick([],[],pointer);
        var high=Math.Abs(player.Yaw-yaw);
        Check(Math.Abs(low-0.02f)<0.0001f&&Math.Abs(high-0.1f)<0.0001f,"mouse sensitivity changes actual yaw increment");
        var forward=Vector3.Transform(-Vector3.UnitZ,camera.Transform.Rotation);
        start=body.Transform.Position;Tick([87],[],pointer);var moved=body.Transform.Position-start;moved.Y=0;
        Check(Vector3.Dot(Vector3.Normalize(moved),Vector3.Normalize(new Vector3(forward.X,0,forward.Z)))>0.999f,"walk direction follows yaw");
        pointer.Y+=10;Tick([],[],pointer);
        Check(player.Pitch<0&&Vector3.Transform(-Vector3.UnitZ,camera.Transform.Rotation).Y<0,"pitch updates camera transform");
        Tick([32],[32],pointer);var airborne=player.CharacterState;Check(!airborne.Grounded&&airborne.VerticalVelocity>0,"jump setup");
        menu.SetSettings(3,0.005f);Check(player.CharacterState==airborne,"airborne setting change does not reset vertical velocity");
        camera.Destroy();Tick([],[],pointer);Check(!player.ViewAvailable,"missing optional camera leaves movement available without pretending view output");
        Check(diagnostics.Count==0,"first-person settings have no script errors");
        Console.WriteLine($"Player settings: slow={slow:F3}m fast={fast:F3}m yawLow={low:F3}rad yawHigh={high:F3}rad");
    }
}

[ScriptId("test.menu_game")]
public sealed class ProbeGame:Behaviour
{
    public int Updates,Interactions,Starts;
    public bool ProvideInteraction;
    public ulong Target;
    public float Speed=4;
    public override void OnCreate()
    {
        Subscribe<MenuSettingsChanged>(value=>Speed=value.MoveSpeed);
        if(ProvideInteraction)Scripts.Commands.Register<MenuInteractionCommand>(this,"menu.interact",value=>{Scripts.Events.Publish(new MenuInteractionCompleted(value.Target));return true;});
        Subscribe<MenuInteractionCompleted>(value=>{++Interactions;Target=value.Target.StableId;});
        Subscribe<MenuGameStarted>(_=>++Starts);
    }
    public override void OnUpdate(double seconds)
    {
        ++Updates;var transform=Entity.Transform;transform.Position.X+=Speed*(float)seconds;Entity.Transform=transform;
    }
}

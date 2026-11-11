using System.Numerics;
using System.Text.Json;
using System.Text.Json.Nodes;
using SelfEngine;
using ProjectScripts;

internal static class MenuFlowWindowTests
{
    private sealed record Image(int Width,int Height,byte[] Pixels);
    private static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    private static Image Ppm(string path)
    {
        var bytes=File.ReadAllBytes(path);int offset=0;
        string Token(){while(offset<bytes.Length&&char.IsWhiteSpace((char)bytes[offset]))++offset;int first=offset;while(offset<bytes.Length&&!char.IsWhiteSpace((char)bytes[offset]))++offset;return System.Text.Encoding.ASCII.GetString(bytes,first,offset-first);}
        Check(Token()=="P6","menu PPM");int width=int.Parse(Token()),height=int.Parse(Token());Check(Token()=="255","menu PPM range");++offset;var pixels=bytes[offset..];Check(pixels.Length==width*height*3,"complete menu PPM");return new(width,height,pixels);
    }
    private static int Difference(Image first,Image second)
    {Check(first.Width==second.Width&&first.Height==second.Height,"equal menu image dimensions");int count=0;for(int i=0;i<first.Pixels.Length;i+=3)if(first.Pixels[i]!=second.Pixels[i]||first.Pixels[i+1]!=second.Pixels[i+1]||first.Pixels[i+2]!=second.Pixels[i+2])++count;return count;}
    public static void Run()
    {
        var root=Path.Combine(AppContext.BaseDirectory,"Validation","T63",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);var scene=File.ReadAllText("Resources/Samples/Scenes/MenuFlow.scene.json");using var session=new EngineSession();session.LoadLocale("en-US");session.ImportScene(scene);using var scripts=new ScriptSystem(session,new ScriptRegistry());var diagnostics=new List<ScriptDiagnostic>();scripts.Diagnostic+=diagnostics.Add;
        var actor=session.FindEntity(1)!;var view=session.FindEntity(2)!;var stage=scripts.Attach<MenuWorld>(actor,value=>{value.Floor=new(3);value.Target=new(4);});var character=scripts.Attach<PlayerCharacter>(actor,value=>value.ViewEntity=EntityReference.From(view));var menu=scripts.Attach<MenuFlow>(session.FindEntity(5)!,value=>value.ViewEntity=EntityReference.From(view));var game=scripts.Attach<ProbeGame>(session.CreateEntity("menu window counter"));scripts.Start();
        void Advance(int count){for(int i=0;i<count;++i)session.Step(1.0/60);}
        void Click(string id)
        {session.Ui.Layout(1264,681);var visual=session.Ui.Visuals().Single(v=>v.Id==menu.Controls[id]);var point=new Vector2(visual.X+visual.Width/2,visual.Y+visual.Height/2);Check(session.Ui.Pointer(UiPointerOperation.Down,point)&&session.Ui.Pointer(UiPointerOperation.Up,point),"window menu target "+id);session.Step(1.0/60);}
        void Escape(){scripts.SetInput(new ScriptInput([27],[27],[]));session.Step(1.0/60);scripts.SetInput(ScriptInput.Empty);}
        Image Capture(string name)
        {var path=Path.Combine(root,name+".ppm");session.SetWindowCapture(path,3);session.RunWindow("en-US",false,3);return Ppm(path);}
        bool busyCapture=false;void BusyProbe(double _){if(busyCapture)return;try{session.SetWindowCapture(null);}catch(EngineException e){busyCapture=e.Key=="Bridge.SessionBusy";}}
        session.Update+=BusyProbe;Advance(24);Check(scripts.SimulationPaused,"window starts paused in main menu");var initialTarget=session.FindEntity(4)!.Transform.Position;var main=Capture("main");Check(busyCapture,"window capture cannot be changed in native callback");session.Update-=BusyProbe;
        Click("main-settings");Advance(20);var mainSettings=Capture("settings-main");Check(menu.State==MenuFlowState.Settings&&scripts.SimulationPaused&&session.FindEntity(4)!.Transform.Position==initialTarget,"main settings keep actual physics paused");Click("back");Advance(20);Click("start");Advance(24);var playing=Capture("playing");Check(menu.State==MenuFlowState.Playing&&menu.Target.StableId==4&&game.Starts==1&&character.ViewAvailable,"real world, crosshair ray and one start action");
        var uiSnapshot=session.Ui.Save();Image empty;
        using(var baseline=new EngineSession()){baseline.Ui.Load(uiSnapshot);baseline.SetWindowCapture(Path.Combine(root,"ui-only.ppm"),3);baseline.RunWindow("en-US",false,3);empty=Ppm(Path.Combine(root,"ui-only.ppm"));}
        int worldPixels=Difference(playing,empty);Check(worldPixels>10000,"native C# window actually draws World meshes behind UI");
        var withoutCamera=JsonNode.Parse(scene)!;foreach(var entity in withoutCamera["entities"]!.AsArray())entity!["components"]!.AsObject().Remove("camera");
        using(var missing=new EngineSession()){missing.ImportScene(withoutCamera.ToJsonString());missing.Ui.Load(uiSnapshot);missing.SetWindowCapture(Path.Combine(root,"no-camera.ppm"),3);missing.RunWindow("en-US",false,3);Check(Difference(empty,Ppm(Path.Combine(root,"no-camera.ppm")))==0,"mesh world without a camera safely shows only UI");}
        int center=(playing.Height/2*playing.Width+playing.Width/2)*3;Check(playing.Pixels[center]>240&&playing.Pixels[center+1]>240&&playing.Pixels[center+2]>240,"crosshair rendered at actual canvas center");
        scripts.SetInput(new ScriptInput([69],[69],[]));session.Step(1.0/60);scripts.SetInput(ScriptInput.Empty);Check(stage.Interactions==1&&game.Interactions==1,"E action reaches actual target once");Escape();Advance(24);var pausedPosition=session.FindEntity(4)!.Transform.Position;Advance(30);var paused=Capture("paused");Check(menu.State==MenuFlowState.Paused&&scripts.SimulationPaused&&session.FindEntity(4)!.Transform.Position==pausedPosition,"pause freezes real body while UI continues");
        Click("pause-settings");session.Ui.Find(menu.Controls["speed"])!.SetValue(7);session.Ui.Find(menu.Controls["sensitivity"])!.SetValue(.005);session.Step(1.0/60);Advance(20);var settings=Capture("settings-paused");Check(menu.MoveSpeed==7&&Math.Abs(menu.LookSensitivity-.005f)<.00001f&&scripts.SimulationPaused,"bound settings sliders update real menu model while paused");Click("back");Click("resume");Advance(20);var resumed=Capture("resumed");Check(menu.State==MenuFlowState.Playing&&!scripts.SimulationPaused&&character.MoveSpeed==7&&Math.Abs(character.LookSensitivity-.005f)<.00001f,"resume applies settings to real character");
        var start=actor.Transform.Position;scripts.SetInput(new ScriptInput([65],[],[]));Advance(30);scripts.SetInput(ScriptInput.Empty);float distance=Math.Abs(actor.Transform.Position.X-start.X);Check(Math.Abs(distance-3.5f)<.04f,"configured speed changes actual capsule movement");
        const int cycles=16;for(int i=0;i<cycles;++i){Escape();Click("pause-settings");Click("back");Click("resume");menu.ReturnToMainMenu();Advance(15);Click("start");Check(game.Starts==i+2&&scripts.UiActions.Count==8,"repeated menu entry/exit cannot duplicate bound actions");}
        Check(Difference(main,playing)>1000&&Difference(mainSettings,main)>1000&&Difference(paused,playing)>1000&&Difference(settings,paused)>1000,"actual GPU pages reflect menu state changes");
        bool missingCapture=false;session.SetWindowCapture(Path.Combine(root,"unreached.ppm"),5);try{session.RunWindow("en-US",false,2);}catch(EngineException e){missingCapture=e.Key=="Sample.CaptureMissing";}Check(missingCapture&&!File.Exists(Path.Combine(root,"unreached.ppm")),"unreached explicit capture frame cannot report success");session.SetWindowCapture(null);
        scripts.Remove(menu);Check(scripts.UiActions.Count==0&&session.Ui.Visuals().Count==0&&!scripts.SimulationPaused,"menu teardown removes all UI roots and actions");Check(diagnostics.All(d=>d.Key=="Script.FixedStepBudget"),"no swallowed menu/window script errors");
        File.WriteAllText(Path.Combine(root,"menu-window.json"),JsonSerializer.Serialize(new{success=true,worldPixels,crosshairCenter=true,oneInteraction=true,pausedPhysics=true,settings=true,actualMove=distance,cycles,starts=game.Starts,actionsAfterTeardown=scripts.UiActions.Count,uiAfterTeardown=session.Ui.Visuals().Count,captureBusy=busyCapture,missingCapture,noCameraSafe=true,manualInputVerified=false}));Console.WriteLine($"MenuFlowWindowTests passed (real World/UI GPU, {worldPixels} scene pixels, pause/settings/movement and {cycles} menu cycles): {root}");GC.KeepAlive(resumed);
    }
}

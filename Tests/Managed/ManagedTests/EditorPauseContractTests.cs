using System.Numerics;
using System.Text.Json;
using SelfEngine;

internal static class EditorPauseContractTests
{
    private static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    public static void Run()
    {
        var root=Path.Combine(AppContext.BaseDirectory,"Validation","EditorPause",Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(root,"Assets"));
        using var session=new EngineSession();
        // 正式 editor.play 在启动回调之前验证相机；夹具采用与 BlankScene 模板相同的场景格式。
        session.ImportScene("""
            {
              "format":"SelfEngine.Scene",
              "version":1,
              "entities":[
                {"id":"1","name":"Camera","enabled":true,"parent":null,
                 "transform":{"position":[0,2,5],"rotation":[0,0,0,1],"scale":[1,1,1]},
                 "components":{"camera":{"fov":1.0471976,"near":0.1,"far":1000,"primary":true}}}
              ]
            }
            """);
        var entity=session.CreateEntity("editor pause probe");var id=entity.StableId;
        var scene=Path.Combine(root,"Main.scene.json");File.WriteAllText(scene,session.ExportScene());
        var report=Path.Combine(root,"playback.json");
        var variables=new Dictionary<string,string?>
        {
            ["SELFENGINE_EDITOR_SCENE_FILE"]=scene,["SELFENGINE_EDITOR_SCENE_EXPLICIT"]="1",
            ["SELFENGINE_EDITOR_UI_FILE"]=null,["SELFENGINE_EDITOR_RESTART_REQUEST"]=null,
            ["SELFENGINE_EDITOR_BUILD_STATUS"]=null,["SELFENGINE_EDITOR_BUILD_DIAGNOSTICS"]=null,
            ["SELFENGINE_EDITOR_SMOKE_REPORT"]=report,["SELFENGINE_EDITOR_SMOKE_PLAYBACK"]="1",
            ["SELFENGINE_EDITOR_SMOKE_EXPORT"]=null,["SELFENGINE_EDITOR_SMOKE_CANCEL_MS"]=null,
            ["SELFENGINE_EDITOR_MANAGED_HOST"]="1"
        };
        var previous=variables.Keys.ToDictionary(key=>key,Environment.GetEnvironmentVariable);
        ScriptSystem? scripts=null;EditorPauseProbe? probe=null;EditorPausedControlProbe? control=null;
        int updates=0,stops=0;bool stoppedCorrectly=false;
        session.Update+=_=>{if(scripts is {IsRunning:true})++updates;};
        session.EditorPlayChanged+=playing=>
        {
            if(playing)
            {
                scripts=new ScriptSystem(session,new ScriptRegistry(),fixedDeltaSeconds:.02);
                probe=scripts.Attach<EditorPauseProbe>(session.FindEntity(id)!);
                control=scripts.Attach<EditorPausedControlProbe>(session.FindEntity(id)!);
                scripts.Start();scripts.SimulationPaused=true;
            }
            else
            {
                ++stops;
                try
                {
                    session.Ui.Layout(640,480);
                    var opacity=session.Ui.Visuals().Single(value=>value.Id==probe!.PanelId).Opacity;
                    stoppedCorrectly=scripts is {SimulationPaused:true}&&probe is {Updates:1,FixedCalls:1}
                        &&control is {Updates:1}&&updates==1&&session.SimulationClock.Steps==1
                        &&probe.Entity.Transform.Position.Y<20&&Math.Abs(opacity-.98f)<.001f;
                }
                finally{scripts?.Dispose();scripts=null;}
            }
        };
        try
        {
            foreach(var pair in variables)Environment.SetEnvironmentVariable(pair.Key,pair.Value);
            session.RunEditor(root,"en-US",false,120);
        }
        finally
        {
            scripts?.Dispose();
            foreach(var pair in previous)Environment.SetEnvironmentVariable(pair.Key,pair.Value);
        }
        using var document=JsonDocument.Parse(File.ReadAllText(report));var proof=document.RootElement;
        Check(proof.GetProperty("success").GetBoolean()&&proof.GetProperty("paused_wait_stable").GetBoolean()
            &&proof.GetProperty("paused_after_step").GetBoolean(),"actual editor command driver holds pause before and after one step");
        Check(proof.GetProperty("paused_wait_samples").EnumerateArray().All(value=>value.GetUInt64()==0)
            &&proof.GetProperty("after_step_samples").EnumerateArray().All(value=>value.GetUInt64()==1),"ordinary editor paused frames never advance physical steps");
        Check(stops==1&&stoppedCorrectly,"both pauses allow exactly one real managed update, control tick, UI update and physics step");
        Check(session.FindEntity(id)!.Transform.Position==Vector3.Zero&&session.Ui.Visuals().Count==0,"single step never changes edit scene or UI");
        Console.WriteLine("EditorPauseContractTests passed (runtime pause, editor frame pause, real managed single step and UI/physics): "+report);
    }
}

[ScriptId("test.editor_pause.regular")]
public sealed class EditorPauseProbe:Behaviour
{
    public int Updates,FixedCalls;public ulong PanelId;
    public override void OnCreate()
    {
        var transform=Entity.Transform;transform.Position=new(0,20,0);Entity.Transform=transform;
        Entity.Session.Physics.CreateBox(Entity,BoxBodyDescription.Dynamic);
        var panel=Entity.Session.Ui.Create(UiKind.Panel);PanelId=panel.Id;
        var style=UiStyle.Default;style.Size=new(100,100);panel.SetStyle(style);panel.FadeTo(0,1);
    }
    public override void OnFixedUpdate(double seconds)=>++FixedCalls;
    public override void OnUpdate(double seconds)=>++Updates;
}

[ScriptId("test.editor_pause.control")]
public sealed class EditorPausedControlProbe:Behaviour
{
    public int Updates;
    public override bool UpdateWhenPaused=>true;
    public override void OnUpdate(double seconds)=>++Updates;
}

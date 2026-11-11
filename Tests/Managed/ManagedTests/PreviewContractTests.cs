using System.Diagnostics;
using System.Numerics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using SelfEngine;

internal static class PreviewContractTests
{
    private static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    private static string Hash(string path)=>Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static (List<WeakReference> Contexts,List<ScriptSystem> Systems,int Cycles) EditorCycles(EngineSession session,string root,string assembly,string bindings,int limit)
    {
        using var project=new ScriptProject(session);var diagnostics=new List<ScriptDiagnostic>();project.Diagnostic+=diagnostics.Add;
        var contexts=new List<WeakReference>();var systems=new List<ScriptSystem>();int phase=0,stops=0;bool observing=false;UiElement? oldButton=null;Entity? spawned=null;
        var oldEntities=new List<Entity>();var oldScenes=new List<SceneHandle>();ScriptEventPublisher<int>? oldPublisher=null;
        void Observed(bool playing)
        {
            if(playing){phase=0;return;}
            ++stops;Check(systems[^1].Count==0&&systems[^1].Events.Count==0&&systems[^1].UiActions.Count==0&&!systems[^1].IsRunning,"same-window Stop clears scripts, subscriptions and actions");
            Check(oldPublisher!.Publish(1)==EventPostResult.Closed&&session.Scenes.Statistics.PendingJobs==0,"Stop closes old publishers and joins scene work before managed teardown returns");
            bool closedTime=false;try{session.TimeOfDay.SetTime(0,.5);}catch(EngineException error){closedTime=error.Key=="Environment.Closed";}Check(closedTime,"Stop callback cannot restart runtime TOD");
            Check(session.Ui.Visuals().Count==1,"runtime menu roots removed while borrowed Play UI remains alive");
            bool retired=false;try{oldButton!.SetText("stale");}catch(EngineException e){retired=e.Key=="Bridge.InvalidUi";}Check(retired,"removed menu proxy is rejected before Play UI release");
            contexts.Add(project.LastUnloadedContext!);
            if(stops<limit)session.RequestEditorPlay(true);else session.RequestWindowClose();
        }
        bool Click(ScriptSystem scripts,string action)
        {
            session.Ui.Layout(960,640);
            var visual=session.Ui.Visuals().FirstOrDefault(v=>v.Enabled&&v.Opacity>.99f&&session.Ui.Find(v.Id)!.Bindings.Any(b=>b.Action==action));
            if(visual is null)return false;
            var point=new Vector2(visual.X+visual.Width/2,visual.Y+visual.Height/2);Check(session.Ui.Pointer(UiPointerOperation.Down,point)&&session.Ui.Pointer(UiPointerOperation.Up,point),"preview bound control "+action);return true;
        }
        void Update(double _)
        {
            if(!project.IsRunning)return;
            if(!observing){observing=true;session.EditorPlayChanged+=Observed;}
            var scripts=project.Scripts;
            switch(phase)
            {
                case 0:
                    Check(session.TimeOfDay.Snapshot is {Active:true,Day:0,Time:.25,SimulationSeconds:0},"every Play starts its own configured TOD and clock");
                    Check(oldEntities.All(entity=>!entity.IsAlive),"earlier Play entities remain invalid");
                    Check(scripts.Count==3&&scripts.UiActions.Count==8,"each Play creates exactly the menu, character and world scripts");
                    if(!Click(scripts,"menu.start"))return;
                    var currentScene=session.Scenes.Active!.Handle;Check(!oldScenes.Contains(currentScene),"each Play owns a fresh scene domain");oldScenes.Add(currentScene);oldEntities.Add(session.FindEntity(1)!);
                    oldPublisher=scripts.Events.CreatePublisher<int>();
                    systems.Add(scripts);oldButton=session.Ui.Visuals().Select(v=>session.Ui.Find(v.Id)!).First(n=>n.Bindings.Any(b=>b.Action=="menu.start"));++phase;break;
                case 1:
                    session.TimeOfDay.SetTime(9,.9);
                    Check(!scripts.SimulationPaused,"menu Start enables real simulation");
                    // 生命周期测试调用菜单的公开 Pause；窗口输入仍由真实 native 快照提供，不伪造 SetInput。
                    var attachments=(IReadOnlyList<Behaviour>)typeof(ScriptSystem).GetProperty("Attachments",BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(scripts)!;
                    var menu=attachments.Single(value=>ScriptRegistry.IdOf(value.GetType())=="sample.menu_flow");Check((bool)menu.GetType().GetMethod("Pause")!.Invoke(menu,null)!,"public menu pause succeeds");++phase;break;
                case 2:Check(scripts.SimulationPaused,"same-window menu pauses simulation");if(Click(scripts,"menu.settings"))phase=4;break;
                case 4:
                    if(!Click(scripts,"menu.back"))return;
                    var speed=session.Ui.Visuals().Select(v=>session.Ui.Find(v.Id)!).First(n=>n.Bindings.Any(b=>b.Action=="menu.speed"));speed.SetValue(6);++phase;break;
                case 5:if(Click(scripts,"menu.resume"))++phase;break;
                case 6:Check(!scripts.SimulationPaused,"preview resumes after settings");scripts.Destroy(session.FindEntity(4)!);++phase;break;
                case 7:Check(session.FindEntity(4) is null,"runtime deletion of referenced target is applied");spawned=session.CreateEntity("runtime replacement");session.RequestEditorPlay(false);++phase;break;
            }
        }
        session.Update+=Update;
        try{project.RunEditor(root,assembly,ScriptBindings.ScriptTypes(bindings),bindings,"en-US",false,3000,true);}
        finally{session.Update-=Update;session.EditorPlayChanged-=Observed;}
        Check(stops==limit&&systems.Count==limit&&spawned is {IsAlive:false},"all same-window preview cycles finish and retire runtime entities");
        Check(diagnostics.All(d=>d.Key=="Script.FixedStepBudget"),"mixed preview has no script callback or reference errors");return(contexts,systems,stops);
    }
    private static void Window(string root)
    {
        var document=JsonNode.Parse(File.ReadAllText("Resources/Samples/Scenes/MenuFlow.scene.json"))!;document["environment"]=new JsonObject{["todProfile"]="c7e70000000040008000000000000001",["preserveTime"]=false};
        var scene=document.ToJsonString();var bindings=File.ReadAllText("Resources/Samples/Scripts/MenuFlow.bindings.json");var assembly=Path.Combine(AppContext.BaseDirectory,"ProjectScripts","SelfEngine.ProjectScripts.dll");
        var assets=Path.Combine(root,"Assets");File.Copy("Resources/Samples/TOD/DayNight.tod",Path.Combine(assets,"day.tod"));File.Copy("Resources/Samples/TOD/DayNight.tod.asset.json",Path.Combine(assets,"day.tod.asset.json"));
        using var session=new EngineSession();session.LoadLocale("en-US");session.SetAssetRoot(assets,Path.Combine(root,"Cache"));session.ImportScene(scene);session.Ui.Create(UiKind.Label).SetText("edit-only label");string originalScene=session.ExportScene(),originalUi=session.Ui.Save();
        var first=EditorCycles(session,root,assembly,bindings,16);Check(session.ExportScene()==originalScene&&session.Ui.Save()==originalUi,"sixteen mixed previews never write runtime changes to edit scene/UI");
        var other=JsonNode.Parse(scene)!;other["entities"]![3]!["name"]="Other scene target";session.ImportScene(other.ToJsonString());string switched=session.ExportScene();var second=EditorCycles(session,root,assembly,bindings,2);Check(session.ExportScene()==switched&&session.Ui.Save()==originalUi,"scene switch and later previews retain the new edit data");
        var contexts=first.Contexts.Concat(second.Contexts).ToArray();var held=first.Systems.Concat(second.Systems).ToArray();
        for(int i=0;i<16&&contexts.Any(c=>c.IsAlive);++i){GC.Collect();GC.WaitForPendingFinalizers();GC.Collect();}
        Check(contexts.All(c=>!c.IsAlive)&&held.All(s=>s.Count==0&&s.Events.Count==0&&s.UiActions.Count==0),"held stopped systems do not retain any of eighteen project contexts");
        File.WriteAllText(Path.Combine(root,"editor-preview.json"),JsonSerializer.Serialize(new{success=true,sameWindowCycles=first.Cycles,secondSceneCycles=second.Cycles,editSceneUnchanged=true,editUiUnchanged=true,referencedDelete=true,staleUi=true,systemsAfterStop=0,eventsAfterStop=0,actionsAfterStop=0,collectedContexts=contexts.Length,isolatedEnvironment=true,freshSceneDomains=true,resetTod=true,closedPublishers=true,manualInputVerified=false}));GC.KeepAlive(held);
    }
    private static (int Exit,string Text) Process(string executable,IEnumerable<string> args)
    {
        var info=new ProcessStartInfo(executable){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true};foreach(var arg in args)info.ArgumentList.Add(arg);
        using var process=System.Diagnostics.Process.Start(info)!;var output=process.StandardOutput.ReadToEndAsync();var error=process.StandardError.ReadToEndAsync();
        if(!process.WaitForExit(120000)){process.Kill(true);throw new InvalidOperationException("preview child exceeded its bounded integration-test duration");}
        return(process.ExitCode,output.Result+error.Result);
    }
    [SupportedOSPlatform("windows")]
    private static void Rebuild(string root)
    {
        var projectRoot=Path.Combine(root,"预览 Project");Directory.CreateDirectory(Path.Combine(projectRoot,"Assets"));var configuration=AppContext.BaseDirectory.Contains("/Release/",StringComparison.OrdinalIgnoreCase)||AppContext.BaseDirectory.Contains("\\Release\\",StringComparison.OrdinalIgnoreCase)?"Release":"Debug";
        var scene=Path.Combine(projectRoot,"main.scene.json");var bindings=Path.Combine(projectRoot,"Bindings.json");var source=Path.Combine(projectRoot,"Probe.cs");var result=Path.Combine(projectRoot,"result.json");
        File.Copy("Resources/Samples/Scenes/MenuFlow.scene.json",scene);File.WriteAllText(bindings,"{\"Version\":1,\"Scripts\":[{\"Type\":\"test.preview\",\"Entity\":\"1\",\"Enabled\":true,\"Fields\":{}}]}");
        var assembly=Path.Combine(AppContext.BaseDirectory,"SelfEngine.Managed.dll");File.WriteAllText(Path.Combine(projectRoot,"PreviewFixture.csproj"),$"<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net10.0</TargetFramework><AssemblyName>PreviewFixture</AssemblyName><ImplicitUsings>enable</ImplicitUsings><Nullable>enable</Nullable><EnableDefaultCompileItems>false</EnableDefaultCompileItems></PropertyGroup><ItemGroup><Compile Include=\"Probe.cs\"/><Reference Include=\"SelfEngine.Managed\"><HintPath>{System.Security.SecurityElement.Escape(assembly)}</HintPath></Reference></ItemGroup></Project>");
        string Code(int version)=>$$"""
            using SelfEngine;using System.Text.Json;
            [ScriptId("test.preview")] public sealed class Probe:Behaviour {
                private RigidBody? body;private UiElement? button;
                public override void OnCreate(){var t=Entity.Transform;t.Position.X={{version}};Entity.Transform=t;body=Entity.Session.Physics.CreateBox(Entity,BoxBodyDescription.Dynamic);button=Entity.Session.Ui.Create(UiKind.Button);Scripts.UiActions.Register(this,"fire",_=>{});button.Bind(UiEventKind.Click,this,"fire");}
                public override void OnDestroy(){File.WriteAllText({{JsonSerializer.Serialize(result)}},JsonSerializer.Serialize(new{version={{version}},process=Environment.ProcessId,x=Entity.Transform.Position.X,alive=Entity.IsAlive}));body?.Destroy();button?.Remove();}
            }
            """;
        string sceneHash=Hash(scene),bindingsHash=Hash(bindings);var reports=new List<JsonObject>();
        (int Exit,string Text) Run(string mode)=>Process("powershell.exe",["-NoProfile","-File",Path.GetFullPath("Scripts/Run-ProjectPreview.ps1"),"-ProjectRoot",projectRoot,"-ScriptsProject","PreviewFixture.csproj","-Scene","main.scene.json","-Bindings","Bindings.json","-Configuration",configuration,"-Mode",mode,"-Locale","en-US","-Frames","3","-Hidden","-Play"]);
        JsonObject Latest()=>JsonNode.Parse(File.ReadAllText(Directory.GetDirectories(Path.Combine(projectRoot,"out","Preview")).OrderByDescending(Directory.GetLastWriteTimeUtc).Select(p=>Path.Combine(p,"preview.json")).First()))!.AsObject();
        for(int version=1;version<=2;++version){File.WriteAllText(source,Code(version));var run=Run(version==1?"Headless":"Editor");Check(run.Exit==0,"fresh preview workflow: "+run.Text);var value=JsonNode.Parse(File.ReadAllText(result))!;var report=Latest();Check(value["version"]!.GetValue<int>()==version&&value["x"]!.GetValue<float>()==version&&value["alive"]!.GetValue<bool>()&&value["process"]!.GetValue<int>()==report["process_id"]!.GetValue<int>(),"changed script executes in the new preview process and destroys while World is alive");reports.Add(report);}
        Check(reports[0]["process_id"]!.GetValue<int>()!=reports[1]["process_id"]!.GetValue<int>()&&reports[0]["assembly_sha256"]!.GetValue<string>()!=reports[1]["assembly_sha256"]!.GetValue<string>(),"recompile changes the assembly and starts a distinct process");
        var previous=File.ReadAllText(result);File.WriteAllText(source,Code(3)+"\nthis is invalid C#;");var broken=Run("Editor");var failed=Latest();
        Check(broken.Exit!=0&&!failed["success"]!.GetValue<bool>()&&failed["process_id"] is null&&File.ReadAllText(result)==previous,"compile failure never starts a partial preview");
        var latestDirectory=Directory.GetDirectories(Path.Combine(projectRoot,"out","Preview")).OrderByDescending(Directory.GetLastWriteTimeUtc).First();var buildLog=File.ReadAllText(Path.Combine(latestDirectory,"project-build.log"));Check(buildLog.Contains("Probe.cs(")&&buildLog.Contains("error CS"),"compile diagnostic retains source file, line and error code");
        File.WriteAllText(source,Code(3).Replace("public override void OnDestroy()","public override void OnUpdate(double seconds)=>throw new InvalidOperationException(\"PreviewExpectedFailure\");public override void OnDestroy()"));
        var runtimeFailure=Run("Editor");var runtimeReport=Latest();var runtimeDirectory=Directory.GetDirectories(Path.Combine(projectRoot,"out","Preview")).OrderByDescending(Directory.GetLastWriteTimeUtc).First();var runtimeLog=File.ReadAllText(Path.Combine(runtimeDirectory,"preview.log"));
        Check(runtimeFailure.Exit!=0&&!runtimeReport["success"]!.GetValue<bool>()&&runtimeReport["phase"]!.GetValue<string>()=="run_failed"&&runtimeReport["process_id"] is not null&&runtimeLog.Contains("phase=OnUpdate")&&runtimeLog.Contains("PreviewExpectedFailure"),"runtime callback failure preserves its phase and cannot report a successful preview");
        Check(Hash(scene)==sceneHash&&Hash(bindings)==bindingsHash,"successful, compile-failed and runtime-failed previews preserve edit scene and bindings files");
        var leasePath=Path.GetFullPath("out/preview/preview.lock");using(var lease=File.Open(leasePath,FileMode.OpenOrCreate,FileAccess.ReadWrite,FileShare.ReadWrite))
        {
            lease.Lock(1,1);int cases=Directory.GetDirectories(Path.Combine(projectRoot,"out","Preview")).Length;var busy=Run("Headless");Check(busy.Exit!=0&&Directory.GetDirectories(Path.Combine(projectRoot,"out","Preview")).Length==cases,"running child lease prevents build before any output is created");
            var bootstrap=Path.GetFullPath(Path.Combine("out","managed","Bootstrap",configuration,"net10.0","SelfEngine.Bootstrap.dll"));var blockedChild=Process("dotnet.exe",[bootstrap,"--preview-lease",leasePath,"--frames","1"]);Check(blockedChild.Exit!=0&&blockedChild.Text.Contains("Preview.Busy"),"Bootstrap refuses a conflicting child lease before creating the engine session");lease.Unlock(1,1);
        }
        File.WriteAllText(Path.Combine(root,"script-rebuild.json"),JsonSerializer.Serialize(new{success=true,newProcesses=true,changedAssembly=true,changedBehaviour=true,compileFailure=true,runtimeFailure=true,fileLineDiagnostic=true,editFilesUnchanged=true,activeLeaseRejected=true,childLeaseRejected=true,versions=2,reports}));
    }
    public static void Run(bool window,bool rebuild=true)
    {
        if(!window||!OperatingSystem.IsWindows())return;
        var root=Path.Combine(AppContext.BaseDirectory,"Validation","T64",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(Path.Combine(root,"Assets"));Window(root);if(rebuild)Rebuild(root);
        Console.WriteLine("PreviewContractTests passed (same-window Play/Stop, scene switch, collectible projects and compile/new-process workflow): "+root);
    }
}

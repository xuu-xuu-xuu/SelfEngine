using SelfEngine;
using System.Numerics;
using System.Text.Json.Nodes;

static void Check(bool condition, string name) { if (!condition) throw new InvalidOperationException(name); }

if(args.Length==2&&args[0]=="--field-reopen")
{
    try{FieldWorkflowContractTests.VerifyFreshProcess(args[1]);return 0;}
    catch(Exception exception){Console.Error.WriteLine(exception);return 1;}
}
if(args.Length==1&&args[0]=="--environment-preview")
{
    try{PreviewContractTests.Run(true,false);return 0;}
    catch(Exception exception){Console.Error.WriteLine(exception);return 1;}
}
if(args.Length==3&&args[0]=="--prepare-export")
{
    try{ExporterAuthoringContractTests.Prepare(args[1],args[2]);return 0;}
    catch(Exception exception){Console.Error.WriteLine(exception);return 1;}
}

try
{
    LifecycleContractTests.Run();
    EnvironmentContractTests.Run();
    WindowProfileContractTests.Run(args.Contains("--window-smoke"));
    EnvironmentSchedulingContractTests.Run();
    if(args.Contains("--window-smoke"))EditorPauseContractTests.Run();
    EventContractTests.Run();
    SceneContractTests.Run();
    FieldWorkflowContractTests.Run(args.Contains("--window-smoke"));
    SchedulingContractTests.Run();
    PhysicsIntegrationContractTests.Run();
    PhysicsInteractionContractTests.Run(args.Contains("--window-smoke"));
    UiRoutingContractTests.Run(args.Contains("--window-smoke"));
    UiTextContractTests.Run(args.Contains("--window-smoke"));
    UiControlsContractTests.Run();
    UiTransitionContractTests.Run();
    UiCanvasContractTests.Run();
    UiBindingContractTests.Run();
    ScriptValidationContractTests.Run();
    BindingDocumentContractTests.Run();
    PreviewContractTests.Run(args.Contains("--window-smoke"));
    using(var session=new EngineSession())
    {
        var root=Path.Combine(AppContext.BaseDirectory,"AssetSamples");var cache=Path.Combine(AppContext.BaseDirectory,"AssetCache");
        session.SetAssetRoot(root,cache);const string modelId="11111111111111111111111111111111";
        Check(session.AssetIds.Contains(modelId),"configured asset root preserves model ID");
        var model=session.LoadModelInfo(modelId);Check(model.Meshes>0&&model.Vertices>0&&model.Indices>0,"actual model ID resolves to CPU mesh data");
        bool failed=false;try{session.SetAssetRoot(Path.Combine(root,"missing"),cache);}catch(EngineException exception){failed=exception.Key=="Bridge.AssetScanFailed";}
        Check(failed&&session.AssetIds.Contains(modelId)&&session.LoadModelInfo(modelId).Vertices==model.Vertices,"failed asset scan preserves previous store");
        bool rejected=false;session.Update+=_=>{try{session.SetAssetRoot(root,cache);}catch(EngineException exception){rejected=exception.Key=="Bridge.SessionBusy";}};
        session.Step(0);Check(rejected,"asset root cannot be replaced during callback/render lifetime");
    }
    using (var session = new EngineSession())
    {
        // 终结器可能在原生回调中运行；注册表锁不能跨回调阻塞最终释放。
        for (int i = 0; i < 10; ++i) MakeOrphanSession();
        int calls = 0;
        session.Update += dt => { ++calls; session.Value += dt; GC.Collect(); GC.WaitForPendingFinalizers(); };
        for (int frame = 0; frame < 120; ++frame) session.Step(1.0 / 60.0);
        Check(calls == 120 && session.StepCount == 120 && Math.Abs(session.Value - 2) < 1e-9, "GC-safe callback and native value roundtrip");
        var entity = session.CreateEntity("中文实体");
        Check(entity.Name == "中文实体" && entity.IsAlive, "UTF8 name roundtrip");
        var transform = Transform.Identity;
        transform.Position = new(1,2,3);
        entity.Transform = transform;
        Check(entity.Transform.Position == transform.Position, "native entity transform");
        entity.Destroy();
        var replacement = session.CreateEntity("replacement");
        Check(!entity.IsAlive && replacement.Handle != entity.Handle, "stale proxy after slot reuse");
        bool rejected = false;
        try { _ = entity.Transform; } catch (EngineException exception) { rejected = exception.Key == "Bridge.InvalidEntity"; }
        Check(rejected, "stale transform throws readable error");
    }
    using (var session = new EngineSession())
    {
        bool diagnosed = false;
        session.Diagnostic += (key, _) => diagnosed = key == "Bridge.ManagedCallbackFailed";
        session.Update += _ => throw new InvalidOperationException("expected");
        bool rejected = false;
        try { session.Step(0.01); } catch (EngineException exception) { rejected = exception.Result == 5 && exception.InnerException is InvalidOperationException; }
        Check(rejected && diagnosed, "managed exception does not cross ABI");
    }
    var disposing = new EngineSession();
    disposing.Update += _ => disposing.Dispose();
    disposing.Step(0.01);
    Check(disposing.IsDisposed, "Dispose in callback deferred until native step returns");
    for (int iteration = 0; iteration < 50; ++iteration) { using var session = new EngineSession(); session.Step(0); }

    using (var session = new EngineSession())
    {
        var registry = new ScriptRegistry();
        registry.Discover(typeof(MoveBehaviour).Assembly);
        using var scripts = new ScriptSystem(session, registry);
        var diagnostics = new List<ScriptDiagnostic>();
        scripts.Diagnostic += diagnostics.Add;
        var mover = scripts.Attach<MoveBehaviour>(session.CreateEntity("mover"), value => value.Speed = 3);
        var failing = scripts.Attach<FailBehaviour>(session.CreateEntity("failing"));
        var self = scripts.Attach<SelfDestroyBehaviour>(session.CreateEntity("self"));
        var listener = scripts.Attach<ListenBehaviour>(session.CreateEntity("listener"));
        scripts.Start();
        Check(mover.Created == 1 && mover.EnabledCalls == 1 && scripts.Events.Count == 1, "create/enable/subscription lifecycle");
        session.Step(1.0/30.0);
        Check(self.Destroyed == 1 && !self.Entity.IsAlive && scripts.Count == 3, "self destruction queued and OnDestroy once");
        Check(failing.Updates == 1 && !failing.Enabled && diagnostics.Any(value => value.Key == "Script.CallbackFailed"), "one failure disabled and isolated");
        session.Step(1.0/30.0);
        Check(failing.Updates == 1 && mover.Updates == 2 && mover.Entity.Transform.Position.X > 0.19f, "healthy behaviour continues after failure");
        mover.Enabled = false;
        session.Step(1.0/30.0);
        Check(mover.Updates == 2 && mover.DisabledCalls == 1, "disable stops updates");
        mover.Enabled = true;
        session.Step(1.0/30.0);
        Check(mover.Updates == 3 && mover.EnabledCalls == 2, "enable resumes updates");
        scripts.Events.Publish(7);
        Check(listener.Received==0,"event waits for AfterLogic");session.Step(0);
        Check(listener.Received == 7, "event dispatch");
        scripts.Destroy(listener.Entity);
        scripts.Events.Publish(11);
        session.Step(0);
        Check(listener.Destroyed == 1 && listener.Received == 7 && scripts.Events.Count == 0, "subscription removed on destruction");
        scripts.Stop();
        Check(mover.Destroyed == 1 && failing.Destroyed == 1 && scripts.Count == 0, "stop cleans behaviours");
    }
    using (var session = new EngineSession())
    {
        var registry = new ScriptRegistry();
        using var scripts = new ScriptSystem(session, registry);
        var input = scripts.Attach<InputBehaviour>(session.CreateEntity("input"));
        scripts.Start();
        scripts.SetInput(new ScriptInput([87], [87], [], false, new(new(8,9),1,1,0,2,false)));
        session.Step(1.0/120.0);
        Check(input.UpdatePresses == 1 && input.FixedPresses == 0, "edge survives until first fixed step");
        scripts.SetInput(new ScriptInput([87], [], []));
        session.Step(1.0/120.0);
        session.Step(1.0/30.0);
        Check(input.FixedPresses == 1 && input.UpdatePresses == 1 && input.FixedCalls == 3, "edge consumed once across multiple fixed steps");
        Check(input.FixedClicks == 1 && input.UpdateClicks == 1 && input.FixedWheel == 2, "mouse edge and wheel consumed once by fixed clock");
        scripts.SetInput(new ScriptInput([87], [87], [], true));
        session.Step(1.0/120.0);
        scripts.SetInput(new ScriptInput([], [], []));
        session.Step(1.0/60.0);
        Check(input.FixedPresses == 1 && input.UpdatePresses == 1, "UI-consumed input does not leak later");
        var clicksBefore = input.FixedClicks;
        scripts.SetInput(new ScriptInput([],[],[],false,new(new(9,10),1,1,0,2,false)));
        session.Step(0);
        scripts.SetInput(new ScriptInput([],[],[],true,new(new(9,10),0,0,0,0,true)));
        session.Step(1.0/60.0);
        Check(input.FixedClicks == clicksBefore, "pointer cancellation clears queued fixed edges");
        session.Step(1.0);
        Check(scripts.DroppedFixedSteps > 0, "fixed-step catch-up budget");
    }
    using (var session = new EngineSession())
    {
        var registry = new ScriptRegistry(); registry.Register<FieldsBehaviour>();
        using var scripts = new ScriptSystem(session, registry);
        var referenced = session.CreateEntity("target");
        var owner = session.CreateEntity("fields");
        var value = scripts.Attach<FieldsBehaviour>(owner, field =>
        {
            field.Count = 8; field.Speed = 2.5f; field.EnabledFlag = true; field.Label = "中文字段";
            field.Direction = new(2,3,4); field.Target = EntityReference.From(referenced);
        });
        var scene = session.ExportScene();
        var fields = ScriptFields.Save(scripts);
        Check(JsonNode.Parse(scene)!["version"]!.GetValue<int>()==2,"native scene v2 uses registered properties");
        Check(ScriptFields.Describe(value).Count == 6, "field metadata whitelist");
        Check(!ScriptFields.Load(scripts, "{") && scripts.Count == 1, "bad JSON preserves old attachments");
        scripts.Stop();
        session.ImportScene(scene);
        Check(!owner.IsAlive && ScriptFields.Load(scripts, fields) && scripts.Count == 1, "scene handles refreshed and field references restored by stable ID");
        FieldsBehaviour.LastCreated = null;
        scripts.Start();
        var restored = FieldsBehaviour.LastCreated!;
        Check(restored.Count == 8 && restored.Speed == 2.5f && restored.EnabledFlag && restored.Label == "中文字段"
            && restored.Direction == new Vector3(2,3,4) && restored.Target.Resolve(session)?.Name == "target", "serialized field values restored");
        Check(session.NativeScriptComponentCount==1,"bindings recreate a controlled component after scene v2 import");
        var propertyProof=Path.Combine(AppContext.BaseDirectory,"Validation","E02",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(propertyProof);
        File.WriteAllText(Path.Combine(propertyProof,"managed-properties.json"),System.Text.Json.JsonSerializer.Serialize(new{success=true,sceneVersion=2,fields=6,stableScriptType=true,entityReferenceRestored=true,runtimeHandleRefreshed=!owner.IsAlive,nativeAssociations=1}));
        scripts.Stop();
        var diagnostics = new List<ScriptDiagnostic>(); scripts.Diagnostic += diagnostics.Add;
        var document = JsonNode.Parse(fields)!;
        document["Scripts"]![0]!["Fields"]!["Speed"]!["Kind"] = "string";
        Check(ScriptFields.Load(scripts, document.ToJsonString()) && diagnostics.Any(item => item.Key == "Script.FieldTypeChanged"), "field type change diagnosed");
        scripts.Stop();
        document["Scripts"]![0]!["Type"] = "missing.type";
        Check(ScriptFields.Load(scripts, document.ToJsonString()) && scripts.Count == 0 && diagnostics.Any(item => item.Key == "Script.TypeMissing"), "missing type remains readable");
        var editable=new ScriptBindings(typeof(FieldsBehaviour).Assembly.Location,["tests.fields"],fields);
        var editId=ulong.Parse(JsonNode.Parse(fields)!["Scripts"]![0]!["Entity"]!.GetValue<string>());
        var metadata=JsonNode.Parse(editable.Describe(editId))!;
        Check(metadata["Scripts"]![0]!["Fields"]!.AsArray().Count==6,"Inspector exposes whitelist metadata");
        editable.Apply(editId,"{\"Type\":\"tests.fields\",\"Field\":\"Count\",\"Value\":42}");
        editable.Apply(editId,"{\"Type\":\"tests.fields\",\"Field\":\"Direction\",\"Value\":{\"X\":5,\"Y\":6,\"Z\":7}}");
        var validJson=editable.Json;bool invalidRejected=false;
        try{editable.Apply(editId,"{\"Type\":\"tests.fields\",\"Field\":\"Count\",\"Value\":\"wrong\"}");}catch(ArgumentException){invalidRejected=true;}
        Check(invalidRejected&&editable.Json==validJson,"Inspector rejects bad values without losing bindings");
        var bindingPath=Path.Combine(AppContext.BaseDirectory,"Bindings.test.json");editable.Save(bindingPath);
        Check(ScriptFields.Load(scripts,File.ReadAllText(bindingPath)),"Inspector saved JSON can load into runtime");
        scripts.Start();Check(FieldsBehaviour.LastCreated!.Count==42&&FieldsBehaviour.LastCreated.Direction==new Vector3(5,6,7),"Inspector edits reach next Play instance");scripts.Stop();
    }
    using (var session = new EngineSession())
    {
        var floor = session.CreateEntity("floor");
        var floorTransform = Transform.Identity; floorTransform.Position.Y = -0.5f; floor.Transform = floorTransform;
        var floorDesc = BoxBodyDescription.Static; floorDesc.HalfExtents = new(5,0.5f,5);
        session.Physics.CreateBox(floor, floorDesc);
        var box = session.CreateEntity("box");
        var boxTransform = Transform.Identity; boxTransform.Position.Y = 2; box.Transform = boxTransform;
        var body = session.Physics.CreateBox(box, BoxBodyDescription.Dynamic);
        bool rejected = false;
        try { box.Transform = boxTransform; } catch (EngineException exception) { rejected = exception.Key == "Bridge.PhysicsOwnsTransform"; }
        Check(rejected, "physics is the sole transform writer");
        var hit = session.Physics.Raycast(new(0,5,0), -Vector3.UnitY, 10);
        Check(hit?.Entity.StableId == box.StableId, "raycast resolves actual world entity");
        bool contactBegan = false;
        for (int i = 0; i < 180; ++i)
        {
            session.Physics.Step(1.0/60.0);
            contactBegan |= session.Physics.DrainContacts().Any(contact => contact.Began);
        }
        Check(contactBegan && box.Transform.Position.Y > 0.35f && box.Transform.Position.Y < 0.7f, "physics pose sync and contact events");
        boxTransform.Position = new(0,3,0); body.Teleport(boxTransform);
        body.ApplyImpulse(new(1,0,0), boxTransform.Position);
        Check(session.Physics.Advance(1.0/30.0) == 2 && box.Transform.Position.X > 0, "teleport impulse and fixed advance");
        box.Destroy();
        rejected = false;
        try { body.SetVelocity(Vector3.Zero); } catch (EngineException exception) { rejected = exception.Key == "Bridge.InvalidBody"; }
        Check(rejected, "destroying entity destroys its body");
    }
    using (var session = new EngineSession())
    {
        var floor = session.CreateEntity("contact floor");
        var floorTransform = Transform.Identity; floorTransform.Position.Y = -0.5f; floor.Transform = floorTransform;
        var desc = BoxBodyDescription.Static; desc.HalfExtents = new(5,0.5f,5); session.Physics.CreateBox(floor, desc);
        using var scripts = new ScriptSystem(session, new ScriptRegistry());
        var falling = session.CreateEntity("contact self destroy");
        var transform = Transform.Identity; transform.Position.Y = 1; falling.Transform = transform;
        var listener = scripts.Attach<ContactDestroyBehaviour>(falling);
        session.Physics.CreateBox(falling, BoxBodyDescription.Dynamic);
        scripts.Start();
        for (int i = 0; i < 120 && falling.IsAlive; ++i) session.Step(1.0/60.0);
        Check(!falling.IsAlive && listener.Contacts == 1 && scripts.Events.Count == 0, "contact callbacks happen after physics step and permit safe destruction");
    }
    using (var session = new EngineSession())
    {
        using var scripts = new ScriptSystem(session, new ScriptRegistry());
        var button = session.Ui.Create(UiKind.Button); button.SetText("中文按钮");
        var style = UiStyle.Default; style.Size = new(100,40); button.SetStyle(style);
        var listener = scripts.Attach<UiListenerBehaviour>(session.CreateEntity("UI listener"), value => value.Button = button);
        scripts.Start(); session.Ui.Layout(640,480);
        Check(session.Ui.Visuals().Any(v => v.Id == button.Id && v.Text == "中文按钮" && v.Width == 100), "UI CPU visual data and UTF8");
        Check(session.Ui.Pointer(UiPointerOperation.Down, new(10,10)) && session.Ui.Pointer(UiPointerOperation.Up, new(10,10)), "UI pointer consumes hit");
        session.Step(1.0/60.0);
        Check(listener.Clicks == 1, "UI click to managed behaviour");
        scripts.Destroy(listener.Entity);
        session.Ui.Pointer(UiPointerOperation.Down, new(10,10)); session.Ui.Pointer(UiPointerOperation.Up, new(10,10)); session.Step(1.0/60.0);
        Check(listener.Clicks == 1 && scripts.Events.Count == 0, "destroyed behaviour UI subscription removed");
        var input = session.Ui.Create(UiKind.TextInput); style.Offset.Y = 80; input.SetStyle(style); session.Ui.Layout(640,480); input.Focus();
        Check(session.Ui.ConsumesKeyboard && session.Ui.InputText("输入文本"), "UI focus and UTF8 input");
        var document = session.Ui.Save(); session.Ui.Load(document);
        bool invalidated = false; try { button.SetText("stale"); } catch (EngineException e) { invalidated = e.Key == "Bridge.InvalidUi"; }
        Check(invalidated && session.Ui.Visuals().Any(v => v.Text == "输入文本"), "UI document roundtrip invalidates old proxies");
    }
    using (var session = new EngineSession())
    {
        var entity = session.CreateEntity("project mover");
        var path = Path.Combine(AppContext.BaseDirectory, "ProjectScripts", "SelfEngine.ProjectScripts.dll");
        var fields = new JsonObject
        {
            ["Version"] = 1,
            ["Scripts"] = new JsonArray(new JsonObject { ["Type"] = "sample.project_mover", ["Entity"] = entity.StableId.ToString(), ["Enabled"] = true,
                ["Fields"] = new JsonObject { ["Speed"] = new JsonObject { ["Kind"] = "f32", ["Value"] = 3 } } })
        }.ToJsonString();
        using var project = new ScriptProject(session);
        for (int cycle = 0; cycle < 5; ++cycle)
        {
            RunProjectCycle(project, path, fields, session);
            var weak = project.LastUnloadedContext!;
            for (int attempt = 0; weak.IsAlive && attempt < 10; ++attempt) { GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect(); }
            Check(!weak.IsAlive, "project AssemblyLoadContext unloads after Stop");
        }
        Check(entity.Transform.Position.X > 0.24f, "project behaviours mutate actual entity and restart");
        using (File.Open(path, FileMode.Open, FileAccess.ReadWrite, FileShare.None)) { }
        bool rejected = false;
        try { project.Start(path, ["not.allowed.type"], fields); } catch (InvalidOperationException exception) { rejected = exception.Message == "Script.ProjectTypeMissing"; }
        Check(rejected && !project.IsRunning, "project script whitelist enforced");
        var multi=JsonNode.Parse(fields)!;
        multi["Scripts"]!.AsArray().Add(new JsonObject{["Type"]="sample.project_rotator",["Entity"]=entity.StableId.ToString(),["Enabled"]=true,["Fields"]=new JsonObject()});
        var multiJson=multi.ToJsonString();var types=ScriptBindings.ScriptTypes(multiJson);
        Check(types.Count==2,"project bindings enumerate all requested script types");
        var rotationBefore=entity.Transform.Rotation;
        project.Start(path,types,multiJson);session.Step(1.0/60.0);project.Stop();
        Check(entity.Transform.Rotation!=rotationBefore,"multiple project script types execute from one bindings document");
    }
    if (args.Contains("--window-smoke"))
    {
        using var session = new EngineSession();
        using var scripts = new ScriptSystem(session, new ScriptRegistry());
        var mover = scripts.Attach<MoveBehaviour>(session.CreateEntity("window mover")); scripts.Start();
        var fading=session.Ui.Create(UiKind.Label);fading.SetText("test");fading.FadeTo(0,1);
        double elapsed=0;session.Update+=dt=>elapsed+=dt;
        session.RunWindow("en-US", false, 3);
        Check(session.StepCount == 3 && mover.Updates == 3 && mover.Entity.Transform.Position.X > 0, "window host drives the same native session and managed scripts");
        var opacity=session.Ui.Visuals().Single(value=>value.Id==fading.Id).Opacity;
        Check(Math.Abs(opacity-Math.Max(0,1-elapsed))<0.0001,"window UI animation advances once per actual callback delta");
        var disposingWindow = new EngineSession();
        disposingWindow.Update += _ => disposingWindow.Dispose();
        disposingWindow.RunWindow("en-US", false, 3);
        Check(disposingWindow.IsDisposed, "window callback Dispose requests close and releases safely");
        using var editorSession = new EngineSession();
        var editEntity = editorSession.CreateEntity("editor mover");var stableId=editEntity.StableId;
        var victimId=editorSession.CreateEntity("edit victim").StableId;
        var editLabel=editorSession.Ui.Create(UiKind.Label);editLabel.SetText("edit UI remains");var labelId=editLabel.Id;
        var assembly = Path.Combine(AppContext.BaseDirectory,"ProjectScripts","SelfEngine.ProjectScripts.dll");
        var fields=new JsonObject{["Version"]=1,["Scripts"]=new JsonArray(new JsonObject{["Type"]="sample.project_mover",["Entity"]=stableId.ToString(),["Enabled"]=true,["Fields"]=new JsonObject()})}.ToJsonString();
        var editorRoot=Path.Combine(AppContext.BaseDirectory,"EditorSmokeProject");Directory.CreateDirectory(Path.Combine(editorRoot,"Assets"));
        using var project=new ScriptProject(editorSession);
        float playedX=0;int starts=0,stops=0;
        editorSession.EditorPlayChanged+=playing=>
        {
            if(playing)
            {
                ++starts;var schema=JsonNode.Parse(editorSession.InspectScriptBindings(stableId))!;
                Check(schema["Scripts"]!.AsArray().Count==1,"native Inspector queries managed metadata");
                bool readOnly=false;try{editorSession.EditScriptBindings(stableId,"{\"Save\":true}");}catch(EngineException exception){readOnly=exception.Key=="Bridge.EditorReadOnly";}
                Check(readOnly,"native Inspector prevents runtime edits");
            }
            else{++stops;playedX=editorSession.FindEntity(stableId)!.Transform.Position.X;}
        };
        EditorPlayProbe? probe=null;ScriptSystem? heldSystem=null;
        editorSession.Update+=_=>
        {
            if(!project.IsRunning)return;
            heldSystem=project.Scripts;
            probe??=heldSystem.Attach<EditorPlayProbe>(editorSession.FindEntity(stableId)!,value=>{value.Victim=new(victimId);value.LabelId=labelId;});
            heldSystem.Events.Publish(1);
        };
        for(int cycle=0;cycle<12;++cycle)
        {
            probe=null;
            RunEditorCycle(project,editorRoot,assembly,fields);
            Check(playedX>0 && editorSession.FindEntity(stableId)!.Transform.Position.X==0,"editor scripts use PlayWorld and leave EditWorld unchanged");
            Check(probe is { Created:1,Destroyed:1,AliveDuringDestroy:true,Received:2 }&&heldSystem is { Count:0,IsRunning:false }&&heldSystem.Events.Count==0,$"Play callback attachment starts next FrameBegin, receives exactly two later-frame events and fully stops: create={probe?.Created}, destroy={probe?.Destroyed}, alive={probe?.AliveDuringDestroy}, events={probe?.Received}, remaining={heldSystem?.Count}/{heldSystem?.Events.Count}");
            var completed=probe??throw new InvalidOperationException("missing Play probe");
            Check(editorSession.FindEntity(victimId) is { IsAlive:true }&&!completed.Spawned!.IsAlive,"runtime deletion/spawn never writes back to edit world");
            Check(editorSession.Ui.Visuals().Count==1&&editorSession.Ui.Visuals().Single().Text=="edit UI remains","runtime UI creation and edits never write back");
            bool oldBody=false,oldUi=false;
            try{completed.Body!.SetVelocity(Vector3.Zero);}catch(EngineException exception){oldBody=exception.Key=="Bridge.InvalidBody";}
            try{completed.Button!.SetText("stale");}catch(EngineException exception){oldUi=exception.Key=="Bridge.InvalidUi";}
            Check(oldBody&&oldUi,"Stop invalidates runtime physics and UI proxies");
            var weak=project.LastUnloadedContext!;
            for(int attempt=0;weak.IsAlive&&attempt<10;++attempt){GC.Collect();GC.WaitForPendingFinalizers();GC.Collect();}
            Check(!weak.IsAlive,"editor Stop unloads project scripts before PlayWorld destruction");
        }
        Check(starts==12&&stops==12&&editorSession.StepCount==36,"editor repeated Play lifecycle and real update loop");
        Action<bool> failingStop=playing=>{if(!playing)throw new InvalidOperationException("expected editor stop observer failure");};
        editorSession.EditorPlayChanged+=failingStop;
        bool stopFailed=false;
        probe=null;try{RunEditorCycle(project,editorRoot,assembly,fields);}catch(EngineException exception){stopFailed=exception.Result==5;}
        editorSession.EditorPlayChanged-=failingStop;
        Check(stopFailed&&!project.IsRunning&&!editorSession.IsWindowRunning&&editorSession.FindEntity(stableId)!.Transform.Position.X==0,"editor callback failure still stops scripts and restores edit ownership");
    }
    using (var session = new EngineSession())
    {
        var floor = session.CreateEntity("character floor"); var t = Transform.Identity; t.Position.Y = -0.5f; floor.Transform = t;
        var desc = BoxBodyDescription.Static; desc.HalfExtents = new(10,0.5f,10); session.Physics.CreateBox(floor, desc);
        var player = session.CreateEntity("character"); t.Position.Y = 2; player.Transform = t;
        using var scripts = new ScriptSystem(session, new ScriptRegistry());
        var behaviour = scripts.Attach<CharacterBehaviour>(player); scripts.Start();
        for (int i = 0; i < 120; ++i) session.Step(1.0/60.0);
        Check(behaviour.Controller.State.Grounded && Math.Abs(player.Transform.Position.Y - 0.9f) < 0.05f, "character settles on floor and synchronizes real transform");
        var before = player.Transform.Position;
        behaviour.Direction = Vector3.UnitX; behaviour.Jump = true; session.Step(1.0/60.0);
        Check(player.Transform.Position.Y > before.Y && player.Transform.Position.X > before.X, "character jump and walking through fixed update");
        behaviour.Controller.Teleport(new(2,2,0)); Check(player.Transform.Position == new Vector3(2,2,0), "character teleport");
        var controller = behaviour.Controller; scripts.Destroy(player);
        bool rejected = false; try { controller.Move(Vector3.Zero,false,1.0/60.0); } catch (EngineException exception) { rejected = exception.Key == "Bridge.InvalidCharacter"; }
        Check(rejected, "character invalidated with entity");
    }
    Console.WriteLine("ManagedTests PASS: ABI, GC, exceptions, entities, lifecycle, input, events, fields, physics, character, UI, project load/unload" + (args.Contains("--window-smoke") ? ", window/editor Play" : ""));
    return 0;
}
catch (Exception exception) { Console.Error.WriteLine("ManagedTests FAIL: " + exception); return 1; }

[System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
static void MakeOrphanSession() { _ = new EngineSession(); }

[System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
static void RunProjectCycle(ScriptProject project, string path, string fields, EngineSession session)
{
    project.Start(path, ["sample.project_mover"], fields);
    bool rejected = false;
    try { project.Start(path, ["sample.project_mover"], fields); } catch (InvalidOperationException exception) { rejected = exception.Message == "Script.ProjectAlreadyRunning"; }
    Check(rejected, "running project cannot hot replace assembly");
    session.Step(1.0/60.0);
    project.Stop();
}
[System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
static void RunEditorCycle(ScriptProject project,string root,string assembly,string fields)
    => project.RunEditor(root,assembly,["sample.project_mover"],fields,"en-US",false,3,true);

[ScriptId("tests.move")]
public sealed class MoveBehaviour : Behaviour
{
    [Expose] public float Speed = 1;
    public int Created, EnabledCalls, DisabledCalls, Updates, Destroyed;
    public override void OnCreate() => ++Created;
    public override void OnEnable() => ++EnabledCalls;
    public override void OnDisable() => ++DisabledCalls;
    public override void OnUpdate(double dt) { ++Updates; var t = Entity.Transform; t.Position.X += Speed*(float)dt; Entity.Transform = t; }
    public override void OnDestroy() => ++Destroyed;
}
[ScriptId("tests.fail")]
public sealed class FailBehaviour : Behaviour
{
    public int Updates, Destroyed;
    public override void OnUpdate(double _) { ++Updates; throw new InvalidOperationException("expected script fault"); }
    public override void OnDestroy() => ++Destroyed;
}
[ScriptId("tests.self")]
public sealed class SelfDestroyBehaviour : Behaviour
{
    public int Destroyed;
    public override void OnUpdate(double _) => DestroySelf();
    public override void OnDestroy() => ++Destroyed;
}
[ScriptId("tests.listen")]
public sealed class ListenBehaviour : Behaviour
{
    public int Received, Destroyed;
    public override void OnCreate() => Subscribe<int>(value => Received += value);
    public override void OnDestroy() => ++Destroyed;
}
[ScriptId("tests.input")]
public sealed class InputBehaviour : Behaviour
{
    public int UpdatePresses, FixedPresses, FixedCalls;
    public int UpdateClicks, FixedClicks;
    public float FixedWheel;
    public override void OnUpdate(double _) { if (Input.Pressed(87)) ++UpdatePresses; if(Input.Pointer.Pressed(MouseButton.Left)) ++UpdateClicks; }
    public override void OnFixedUpdate(double _) { ++FixedCalls; if (Input.Pressed(87)) ++FixedPresses; if(Input.Pointer.Pressed(MouseButton.Left)) ++FixedClicks; FixedWheel += Input.Pointer.Wheel; }
}
[ScriptId("tests.fields")]
public sealed class FieldsBehaviour : Behaviour
{
    [Expose] public int Count;
    [Expose] public float Speed;
    [Expose] public bool EnabledFlag;
    [Expose] public string Label = "";
    [Expose] public Vector3 Direction;
    [Expose] public EntityReference Target;
    public static FieldsBehaviour? LastCreated;
    public override void OnCreate() => LastCreated = this;
}
[ScriptId("tests.contact")]
public sealed class ContactDestroyBehaviour : Behaviour
{
    public int Contacts;
    public override void OnCreate() => Subscribe<PhysicsContact>(contact =>
    {
        if (contact.Began && (contact.EntityA.StableId == Entity.StableId || contact.EntityB.StableId == Entity.StableId))
        { ++Contacts; DestroySelf(); }
    });
}
[ScriptId("tests.ui")]
public sealed class UiListenerBehaviour : Behaviour
{
    public UiElement Button = null!;
    public int Clicks;
    public override void OnCreate() => Button.Subscribe(this, UiEventKind.Click, _ => ++Clicks);
}
[ScriptId("tests.character")]
public sealed class CharacterBehaviour : Behaviour
{
    public CharacterController Controller = null!;
    public Vector3 Direction;
    public bool Jump;
    public override void OnCreate() => Controller = new(Entity, CharacterDescription.Default);
    public override void OnFixedUpdate(double seconds) { Controller.Move(Direction,Jump,seconds); Jump = false; }
    public override void OnDestroy() { if (Entity.IsAlive) Controller.Destroy(); }
}

[ScriptId("tests.editor_play_probe")]
public sealed class EditorPlayProbe : Behaviour
{
    public EntityReference Victim;
    public ulong LabelId;
    public Entity? Spawned;
    public RigidBody? Body;
    public UiElement? Button;
    public int Created,Destroyed,Received;
    public bool AliveDuringDestroy;
    private bool changed;
    public override void OnCreate()
    {
        ++Created;Subscribe<int>(value=>Received+=value);
        Button=Entity.Session.Ui.Create(UiKind.Button);Button.SetText("runtime button");Button.Subscribe(this,UiEventKind.Click,_=>++Received);
        Entity.Session.Ui.Find(LabelId)!.SetText("runtime label");
        Scripts.Spawn("runtime only",entity=>{Spawned=entity;Body=Entity.Session.Physics.CreateBox(entity,BoxBodyDescription.Dynamic);});
    }
    public override void OnUpdate(double seconds)
    {if(!changed){changed=true;if(Victim.Resolve(Entity.Session) is { } victim)Scripts.Destroy(victim);}}
    public override void OnDestroy()
    {++Destroyed;AliveDuringDestroy=Entity.IsAlive&&Spawned is { IsAlive:true };Body!.SetVelocity(Vector3.Zero);}
}

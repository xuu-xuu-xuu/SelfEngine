using SelfEngine;
using System.Text.Json.Nodes;

string failureText="Bootstrap.Failed"; // 语言表尚不可用时的启动回退。
BootstrapDiagnostics? diagnostics=null;
try
{
    string? Option(string name)
    {
        var index = Array.IndexOf(args, name);
        return index >= 0 && index + 1 < args.Length ? args[index + 1] : null;
    }
    using var previewLease=PreviewLease.Acquire(Option("--preview-lease"));
    int frames = int.TryParse(Option("--frames"), out var requested) ? Math.Clamp(requested,1,100000) : 120;
    var projectRoot=Path.GetFullPath(Option("--project-root")??Environment.CurrentDirectory);
    diagnostics=new BootstrapDiagnostics(projectRoot);
    if(Option("--verify-runtime") is { } runtimeReport)
    {
        diagnostics.Write("Managed.RuntimeCheck.Begin");
        RuntimePreflight.Verify(runtimeReport);
        diagnostics.Write("Managed.RuntimeCheck.End");
        return 0;
    }
    string ProjectFile(string path)=>Path.IsPathRooted(path)?path:Path.Combine(projectRoot,path);
    string assembly = Option("--project") is { } assemblyOption?ProjectFile(assemblyOption):Path.Combine(AppContext.BaseDirectory,"ProjectScripts","SelfEngine.ProjectScripts.dll");
    string script = Option("--script") ?? "sample.project_mover";
    if(Option("--inspect-scripts") is { } inspectPath)
    {
        var types=ScriptProject.DiscoverTypes(assembly);
        var output=ProjectFile(inspectPath);Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(output))!);
        File.WriteAllText(output,System.Text.Json.JsonSerializer.Serialize(new{schemaVersion=1,types}),new System.Text.UTF8Encoding(false));
        return 0;
    }
    bool editorMode=args.Contains("--editor");
    diagnostics.Write("Managed.Session.Create.Begin");
    using var session = new EngineSession();
    diagnostics.Write("Managed.Session.Create.End");
    session.LoadLocale(Option("--locale") ?? "zh-CN");
    failureText=session.Text("Bootstrap.Failed");
    if(Option("--asset-root") is { } assetRoot)session.SetAssetRoot(ProjectFile(assetRoot),Option("--asset-cache") is { } cacheRoot?ProjectFile(cacheRoot):Path.Combine(projectRoot,"out","Cache","ManagedAssets"));
    else if(Option("--asset-cache") is not null)throw new ArgumentException("Bridge.InvalidArgument");
    bool validateBindings=args.Contains("--validate-bindings");
    if(validateBindings&&(args.Contains("--window")||args.Contains("--editor")||Option("--capture") is not null))throw new ArgumentException("Bridge.InvalidArgument");
    bool physicsDemo=args.Contains("--physics-demo");
    bool menuDemo=args.Contains("--menu-demo");if(physicsDemo&&menuDemo)throw new ArgumentException("Bootstrap.ModeConflict");
    var scenePath = Option("--scene")??(menuDemo?"Resources/Samples/Scenes/MenuFlow.scene.json":physicsDemo?"Resources/Samples/Scenes/PhysicsPlayground.scene.json":null);
    if(editorMode)
    {
        // 独立候选的可执行文件不在项目/引擎根；原生编辑器按已导入的实际文件建立干净文档。
        Environment.SetEnvironmentVariable("SELFENGINE_EDITOR_SCENE_FILE",scenePath is null?null:Path.GetFullPath(ProjectFile(scenePath)));
        Environment.SetEnvironmentVariable("SELFENGINE_EDITOR_UI_FILE",Option("--ui") is { } editorUi?Path.GetFullPath(ProjectFile(editorUi)):null);
    }
    if (scenePath is not null) session.ImportScene(File.ReadAllText(ProjectFile(scenePath)));
    if(Option("--ui") is { } uiPath)session.Ui.Load(File.ReadAllText(ProjectFile(uiPath)));
    var fieldsPath = Option("--bindings") is { } fieldsOption?ProjectFile(fieldsOption):menuDemo?ProjectFile("Resources/Samples/Scripts/MenuFlow.bindings.json"):physicsDemo?ProjectFile("Resources/Samples/Scripts/PhysicsPlayground.bindings.json"):null;
    if(Option("--capture") is { } capture)
    {
        if(!args.Contains("--window")||args.Contains("--editor"))throw new ArgumentException("Bridge.InvalidArgument");
        ulong captureFrame=(ulong)frames;if(Option("--capture-frame") is { } frameOption&&(!ulong.TryParse(frameOption,out captureFrame)||captureFrame==0))throw new ArgumentException("Bridge.InvalidArgument");
        var path=ProjectFile(capture);Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);session.SetWindowCapture(path,captureFrame);
    }
    else if(Option("--capture-frame") is not null)throw new ArgumentException("Bridge.InvalidArgument");
    var entity = fieldsPath is null&&!editorMode?session.CreateEntity("ManagedMover"):null;
    var defaultFields=new JsonObject();
    if(script=="sample.project_mover")defaultFields["Speed"]=new JsonObject{["Kind"]="f32",["Value"]=2};
    var bindings = fieldsPath is not null ? File.ReadAllText(fieldsPath) : editorMode?"{\"Version\":1,\"Scripts\":[]}":new JsonObject
    {
        ["Version"] = 1,
        ["Scripts"] = new JsonArray(new JsonObject { ["Type"] = script, ["Entity"] = entity!.StableId.ToString(), ["Enabled"] = true,
            ["Fields"] = defaultFields })
    }.ToJsonString();
    using var project = new ScriptProject(session);
    bool scriptFailed=false;
    project.Diagnostic += diagnostic =>
    {
        diagnostics.Write("Managed.ScriptDiagnostic", $"key={diagnostic.Key} phase={diagnostic.Phase} {diagnostic.Detail}");
        Console.Error.WriteLine($"{session.Text(diagnostic.Key)}: entity={diagnostic.EntityHandle} type={diagnostic.ScriptType} phase={diagnostic.Phase} {diagnostic.Detail}");
        if(diagnostic.Key!="Script.FixedStepBudget")scriptFailed=true;
    };
    var allowed = (editorMode?ScriptProject.DiscoverTypes(assembly):ScriptBindings.ScriptTypes(bindings)).ToList();
    if (args.Contains("--menu")&&!menuDemo)
    {
        var menuEntity = session.CreateEntity("Menu");
        var document = JsonNode.Parse(bindings)!;
        document["Scripts"]!.AsArray().Add(new JsonObject { ["Type"] = "sample.menu_flow", ["Entity"] = menuEntity.StableId.ToString(), ["Enabled"] = true, ["Fields"] = new JsonObject() });
        bindings = document.ToJsonString(); allowed.Add("sample.menu_flow");
    }
    var stableId=entity?.StableId??0;
    if(validateBindings)
    {
        project.Validate(assembly,allowed,bindings);
        Console.WriteLine(session.Text("Bootstrap.BindingsValidated"));return scriptFailed?1:0;
    }
    session.EditorPlayChanged+=playing=>diagnostics.Write(playing?"Managed.Play.Callback":"Managed.Stop.Callback");
    diagnostics.Write(editorMode?"Managed.Editor.Begin":"Managed.Play.Begin");
    if(args.Contains("--editor"))project.RunEditor(projectRoot,assembly,allowed,bindings,Option("--locale")??"zh-CN",!args.Contains("--hidden"),args.Contains("--frames")?(ulong)frames:0,args.Contains("--play"),fieldsPath);
    else
    {
        project.Start(assembly, allowed, bindings);
        if (args.Contains("--window")) session.RunWindow(Option("--locale") ?? "zh-CN", !args.Contains("--hidden"), args.Contains("--frames") ? (ulong)frames : 0);
        else for (int frame = 0; frame < frames; ++frame) session.Step(1.0/60.0);
    }
    diagnostics.Write(editorMode?"Managed.Editor.End":"Managed.Play.End", $"steps={session.StepCount}");
    diagnostics.Write("Managed.Scripts.Stop.Begin");
    project.Stop();
    diagnostics.Write("Managed.Scripts.Stop.End");
    Console.WriteLine($"{session.Text("Bootstrap.Completed")}: steps={session.StepCount}, position={session.FindEntity(stableId)?.Transform.Position}");
    return scriptFailed?1:0;
}
catch (Exception exception)
{
    diagnostics?.Write("Managed.Failed", exception.ToString());
    Console.Error.WriteLine($"{failureText}: {exception}");
    return 1;
}
finally { diagnostics?.Dispose(); }

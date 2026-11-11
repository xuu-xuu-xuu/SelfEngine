using System.Reflection;
using System.Runtime.Loader;

namespace SelfEngine;

// 只在停止状态装载下一次编译结果；流加载避免占用脚本 DLL，ALC 隔离项目依赖。
public sealed class ScriptProject : IDisposable
{
    internal sealed class ProjectContext(string assemblyPath) : AssemblyLoadContext(isCollectible: true)
    {
        private readonly AssemblyDependencyResolver resolver = new(assemblyPath);
        protected override Assembly? Load(AssemblyName name)
        {
            if (name.Name == typeof(Behaviour).Assembly.GetName().Name) return typeof(Behaviour).Assembly;
            var path = resolver.ResolveAssemblyToPath(name);
            if (path is null) return null;
            using var stream = File.OpenRead(path);
            return LoadFromStream(stream);
        }
        protected override nint LoadUnmanagedDll(string name) => throw new InvalidOperationException("Script.ProjectNativeDependencyDenied");
    }
    private readonly EngineSession session;
    private ProjectContext? context;
    private ScriptSystem? scripts;
    private ScriptRegistry? registry;
    private bool disposed;
    public bool IsRunning => scripts?.IsRunning ?? false;
    public string LastBindings { get; private set; } = "";
    public WeakReference? LastUnloadedContext { get; private set; }
    public ScriptSystem Scripts => scripts ?? throw new InvalidOperationException("Script.ProjectNotRunning");
    public event Action<ScriptDiagnostic>? Diagnostic;
    public string EditedBindings { get; private set; } = "";
    public ScriptProject(EngineSession session) => this.session = session;

    // 候选检查与正式 Inspector 使用同一类型目录；尚未绑定到实体的脚本也必须可添加。
    // 只返回字符串，不把项目 Type/Assembly 留在宿主中，也不调用任何脚本生命周期。
    public static IReadOnlyList<string> DiscoverTypes(string assemblyPath)
    {
        assemblyPath=Path.GetFullPath(assemblyPath);
        if(!File.Exists(assemblyPath))throw new FileNotFoundException("Script.ProjectAssemblyMissing",assemblyPath);
        var context=new ProjectContext(assemblyPath);
        var registry=new ScriptRegistry();
        try
        {
            using var stream=File.OpenRead(assemblyPath);
            foreach(var type in context.LoadFromStream(stream).GetTypes().Where(type=>type.IsPublic&&!type.IsAbstract&&typeof(Behaviour).IsAssignableFrom(type)))
                registry.Register(type);
            return registry.Types.Keys.Order(StringComparer.Ordinal).ToArray();
        }
        finally{registry.Clear();context.Unload();}
    }

    public void Start(string assemblyPath, IEnumerable<string> allowedScriptIds, string bindingsJson)
        =>Load(assemblyPath,allowedScriptIds,bindingsJson,true);

    // 导出前只装载类型、实例化字段候选并校验引用；不运行 OnCreate/OnEnable/OnDestroy。
    public string Validate(string assemblyPath,IEnumerable<string> allowedScriptIds,string bindingsJson)
    {
        ObjectDisposedException.ThrowIf(disposed,this);
        if(context is not null)throw new InvalidOperationException("Script.ProjectAlreadyRunning");
        bool invalid=false;void Record(ScriptDiagnostic _)=>invalid=true;
        Diagnostic+=Record;
        try
        {
            Load(assemblyPath,allowedScriptIds,bindingsJson,false);
            if(invalid)throw new InvalidOperationException("Script.PreflightFailed");
            return ScriptFields.Save(scripts!);
        }
        finally{Diagnostic-=Record;Release();}
    }
    private void Load(string assemblyPath,IEnumerable<string> allowedScriptIds,string bindingsJson,bool start)
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        if (context is not null) throw new InvalidOperationException("Script.ProjectAlreadyRunning");
        var allow = new HashSet<string>(allowedScriptIds, StringComparer.Ordinal);
        if (allow.Count == 0 || allow.Any(string.IsNullOrWhiteSpace)) throw new ArgumentException("Script.ProjectWhitelistEmpty");
        assemblyPath = Path.GetFullPath(assemblyPath);
        if (!File.Exists(assemblyPath)) throw new FileNotFoundException("Script.ProjectAssemblyMissing", assemblyPath);
        context = new ProjectContext(assemblyPath);
        registry = new ScriptRegistry();
        try
        {
            using var stream = File.OpenRead(assemblyPath);
            var assembly = context.LoadFromStream(stream);
            foreach (var type in assembly.GetTypes().Where(type => type.IsPublic && !type.IsAbstract && typeof(Behaviour).IsAssignableFrom(type)))
            {
                var id = ScriptRegistry.IdOf(type);
                if (allow.Contains(id)) registry.Register(type);
            }
            if (allow.Any(id => !registry.Types.ContainsKey(id))) throw new InvalidOperationException("Script.ProjectTypeMissing");
            scripts = new ScriptSystem(session, registry);
            scripts.Diagnostic += ForwardDiagnostic;
            if (!ScriptFields.Load(scripts, bindingsJson)) throw new InvalidOperationException("Script.DocumentInvalid");
            if(start)scripts.Start();
        }
        catch { Release(); throw; }
    }
    public void Stop()
    {
        if (context is null) return;
        try { if (scripts is not null) LastBindings = ScriptFields.Save(scripts); }
        finally { Release(); }
    }
    public void RunEditor(string projectRoot,string assemblyPath,IEnumerable<string> allowedScriptIds,string bindingsJson,
        string locale="zh-CN",bool visible=true,ulong maximumFrames=0,bool startPlaying=false,string? bindingsPath=null)
    {
        ObjectDisposedException.ThrowIf(disposed,this);
        if(context is not null)throw new InvalidOperationException("Script.ProjectAlreadyRunning");
        var allowed=allowedScriptIds.ToArray();
        var bindings=new ScriptBindings(assemblyPath,allowed,bindingsJson);
        bindings.ConfigureDocument(projectRoot,bindingsPath??Path.Combine(projectRoot,"Resources","Scripts","Bindings.json"));
        var previousData=session.EditorDataRequested;
        session.EditorDataRequested=(entity,request)=>
        {
            if(request.Length==0)return bindings.Describe(entity,scripts,id=>session.FindEntity(id) is not null);
            var command=System.Text.Json.Nodes.JsonNode.Parse(request)?.AsObject()??throw new System.Text.Json.JsonException("Script.DocumentInvalid");
            if(command.ContainsKey("Document"))
            {
                var response=bindings.ProcessDocumentRequest(command,IsRunning);
                EditedBindings=bindings.Json;return response;
            }
            if(IsRunning)throw new InvalidOperationException("Script.LoadWhileRunning");
            if(command["Save"]?.GetValue<bool>()==true)bindings.SaveDocument();
            else bindings.Apply(entity,request);
            EditedBindings=bindings.Json;return "{}";
        };
        void Changed(bool playing)
        {
            // 没有任何脚本附件的编辑场景仍能运行原生模拟；不需要空白名单的 ScriptSystem。
            if(playing){if(ScriptBindings.ScriptTypes(bindings.Json).Count!=0)Start(assemblyPath,allowed,bindings.Json);}
            else Stop();
        }
        session.EditorPlayChanged+=Changed;
        try{session.RunEditor(projectRoot,locale,visible,maximumFrames,startPlaying);}
        finally{session.EditorPlayChanged-=Changed;session.EditorDataRequested=previousData;EditedBindings=bindings.Json;Stop();}
    }
    private void ForwardDiagnostic(ScriptDiagnostic diagnostic)
    {
        try { Diagnostic?.Invoke(diagnostic); } catch { }
    }
    private void Release()
    {
        var releasing=scripts;scripts=null;
        try {if(releasing is not null){releasing.Diagnostic-=ForwardDiagnostic;releasing.Dispose();}}
        finally
        {
            registry?.Clear();registry=null;
            var unloading=context;context=null;
            if(unloading is not null){LastUnloadedContext=new WeakReference(unloading);unloading.Unload();}
        }
    }
    public void Dispose() { if (disposed) return; try{Stop();}finally{disposed=true;Diagnostic=null;} }
}

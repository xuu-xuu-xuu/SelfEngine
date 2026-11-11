using SelfEngine;
using System.Text.Json;

internal static class SceneContractTests
{
    internal static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    private static string Document(ulong id,float x=0)=>JsonSerializer.Serialize(new
    {
        format="SelfEngine.Scene",version=2,entities=new[]{new
        {
            id=id.ToString(),name="scene actor",enabled=true,parent=(string?)null,
            components=new[]{new {type="1",version=1,enabled=true,properties=new{position=new[]{x,0,0},rotation=new[]{0,0,0,1},scale=new[]{1,1,1}}}}
        }}
    });
    public static void Run()
    {
        using var session=new EngineSession();using var scripts=new ScriptSystem(session,new ScriptRegistry());
        var diagnostics=new List<ScriptDiagnostic>();scripts.Diagnostic+=diagnostics.Add;
        var a=session.Scenes.Create("A");a.RequestJson(Document(101));session.Step(0);
        var original=session.FindEntity(101)!;var reference=EntityReference.From(original);
        var old=scripts.Attach<SceneLifecycleProbe>(original);scripts.Start();session.Step(0);
        var overlay=session.Scenes.Create("Overlay");overlay.RequestJson(Document(201),SceneLoadMode.Additive);session.Step(0);
        var observer=scripts.Attach<SceneLifecycleProbe>(session.FindEntity(201)!,value=>value.Target=reference);
        var born=scripts.Attach<SceneBirthProbe>(overlay.CreateEntity("birth source"));
        session.Step(0);session.Step(0);
        Check(born.Direct is not null&&session.Scenes.Of(born.Direct)!.Handle==overlay.Handle&&born.Deferred is not null&&session.Scenes.Of(born.Deferred)!.Handle==overlay.Handle,"OnCreate and deferred Spawn inherit additive scene");
        var oldScope=a.Scope;var observerHandle=observer.Entity.Handle;
        var oldBody=session.Physics.CreateBox(original,BoxBodyDescription.Dynamic);old.Body=oldBody;
        a.RequestJson(Document(101,7));session.Step(0);
        var replacement=session.FindEntity(101)!;
        Check(!original.IsAlive&&replacement.Handle!=original.Handle&&a.Scope!=oldScope&&replacement.Transform.Position.X==7,"same-ID replacement rotates runtime entity and scene scope");
        Check(old.Destroyed==1&&old.Disabled==1&&observer.Entity.Handle==observerHandle&&observer.Target.Resolve(session)!.Handle==replacement.Handle,"retirement occurs once and preserved references resolve replacement");
        bool invalidBody=false;try{_=oldBody.State;}catch(EngineException){invalidBody=true;}
        Check(invalidBody,"old physical handle never binds replacement");oldBody.Destroy();
        var current=scripts.Attach<SceneLifecycleProbe>(replacement);session.Step(0);
        a.RequestJson(Document(101,9));a.Cancel();session.Step(0);
        Check(a.Snapshot.State==SceneState.Cancelled&&a.Snapshot.Active&&replacement.IsAlive&&replacement.Transform.Position.X==7,"cancel preserves active members");
        var conflict=session.Scenes.Create("conflict");conflict.RequestJson(Document(201),SceneLoadMode.Additive);session.Step(0);
        Check(conflict.Snapshot.State==SceneState.Failed&&observer.Entity.IsAlive&&replacement.IsAlive,"additive stable ID conflict is atomic");
        var facts=new List<SceneUnloaded>();using var fact=scripts.Events.Subscribe<SceneUnloaded>(observer,facts.Add);
        using var unload=scripts.Events.Subscribe<int>(current,_=>a.Unload());
        int afterUnload=0;using var late=scripts.Events.Subscribe<int>(current,_=>++afterUnload);
        scripts.Events.Publish(1,scene:a.Scope);session.Step(0);
        Check(afterUnload==0&&current.Destroyed==1&&current.Disabled==1&&!current.Enabled&&observer.Target.StableId==0,"callback unload stops same-batch delivery and clears exposed references");
        var updates=current.Updates;session.Step(0);
        Check(!replacement.IsAlive&&current.Updates==updates&&a.Snapshot.State==SceneState.Unloaded&&facts.Count==1,"safe boundary destroys scene and publishes one fact");
        var root=Path.Combine(AppContext.BaseDirectory,"Validation","E05",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);
        var path=Path.Combine(root,"中文场景.json");File.WriteAllText(path,Document(301));
        var disk=session.Scenes.Create("Disk");disk.RequestLoad(path,SceneLoadMode.Additive);
        for(int i=0;i<2000&&disk.Snapshot.State!=SceneState.ReadyToActivate;++i){session.Step(0);Thread.Sleep(1);}
        Check(disk.Snapshot.State==SceneState.ReadyToActivate&&!disk.Snapshot.Active,"C# sees received candidate before activation");session.Step(0);
        Check(disk.Snapshot.Active&&session.FindEntity(301)!.IsAlive,"Unicode file activates next frame");
        var saved=disk.Snapshot.Members.ToArray();disk.RequestLoad(path+".missing");
        for(int i=0;i<2000&&disk.Snapshot.State!=SceneState.Failed;++i){session.Step(0);Thread.Sleep(1);}
        Check(disk.Snapshot.State==SceneState.Failed&&disk.Snapshot.Active&&disk.Snapshot.Members.SequenceEqual(saved),"missing file preserves current scene");
        using(var other=new EngineSession())
        {bool foreign=false;try{_=other.Scenes.Get(disk.Handle);}catch(EngineException){foreign=true;}Check(foreign,"scene handles reject foreign session");}
        for(int i=0;i<16;++i)
        {
            using var closing=new EngineSession();var pending=closing.Scenes.Create("pending");pending.RequestLoad(path);closing.Dispose();
            Check(closing.IsDisposed,"pending load close joins only owned work");
        }
        scripts.Stop();Check(scripts.Count==0&&session.NativeScriptComponentCount==0&&diagnostics.Count==0&&session.Scenes.Statistics.CallbackFaults==0,"retirement and Stop clean all script/physical callbacks without faults");
        File.WriteAllText(Path.Combine(root,"managed-scenes.json"),JsonSerializer.Serialize(new{success=true,sameIdReload=true,additiveScope=true,deferredBirth=true,referenceRebound=true,referenceCleared=true,oldBodyInvalid=true,callbackUnload=true,factOnce=true,cancelPreserves=true,conflictAtomic=true,receivedBeforeCommit=true,unicodeFile=true,missingPreserves=true,foreignRejected=true,pendingCloseCycles=16,noCallbackFaults=true}));
        Console.WriteLine("SceneContractTests passed (managed scene lifetime, references, callbacks, physical proxies and pending close).");
    }
}
[ScriptId("test.e05.lifecycle")]
public sealed class SceneLifecycleProbe:Behaviour
{
    [Expose] public EntityReference Target;
    public int Destroyed,Disabled,Updates;public RigidBody? Body;
    public override void OnUpdate(double seconds)=>++Updates;
    public override void OnDisable()=>++Disabled;
    public override void OnDestroy(){++Destroyed;Body?.Destroy();}
}
[ScriptId("test.e05.birth")]
public sealed class SceneBirthProbe:Behaviour
{
    public Entity? Direct,Deferred;private bool spawned;
    public override void OnCreate()=>Direct=Entity.Session.CreateEntity("direct additive child");
    public override void OnUpdate(double seconds)
    {if(spawned)return;spawned=true;Scripts.Spawn("deferred additive child",entity=>{Deferred=entity;Entity.Session.CreateEntity("configured additive child");});}
}

using System.Text.Json;
using SelfEngine;

internal static class EnvironmentContractTests
{
    private static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    public static void Run()
    {
        using var session=new EngineSession();session.ImportScene("""
            {"format":"SelfEngine.Scene","version":1,"entities":[
              {"id":"1","name":"parent","enabled":true,"parent":null,"transform":{"position":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},"components":{}},
              {"id":"2","name":"child","enabled":true,"parent":"1","transform":{"position":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},"components":{}}
            ]}
            """);
        var parent=session.FindEntity(1)!;var child=session.FindEntity(2)!;using var scripts=new ScriptSystem(session,new ScriptRegistry(),automaticFixedUpdates:false,simulatePhysics:false);var diagnostics=new List<ScriptDiagnostic>();scripts.Diagnostic+=diagnostics.Add;
        var first=scripts.Attach<EnvironmentProbe>(child);var second=scripts.Attach<EnvironmentProbe>(child);Check(session.NativeScriptComponentCount==2&&first.Created==0&&second.Created==0,"two behaviours attach independent native identities before Start");
        scripts.Start();Check(first.Created==1&&first.Enables==1&&second.Created==1&&second.Enables==1,"one managed lifecycle for each native association");scripts.Events.Publish(1);Check(first.Received==0&&second.Received==0,"publication waits for its phase");session.Step(0);Check(first.Received==1&&second.Received==1,"both instances receive their owned subscriptions");
        parent.Enabled=false;scripts.Events.Publish(1);Check(first.Received==1&&second.Received==1,"native owner state filters events immediately before Tick reconciliation");scripts.FixedUpdate(1.0/60);session.Step(1.0/60);
        Check(child.Enabled&&!parent.Enabled&&first.Disables==1&&second.Disables==1&&first.Updates==1&&first.Fixed==0,"ancestor disable synchronizes callbacks and suppresses updates");
        parent.Enabled=true;session.Step(1.0/60);scripts.FixedUpdate(1.0/60);Check(first.Enables==2&&second.Enables==2&&first.Updates==2&&first.Fixed==1,"ancestor reactivation resumes both instances");
        first.Enabled=false;Check(first.Disables==2&&session.NativeScriptComponentCount==2,"disabled behaviour keeps its association until removal");scripts.Remove(first);Check(first.Destroyed==1&&first.AliveDuringDestroy&&session.NativeScriptComponentCount==1,"remove clears native identity after managed cleanup");
        int received=second.Received;child.Destroy();Check(session.NativeScriptComponentCount==0&&!child.IsAlive,"GO destruction immediately retires native script identities");scripts.Events.Publish(1);Check(second.Received==received,"retired owner cannot receive pending events");session.Step(1.0/60);
        Check(second.Disables==2&&second.Destroyed==1&&!second.AliveDuringDestroy&&scripts.Count==0&&scripts.Events.Count==0,"dead-owner sweep finishes managed cleanup once");
        var third=scripts.Attach<EnvironmentProbe>(session.CreateEntity("after slot reuse"));Check(session.NativeScriptComponentCount==1&&third.Created==1,"native component slot can be safely reused");scripts.Stop();scripts.Stop();Check(session.NativeScriptComponentCount==0&&third.Destroyed==1&&scripts.Count==0,"repeated Stop clears every association once");Check(diagnostics.Count==0,"native activation adaptation has no swallowed errors");
        var root=Path.Combine(AppContext.BaseDirectory,"Validation","E01",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);File.WriteAllText(Path.Combine(root,"managed-environment.json"),JsonSerializer.Serialize(new{success=true,multiInstance=true,nativeCountBeforeStart=2,ownerHierarchy=true,immediateEventFilter=true,lifecycleOnce=true,disabledAssociation=true,removeCleanup=true,destroyCleanup=true,reusedSlot=true,nativeCountAfterStop=0,managedCountAfterStop=0}));Console.WriteLine("EnvironmentContractTests passed (native controlled associations, inherited enable, managed lifecycle and teardown).");
    }
}
[ScriptId("test.environment")]
public sealed class EnvironmentProbe:Behaviour
{
    public int Created,Enables,Disables,Destroyed,Updates,Fixed,Received;
    public bool AliveDuringDestroy;
    public override void OnCreate(){++Created;Subscribe<int>(value=>Received+=value);}
    public override void OnEnable()=>++Enables;
    public override void OnDisable()=>++Disables;
    public override void OnUpdate(double seconds)=>++Updates;
    public override void OnFixedUpdate(double seconds)=>++Fixed;
    public override void OnDestroy(){++Destroyed;AliveDuringDestroy=Entity.IsAlive;}
}

using System.Numerics;
using System.Runtime.CompilerServices;
using System.Text.Json;
using SelfEngine;

internal static class UiBindingContractTests
{
    private static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    private static UiElement Button(EngineSession session,UiActionProbe owner,float x=0)
    {var button=session.Ui.Create(UiKind.Button);var style=UiStyle.Default;style.Offset=new(x,20);button.SetStyle(style);button.Bind(UiEventKind.Click,owner,"fire");return button;}
    private static void Queue(EngineSession session,ulong id)
    {session.Ui.Layout(800,600);var visual=session.Ui.Visuals().Single(v=>v.Id==id);var p=new Vector2(visual.X+10,visual.Y+10);Check(session.Ui.Pointer(UiPointerOperation.Down,p)&&session.Ui.Pointer(UiPointerOperation.Up,p),"queued UI click");}
    private static void Click(EngineSession session,ulong id){Queue(session,id);session.Step(1.0/60);}
    private static void OwnerRules()
    {
        using var session=new EngineSession();using var scripts=new ScriptSystem(session,new ScriptRegistry(),simulatePhysics:false);var entity=session.CreateEntity("UI action owner");var owner=scripts.Attach<UiActionProbe>(entity);scripts.Start();var button=Button(session,owner);int manual=0;button.Subscribe(owner,UiEventKind.Click,_=>++manual);
        Check(button.Bindings.Single()==new UiBinding(UiEventKind.Click,entity.StableId,"test.ui_actions","fire"),"typed binding query");button.Bind(UiEventKind.Click,owner,"fire");Click(session,button.Id);Check(owner.Calls==1&&manual==1&&scripts.UiActions.Count==1,"rebinding does not duplicate callbacks");
        bool duplicate=false;try{scripts.UiActions.Register(owner,"fire",_=>{});}catch(InvalidOperationException e){duplicate=e.Message=="Script.UiActionDuplicate";}Check(duplicate&&scripts.UiActions.Count==1,"duplicate action rejected");
        var saved=session.Ui.Save();session.Ui.Load(saved);button=session.Ui.Find(button.Id)!;Click(session,button.Id);Check(owner.Calls==2&&manual==1,"stable binding restored while old subscription proxy stays retired");
        var oldToken=owner.Token!;scripts.Remove(owner);Check(scripts.UiActions.Count==0,"owner removal clears actions");var replacement=scripts.Attach<UiActionProbe>(entity);oldToken.Dispose();Check(scripts.UiActions.Count==1,"old token cannot remove replacement registration");Click(session,button.Id);Check(replacement.Calls==1&&owner.Calls==2,"binding resolves current owner only");
        replacement.Enabled=false;Click(session,button.Id);Check(replacement.Calls==1,"disabled owner cannot run");replacement.Enabled=true;scripts.SimulationPaused=true;Click(session,button.Id);Check(replacement.Calls==2,"UI owner explicitly updates when paused");replacement.AllowPaused=false;Click(session,button.Id);Check(replacement.Calls==2,"ordinary owner follows pause policy");replacement.AllowPaused=true;
        replacement.Hook=_=>scripts.Stop();Click(session,button.Id);Check(!scripts.IsRunning&&scripts.UiActions.Count==0&&scripts.Events.Count==0&&replacement.Destroyed==1,"Stop inside action drains cleanup safely");
    }
    private static void StaleSources(bool reload)
    {
        using var session=new EngineSession();using var scripts=new ScriptSystem(session,new ScriptRegistry(),simulatePhysics:false);var owner=scripts.Attach<UiActionProbe>(session.CreateEntity("source owner"));scripts.Start();var first=Button(session,owner);var second=Button(session,owner,160);var saved=session.Ui.Save();int secondManual=0;second.Subscribe(owner,UiEventKind.Click,_=>++secondManual);
        owner.Hook=value=>{if(value.Target==first.Id){if(reload)session.Ui.Load(saved);else second.Remove();}};Queue(session,first.Id);Queue(session,second.Id);session.Step(1.0/60);Check(owner.Calls==1&&secondManual==0,"same-batch retired UI source/epoch cannot run old callbacks");
    }
    private static void FaultAndMissing()
    {
        using var session=new EngineSession();using var scripts=new ScriptSystem(session,new ScriptRegistry(),simulatePhysics:false);var diagnostics=new List<ScriptDiagnostic>();scripts.Diagnostic+=diagnostics.Add;var owner=scripts.Attach<UiActionProbe>(session.CreateEntity("fault owner"));scripts.Start();var button=Button(session,owner);owner.Hook=_=>throw new InvalidOperationException("expected UI action failure");Click(session,button.Id);Click(session,button.Id);Check(owner.Calls==1&&!owner.Enabled&&diagnostics.Count(d=>d.Key=="Script.CallbackFailed"&&d.Phase=="UiAction")==1,"UI action exception isolated with phase");scripts.Remove(owner);Check(scripts.UiActions.Count==0,"faulted owner removed");
        var healthy=scripts.Attach<UiActionProbe>(session.CreateEntity("missing action owner"));button.SetBinding(new(UiEventKind.Click,healthy.Entity.StableId,"test.ui_actions","later"));for(int i=0;i<3;++i)Click(session,button.Id);Check(diagnostics.Count(d=>d.Key=="Script.UiBindingMissing")==1,"missing binding diagnosis is bounded");using var token=scripts.UiActions.Register(healthy,"later",_=>healthy.Calls+=2);Click(session,button.Id);Check(healthy.Calls==2,"registration can resolve a previously missing action");healthy.Entity.Destroy();session.Step(1.0/60);Check(scripts.UiActions.Count==0,"dead entity releases all owner actions");
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static (WeakReference Context,UiActionBindings HeldActions) ProjectCycle(string assembly,string scene,string bindings)
    {
        using var session=new EngineSession();session.LoadLocale("en-US");session.ImportScene(scene);using var project=new ScriptProject(session);var errors=new List<ScriptDiagnostic>();project.Diagnostic+=errors.Add;project.Start(assembly,ScriptBindings.ScriptTypes(bindings),bindings);var held=project.Scripts.UiActions;Check(held.Count==8,"project menu registers eight stable actions");session.Step(1.0/60);project.Stop();Check(held.Count==0&&session.Ui.Visuals().Count==0&&errors.Count==0,"project stop clears actions, roots and script callbacks");return(project.LastUnloadedContext!,held);
    }
    public static void Run()
    {
        OwnerRules();StaleSources(false);StaleSources(true);FaultAndMissing();
        var assembly=Path.Combine(AppContext.BaseDirectory,"ProjectScripts","SelfEngine.ProjectScripts.dll");var scene=File.ReadAllText("Resources/Samples/Scenes/MenuFlow.scene.json");var bindings=File.ReadAllText("Resources/Samples/Scripts/MenuFlow.bindings.json");var contexts=new List<WeakReference>();var heldActions=new List<UiActionBindings>();
        for(int i=0;i<16;++i){var cycle=ProjectCycle(assembly,scene,bindings);contexts.Add(cycle.Context);heldActions.Add(cycle.HeldActions);}
        for(int i=0;i<16&&contexts.Any(c=>c.IsAlive);++i){GC.Collect();GC.WaitForPendingFinalizers();GC.Collect();}Check(contexts.All(c=>!c.IsAlive)&&heldActions.All(a=>a.Count==0),"held engine action registries do not retain project ALCs");
        var root=Path.Combine(AppContext.BaseDirectory,"Validation","T63",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);File.WriteAllText(Path.Combine(root,"ui-binding.json"),JsonSerializer.Serialize(new{success=true,typedBinding=true,noDuplicate=true,ownerCleanup=true,tokenAba=true,pausePolicy=true,staleNode=true,staleEpoch=true,exceptionPhase=true,boundedMissing=true,stopInsideAction=true,unloadCycles=16,heldRegistriesReleased=true}));Console.WriteLine("UiBindingContractTests passed (typed bindings, owner/action lifetime, pause/fault, stale sources and 16 collectible projects).");GC.KeepAlive(heldActions);
    }
}
[ScriptId("test.ui_actions")]
public sealed class UiActionProbe:Behaviour
{
    public bool AllowPaused=true;public override bool UpdateWhenPaused=>AllowPaused;
    public int Calls,Destroyed;public IDisposable? Token;public Action<UiEvent>? Hook;
    public override void OnCreate()=>Token=Scripts.UiActions.Register(this,"fire",value=>{++Calls;Hook?.Invoke(value);});
    public override void OnDestroy()=>++Destroyed;
}

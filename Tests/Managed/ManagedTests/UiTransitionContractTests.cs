using System.Numerics;
using System.Text.Json;
using SelfEngine;
internal static class UiTransitionContractTests
{
    private static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    private static bool Near(float a,float b)=>Math.Abs(a-b)<.001f;
    public static void Run()
    {
        using var session=new EngineSession();var entity=session.CreateEntity("paused transition body");var transform=Transform.Identity;transform.Position=new(0,20,0);entity.Transform=transform;var body=session.Physics.CreateBox(entity,BoxBodyDescription.Dynamic);
        using var scripts=new ScriptSystem(session,new ScriptRegistry());scripts.Start();scripts.SimulationPaused=true;
        var panel=session.Ui.Create(UiKind.Panel);var style=UiStyle.Default;style.Size=new(240,160);panel.SetStyle(style);panel.SetColor(new(1,1,1,1));session.Ui.Layout(640,480);
        panel.MoveTo(new(120,80),1);panel.ColorTo(new(.2f,.4f,.8f,.6f),1);panel.FadeTo(.25f,1);
        for(int i=0;i<30;++i)session.Step(1.0/60);session.Ui.Layout(640,480);var half=session.Ui.Visuals().Single(v=>v.Id==panel.Id);
        Check(Near(half.X,60)&&Near(half.Y,40)&&Near(half.Opacity,.625f)&&body.State.Transform.Position==transform.Position,"UI time advances while real physics is paused");
        panel.MoveTo(Vector2.Zero,.5);panel.FadeTo(1,.5);session.Step(.25);session.Ui.Layout(640,480);Check(Near(session.Ui.Visuals().Single(v=>v.Id==panel.Id).X,30),"C# reverse starts at current position");
        panel.CancelTransitions();var held=session.Ui.Save();session.Step(.5);Check(session.Ui.Save()==held,"C# cancellation holds all values");panel.MoveTo(new(15,25),0);panel.ColorTo(new(.3f,.5f,.7f,1),0);panel.FadeTo(.8f,0);session.Ui.Layout(640,480);Check(session.Ui.Visuals().Single(v=>v.Id==panel.Id) is {X:15,Y:25,Opacity:.8f},"C# zero duration is immediate");
        using(var document=JsonDocument.Parse(session.Ui.Save())){var color=document.RootElement.GetProperty("nodes").EnumerateArray().Single(n=>n.GetProperty("id").GetUInt64()==panel.Id).GetProperty("color");Check(color[0].GetSingle()==.3f&&color[1].GetSingle()==.5f&&color[2].GetSingle()==.7f&&color[3].GetSingle()==1,"C# color target reaches native document");}
        bool invalidMove=false,invalidColor=false;try{panel.MoveTo(new(float.NaN,0),1);}catch(EngineException e){invalidMove=e.Key=="Bridge.InvalidUi";}try{panel.ColorTo(new(2,0,0,1),1);}catch(EngineException e){invalidColor=e.Key=="Bridge.InvalidUi";}Check(invalidMove&&invalidColor,"invalid transition arguments rejected");
        panel.MoveTo(new(50,50),1);panel.FadeTo(0,1);panel.Remove();session.Step(1);bool stale=false;try{panel.ColorTo(Vector4.One,1);}catch(EngineException e){stale=e.Key=="Bridge.InvalidUi";}Check(stale,"destroyed C# proxy cannot start transition");
        scripts.SimulationPaused=false;session.Step(1.0/60);Check(body.State.Transform.Position.Y<20,"physics resumes normally after paused UI");
        var root=Path.Combine(AppContext.BaseDirectory,"Validation","T61",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);File.WriteAllText(Path.Combine(root,"ui-transition.json"),JsonSerializer.Serialize(new{success=true,pausedUi=true,physicsHeld=true,physicsResumed=true,reverse=true,cancel=true,immediate=true,invalidMove,invalidColor,staleProxy=stale}));Console.WriteLine("UiTransitionContractTests passed (position/color/fade, cancellation, paused physics and proxy lifetime).");
    }
}

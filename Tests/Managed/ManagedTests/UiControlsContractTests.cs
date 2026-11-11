using System.Text.Json;
using SelfEngine;
internal static class UiControlsContractTests
{
    private static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    public static void Run()
    {
        using var session=new EngineSession();var slider=session.Ui.Create(UiKind.Slider);slider.SetRange(-10,90);slider.SetStep(5);slider.SetValue(73);var value=session.Ui.Visuals().Single(v=>v.Id==slider.Id);Check(value.Value==75,"C# step property relative to min");
        bool rejected=false;try{slider.SetStep(-1);}catch(EngineException e){rejected=e.Key=="Bridge.InvalidUi";}Check(rejected,"invalid step rejected");slider.Focus();session.Ui.Key(36);Check(session.Ui.Visuals().Single(v=>v.Id==slider.Id).Value==-10,"slider keyboard minimum");session.Ui.Key(35);Check(session.Ui.Visuals().Single(v=>v.Id==slider.Id).Value==90,"slider keyboard maximum");
        var document=session.Ui.Save();session.Ui.Load(document);var restored=session.Ui.Find(slider.Id)!;restored.SetValue(12);Check(session.Ui.Visuals().Single(v=>v.Id==slider.Id).Value==10,"step survives JSON load");
        var root=Path.Combine(AppContext.BaseDirectory,"Validation","T60",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);File.WriteAllText(Path.Combine(root,"ui-controls.json"),JsonSerializer.Serialize(new{success=true,step=true,invalidStep=true,keyboard=true,persistence=true}));Console.WriteLine("UiControlsContractTests passed (step API, range, keyboard and persistence).");
    }
}

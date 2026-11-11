using System.Text;
using System.Text.Json;
using SelfEngine;
internal static class UiTextContractTests
{
    private static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    public static void Run(bool windowSmoke)
    {
        using var session=new EngineSession();session.LoadLocale("en-US");var field=session.Ui.Create(UiKind.TextInput);var style=UiStyle.Default;style.Offset=new(20,20);style.Size=new(260,76);field.SetStyle(style);field.SetText("A中😀B");session.Ui.Layout(640,480);field.Focus();
        UiVisual Snapshot()=>session.Ui.Visuals().Single(v=>v.Id==field.Id);
        Check(Snapshot().Caret==4,"caret JSON counts codepoints");session.Ui.Key(37);session.Ui.Key(37,true);Check(Snapshot().Caret==2&&Snapshot().SelectionAnchor==3,"selection metadata remains codepoint based");session.Ui.DrainEvents();session.Ui.Key(46);
        Check(Snapshot().Text=="A中B"&&session.Ui.DrainEvents().Count(e=>e.Kind==UiEventKind.TextChanged)==1,"C# delete removes complete emoji");field.SetText("");session.Ui.DrainEvents();
        Check(session.Ui.InputText("拼音",true)&&Snapshot().Text==""&&Snapshot().Composition=="拼音"&&Snapshot().CompositionCaret==2,"native preview remains separate");
        Check(session.Ui.InputText("中😀")&&Snapshot().Text=="中😀"&&Snapshot().Composition==""&&session.Ui.DrainEvents().Count(e=>e.Kind==UiEventKind.TextChanged)==1,"one C# commit");session.Ui.Key(8);Check(Snapshot().Text=="中"&&Encoding.UTF8.GetByteCount(Snapshot().Text)==3,"UTF8 remains valid after non-BMP deletion");
        session.Ui.InputText("cancel",true);session.Ui.Key(27);Check(Snapshot().Text=="中"&&Snapshot().Composition=="","composition cancel leaves value");session.Ui.InputText("old",true);field.SetText("reset");Check(Snapshot().Composition==""&&Snapshot().Caret==5,"external replacement resets preview/caret");
        if(windowSmoke)session.RunWindow("en-US",false,3);
        var root=Path.Combine(AppContext.BaseDirectory,"Validation","T59",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);File.WriteAllText(Path.Combine(root,"ui-text.json"),JsonSerializer.Serialize(new{success=true,codepointCaret=true,oneCommit=true,cancel=true,externalReset=true,window=windowSmoke,manualImeVerified=false}));
        Console.WriteLine("UiTextContractTests passed (codepoint snapshot, delete, composition/result/cancel and window).");
    }
}

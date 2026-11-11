using System.Numerics;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using SelfEngine;

internal static class UiRoutingContractTests
{
    private static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    public static void Run(bool windowSmoke)
    {
        using var session=new EngineSession();session.LoadLocale("en-US");
        var button=session.Ui.Create(UiKind.Button);var style=UiStyle.Default;style.Offset=new(10,10);style.Size=new(100,40);button.SetStyle(style);button.SetText("Button");
        var panel=session.Ui.Create(UiKind.Panel);style.Offset=Vector2.Zero;style.Size=new(200,160);panel.SetStyle(style);
        session.Ui.Layout(640,480);
        session.Ui.Pointer(UiPointerOperation.Down,new(20,20));session.Ui.Pointer(UiPointerOperation.Up,new(20,20));
        Check(session.Ui.DrainEvents().Count(e=>e.Kind==UiEventKind.Click&&e.Target==button.Id)==1,"visual panel defaults to pass-through");
        panel.SetPointerBlocking(true);session.Ui.Pointer(UiPointerOperation.Down,new(20,20));session.Ui.Pointer(UiPointerOperation.Up,new(20,20));
        Check(session.Ui.ConsumesKeyboard&&!session.Ui.DrainEvents().Any(e=>e.Kind==UiEventKind.Click),"blocking panel focuses without clicking scene button");
        var document=session.Ui.Save();var json=JsonNode.Parse(document)!;
        Check(json["nodes"]!.AsArray().Single(node=>node!["id"]!.GetValue<ulong>()==panel.Id)!["blocksPointer"]!.GetValue<bool>(),"blocking property persisted");
        panel.SetPointerBlocking(false);Check(!session.Ui.ConsumesKeyboard,"removing panel blocking clears its focus");
        session.Ui.Load(document);bool stale=false;try{panel.SetPointerBlocking(false);}catch(EngineException e){stale=e.Key=="Bridge.InvalidUi";}
        Check(stale,"new property respects UI proxy epoch");var restored=session.Ui.Find(panel.Id)!;
        session.Ui.Layout(640,480);Check(session.Ui.Pointer(UiPointerOperation.Down,new(20,20))&&session.Ui.ConsumesKeyboard,"loaded blocking node captures");
        restored.SetVisible(false);Check(!session.Ui.ConsumesKeyboard,"hiding menu clears captured focus");session.Ui.Pointer(UiPointerOperation.Up,new(20,20));
        Check(!session.Ui.DrainEvents().Any(e=>e.Kind==UiEventKind.Click),"hidden captured menu never clicks sibling");
        foreach(var node in json["nodes"]!.AsArray())node!.AsObject().Remove("blocksPointer");
        session.Ui.Load(json.ToJsonString());session.Ui.Layout(640,480);session.Ui.Pointer(UiPointerOperation.Down,new(20,20));session.Ui.Pointer(UiPointerOperation.Up,new(20,20));
        Check(session.Ui.DrainEvents().Count(e=>e.Kind==UiEventKind.Click&&e.Target==button.Id)==1,"old UI JSON remains pass-through");
        var input=session.Ui.Create(UiKind.TextInput);style.Offset=new(0,200);style.Size=new(200,60);input.SetStyle(style);input.Focus();Check(session.Ui.ConsumesKeyboard,"input focus consumes keyboard");input.Remove();Check(!session.Ui.ConsumesKeyboard,"deleted input releases keyboard focus");
        Check(Marshal.SizeOf<UiStyle>()==56,"UI style ABI remains 56 bytes");
        if(windowSmoke)session.RunWindow("en-US",false,3);
        var root=Path.Combine(AppContext.BaseDirectory,"Validation","T58",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);
        File.WriteAllText(Path.Combine(root,"ui-routing.json"),JsonSerializer.Serialize(new{success=true,blocking=true,legacy=true,staleProxy=true,styleBytes=56,window=windowSmoke}));
        Console.WriteLine("UiRoutingContractTests passed (blocking, focus/capture, old JSON, proxy lifetime, ABI and window).");
    }
}

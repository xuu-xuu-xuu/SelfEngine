using System.Text.Json;
using System.Text.Json.Nodes;
using SelfEngine;
internal static class UiCanvasContractTests
{
    private static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    public static void Run()
    {
        using var session=new EngineSession();var button=session.Ui.Create(UiKind.Button);var legacy=session.Ui.Save();var document=JsonNode.Parse(legacy)!.AsObject();var node=document["nodes"]!.AsArray()[0]!.AsObject();
        node["bindings"]=new JsonArray(new JsonObject{["event"]=0,["entity"]=99,["script"]="sample.menu_flow",["action"]="menu.start"});session.Ui.Load(document.ToJsonString());
        var saved=session.Ui.Save();using(var proof=JsonDocument.Parse(saved)){var binding=proof.RootElement.GetProperty("nodes")[0].GetProperty("bindings")[0];Check(binding.GetProperty("entity").GetUInt64()==99&&binding.GetProperty("script").GetString()=="sample.menu_flow"&&binding.GetProperty("action").GetString()=="menu.start","C# load/save preserves stable binding descriptions");}
        bool stale=false;try{button.SetText("old");}catch(EngineException e){stale=e.Key=="Bridge.InvalidUi";}Check(stale,"successful document load retires old proxy");var current=session.Ui.Find(button.Id)!;
        var bad=JsonNode.Parse(saved)!;var bindings=bad["nodes"]![0]!["bindings"]!.AsArray();bindings.Add(bindings[0]!.DeepClone());bool rejected=false;try{session.Ui.Load(bad.ToJsonString());}catch(EngineException e){rejected=e.Key=="Bridge.InvalidUi";}
        Check(rejected&&session.Ui.Save()==saved,"duplicate binding load is atomic through C ABI");current.SetText("still valid");session.Ui.Load(legacy);Check(session.Ui.Save()==legacy,"legacy document remains unchanged without binding fields");
        var root=Path.Combine(AppContext.BaseDirectory,"Validation","T62",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);File.WriteAllText(Path.Combine(root,"ui-canvas.json"),JsonSerializer.Serialize(new{success=true,bindingsPreserved=true,atomicLoad=true,legacy=true,staleProxy=stale,descriptionsOnly=true}));Console.WriteLine("UiCanvasContractTests passed (binding document roundtrip, atomic load and proxy lifetime).");
    }
}

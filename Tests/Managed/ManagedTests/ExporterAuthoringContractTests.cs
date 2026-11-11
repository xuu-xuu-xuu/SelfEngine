using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using SelfEngine;

internal static class ExporterAuthoringContractTests
{
    public static void Prepare(string projectRoot,string assembly)
    {
        var scenePath=Path.Combine(projectRoot,"Scenes","Main.scene.json");var bindingsPath=Path.Combine(projectRoot,"Scripts","Main.bindings.json");
        using var session=new EngineSession();session.ImportScene(File.ReadAllText(scenePath));var model=session.FindEntity(3)??throw new InvalidOperationException("template model missing");var transform=model.Transform;transform.Position.X=0;model.Transform=transform;File.WriteAllText(scenePath,session.ExportScene());
        var bindings=new ScriptBindings(assembly,["emptygame.spin"],File.ReadAllText(bindingsPath));
        bindings.Apply(3,"{\"Type\":\"emptygame.spin\",\"Remove\":true}");bindings.Apply(3,"{\"Type\":\"emptygame.spin\",\"Add\":true}");bindings.Apply(3,"{\"Type\":\"emptygame.spin\",\"Field\":\"RadiansPerSecond\",\"Value\":2}");bindings.Apply(2,"{\"Type\":\"emptygame.spin\",\"Add\":true}");bindings.Save(bindingsPath);
        using var project=new ScriptProject(session);var fields=project.Validate(assembly,["emptygame.spin"],File.ReadAllText(bindingsPath));var document=JsonNode.Parse(fields)!;
        if(document["Scripts"]!.AsArray().Count!=2||document["Scripts"]!.AsArray().Single(value=>value!["Entity"]!.GetValue<string>()=="3")!["Fields"]!["RadiansPerSecond"]!["Value"]!.GetValue<float>()!=2||model.Transform.Position.X!=0)throw new InvalidOperationException("template authoring/save/strict-validation roundtrip");
        var output=Path.Combine(projectRoot,"out");Directory.CreateDirectory(output);File.WriteAllText(Path.Combine(output,"authoring.json"),JsonSerializer.Serialize(new{success=true,sceneEdited=true,behaviourRemovedAdded=true,fieldsSaved=true,attachments=2,modelRadians=2,strictPreflight=true,sceneSha256=Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(scenePath)))}));Console.WriteLine("ExporterAuthoringContractTests passed.");
    }
}

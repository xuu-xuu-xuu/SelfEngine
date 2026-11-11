using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using SelfEngine;

internal static class ScriptValidationContractTests
{
    private static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static WeakReference Validate(ScriptProject project,string assembly,string document)
    {
        var normalized=project.Validate(assembly,["test.validation"],document);
        Check(!project.IsRunning&&JsonNode.Parse(normalized)!["Scripts"]!.AsArray().Count==1,"validation returns canonical fields without running the project");
        return project.LastUnloadedContext!;
    }
    public static void Run()
    {
        using var session=new EngineSession();var owner=session.CreateEntity("validation owner");var target=session.CreateEntity("validation reference");
        using var project=new ScriptProject(session);var diagnostics=new List<ScriptDiagnostic>();project.Diagnostic+=diagnostics.Add;
        var document=new JsonObject{["Version"]=1,["Scripts"]=new JsonArray(new JsonObject{["Type"]="test.validation",["Entity"]=owner.StableId.ToString(),["Enabled"]=true,["Fields"]=new JsonObject{["Speed"]=new JsonObject{["Kind"]="f32",["Value"]=3.5f},["Target"]=new JsonObject{["Kind"]="entity",["Value"]=target.StableId.ToString()}}})}.ToJsonString();
        var assembly=Path.Combine(AppContext.BaseDirectory,"SelfEngine.ManagedTests.dll");string before=session.ExportScene();var contexts=new List<WeakReference>();
        var catalogue=ScriptProject.DiscoverTypes(assembly);
        Check(catalogue.Contains("test.validation")&&catalogue.Contains("tests.move"),"candidate catalogue includes both attached and unattached public behaviours");
        var editable=new ScriptBindings(assembly,catalogue,document);
        var available=JsonNode.Parse(editable.Describe(owner.StableId))!["Available"]!.AsArray().Select(value=>value!.GetValue<string>()).ToArray();
        Check(available.Contains("tests.move"),"formal editor can select a compiled behaviour that was not already in bindings");
        editable.Apply(owner.StableId,"{\"Type\":\"tests.move\",\"Add\":true}");
        Check(ScriptBindings.ScriptTypes(editable.Json).Contains("tests.move")&&session.ExportScene()==before,"adding an unbound compiled type edits bindings without calling lifecycle or changing the world");
        for(int i=0;i<12;++i)contexts.Add(Validate(project,assembly,document));
        Check(session.ExportScene()==before&&session.Ui.Visuals().Count==0&&diagnostics.Count==0,"field validation never calls OnCreate or OnDestroy and leaves world/UI untouched");
        int rejected=0;
        void Invalid(Action<JsonObject> change,string key)
        {
            var candidate=JsonNode.Parse(document)!.AsObject();change(candidate);diagnostics.Clear();bool failed=false;
            try{project.Validate(assembly,["test.validation"],candidate.ToJsonString());}catch(InvalidOperationException e){failed=e.Message=="Script.PreflightFailed";}
            Check(failed&&diagnostics.Any(d=>d.Key==key)&&!project.IsRunning&&session.ExportScene()==before,"strict preflight rejects "+key+" without editing or running the world");++rejected;
        }
        Invalid(d=>d["Scripts"]![0]!["Entity"]="999","Script.EntityMissing");
        Invalid(d=>d["Scripts"]![0]!["Type"]="test.unknown","Script.TypeMissing");
        Invalid(d=>d["Scripts"]![0]!["Fields"]!["Speed"]!["Kind"]="i32","Script.FieldTypeChanged");
        Invalid(d=>d["Scripts"]![0]!["Fields"]!["Speed"]!["Value"]="invalid","Script.FieldValueInvalid");
        Invalid(d=>d["Scripts"]![0]!["Fields"]!["Target"]!["Value"]="999","Script.ReferenceMissing");
        Invalid(d=>d["Scripts"]![0]!["Fields"]!["Unknown"]=new JsonObject{["Kind"]="f32",["Value"]=1},"Script.FieldMissing");
        for(int i=0;i<16&&contexts.Any(c=>c.IsAlive);++i){GC.Collect();GC.WaitForPendingFinalizers();GC.Collect();}
        Check(contexts.All(c=>!c.IsAlive),"preflight releases its collectible project contexts");
        var root=Path.Combine(AppContext.BaseDirectory,"Validation","T65",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);File.WriteAllText(Path.Combine(root,"script-validation.json"),JsonSerializer.Serialize(new{success=true,noLifecycleCallbacks=true,worldUnchanged=true,strictRejected=rejected,unloadedContexts=12}));
        Console.WriteLine("ScriptValidationContractTests passed (no behaviour callbacks, six strict field/reference failures and twelve unloaded contexts).");
    }
}
[ScriptId("test.validation")]
public sealed class ValidationProbe:Behaviour
{
    [Expose] public float Speed=1;
    [Expose] public EntityReference Target;
    public override void OnCreate(){var transform=Entity.Transform;transform.Position.X=999;Entity.Transform=transform;Entity.Session.Ui.Create(UiKind.Label);}
    public override void OnDestroy(){var transform=Entity.Transform;transform.Position.X=-999;Entity.Transform=transform;}
}

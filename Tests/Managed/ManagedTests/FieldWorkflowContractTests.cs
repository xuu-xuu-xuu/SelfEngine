using SelfEngine;
using System.Numerics;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Diagnostics;

internal static class FieldWorkflowContractTests
{
    private static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    private static JsonArray Warnings(ScriptBindings bindings,ulong owner,EngineSession session)
        =>JsonNode.Parse(bindings.Describe(owner,null,id=>session.FindEntity(id) is not null))!["Scripts"]![0]!["Diagnostics"]!.AsArray();
    private static bool Has(JsonArray warnings,string key,string field)=>warnings.OfType<JsonObject>().Any(value=>value["Key"]!.GetValue<string>()==key&&value["Field"]!.GetValue<string>()==field);
    public static void VerifyFreshProcess(string directory)
    {
        using var session=new EngineSession();using var scripts=new ScriptSystem(session,new ScriptRegistry(),simulatePhysics:false);
        session.ImportScene(File.ReadAllText(Path.Combine(directory,"scene.json")));scripts.Registry.Register<FieldsContractProbe>();
        Check(ScriptFields.Load(scripts,File.ReadAllText(Path.Combine(directory,"bindings.json"))),"separate process loads scene and bindings");scripts.Start();var restored=FieldsContractProbe.LastCreated!;
        Check(restored.Counter==43&&restored.Rate==2.5f&&restored.Amount==1.234567890123e-10&&restored.Flag&&restored.Message=="字段😀"&&restored.Direction==new Vector3(1,2,3)&&restored.Target.Resolve(session)?.Name=="field target","separate process restores all fields and stable reference");
        File.WriteAllText(Path.Combine(directory,"process-reopen.json"),JsonSerializer.Serialize(new{schema=1,success=true,processId=Environment.ProcessId,kinds=7}));Console.WriteLine("Field process reopen passed.");
    }
    private static void Restart(string directory)
    {
        var executable=Environment.ProcessPath??throw new InvalidOperationException("missing test process path");
        var start=new ProcessStartInfo(executable){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true};
        if(Path.GetFileNameWithoutExtension(executable).Equals("dotnet",StringComparison.OrdinalIgnoreCase))start.ArgumentList.Add(typeof(FieldWorkflowContractTests).Assembly.Location);
        start.ArgumentList.Add("--field-reopen");start.ArgumentList.Add(directory);using var process=Process.Start(start)??throw new InvalidOperationException("failed to launch field restart test");
        var output=process.StandardOutput.ReadToEndAsync();var error=process.StandardError.ReadToEndAsync();
        if(!process.WaitForExit(30000)){process.Kill();throw new InvalidOperationException("field restart timeout");}
        File.WriteAllText(Path.Combine(directory,"process-reopen.log"),output.GetAwaiter().GetResult()+error.GetAwaiter().GetResult());
        Check(process.ExitCode==0,"new OS process restores saved fields");var proof=JsonNode.Parse(File.ReadAllText(Path.Combine(directory,"process-reopen.json")))!;
        Check(proof["success"]!.GetValue<bool>()&&proof["processId"]!.GetValue<int>()!=Environment.ProcessId,"restart evidence comes from a different process");
    }
    public static void Run(bool windowSmoke)
    {
        var directory=Path.Combine(AppContext.BaseDirectory,"Validation","T47",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(directory);
        var scenePath=Path.Combine(directory,"scene.json");var bindingsPath=Path.Combine(directory,"bindings.json");string fields;ulong ownerId,targetId;
        using(var session=new EngineSession())
        using(var scripts=new ScriptSystem(session,new ScriptRegistry(),simulatePhysics:false))
        {
            var owner=session.CreateEntity("field owner");var target=session.CreateEntity("field target");ownerId=owner.StableId;targetId=target.StableId;
            var source=scripts.Attach<FieldsContractProbe>(owner,value=>{value.Counter=42;value.Rate=2.5f;value.Amount=1.234567890123e-10;value.Flag=true;value.Message="字段😀";value.Direction=new(1,2,3);value.Target=EntityReference.From(target);});
            Check(ScriptFields.Describe(source).Count==7,"seven exposed field kinds");fields=ScriptFields.Save(scripts);
            var editable=new ScriptBindings(typeof(FieldsContractProbe).Assembly.Location,["tests.fields_contract"],fields);
            editable.Apply(ownerId,"{\"Type\":\"tests.fields_contract\",\"Field\":\"Counter\",\"Value\":43}");editable.Save(bindingsPath);fields=editable.Json;
            File.WriteAllText(scenePath,session.ExportScene(),new System.Text.UTF8Encoding(false));
            var node=JsonNode.Parse(fields)!;Check(node["Scripts"]![0]!["Entity"]!.GetValue<string>()==ownerId.ToString()&&node["Scripts"]![0]!["Fields"]!["Target"]!["Value"]!.GetValue<string>()==targetId.ToString(),"persistent references use decimal stable IDs");
            var prior=File.ReadAllText(bindingsPath);var attributes=File.GetAttributes(bindingsPath);bool failed=false;
            try{File.SetAttributes(bindingsPath,attributes|FileAttributes.ReadOnly);editable.Save(bindingsPath);}
            catch(Exception exception)when(exception is IOException or UnauthorizedAccessException){failed=true;}
            finally{File.SetAttributes(bindingsPath,attributes);}
            Check(failed&&File.ReadAllText(bindingsPath)==prior&&!Directory.EnumerateFiles(directory,"*.tmp").Any(),"binding save failure preserves previous file and removes temporary output");
        }
        Restart(directory);
        using(var session=new EngineSession())
        using(var scripts=new ScriptSystem(session,new ScriptRegistry(),simulatePhysics:false))
        {
            session.ImportScene(File.ReadAllText(scenePath));scripts.Registry.Register<FieldsContractProbe>();
            Check(ScriptFields.Load(scripts,File.ReadAllText(bindingsPath)),"fresh session loads saved bindings");scripts.Start();var restored=FieldsContractProbe.LastCreated!;
            Check(restored.Counter==43&&restored.Rate==2.5f&&restored.Amount==1.234567890123e-10&&restored.Flag&&restored.Message=="字段😀"&&restored.Direction==new Vector3(1,2,3)&&restored.Target.Resolve(session)?.Name=="field target","all seven fields and double precision survive disk and session restart");scripts.Stop();
            var changed=JsonNode.Parse(fields)!;var changedFields=changed["Scripts"]![0]!["Fields"]!;
            changedFields["RemovedField"]=new JsonObject{["Kind"]="i32",["Value"]=9};changedFields["Rate"]!["Kind"]="string";changedFields["Rate"]!["Value"]="old field type";
            changed["Scripts"]!.AsArray().Add(new JsonObject{["Type"]="missing.behaviour",["Entity"]=ownerId.ToString(),["Enabled"]=true,["Fields"]=new JsonObject()});
            var changedText=changed.ToJsonString();var metadata=new ScriptBindings(typeof(FieldsContractProbe).Assembly.Location,ScriptBindings.ScriptTypes(changedText),changedText);var before=metadata.Json;
            var warnings=Warnings(metadata,ownerId,session);var document=JsonNode.Parse(metadata.Describe(ownerId))!;
            Check(Has(warnings,"Script.FieldMissing","RemovedField")&&Has(warnings,"Script.FieldTypeChanged","Rate")&&document["Scripts"]![1]!["Missing"]!.GetValue<bool>()&&metadata.Json==before,"Inspector diagnoses removed fields, changed kinds and missing types without rewriting bindings");
            var diagnostics=new List<ScriptDiagnostic>();scripts.Diagnostic+=diagnostics.Add;
            Check(ScriptFields.Load(scripts,changedText)&&scripts.Count==1&&diagnostics.Any(value=>value.Key=="Script.FieldMissing")&&diagnostics.Any(value=>value.Key=="Script.FieldTypeChanged")&&diagnostics.Any(value=>value.Key=="Script.TypeMissing"),"runtime preserves readable scene and healthy script across schema changes");
            scripts.Start();Check(FieldsContractProbe.LastCreated!.Rate==1,"changed field uses declared default without conversion");scripts.Stop();
            File.WriteAllText(Path.Combine(directory,"version-diagnostics.json"),metadata.Describe(ownerId));
            if(windowSmoke)
            {
                var errors=new List<string>();session.Diagnostic+=(key,_)=>errors.Add(key);using var project=new ScriptProject(session);
                var projectRoot=Path.Combine(directory,"project");Directory.CreateDirectory(Path.Combine(projectRoot,"Assets"));
                project.RunEditor(projectRoot,typeof(FieldsContractProbe).Assembly.Location,ScriptBindings.ScriptTypes(changedText),changedText,"en-US",false,3,false);
                Check(!project.IsRunning&&!session.IsWindowRunning&&errors.Count==0&&JsonNode.DeepEquals(JsonNode.Parse(project.EditedBindings),JsonNode.Parse(metadata.Json)),"actual native Inspector opens old fields/missing type and leaves bindings intact");
            }
            var invalid=JsonNode.Parse(fields)!;var values=invalid["Scripts"]![0]!["Fields"]!;
            values["Counter"]!["Value"]="not an integer";values["Rate"]!["Value"]=JsonNode.Parse("1e100");values["Amount"]!["Value"]=JsonNode.Parse("1e1000");
            values["Message"]!["Value"]=new string('x',65537);values["Direction"]!["Value"]=new JsonObject{["X"]=1,["Y"]=2};values["Target"]!["Value"]="-1";
            var invalidText=invalid.ToJsonString();var invalidMetadata=new ScriptBindings(typeof(FieldsContractProbe).Assembly.Location,["tests.fields_contract"],invalidText);var invalidWarnings=Warnings(invalidMetadata,ownerId,session);
            diagnostics.Clear();Check(ScriptFields.Load(scripts,invalidText),"invalid field values remain diagnosable");
            foreach(var name in new[]{"Counter","Rate","Amount","Message","Direction","Target"})Check(Has(invalidWarnings,"Script.FieldValueInvalid",name)&&diagnostics.Any(value=>value.Key=="Script.FieldValueInvalid"&&value.Detail==name),"Inspector and runtime share exact invalid-value validation");
            scripts.Start();restored=FieldsContractProbe.LastCreated!;Check(restored.Counter==0&&restored.Rate==1&&restored.Amount==2&&restored.Message=="default"&&restored.Direction==Vector3.UnitY&&restored.Target.StableId==0&&restored.Flag,"invalid values preserve defaults, healthy fields still restore");scripts.Stop();
            File.WriteAllText(Path.Combine(directory,"invalid-value-diagnostics.json"),invalidMetadata.Describe(ownerId));
            session.FindEntity(targetId)!.Destroy();var replacement=session.CreateEntity("unrelated replacement");Check(replacement.StableId!=targetId,"deleted stable reference is not reused");
            var referenceMetadata=new ScriptBindings(typeof(FieldsContractProbe).Assembly.Location,["tests.fields_contract"],fields);
            Check(Has(Warnings(referenceMetadata,ownerId,session),"Script.ReferenceMissing","Target"),"Inspector names unresolved reference field");
            diagnostics.Clear();Check(ScriptFields.Load(scripts,fields)&&diagnostics.Any(value=>value.Key=="Script.ReferenceMissing"&&value.Detail=="Target"),"runtime reports unresolved stable ID");scripts.Start();Check(FieldsContractProbe.LastCreated!.Target.StableId==targetId&&FieldsContractProbe.LastCreated.Target.Resolve(session) is null,"missing reference remains intact without targeting replacement");scripts.Stop();
            File.WriteAllText(Path.Combine(directory,"retired.scene.json"),session.ExportScene());
        }
        using(var session=new EngineSession())
        {session.ImportScene(File.ReadAllText(Path.Combine(directory,"retired.scene.json")));Check(new EntityReference(targetId).Resolve(session) is null&&session.FindEntity(ownerId) is not null,"retired reference remains missing after another disk restart");}
        File.WriteAllText(Path.Combine(directory,"verification.json"),JsonSerializer.Serialize(new{schema=1,success=true,kinds=7,invalidFields=6,ownerId,targetId,stableReferenceRetired=true,writeFailurePreserved=true},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine("Field workflow contract passed; evidence: "+directory);
    }
}

[ScriptId("tests.fields_contract")]
public sealed class FieldsContractProbe:Behaviour
{
    [Expose] public int Counter;
    [Expose] public float Rate=1;
    [Expose] public double Amount=2;
    [Expose] public bool Flag;
    [Expose] public string Message="default";
    [Expose] public Vector3 Direction=Vector3.UnitY;
    [Expose] public EntityReference Target;
    public static FieldsContractProbe? LastCreated;
    public override void OnCreate()=>LastCreated=this;
}

using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using SelfEngine;

internal static class BindingDocumentContractTests
{
    private static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    private static string Digest(string path)=>Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();
    private static void Rejected(Action action,string key)
    {
        bool rejected=false;
        try{action();}catch(Exception exception)when(exception is InvalidOperationException or System.Text.Json.JsonException or ArgumentException)
        {rejected=key.Length==0||exception.Message==key;}
        Check(rejected,"binding document rejects "+key);
    }
    public static void Run()
    {
        var root=Path.Combine(AppContext.BaseDirectory,"Validation","U07-bindings",Guid.NewGuid().ToString("N"),"项目");
        var path=Path.Combine(root,"Scripts","绑定.json");Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        const string source="{\"Version\":1,\"Scripts\":[{\"Type\":\"tests.fields_contract\",\"Entity\":\"1\",\"Enabled\":true,\"Fields\":{\"Counter\":{\"Kind\":\"i32\",\"Value\":1}}}]}";
        File.WriteAllText(path,source,new UTF8Encoding(true));var originalBytes=File.ReadAllBytes(path);var originalDigest=Digest(path);
        var bindings=new ScriptBindings(typeof(FieldsContractProbe).Assembly.Location,["tests.fields_contract"],File.ReadAllText(path));
        bindings.ConfigureDocument(root,path);
        var initial=JsonNode.Parse(bindings.SnapshotDocument())!;
        Check(!bindings.Dirty&&bindings.Revision==0&&bindings.SourceDigest==originalDigest
            &&initial["RelativePath"]!.GetValue<string>()=="Scripts/绑定.json"&&initial["Content"]!.GetValue<string>()==bindings.Json,"initial snapshot is complete, clean and hashes raw file bytes");
        var snapshot=bindings.SnapshotDocument();Check(bindings.SnapshotDocument()==snapshot,"repeated snapshot is pure");
        var noAssembly=new ScriptBindings(Path.Combine(root,"unavailable.dll"),[],"{\"Version\":1,\"Scripts\":[]}");
        noAssembly.ConfigureDocument(root,Path.Combine(root,"Scripts","empty.json"));
        Check(JsonNode.Parse(noAssembly.SnapshotDocument())!["SourceDigest"]!.GetValue<string>()=="","snapshot never loads project types or requires an assembly");

        void Counter(int value)=>bindings.Apply(1,"{\"Type\":\"tests.fields_contract\",\"Field\":\"Counter\",\"Value\":"+value+"}");
        Counter(2);Check(bindings.Dirty&&bindings.Revision==1&&File.ReadAllBytes(path).SequenceEqual(originalBytes),"field edit changes memory and revision only");
        Counter(2);Check(bindings.Revision==1,"no-op edit does not increase revision");
        var before=bindings.SnapshotDocument();
        Rejected(()=>bindings.Apply(1,"{\"Type\":\"tests.fields_contract\",\"Field\":\"Counter\",\"Value\":\"bad\"}"),"Script.FieldValueInvalid");
        Check(bindings.SnapshotDocument()==before,"invalid edit preserves document state");
        Counter(1);Check(!bindings.Dirty&&bindings.Revision==2,"editing back to saved values becomes clean");
        Counter(3);var editedRevision=bindings.Revision;var attributes=File.GetAttributes(path);bool failed=false;
        try{File.SetAttributes(path,attributes|FileAttributes.ReadOnly);bindings.SaveDocument();}
        catch(Exception exception)when(exception is IOException or UnauthorizedAccessException){failed=true;}
        finally{File.SetAttributes(path,attributes);}
        Check(failed&&bindings.Dirty&&bindings.Revision==editedRevision&&bindings.SourceDigest==originalDigest
            &&File.ReadAllBytes(path).SequenceEqual(originalBytes)&&!Directory.EnumerateFiles(Path.GetDirectoryName(path)!,"*.tmp").Any(),"failed save keeps dirty state, original bytes and baseline without temporary residue");

        var external=JsonNode.Parse(source)!;external["Scripts"]![0]!["Fields"]!["Counter"]!["Value"]=90;
        File.WriteAllText(path,external.ToJsonString(),new UTF8Encoding(false));var externalBytes=File.ReadAllBytes(path);
        Rejected(()=>bindings.SaveDocument(),"Script.DocumentExternalChanged");
        Check(bindings.Dirty&&File.ReadAllBytes(path).SequenceEqual(externalBytes),"save conflict never overwrites external changes");
        var recovered=JsonNode.Parse(source)!;recovered["Scripts"]![0]!["Fields"]!["Counter"]!["Value"]=4;
        var recoveredContent=recovered.ToJsonString();before=bindings.SnapshotDocument();
        bindings.ValidateRestore(recoveredContent,bindings.RelativePath,originalDigest);
        bindings.ValidateRestore(recoveredContent,bindings.RelativePath,originalDigest);
        Check(bindings.SnapshotDocument()==before&&File.ReadAllBytes(path).SequenceEqual(externalBytes),"restore validation is repeatable without memory or disk mutations");
        Rejected(()=>bindings.ValidateRestore(recoveredContent,"../outside.json",originalDigest),"Script.DocumentPathInvalid");
        Rejected(()=>bindings.ValidateRestore(recoveredContent,"Scripts/other.json",originalDigest),"Script.DocumentPathInvalid");
        Rejected(()=>bindings.ValidateRestore(recoveredContent,bindings.RelativePath,"invalid"),"Script.DocumentPathInvalid");
        Rejected(()=>bindings.RestoreDocument("{\"Version\":2,\"Scripts\":[]}",bindings.RelativePath,originalDigest),"Script.DocumentInvalid");
        Check(bindings.SnapshotDocument()==before,"invalid restore cannot replace active document");
        bindings.RestoreDocument(recoveredContent,bindings.RelativePath,originalDigest);
        Check(bindings.Dirty&&bindings.Revision==editedRevision+1&&bindings.SourceDigest==originalDigest
            &&JsonNode.Parse(bindings.Json)!["Scripts"]![0]!["Fields"]!["Counter"]!["Value"]!.GetValue<int>()==4
            &&File.ReadAllBytes(path).SequenceEqual(externalBytes),"restore changes only memory and retains source-conflict identity");
        Rejected(()=>bindings.SaveDocument(),"Script.DocumentExternalChanged");
        bindings.SaveDocument(true);
        Check(!bindings.Dirty&&bindings.SourceDigest==Digest(path)&&File.ReadAllText(path)==bindings.Json,"explicit overwrite updates saved baseline and raw-byte digest");
        var savedRevision=bindings.Revision;
        bindings.RestoreDocument(bindings.Json,bindings.RelativePath,bindings.SourceDigest);
        Check(bindings.Dirty&&bindings.Revision==savedRevision,"identical restored content stays unsaved without inventing content revision");
        bindings.SaveDocument();Check(!bindings.Dirty&&bindings.Revision==savedRevision,"saving does not manufacture an edit revision");
        before=bindings.SnapshotDocument();
        Rejected(()=>bindings.ConfigureDocument(root,Path.Combine(root,"..","outside.json")),"Script.DocumentPathInvalid");
        Rejected(()=>bindings.ConfigureDocument(root,Path.Combine(root,"Scripts","binding.json:stream")),"Script.DocumentPathInvalid");
        Rejected(()=>bindings.ConfigureDocument(root,Path.Combine(root,"Scripts","CON.json")),"Script.DocumentPathInvalid");
        Check(bindings.SnapshotDocument()==before,"invalid document path keeps prior configuration");
        var staleInput=new ScriptBindings(typeof(FieldsContractProbe).Assembly.Location,["tests.fields_contract"],source);
        Rejected(()=>staleInput.ConfigureDocument(root,path),"Script.DocumentExternalChanged");
        File.WriteAllText(Path.Combine(root,"verification.json"),"{\"schema\":1,\"success\":true,\"rawDigest\":true,\"revision\":true,\"conflictPreserved\":true,\"restoreValidationPure\":true,\"restoreMemoryOnly\":true}");
        Console.WriteLine("Binding document contract passed; evidence: "+root);
    }
}

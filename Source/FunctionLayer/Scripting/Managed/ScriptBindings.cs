using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace SelfEngine;

// 编辑态仅保留 JSON/字符串元数据，不永久保留项目 Type，允许停止后重编译 DLL。
public sealed class ScriptBindings
{
    private readonly string assemblyPath;
    private readonly HashSet<string> allowed;
    private readonly Dictionary<string,JsonArray> schemas = new(StringComparer.Ordinal);
    private DateTime schemaStamp;
    private long schemaLength;
    private JsonObject document;
    private JsonObject savedDocument;
    private string? documentRoot,documentPath;
    private string sourceDigest="";
    private ulong revision;
    private bool restoredDirty;
    private static readonly JsonSerializerOptions Options = new() { IncludeFields = true, WriteIndented = true };
    public string Json => document.ToJsonString(Options);
    public bool Dirty=>restoredDirty||!JsonNode.DeepEquals(document,savedDocument);
    public ulong Revision=>revision;
    public string SourceDigest=>sourceDigest;
    public string RelativePath=>documentRoot is null||documentPath is null?"":Path.GetRelativePath(documentRoot,documentPath).Replace('\\','/');
    public static IReadOnlyList<string> ScriptTypes(string json)
    {
        var document=JsonNode.Parse(json)?.AsObject()??throw new JsonException("Script.DocumentInvalid");
        if(document["Version"]?.GetValue<int>()!=1||document["Scripts"] is not JsonArray scripts)throw new JsonException("Script.DocumentInvalid");
        return scripts.Select(entry=>entry?["Type"]?.GetValue<string>()??throw new JsonException("Script.TypeMissing")).Distinct(StringComparer.Ordinal).ToArray();
    }
    public ScriptBindings(string assemblyPath,IEnumerable<string> allowedScriptIds,string json)
    {
        this.assemblyPath=Path.GetFullPath(assemblyPath);allowed=new(allowedScriptIds,StringComparer.Ordinal);
        document=JsonNode.Parse(json)?.AsObject()??throw new JsonException("Script.DocumentInvalid");
        if(document["Version"]?.GetValue<int>()!=1||document["Scripts"] is not JsonArray)throw new JsonException("Script.DocumentInvalid");
        savedDocument=(JsonObject)document.DeepClone();
    }
    private static string HashFile(string path)=>File.Exists(path)?Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant():"";
    private static bool ValidDigest(string value)=>value.Length==0||value.Length==64&&value.All(c=>c is >= '0' and <= '9' or >= 'a' and <= 'f');
    private static string CheckedPath(string root,string path)
    {
        var target=Path.GetFullPath(path,root);
        var prefix=Path.EndsInDirectorySeparator(root)?root:root+Path.DirectorySeparatorChar;
        if(!target.StartsWith(prefix,StringComparison.OrdinalIgnoreCase)||Directory.Exists(target))throw new InvalidOperationException("Script.DocumentPathInvalid");
        var relative=Path.GetRelativePath(root,target);
        foreach(var part in relative.Split(Path.DirectorySeparatorChar,Path.AltDirectorySeparatorChar))
        {
            if(part is "" or "." or ".."||part.IndexOfAny(Path.GetInvalidFileNameChars())>=0||part.EndsWith('.')||part.EndsWith(' '))throw new InvalidOperationException("Script.DocumentPathInvalid");
            var stem=part.Split('.')[0].ToUpperInvariant();
            if(stem is "CON" or "PRN" or "AUX" or "NUL"||(stem.Length==4&&(stem.StartsWith("COM",StringComparison.Ordinal)||stem.StartsWith("LPT",StringComparison.Ordinal))&&stem[3] is >= '1' and <= '9'))throw new InvalidOperationException("Script.DocumentPathInvalid");
        }
        // 不沿项目内部的目录联接或符号链接访问项目外文件；每次保存重新检查。
        for(var current=target;!current.Equals(Path.TrimEndingDirectorySeparator(root),StringComparison.OrdinalIgnoreCase);current=Path.GetDirectoryName(current)!)
        {
            if(string.IsNullOrEmpty(current))throw new InvalidOperationException("Script.DocumentPathInvalid");
            try{if((File.GetAttributes(current)&FileAttributes.ReparsePoint)!=0)throw new InvalidOperationException("Script.DocumentPathInvalid");}
            catch(FileNotFoundException){}
            catch(DirectoryNotFoundException){}
        }
        return target;
    }
    public void ConfigureDocument(string projectRoot,string path)
    {
        var root=Path.TrimEndingDirectorySeparator(Path.GetFullPath(projectRoot));
        var target=CheckedPath(root,path);var digest="";
        if(File.Exists(target))
        {
            var bytes=File.ReadAllBytes(target);
            var offset=bytes.Length>=3&&bytes[0]==0xef&&bytes[1]==0xbb&&bytes[2]==0xbf?3:0;
            var current=JsonNode.Parse(Encoding.UTF8.GetString(bytes,offset,bytes.Length-offset));
            if(!JsonNode.DeepEquals(document,current))throw new InvalidOperationException("Script.DocumentExternalChanged");
            digest=Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
        }
        documentRoot=root;documentPath=target;sourceDigest=digest;
    }
    public string SnapshotDocument()
    {
        if(documentRoot is null||documentPath is null)throw new InvalidOperationException("Script.DocumentPathInvalid");
        return new JsonObject{["Schema"]=1,["Id"]="script-bindings",["Kind"]="bindings",["RelativePath"]=RelativePath,
            ["Content"]=Json,["SourceDigest"]=sourceDigest,["Revision"]=revision,["Dirty"]=Dirty}.ToJsonString();
    }
    private static JsonObject RestoreCandidate(string content)
    {
        var candidate=JsonNode.Parse(content)?.AsObject()??throw new JsonException("Script.DocumentInvalid");
        if(candidate["Version"]?.GetValue<int>()!=1||candidate["Scripts"] is not JsonArray scripts)throw new JsonException("Script.DocumentInvalid");
        var identities=new HashSet<(string,string)>();
        foreach(var value in scripts)
        {
            if(value is not JsonObject entry||entry["Type"] is not JsonValue typeNode||!typeNode.TryGetValue<string>(out var type)||string.IsNullOrWhiteSpace(type)
                ||entry["Entity"] is not JsonValue entityNode||!entityNode.TryGetValue<string>(out var entity)
                ||string.IsNullOrEmpty(entity)
                ||!ulong.TryParse(entity,NumberStyles.None,CultureInfo.InvariantCulture,out var stable)||stable==0
                ||entry["Fields"] is not JsonObject||!identities.Add((entity,type)))throw new JsonException("Script.DocumentInvalid");
            if(entry["Enabled"] is not null&&(entry["Enabled"] is not JsonValue enabledNode||!enabledNode.TryGetValue<bool>(out _)))throw new JsonException("Script.DocumentInvalid");
        }
        return candidate;
    }
    private JsonObject ValidateRestoreCandidate(string content,string relativePath,string digest)
    {
        if(documentRoot is null||documentPath is null||Path.IsPathRooted(relativePath)||!ValidDigest(digest)
            ||!CheckedPath(documentRoot,relativePath).Equals(documentPath,StringComparison.OrdinalIgnoreCase))throw new InvalidOperationException("Script.DocumentPathInvalid");
        return RestoreCandidate(content);
    }
    public void ValidateRestore(string content,string relativePath,string digest)
        =>_=ValidateRestoreCandidate(content,relativePath,digest);
    public void RestoreDocument(string content,string relativePath,string digest)
    {
        var candidate=ValidateRestoreCandidate(content,relativePath,digest);
        ReplaceDocument(candidate);sourceDigest=digest;restoredDirty=true;
    }
    private void ReplaceDocument(JsonObject candidate)
    {
        if(JsonNode.DeepEquals(document,candidate))return;
        if(revision==ulong.MaxValue)throw new InvalidOperationException("Script.DocumentRevisionExhausted");
        document=candidate;++revision;
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private void LoadSchema()
    {
        var file=new FileInfo(assemblyPath);
        if(schemas.Count!=0&&file.LastWriteTimeUtc==schemaStamp&&file.Length==schemaLength)return;
        var next=new Dictionary<string,JsonArray>(StringComparer.Ordinal);
        var context=new ScriptProject.ProjectContext(assemblyPath);
        try
        {
            using var stream=File.OpenRead(assemblyPath);
            foreach(var type in context.LoadFromStream(stream).GetTypes().Where(t=>t.IsPublic&&!t.IsAbstract&&typeof(Behaviour).IsAssignableFrom(t)))
            {
                var id=ScriptRegistry.IdOf(type);if(!allowed.Contains(id))continue;
                if(Activator.CreateInstance(type) is not Behaviour instance)continue;
                var fields=new JsonArray();
                foreach(var field in ScriptFields.Describe(instance))
                {
                    var value=field.Value is EntityReference reference?JsonValue.Create(reference.StableId.ToString(CultureInfo.InvariantCulture))
                        :JsonSerializer.SerializeToNode(field.Value,field.Value?.GetType()??typeof(string),Options);
                    fields.Add(new JsonObject{["Name"]=field.Name,["Kind"]=field.Kind,["Value"]=value});
                }
                if(!next.TryAdd(id,fields))throw new InvalidOperationException("Script.DuplicateType");
            }
        }
        finally {context.Unload();}
        schemas.Clear();foreach(var pair in next)schemas.Add(pair.Key,pair.Value);
        schemaStamp=file.LastWriteTimeUtc;schemaLength=file.Length;
    }
    public string Describe(ulong entityStableId,ScriptSystem? running=null)=>Describe(entityStableId,running,null);
    public string Describe(ulong entityStableId,ScriptSystem? running,Func<ulong,bool>? referenceExists)
    {
        LoadSchema();var scripts=new JsonArray();
        var current=running is null?document:JsonNode.Parse(ScriptFields.Save(running))!.AsObject();
        foreach(var entry in current["Scripts"]!.AsArray().OfType<JsonObject>().Where(e=>e["Entity"]?.GetValue<string>()==entityStableId.ToString(CultureInfo.InvariantCulture)))
        {
            var type=entry["Type"]!.GetValue<string>();var fields=new JsonArray();var diagnostics=new JsonArray();var seen=new HashSet<(string,string)>();
            void Warn(string key,string field){if(seen.Add((key,field)))diagnostics.Add(new JsonObject{["Key"]=key,["Field"]=field});}
            if(schemas.TryGetValue(type,out var schema))
            {
                var known=schema.OfType<JsonObject>().Select(field=>field["Name"]!.GetValue<string>()).ToHashSet(StringComparer.Ordinal);
                var savedFields=entry["Fields"] as JsonObject;
                if(savedFields is null)Warn("Script.DocumentInvalid","Fields");
                else foreach(var pair in savedFields)if(!known.Contains(pair.Key))Warn("Script.FieldMissing",pair.Key);
                foreach(var field in schema.OfType<JsonObject>())
                {
                    var item=(JsonObject)field.DeepClone();var name=item["Name"]!.GetValue<string>();var kind=item["Kind"]!.GetValue<string>();
                    if(savedFields?.TryGetPropertyValue(name,out var savedNode)==true)
                    {
                        if(savedNode is not JsonObject saved||saved["Kind"] is not JsonValue kindNode||!kindNode.TryGetValue<string>(out var savedKind)||savedKind!=kind)Warn("Script.FieldTypeChanged",name);
                        else if(!ValidValue(kind,saved["Value"]))Warn("Script.FieldValueInvalid",name);
                        else item["Value"]=saved["Value"]?.DeepClone();
                    }
                    if(kind=="entity"&&referenceExists is not null&&ulong.TryParse(item["Value"]?.GetValue<string>(),NumberStyles.None,CultureInfo.InvariantCulture,out var reference)&&reference!=0&&!referenceExists(reference))Warn("Script.ReferenceMissing",name);
                    fields.Add(item);
                }
            }
            else Warn("Script.TypeMissing","");
            scripts.Add(new JsonObject{["Type"]=type,["Enabled"]=entry["Enabled"]?.GetValue<bool>()??true,["Missing"]=!schemas.ContainsKey(type),["Fields"]=fields,["Diagnostics"]=diagnostics});
        }
        return new JsonObject{["Scripts"]=scripts,["Available"]=new JsonArray(schemas.Keys.Order().Select(id=>(JsonNode?)JsonValue.Create(id)).ToArray())}.ToJsonString();
    }
    public void Apply(ulong entityStableId,string request)
    {
        LoadSchema();var change=JsonNode.Parse(request)?.AsObject()??throw new JsonException("Script.DocumentInvalid");
        var type=change["Type"]?.GetValue<string>()??throw new JsonException("Script.TypeMissing");
        var next=(JsonObject)document.DeepClone();var list=next["Scripts"]!.AsArray();var id=entityStableId.ToString(CultureInfo.InvariantCulture);
        var entry=list.OfType<JsonObject>().FirstOrDefault(e=>e["Entity"]?.GetValue<string>()==id&&e["Type"]?.GetValue<string>()==type);
        if(change["Remove"]?.GetValue<bool>()==true){if(entry is not null)list.Remove(entry);ReplaceDocument(next);return;}
        if(!schemas.TryGetValue(type,out var schema))throw new InvalidOperationException("Script.TypeMissing");
        if(change["Add"]?.GetValue<bool>()==true)
        {
            if(entry is not null)return;
            var fields=new JsonObject();foreach(var field in schema.OfType<JsonObject>())fields[field["Name"]!.GetValue<string>()]=new JsonObject{["Kind"]=field["Kind"]!.DeepClone(),["Value"]=field["Value"]?.DeepClone()};
            list.Add(new JsonObject{["Type"]=type,["Entity"]=id,["Enabled"]=true,["Fields"]=fields});ReplaceDocument(next);return;
        }
        if(entry is null)throw new InvalidOperationException("Script.InvalidAttachment");
        if(change["Enabled"] is not null)entry["Enabled"]=change["Enabled"]!.GetValue<bool>();
        if(change["Field"] is not null)
        {
            var name=change["Field"]!.GetValue<string>();var field=schema.OfType<JsonObject>().FirstOrDefault(f=>f["Name"]!.GetValue<string>()==name)??throw new InvalidOperationException("Script.FieldMissing");
            var kind=field["Kind"]!.GetValue<string>();var value=change["Value"];
            if(!ValidValue(kind,value))throw new ArgumentException("Script.FieldValueInvalid");
            entry["Fields"]![name]=new JsonObject{["Kind"]=kind,["Value"]=value?.DeepClone()};
        }
        ReplaceDocument(next);
    }
    private static bool ValidValue(string kind,JsonNode? value)
    {
        try
        {
            using var json=JsonDocument.Parse(value?.ToJsonString()??"null");return ScriptFields.TryReadValue(kind,json.RootElement,out _);
        }
        catch(Exception exception)when(exception is JsonException or InvalidOperationException or FormatException or OverflowException or NullReferenceException or ArgumentException){return false;}
    }
    public void Save(string path)
    {
        var target=documentRoot is null?Path.GetFullPath(path):CheckedPath(documentRoot,path);
        if(documentPath is not null)
        {
            if(!target.Equals(documentPath,StringComparison.OrdinalIgnoreCase))throw new InvalidOperationException("Script.DocumentPathInvalid");
            SaveDocument();return;
        }
        SaveCore(target);
    }
    public void SaveDocument(bool allowOverwrite=false)
    {
        if(documentRoot is null||documentPath is null)throw new InvalidOperationException("Script.DocumentPathInvalid");
        var target=CheckedPath(documentRoot,documentPath);
        if(!allowOverwrite&&HashFile(target)!=sourceDigest)throw new InvalidOperationException("Script.DocumentExternalChanged");
        SaveCore(target,allowOverwrite?null:sourceDigest);
    }
    private void SaveCore(string target,string? expectedDigest=null)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        if(documentRoot is not null)CheckedPath(documentRoot,target);
        var content=Json;
        var temporary=target+"."+Guid.NewGuid().ToString("N")+".tmp";
        try
        {
            File.WriteAllText(temporary,content,new UTF8Encoding(false));
            if(expectedDigest is not null&&HashFile(target)!=expectedDigest)throw new InvalidOperationException("Script.DocumentExternalChanged");
            File.Move(temporary,target,true);
            savedDocument=(JsonObject)document.DeepClone();restoredDirty=false;
            sourceDigest=Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(content))).ToLowerInvariant();
        }
        finally{if(File.Exists(temporary))File.Delete(temporary);}
    }
    internal string ProcessDocumentRequest(JsonObject command,bool readOnly)
    {
        var operation="";
        try
        {
            operation=command["Document"]?.GetValue<string>()??throw new JsonException("Script.DocumentInvalid");
            if(operation=="Snapshot")return SnapshotDocument();
            if(readOnly&&(operation is "Save" or "Restore"))throw new InvalidOperationException("Bridge.EditorReadOnly");
            if(operation=="Save")SaveDocument(command["AllowOverwrite"]?.GetValue<bool>()??false);
            else if(operation is "Restore" or "ValidateRestore")
            {
                var content=command["Content"]?.GetValue<string>()??throw new JsonException("Script.DocumentInvalid");
                var path=command["RelativePath"]?.GetValue<string>()??throw new JsonException("Script.DocumentInvalid");
                var digest=command["SourceDigest"]?.GetValue<string>()??throw new JsonException("Script.DocumentInvalid");
                if(operation=="ValidateRestore")ValidateRestore(content,path,digest);else RestoreDocument(content,path,digest);
            }
            else throw new JsonException("Script.DocumentInvalid");
            return "{}";
        }
        catch(Exception exception)when(exception is JsonException or InvalidOperationException or ArgumentException or IOException or UnauthorizedAccessException)
        {
            var key=exception.Message is "Script.DocumentPathInvalid" or "Script.DocumentExternalChanged" or "Script.DocumentRevisionExhausted" or "Bridge.EditorReadOnly"
                ?exception.Message:exception is IOException or UnauthorizedAccessException
                    ?(operation=="Save"?"Script.DocumentWriteFailed":"Script.DocumentReadFailed"):"Script.DocumentInvalid";
            return new JsonObject{["Error"]=key}.ToJsonString();
        }
    }
}

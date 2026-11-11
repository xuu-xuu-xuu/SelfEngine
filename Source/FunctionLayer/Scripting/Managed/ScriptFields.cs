using System.Globalization;
using System.Numerics;
using System.Reflection;
using System.Text.Json;

namespace SelfEngine;

public readonly record struct ExposedField(string Name, string Kind, object? Value);

public static class ScriptFields
{
    internal static void ClearSceneReferences(Behaviour value,IReadOnlySet<ulong> invalidated)
    {
        foreach(var field in Fields(value.GetType()))if(field.FieldType==typeof(EntityReference)&&field.GetValue(value) is EntityReference reference&&invalidated.Contains(reference.StableId))field.SetValue(value,new EntityReference(0));
    }
    private static readonly JsonSerializerOptions JsonOptions = new() { IncludeFields = true, WriteIndented = true };
    private static IEnumerable<FieldInfo> Fields(Type type) => type.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
        .Where(field => field.IsDefined(typeof(ExposeAttribute)) && !field.IsInitOnly && !field.IsStatic);
    private static string? Kind(Type type) => type == typeof(int) ? "i32" : type == typeof(float) ? "f32"
        : type == typeof(double) ? "f64" : type == typeof(bool) ? "bool" : type == typeof(string) ? "string"
        : type == typeof(Vector3) ? "vec3" : type == typeof(EntityReference) ? "entity" : null;
    internal static bool TryReadValue(string kind,JsonElement json,out object? value)
    {
        value=null;
        try
        {
            switch(kind)
            {
                case "i32":value=json.GetInt32();return true;
                case "f32":var single=json.GetSingle();if(!float.IsFinite(single))return false;value=single;return true;
                case "f64":var number=json.GetDouble();if(!double.IsFinite(number))return false;value=number;return true;
                case "bool":value=json.GetBoolean();return true;
                case "string":var text=json.ValueKind==JsonValueKind.Null?null:json.GetString();if(text?.Length>65536)return false;value=text;return true;
                case "vec3":
                    var vector=new Vector3(json.GetProperty("X").GetSingle(),json.GetProperty("Y").GetSingle(),json.GetProperty("Z").GetSingle());
                    if(!float.IsFinite(vector.X)||!float.IsFinite(vector.Y)||!float.IsFinite(vector.Z))return false;value=vector;return true;
                case "entity":
                    if(!ulong.TryParse(json.GetString(),NumberStyles.None,CultureInfo.InvariantCulture,out var reference))return false;value=new EntityReference(reference);return true;
                default:return false;
            }
        }
        catch(Exception exception)when(exception is JsonException or InvalidOperationException or FormatException or OverflowException or KeyNotFoundException or ArgumentException){return false;}
    }
    public static IReadOnlyList<ExposedField> Describe(Behaviour behaviour) => Fields(behaviour.GetType())
        .Where(field => Kind(field.FieldType) is not null)
        .Select(field => new ExposedField(field.Name, Kind(field.FieldType)!, field.GetValue(behaviour))).ToArray();

    public static string Save(ScriptSystem system)
    {
        var scripts = system.Attachments.Where(value => value.Entity.IsAlive).Select(value => new SavedScript
        {
            Type = ScriptRegistry.IdOf(value.GetType()),
            Entity = value.Entity.StableId.ToString(CultureInfo.InvariantCulture),
            Enabled = value.Enabled,
            Fields = Fields(value.GetType()).Where(field => Kind(field.FieldType) is not null).ToDictionary(
                field => field.Name,
                field => new SavedField
                {
                    Kind = Kind(field.FieldType)!,
                    Value = field.FieldType == typeof(EntityReference)
                        ? JsonSerializer.SerializeToElement(((EntityReference)field.GetValue(value)!).StableId.ToString(CultureInfo.InvariantCulture))
                        : JsonSerializer.SerializeToElement(field.GetValue(value), field.FieldType, JsonOptions)
                }, StringComparer.Ordinal)
        }).ToArray();
        return JsonSerializer.Serialize(new SavedDocument { Version = 1, Scripts = scripts }, JsonOptions);
    }

    // 先解析并构建候选列表，再更换附件；损坏 JSON 不清除原有行为配置。
    public static bool Load(ScriptSystem system, string json)
    {
        if (system.IsRunning) throw new InvalidOperationException("Script.LoadWhileRunning");
        SavedDocument? document;
        try
        {
            if (json.Length > 16*1024*1024) throw new JsonException();
            document = JsonSerializer.Deserialize<SavedDocument>(json, JsonOptions);
            if (document is null || document.Version != 1 || document.Scripts is null || document.Scripts.Length > 100000)
                throw new JsonException();
        }
        catch (Exception exception) when (exception is JsonException or NotSupportedException)
        {
            system.Report(null, "Script.DocumentInvalid", exception.GetType().Name);
            return false;
        }
        var candidates = new List<(Entity Entity, Behaviour Value)>();
        try
        {
            foreach (var saved in document.Scripts)
            {
                if (saved is null || string.IsNullOrWhiteSpace(saved.Type) || saved.Fields is null
                    || !ulong.TryParse(saved.Entity, NumberStyles.None, CultureInfo.InvariantCulture, out var stableId))
                    throw new JsonException();
                var entity = system.Session.FindEntity(stableId);
                if (entity is null) { system.Report(null, "Script.EntityMissing", saved.Entity); continue; }
                Behaviour? value;
                try { value = system.Registry.Create(saved.Type); }
                catch (Exception exception) { system.Report(null, "Script.ConstructionFailed", saved.Type + ": " + exception.GetType().Name); continue; }
                if (value is null) { system.Report(null, "Script.TypeMissing", saved.Type); continue; }
                value.Entity = entity;
                value.EnabledValue = saved.Enabled;
                var fields = Fields(value.GetType()).ToDictionary(field => field.Name, StringComparer.Ordinal);
                foreach (var (name, item) in saved.Fields)
                {
                    if (!fields.TryGetValue(name, out var field)) { system.Report(value, "Script.FieldMissing", name); continue; }
                    if (item is null || Kind(field.FieldType) != item.Kind || item.Kind is null)
                    { system.Report(value, "Script.FieldTypeChanged", name); continue; }
                    try
                    {
                        if(!TryReadValue(item.Kind,item.Value,out var fieldValue))throw new JsonException();
                        if(fieldValue is EntityReference reference&&reference.StableId!=0&&system.Session.FindEntity(reference.StableId) is null)system.Report(value,"Script.ReferenceMissing",name);
                        field.SetValue(value, fieldValue);
                    }
                    catch (Exception exception) when (exception is JsonException or InvalidOperationException or ArgumentException or FormatException)
                    { system.Report(value, "Script.FieldValueInvalid", name); }
                }
                candidates.Add((entity, value));
            }
        }
        catch (JsonException)
        {
            system.Report(null, "Script.DocumentInvalid", "schema");
            return false;
        }
        system.Stop();
        foreach (var (entity, value) in candidates) system.Attach(entity, value);
        return true;
    }

    public static void SaveFile(ScriptSystem system, string path)
    {
        var fullPath = Path.GetFullPath(path);
        var directory = Path.GetDirectoryName(fullPath)!;
        Directory.CreateDirectory(directory);
        var temporary = Path.Combine(directory, Path.GetFileName(path) + "." + Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            File.WriteAllText(temporary, Save(system), new System.Text.UTF8Encoding(false));
            File.Move(temporary, fullPath, true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    private sealed class SavedDocument { public int Version { get; set; } public SavedScript[]? Scripts { get; set; } }
    private sealed class SavedScript
    {
        public string Type { get; set; } = "";
        public string Entity { get; set; } = "";
        public bool Enabled { get; set; }
        public Dictionary<string,SavedField>? Fields { get; set; }
    }
    private sealed class SavedField { public string? Kind { get; set; } public JsonElement Value { get; set; } }
}

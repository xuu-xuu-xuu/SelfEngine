using System.Reflection;
using System.Numerics;

namespace SelfEngine;

[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class ScriptIdAttribute(string id) : Attribute { public string Id { get; } = id; }
[AttributeUsage(AttributeTargets.Field)]
public sealed class ExposeAttribute : Attribute;

public readonly record struct EntityReference(ulong StableId)
{
    public Entity? Resolve(EngineSession session) => StableId == 0 ? null : session.FindEntity(StableId);
    public static EntityReference From(Entity? entity) => new(entity?.StableId ?? 0);
}

public abstract class Behaviour
{
    internal ScriptSystem? Owner;
    internal bool EnabledValue = true;
    public Entity Entity { get; internal set; } = null!;
    public ScriptSystem Scripts => Owner ?? throw new InvalidOperationException("Script.NotAttached");
    public ScriptInput Input => Scripts.Input;
    public virtual bool UpdateWhenPaused => false;
    public bool Enabled
    {
        get => EnabledValue;
        set { if (Owner is null) EnabledValue = value; else {if(!value)EnabledValue=false;Owner.SetEnabled(this, value);} }
    }
    public virtual void OnCreate() { }
    public virtual void OnEnable() { }
    public virtual void OnUpdate(double deltaSeconds) { }
    public virtual void OnPreTick(double deltaSeconds) { }
    public virtual void OnPostTick(double deltaSeconds) { }
    public virtual void OnFixedUpdate(double deltaSeconds) { }
    public virtual void OnFixedPostUpdate(double deltaSeconds) { }
    public virtual void OnDisable() { }
    public virtual void OnDestroy() { }
    protected void DestroySelf() => Scripts.Destroy(Entity);
    protected IDisposable Subscribe<T>(Action<T> handler) => Scripts.Events.Subscribe(this, handler);
}

public sealed class ScriptRegistry
{
    private readonly Dictionary<string, Type> types = new(StringComparer.Ordinal);
    public IReadOnlyDictionary<string, Type> Types => types;
    internal void Clear() => types.Clear();
    public void Register<T>() where T : Behaviour, new() => Register(typeof(T));
    public void Register(Type type)
    {
        if (type.IsAbstract || !typeof(Behaviour).IsAssignableFrom(type) || type.GetConstructor(Type.EmptyTypes) is null)
            throw new ArgumentException("Script.InvalidType", nameof(type));
        string id = IdOf(type);
        if(string.IsNullOrWhiteSpace(id)||id.Length>256||id.Any(char.IsControl))throw new ArgumentException("Script.InvalidType",nameof(type));
        if (!types.TryAdd(id, type) && types[id] != type) throw new InvalidOperationException("Script.DuplicateType");
    }
    public void Discover(Assembly assembly)
    {
        foreach (var type in assembly.GetTypes().Where(t => t.IsPublic && !t.IsAbstract && typeof(Behaviour).IsAssignableFrom(t))) Register(type);
    }
    public static string IdOf(Type type) => type.GetCustomAttribute<ScriptIdAttribute>()?.Id ?? type.FullName ?? type.Name;
    internal Behaviour? Create(string id) => types.TryGetValue(id, out var type) ? (Behaviour?)Activator.CreateInstance(type) : null;
}

public enum MouseButton { Left, Right, Middle }
public readonly record struct PointerInput(Vector2 Position, uint ButtonsDown, uint ButtonsPressed, uint ButtonsReleased, float Wheel, bool Consumed)
{
    public bool Down(MouseButton button) => !Consumed && (ButtonsDown & (1u << (int)button)) != 0;
    public bool Pressed(MouseButton button) => !Consumed && (ButtonsPressed & (1u << (int)button)) != 0;
    public bool Released(MouseButton button) => !Consumed && (ButtonsReleased & (1u << (int)button)) != 0;
    internal PointerInput WithoutEdges() => this with { ButtonsPressed = 0, ButtonsReleased = 0, Wheel = 0 };
    internal PointerInput MergeEdges(PointerInput newer) => newer.Consumed ? newer.WithoutEdges()
        : newer with { ButtonsPressed = ButtonsPressed | newer.ButtonsPressed, ButtonsReleased = ButtonsReleased | newer.ButtonsReleased, Wheel = Wheel + newer.Wheel };
}
public sealed class ScriptInput
{
    private readonly HashSet<int> down;
    private readonly HashSet<int> pressed;
    private readonly HashSet<int> released;
    public static ScriptInput Empty { get; } = new([], [], []);
    public bool KeyboardConsumed { get; }
    public PointerInput Pointer { get; }
    public ScriptInput(IEnumerable<int> down, IEnumerable<int> pressed, IEnumerable<int> released, bool keyboardConsumed = false, PointerInput pointer = default)
    {
        this.down = new(down); this.pressed = new(pressed); this.released = new(released);
        if (this.down.Concat(this.pressed).Concat(this.released).Any(key => key is < 0 or > 255)) throw new ArgumentException("Script.InvalidKey");
        KeyboardConsumed = keyboardConsumed;
        Pointer = pointer.Consumed ? pointer with { ButtonsDown = 0, ButtonsPressed = 0, ButtonsReleased = 0, Wheel = 0 } : pointer;
    }
    public bool Down(int key) => !KeyboardConsumed && down.Contains(key);
    public bool Pressed(int key) => !KeyboardConsumed && pressed.Contains(key);
    public bool Released(int key) => !KeyboardConsumed && released.Contains(key);
    internal ScriptInput WithoutEdges() => new(down, [], [], KeyboardConsumed, Pointer.WithoutEdges());
    internal ScriptInput MergeEdges(ScriptInput newer) => newer.KeyboardConsumed
        ? new(newer.down, [], [], true, Pointer.MergeEdges(newer.Pointer))
        : new(newer.down, pressed.Concat(newer.pressed), released.Concat(newer.released), false, Pointer.MergeEdges(newer.Pointer));
}

public readonly record struct ScriptDiagnostic(string Key, ulong EntityHandle, string ScriptType, string Detail, string Phase)
{
    // 保留原四参构造和四字段解构，已编译的项目也能继续调用原签名。
    public ScriptDiagnostic(string Key,ulong EntityHandle,string ScriptType,string Detail):this(Key,EntityHandle,ScriptType,Detail,""){}
    public void Deconstruct(out string Key,out ulong EntityHandle,out string ScriptType,out string Detail)
    {Key=this.Key;EntityHandle=this.EntityHandle;ScriptType=this.ScriptType;Detail=this.Detail;}
}


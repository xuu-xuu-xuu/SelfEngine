using System.Numerics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace SelfEngine;

public enum UiKind { Panel, Label, Image, Button, Checkbox, Slider, ProgressBar, ScrollView, TextInput }
public enum UiLayout { Absolute, Horizontal, Vertical }
public enum UiEventKind { Click, ValueChanged, TextChanged, FocusChanged, PointerEnter, PointerLeave }
public enum UiPointerOperation { Move, Down, Up, Wheel, Cancel }
[StructLayout(LayoutKind.Sequential)]
public struct UiStyle
{
    public Vector2 AnchorMin, AnchorMax, Pivot, Offset, Size;
    public float Padding, Spacing;
    public UiLayout Layout;
    private int clip;
    public bool Clip { readonly get => clip != 0; set => clip = value ? 1 : 0; }
    public static UiStyle Default => new() { Size = new(120,32) };
}
public sealed record UiBinding(UiEventKind Event,ulong Entity,string Script,string Action)
{
    internal static bool ValidName(string? value)=>value is {Length:>0 and <=128}&&value.All(c=>(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c is '.' or '_' or ':' or '-');
}
public sealed record UiEvent(UiEventKind Kind, ulong Target, double Value, string Text)
{
    public UiBinding? Binding {get;init;}
    [JsonIgnore] internal uint? SourceEpoch {get;init;}
}
public sealed record UiVisual(ulong Id, UiKind Kind, float X, float Y, float Width, float Height,
    float ClipX, float ClipY, float ClipWidth, float ClipHeight, string Text, string ImageAsset,
    string Composition, double Value, float Opacity, bool Hovered, bool Pressed, bool Focused, bool Enabled)
{
    public ulong Caret { get; init; }
    public ulong SelectionAnchor { get; init; }
    public ulong CompositionCaret { get; init; }
}

public sealed class UiElement
{
    private readonly GameUi owner;
    private readonly uint epoch;
    internal UiElement(GameUi owner, ulong id) { this.owner = owner; Id = id; epoch = owner.Epoch; }
    public ulong Id { get; }
    internal GameUi Owner => owner;
    internal void Check() { if (epoch != owner.Epoch) throw new EngineException("Bridge.InvalidUi", 2); }
    private void Property(int property, double value) { Check(); owner.Session.Invoke(session => { EngineSession.Check(NativeMethods.UiProperty(session, Id, property, value)); return 0; }); }
    public void SetStyle(UiStyle style) { Check(); owner.Session.Invoke(session => { EngineSession.Check(NativeMethods.UiStyle(session, Id, in style)); return 0; }); }
    public void SetText(string text) { Check(); ArgumentNullException.ThrowIfNull(text); owner.Session.Invoke(session => { EngineSession.Check(NativeMethods.UiText(session, Id, text, (uint)Encoding.UTF8.GetByteCount(text))); return 0; }); }
    public void SetImage(string assetId) { Check(); owner.Session.Invoke(session => { EngineSession.Check(NativeMethods.UiImage(session, Id, assetId, checked((uint)Encoding.UTF8.GetByteCount(assetId)))); return 0; }); }
    public void FadeTo(float opacity, double seconds) { Check(); owner.Session.Invoke(session => { EngineSession.Check(NativeMethods.UiFade(session, Id, opacity, seconds)); return 0; }); }
    public void MoveTo(Vector2 offset,double seconds) { Check(); owner.Session.Invoke(session => { EngineSession.Check(NativeMethods.UiMove(session,Id,offset.X,offset.Y,seconds)); return 0; }); }
    public void ColorTo(Vector4 color,double seconds) { Check(); owner.Session.Invoke(session => { EngineSession.Check(NativeMethods.UiColor(session,Id,color.X,color.Y,color.Z,color.W,seconds)); return 0; }); }
    public void SetColor(Vector4 color) => ColorTo(color,0);
    public void CancelTransitions() => Property(6,1);
    public void SetValue(double value) => Property(0, value);
    public void SetVisible(bool value) => Property(1, value ? 1 : 0);
    public void SetEnabled(bool value) => Property(2, value ? 1 : 0);
    public void SetPointerBlocking(bool value) => Property(4, value ? 1 : 0);
    public void SetStep(double value) => Property(5,value);
    public void Focus() => Property(3, 1);
    public void SetRange(double minimum, double maximum) { Check(); owner.Session.Invoke(session => { EngineSession.Check(NativeMethods.UiRange(session, Id, minimum, maximum)); return 0; }); }
    public void Remove() { Check(); owner.Session.Invoke(session => { EngineSession.Check(NativeMethods.UiRemove(session, Id)); return 0; }); }
    public IReadOnlyList<UiBinding> Bindings {get{Check();return owner.ReadBindings(Id);}}
    public void SetBinding(UiBinding binding)
    {
        Check();ArgumentNullException.ThrowIfNull(binding);ArgumentNullException.ThrowIfNull(binding.Script);ArgumentNullException.ThrowIfNull(binding.Action);
        owner.Session.Invoke(session=>{EngineSession.Check(NativeMethods.UiBind(session,Id,(int)binding.Event,binding.Entity,binding.Script,checked((uint)Encoding.UTF8.GetByteCount(binding.Script)),binding.Action,checked((uint)Encoding.UTF8.GetByteCount(binding.Action))));return 0;});
    }
    public void Bind(UiEventKind kind,Behaviour target,string action)
    {
        ArgumentNullException.ThrowIfNull(target);if(target.Owner is null||target.Entity.Session!=owner.Session)throw new ArgumentException("Script.NotAttached");
        SetBinding(new(kind,target.Entity.StableId,ScriptRegistry.IdOf(target.GetType()),action));
    }
    public void Unbind(UiEventKind kind)
    {Check();owner.Session.Invoke(session=>{EngineSession.Check(NativeMethods.UiBind(session,Id,(int)kind,0,null,0,null,0));return 0;});}
    public IDisposable Subscribe(Behaviour ownerBehaviour, UiEventKind kind, Action<UiEvent> action)
    {
        Check();
        if (ownerBehaviour.Entity.Session != owner.Session) throw new ArgumentException("Bridge.InvalidUi");
        return ownerBehaviour.Scripts.Events.Subscribe<UiEvent>(ownerBehaviour, value => { if (epoch == owner.Epoch && (!value.SourceEpoch.HasValue||value.SourceEpoch==epoch) && value.Target == Id && value.Kind == kind && owner.Find(Id) is not null) action(value); });
    }
}

public sealed unsafe class GameUi
{
    internal EngineSession Session { get; }
    internal uint Epoch { get; private set; }
    internal void InvalidateProxies() => ++Epoch;
    internal GameUi(EngineSession session) => Session = session;
    public UiElement? Find(ulong id) => Session.Invoke<UiElement?>(session => { EngineSession.Check(NativeMethods.UiExists(session,id,out var exists)); return exists != 0 ? new UiElement(this,id) : null; });
    public UiElement Create(UiKind kind, UiElement? parent = null)
    {
        if (parent is not null) { parent.Check(); if (parent.Owner != this) throw new ArgumentException("Bridge.InvalidUi"); }
        return Session.Invoke(session => { EngineSession.Check(NativeMethods.UiCreate(session, (int)kind, parent?.Id ?? 0, out var id)); return new UiElement(this,id); });
    }
    public void Layout(float width, float height) => Session.Invoke(session => { EngineSession.Check(NativeMethods.UiLayout(session, width, height)); return 0; });
    public bool Pointer(UiPointerOperation operation, Vector2 position, float wheel = 0) => Session.Invoke(session => { EngineSession.Check(NativeMethods.UiPointer(session, (int)operation, position.X, position.Y, wheel, out var consumed)); return consumed != 0; });
    public bool Key(uint virtualKey, bool shift = false, bool control = false) => Session.Invoke(session => { EngineSession.Check(NativeMethods.UiKey(session, virtualKey, shift ? 1 : 0, control ? 1 : 0, out var consumed)); return consumed != 0; });
    public bool InputText(string text, bool composition = false) => Session.Invoke(session => { EngineSession.Check(NativeMethods.UiInputText(session, text, checked((uint)Encoding.UTF8.GetByteCount(text)), composition ? 1 : 0, out var consumed)); return consumed != 0; });
    public bool ConsumesKeyboard => Session.Invoke(session => { EngineSession.Check(NativeMethods.UiKeyboard(session, out var value)); return value != 0; });
    private string ReadJson(int kind) => Session.Invoke(session => EngineSession.ReadString((byte* buffer, uint capacity, out uint required) => NativeMethods.UiJson(session, kind, buffer, capacity, out required)));
    public IReadOnlyList<UiVisual> Visuals() => JsonSerializer.Deserialize<List<UiVisual>>(ReadJson(1)) ?? [];
    public IReadOnlyList<UiEvent> DrainEvents()
    {var epoch=Epoch;return (JsonSerializer.Deserialize<List<UiEvent>>(ReadJson(2))??[]).Select(value=>value with{SourceEpoch=epoch}).ToArray();}
    internal IReadOnlyList<UiBinding> ReadBindings(ulong id)=>Session.Invoke<IReadOnlyList<UiBinding>>(session=>JsonSerializer.Deserialize<List<UiBinding>>(EngineSession.ReadString((byte* buffer,uint capacity,out uint required)=>NativeMethods.UiBindings(session,id,buffer,capacity,out required)))??[]);
    public string Save() => ReadJson(0);
    public void Load(string json)
    {
        Session.Invoke(session => { EngineSession.Check(NativeMethods.UiLoad(session, json, checked((uint)Encoding.UTF8.GetByteCount(json)))); return 0; });
        ++Epoch;
    }
}
internal static unsafe partial class NativeMethods
{
    [LibraryImport(Library, EntryPoint="se_ui_create")]
    internal static partial int UiCreate(ulong session,int kind,ulong parent,out ulong id);
    [LibraryImport(Library, EntryPoint="se_ui_remove")]
    internal static partial int UiRemove(ulong session,ulong id);
    [LibraryImport(Library, EntryPoint="se_ui_set_style")]
    internal static partial int UiStyle(ulong session,ulong id,in SelfEngine.UiStyle style);
    [LibraryImport(Library, EntryPoint="se_ui_set_text", StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int UiText(ulong session,ulong id,string text,uint length);
    [LibraryImport(Library, EntryPoint="se_ui_set_image", StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int UiImage(ulong session,ulong id,string asset,uint length);
    [LibraryImport(Library, EntryPoint="se_ui_exists")]
    internal static partial int UiExists(ulong session,ulong id,out int exists);
    [LibraryImport(Library, EntryPoint="se_ui_bind_action", StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int UiBind(ulong session,ulong id,int kind,ulong entity,string? script,uint scriptLength,string? action,uint actionLength);
    [LibraryImport(Library, EntryPoint="se_ui_read_bindings")]
    internal static partial int UiBindings(ulong session,ulong id,byte* buffer,uint capacity,out uint required);
    [LibraryImport(Library, EntryPoint="se_ui_fade")]
    internal static partial int UiFade(ulong session,ulong id,float opacity,double seconds);
    [LibraryImport(Library, EntryPoint="se_ui_move")]
    internal static partial int UiMove(ulong session,ulong id,float x,float y,double seconds);
    [LibraryImport(Library, EntryPoint="se_ui_color")]
    internal static partial int UiColor(ulong session,ulong id,float r,float g,float b,float a,double seconds);
    [LibraryImport(Library, EntryPoint="se_ui_set_property")]
    internal static partial int UiProperty(ulong session,ulong id,int property,double value);
    [LibraryImport(Library, EntryPoint="se_ui_set_range")]
    internal static partial int UiRange(ulong session,ulong id,double minimum,double maximum);
    [LibraryImport(Library, EntryPoint="se_ui_layout")]
    internal static partial int UiLayout(ulong session,float width,float height);
    [LibraryImport(Library, EntryPoint="se_ui_pointer")]
    internal static partial int UiPointer(ulong session,int operation,float x,float y,float wheel,out int consumed);
    [LibraryImport(Library, EntryPoint="se_ui_key")]
    internal static partial int UiKey(ulong session,uint key,int shift,int control,out int consumed);
    [LibraryImport(Library, EntryPoint="se_ui_input_text", StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int UiInputText(ulong session,string text,uint length,int composition,out int consumed);
    [LibraryImport(Library, EntryPoint="se_ui_consumes_keyboard")]
    internal static partial int UiKeyboard(ulong session,out int consumed);
    [LibraryImport(Library, EntryPoint="se_ui_read_json")]
    internal static partial int UiJson(ulong session,int kind,byte* buffer,uint capacity,out uint required);
    [LibraryImport(Library, EntryPoint="se_ui_load_json", StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int UiLoad(ulong session,string text,uint length);
}

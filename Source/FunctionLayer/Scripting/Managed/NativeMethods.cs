using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace SelfEngine;

internal static unsafe partial class NativeMethods
{
    private const string Library = "EngineNative";
    [StructLayout(LayoutKind.Sequential)]
    internal struct AbiInfo { internal uint Version, TransformSize, PointerSize, Reserved; }
    [StructLayout(LayoutKind.Sequential)]
    internal struct InputState { internal fixed byte Down[256]; internal fixed byte Pressed[256]; internal fixed byte Released[256]; internal int KeyboardConsumed; }
    [StructLayout(LayoutKind.Sequential)]
    internal struct PointerState { internal int X,Y; internal uint Down,Pressed,Released; internal float Wheel; internal int Consumed; }

    [LibraryImport(Library, EntryPoint = "se_get_abi_info")]
    internal static partial int Abi(out AbiInfo info);
    [LibraryImport(Library, EntryPoint = "se_get_last_error")]
    internal static partial int LastError(byte* buffer, uint capacity, out uint required);
    [LibraryImport(Library, EntryPoint = "se_session_create")]
    internal static partial int Create(out ulong handle);
    [LibraryImport(Library, EntryPoint = "se_session_close")]
    internal static partial int Close(ulong handle);
    [LibraryImport(Library, EntryPoint = "se_session_set_update")]
    internal static partial int SetUpdate(ulong handle, delegate* unmanaged[Cdecl]<ulong,double,nint,int> callback, nint context);
    [LibraryImport(Library, EntryPoint = "se_session_step")]
    internal static partial int Step(ulong handle, double deltaSeconds);
    [LibraryImport(Library, EntryPoint = "se_session_get_value")]
    internal static partial int GetValue(ulong handle, out double value);
    [LibraryImport(Library, EntryPoint = "se_session_set_value")]
    internal static partial int SetValue(ulong handle, double value);
    [LibraryImport(Library, EntryPoint = "se_session_get_step_count")]
    internal static partial int GetStepCount(ulong handle, out ulong count);
    [LibraryImport(Library, EntryPoint = "se_session_get_input")]
    internal static partial int GetInput(ulong handle, out InputState input);
    [LibraryImport(Library, EntryPoint = "se_session_get_pointer")]
    internal static partial int GetPointer(ulong handle, out PointerState input);
    [LibraryImport(Library, EntryPoint = "se_session_run_window", StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int RunWindow(ulong handle, string locale, uint length, int visible, ulong maximumFrames);
    [LibraryImport(Library, EntryPoint="se_session_set_window_capture", StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int SetWindowCapture(ulong handle,string? path,uint length,ulong frame);
    [LibraryImport(Library, EntryPoint = "se_session_request_stop")]
    internal static partial int RequestStop(ulong handle);
    [LibraryImport(Library, EntryPoint="se_session_set_locale", StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int SetLocale(ulong session,string locale,uint length);
    [LibraryImport(Library, EntryPoint="se_session_text", StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int Text(ulong session,string key,uint length,byte* buffer,uint capacity,out uint required);
    [LibraryImport(Library, EntryPoint="se_session_set_asset_root", StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int SetAssetRoot(ulong session,string root,uint rootLength,string cache,uint cacheLength);
    [LibraryImport(Library, EntryPoint="se_assets_list")]
    internal static partial int AssetList(ulong session,byte* output,uint capacity,out uint required);
    [LibraryImport(Library, EntryPoint="se_assets_load_model_info", StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int LoadModelInfo(ulong session,string id,uint length,out ModelInfo info);
    [LibraryImport(Library, EntryPoint="se_session_set_editor_callback")]
    internal static partial int SetEditorCallback(ulong session,delegate* unmanaged[Cdecl]<ulong,int,nint,int> callback,nint context);
    [LibraryImport(Library, EntryPoint="se_session_run_editor", StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int RunEditor(ulong session,string root,uint rootLength,string locale,uint localeLength,int visible,ulong maximumFrames,int startPlaying);
    [LibraryImport(Library, EntryPoint="se_editor_set_playing")]
    internal static partial int EditorSetPlaying(ulong session,int playing);
    [LibraryImport(Library, EntryPoint="se_session_set_editor_data")]
    internal static partial int SetEditorData(ulong session,delegate* unmanaged[Cdecl]<ulong,ulong,byte*,uint,byte*,uint,uint*,nint,int> callback,nint context);
    [LibraryImport(Library, EntryPoint="se_editor_inspector_query")]
    internal static partial int InspectorQuery(ulong session,ulong entity,byte* output,uint capacity,out uint required);
    [LibraryImport(Library, EntryPoint="se_editor_inspector_apply", StringMarshalling=StringMarshalling.Utf8)]
    internal static partial int InspectorApply(ulong session,ulong entity,string request,uint length);
    [LibraryImport(Library, EntryPoint = "se_entity_create", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int CreateEntity(ulong handle, string name, uint length, out ulong entity);
    [LibraryImport(Library, EntryPoint = "se_entity_destroy")]
    internal static partial int DestroyEntity(ulong handle, ulong entity);
    [LibraryImport(Library, EntryPoint = "se_entity_is_alive")]
    internal static partial int IsAlive(ulong handle, ulong entity, out int alive);
    [LibraryImport(Library,EntryPoint="se_entity_get_enabled")]
    internal static partial int GetEntityEnabled(ulong session,ulong entity,out int enabled);
    [LibraryImport(Library,EntryPoint="se_entity_set_enabled")]
    internal static partial int SetEntityEnabled(ulong session,ulong entity,int enabled);
    [LibraryImport(Library,EntryPoint="se_script_component_attach")]
    internal static partial int ScriptComponentAttach(ulong session,ulong owner,ulong controlled,int enabled,out ulong component);
    [LibraryImport(Library,EntryPoint="se_script_component_remove")]
    internal static partial int ScriptComponentRemove(ulong session,ulong component);
    [LibraryImport(Library,EntryPoint="se_script_component_set_enabled")]
    internal static partial int ScriptComponentEnabled(ulong session,ulong component,int enabled);
    [LibraryImport(Library,EntryPoint="se_script_component_state")]
    internal static partial int ScriptComponentState(ulong session,ulong component,out int alive,out int active);
    [LibraryImport(Library,EntryPoint="se_script_component_count")]
    internal static partial int ScriptComponentCount(ulong session,out uint count);
    [LibraryImport(Library, EntryPoint = "se_entity_get_transform")]
    internal static partial int GetTransform(ulong handle, ulong entity, out Transform transform);
    [LibraryImport(Library, EntryPoint = "se_entity_set_transform")]
    internal static partial int SetTransform(ulong handle, ulong entity, in Transform transform);
    [LibraryImport(Library, EntryPoint = "se_entity_get_stable_id")]
    internal static partial int GetStableId(ulong handle, ulong entity, out ulong id);
    [LibraryImport(Library, EntryPoint = "se_entity_find")]
    internal static partial int FindEntity(ulong handle, ulong id, out ulong entity);
    [LibraryImport(Library, EntryPoint = "se_entity_get_name")]
    internal static partial int GetName(ulong handle, ulong entity, byte* buffer, uint capacity, out uint required);
    [LibraryImport(Library, EntryPoint = "se_scene_export")]
    internal static partial int ExportScene(ulong handle, byte* buffer, uint capacity, out uint required);
    [LibraryImport(Library, EntryPoint = "se_scene_import", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int ImportScene(ulong handle, string json, uint length);
}

internal sealed class NativeSessionHandle : SafeHandle
{
    internal GCHandle CallbackContext;
    internal NativeSessionHandle(ulong value) : base(nint.Zero, true) => SetHandle(unchecked((nint)value));
    public override bool IsInvalid => handle == nint.Zero;
    protected override bool ReleaseHandle()
    {
        // 原生 close 先解除回调，再释放弱句柄；允许终结器线程关闭已空闲的 session。
        var result = NativeMethods.Close(unchecked((ulong)handle));
        if (result != 0 && result != 2) return false;
        if (CallbackContext.IsAllocated) CallbackContext.Free();
        return true;
    }
}

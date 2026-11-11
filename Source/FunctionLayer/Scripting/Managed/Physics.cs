using System.Numerics;
using System.Runtime.InteropServices;

namespace SelfEngine;

[StructLayout(LayoutKind.Sequential)]
public struct BoxBodyDescription
{
    public Vector3 HalfExtents;
    public float Mass, Friction, Restitution;
    public uint Layer, Mask;
    public static BoxBodyDescription Dynamic => new() { HalfExtents = new(0.5f), Mass = 1, Friction = 0.6f, Layer = 1, Mask = uint.MaxValue };
    public static BoxBodyDescription Static { get { var value = Dynamic; value.Mass = 0; return value; } }
}
[StructLayout(LayoutKind.Sequential)]
internal struct NativeRayHit
{
    internal ulong Body, EntityStableId;
    internal Vector3 Point, Normal;
    internal float Distance;
    internal uint Reserved;
}
[StructLayout(LayoutKind.Sequential)]
internal struct NativeContact { internal ulong EntityA, EntityB; internal int Began, Ended; }
public readonly record struct RayHit(Entity Entity, RigidBody Body, Vector3 Point, Vector3 Normal, float Distance);
public readonly record struct PhysicsContact(EntityReference EntityA, EntityReference EntityB, bool Began, bool Ended);

[StructLayout(LayoutKind.Sequential)]
public struct RigidBodyState
{
    public Transform Transform;
    public Vector3 LinearVelocity,AngularVelocity;
    public float Mass;
    private int sleeping;
    public readonly bool Sleeping=>sleeping!=0;
}

public sealed class RigidBody
{
    internal RigidBody(EngineSession session, ulong handle, Entity entity) { Session = session; Handle = handle; Entity = entity; }
    internal EngineSession Session { get; }
    public ulong Handle { get; }
    public Entity Entity { get; }
    public RigidBodyState State=>Session.Invoke(session=>{EngineSession.Check(NativeMethods.GetBodyState(session,Handle,out var state));return state;});
    public void SetVelocity(Vector3 linear, Vector3 angular = default) => Session.Invoke(session => { EngineSession.Check(NativeMethods.SetBodyVelocity(session, Handle, linear, angular)); return 0; });
    public void ApplyForce(Vector3 force, Vector3 worldPoint) => Apply(force, worldPoint, false);
    public void ApplyImpulse(Vector3 impulse, Vector3 worldPoint) => Apply(impulse, worldPoint, true);
    private void Apply(Vector3 force, Vector3 point, bool impulse) => Session.Invoke(session => { EngineSession.Check(NativeMethods.ApplyBodyForce(session, Handle, force, point, impulse ? 1 : 0)); return 0; });
    // 尺寸由创建时的 collider 决定；缩放变化需销毁并重建 body。
    public void Teleport(Transform transform, bool clearVelocity = true) => Session.Invoke(session => { EngineSession.Check(NativeMethods.TeleportBody(session, Handle, in transform, clearVelocity ? 1 : 0)); return 0; });
    public void Destroy()
    {
        try{Session.Invoke(session=>{EngineSession.Check(NativeMethods.DestroyBody(session,Handle));return 0;});}
        catch(EngineException exception)when(exception.Key=="Bridge.InvalidBody"&&(Session.IsSceneRetiring(Entity.Handle)||!Entity.IsAlive)){/* 场景/GO 已代为释放原生代理，所有者清理仍可安全完成。 */}
    }
}

public sealed unsafe class Physics
{
    private readonly EngineSession session;
    internal Physics(EngineSession session) => this.session = session;
    internal object? ClockOwner;
    // 返回旧状态，场景脚本退出时可恢复；模拟和接触事件始终继续。
    public bool SetDebugDrawing(bool enabled)=>session.Invoke(handle=>{EngineSession.Check(NativeMethods.SetPhysicsDebugDraw(handle,enabled?1:0,out var previous));return previous!=0;});
    public double DroppedSeconds { get; private set; }
    public RigidBody CreateBox(Entity entity, BoxBodyDescription description)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        return session.Invoke(handle => { EngineSession.Check(NativeMethods.CreateBody(handle, entity.Handle, in description, out var body)); return new RigidBody(session, body, entity); });
    }
    public void Step(double seconds)
    {
        if (ClockOwner is not null) throw new InvalidOperationException("Script.PhysicsClockOwned");
        StepInternal(seconds);
    }
    internal void StepInternal(double seconds) => session.Invoke(handle => { EngineSession.Check(NativeMethods.PhysicsStep(handle, seconds)); return 0; });
    public uint Advance(double frameSeconds)
    {
        if (ClockOwner is not null) throw new InvalidOperationException("Script.PhysicsClockOwned");
        return session.Invoke(handle => { EngineSession.Check(NativeMethods.PhysicsAdvance(handle, frameSeconds, out var steps, out var dropped)); DroppedSeconds = dropped; return steps; });
    }
    public RayHit? Raycast(Vector3 origin, Vector3 direction, float distance, uint mask = uint.MaxValue) => session.Invoke<RayHit?>(handle =>
    {
        EngineSession.Check(NativeMethods.Raycast(handle, origin, direction, distance, mask, out var hit, out var found));
        if (found == 0) return null;
        var entity = session.FindEntity(hit.EntityStableId);
        return entity is null ? null : new RayHit(entity, new(session, hit.Body, entity), hit.Point, hit.Normal, hit.Distance);
    });
    public IReadOnlyList<PhysicsContact> DrainContacts() => session.Invoke<IReadOnlyList<PhysicsContact>>(handle =>
    {
        var result = NativeMethods.DrainContacts(handle, null, 0, out var required);
        if (result != 3) EngineSession.Check(result);
        if (required == 0) return [];
        var events = new NativeContact[required];
        fixed (NativeContact* buffer = events) EngineSession.Check(NativeMethods.DrainContacts(handle, buffer, required, out _));
        return events.Select(e => new PhysicsContact(new(e.EntityA), new(e.EntityB), e.Began != 0, e.Ended != 0)).ToArray();
    });
}

internal static unsafe partial class NativeMethods
{
    [LibraryImport(Library, EntryPoint = "se_physics_set_debug_draw")]
    internal static partial int SetPhysicsDebugDraw(ulong session,int enabled,out int previous);
    [LibraryImport(Library, EntryPoint = "se_body_create")]
    internal static partial int CreateBody(ulong session, ulong entity, in BoxBodyDescription description, out ulong body);
    [LibraryImport(Library, EntryPoint = "se_body_destroy")]
    internal static partial int DestroyBody(ulong session, ulong body);
    [LibraryImport(Library, EntryPoint = "se_body_get_state")]
    internal static partial int GetBodyState(ulong session, ulong body, out RigidBodyState state);
    [LibraryImport(Library, EntryPoint = "se_body_set_velocity")]
    internal static partial int SetBodyVelocity(ulong session, ulong body, Vector3 linear, Vector3 angular);
    [LibraryImport(Library, EntryPoint = "se_body_apply_force")]
    internal static partial int ApplyBodyForce(ulong session, ulong body, Vector3 force, Vector3 point, int impulse);
    [LibraryImport(Library, EntryPoint = "se_body_teleport")]
    internal static partial int TeleportBody(ulong session, ulong body, in Transform transform, int clearVelocity);
    [LibraryImport(Library, EntryPoint = "se_physics_step")]
    internal static partial int PhysicsStep(ulong session, double seconds);
    [LibraryImport(Library, EntryPoint = "se_physics_advance")]
    internal static partial int PhysicsAdvance(ulong session, double seconds, out uint steps, out double dropped);
    [LibraryImport(Library, EntryPoint = "se_physics_raycast")]
    internal static partial int Raycast(ulong session, Vector3 origin, Vector3 direction, float distance, uint mask, out NativeRayHit hit, out int found);
    [LibraryImport(Library, EntryPoint = "se_physics_drain_contacts")]
    internal static partial int DrainContacts(ulong session, NativeContact* events, uint capacity, out uint required);
}

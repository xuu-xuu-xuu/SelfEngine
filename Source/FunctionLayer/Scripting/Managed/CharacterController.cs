using System.Numerics;
using System.Runtime.InteropServices;

namespace SelfEngine;

[StructLayout(LayoutKind.Sequential)]
public struct CharacterDescription
{
    public float Radius, SegmentHalfHeight, WalkSpeed, JumpSpeed, MaximumPushImpulse;
    public static CharacterDescription Default => new() { Radius = 0.3f, SegmentHalfHeight = 0.6f, WalkSpeed = 4, JumpSpeed = 5, MaximumPushImpulse = 0.5f };
}
[StructLayout(LayoutKind.Sequential)]
internal struct NativeCharacterState { internal Vector3 Position; internal float VerticalVelocity; internal int Grounded; }
public readonly record struct CharacterState(Vector3 Position, float VerticalVelocity, bool Grounded);

public sealed class CharacterController
{
    private bool destroyed;
    private void RequireAlive()
    {
        // 保持原 C ABI 的 SE_INVALID_HANDLE=2 与稳定错误键，旧调用者可沿用异常处理。
        if(destroyed)throw new EngineException("Bridge.InvalidCharacter",2);
    }
    public Entity Entity { get; }
    public CharacterState State { get; private set; }
    public CharacterController(Entity entity, CharacterDescription description)
    {
        Entity = entity;
        entity.Session.Invoke(session => { EngineSession.Check(NativeMethods.CharacterCreate(session, entity.Handle, in description)); return 0; });
        State = new(entity.Transform.Position, 0, false);
    }
    // 在 OnFixedUpdate 调用；此接口只移动角色并向刚体施加有限推力，不再次推进 PhysicsWorld。
    public CharacterState Move(Vector3 horizontalDirection, bool jumpPressed, double seconds)
    {
        RequireAlive();
        State = Entity.Session.Invoke(session =>
        {
            EngineSession.Check(NativeMethods.CharacterMove(session, Entity.Handle, horizontalDirection, jumpPressed ? 1 : 0, (float)seconds, out var state));
            return new CharacterState(state.Position, state.VerticalVelocity, state.Grounded != 0);
        });
        return State;
    }
    public void Teleport(Vector3 position)
    {
        RequireAlive();
        Entity.Session.Invoke(session => { EngineSession.Check(NativeMethods.CharacterTeleport(session, Entity.Handle, position)); return 0; });
        State = new(position, 0, false);
    }
    public void Destroy()
    {
        if(destroyed)return;
        try{Entity.Session.Invoke(session=>{EngineSession.Check(NativeMethods.CharacterDestroy(session,Entity.Handle));return 0;});}
        catch(EngineException exception)when(exception.Key=="Bridge.InvalidCharacter"&&(Entity.Session.IsSceneRetiring(Entity.Handle)||!Entity.IsAlive)){/* 场景/GO 已释放控制器代理。 */}
        destroyed=true;
    }
}
internal static partial class NativeMethods
{
    [LibraryImport(Library, EntryPoint="se_character_create")]
    internal static partial int CharacterCreate(ulong session,ulong entity,in CharacterDescription description);
    [LibraryImport(Library, EntryPoint="se_character_move")]
    internal static partial int CharacterMove(ulong session,ulong entity,Vector3 direction,int jump,float seconds,out NativeCharacterState state);
    [LibraryImport(Library, EntryPoint="se_character_teleport")]
    internal static partial int CharacterTeleport(ulong session,ulong entity,Vector3 position);
    [LibraryImport(Library, EntryPoint="se_character_destroy")]
    internal static partial int CharacterDestroy(ulong session,ulong entity);
}

using SelfEngine;

namespace ProjectScripts;

[ScriptId("sample.project_mover")]
public sealed class Mover : Behaviour
{
    [Expose] public float Speed = 2;
    public override void OnCreate() => Subscribe<MenuSettingsChanged>(settings => Speed = settings.MoveSpeed);
    public override void OnUpdate(double seconds)
    {
        var transform = Entity.Transform;
        transform.Position.X += Speed * (float)seconds;
        Entity.Transform = transform;
    }
}

[ScriptId("sample.project_rotator")]
public sealed class Rotator : Behaviour
{
    [Expose] public float Speed = 1;
    public override void OnUpdate(double seconds)
    {
        var transform = Entity.Transform;
        transform.Rotation = System.Numerics.Quaternion.Normalize(System.Numerics.Quaternion.CreateFromAxisAngle(System.Numerics.Vector3.UnitY, Speed * (float)seconds) * transform.Rotation);
        Entity.Transform = transform;
    }
}

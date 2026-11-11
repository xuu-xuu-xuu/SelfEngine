using SelfEngine;
using System.Numerics;
namespace EmptyGame;

[ScriptId("emptygame.spin")]
public sealed class Spin : Behaviour
{
    [Expose] public float RadiansPerSecond=1;
    public override void OnUpdate(double seconds)
    {
        var transform=Entity.Transform;
        transform.Rotation=Quaternion.Normalize(Quaternion.CreateFromAxisAngle(Vector3.UnitY,RadiansPerSecond*(float)seconds)*transform.Rotation);
        Entity.Transform=transform;
    }
}

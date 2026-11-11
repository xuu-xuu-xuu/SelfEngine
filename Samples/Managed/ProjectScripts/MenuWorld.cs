using System.Numerics;
using SelfEngine;

namespace ProjectScripts;

[ScriptId("sample.menu_world")]
public sealed class MenuWorld:Behaviour
{
    [Expose] public EntityReference Floor,Target;
    private RigidBody? floorBody,targetBody;
    private bool debugChanged,previousDebug;
    public int Interactions {get;private set;}
    public override void OnCreate()
    {
        var floor=Floor.Resolve(Entity.Session)??throw new ArgumentException("Bridge.InvalidEntity");
        var target=Target.Resolve(Entity.Session)??throw new ArgumentException("Bridge.InvalidEntity");
        previousDebug=Entity.Session.Physics.SetDebugDrawing(false);debugChanged=true;
        floorBody=Entity.Session.Physics.CreateBox(floor,BoxBodyDescription.Static);targetBody=Entity.Session.Physics.CreateBox(target,BoxBodyDescription.Dynamic);
        Scripts.Commands.Register<MenuInteractionCommand>(this,"menu.interact",request=>
        {
            if(request.Target.StableId!=Target.StableId||targetBody is null)return false;
            var state=targetBody.State;var direction=state.Transform.Position-Entity.Transform.Position;
            if(direction.LengthSquared()<.0001f)direction=-Vector3.UnitZ;else direction=Vector3.Normalize(direction);
            targetBody.ApplyImpulse(direction*2,state.Transform.Position);++Interactions;
            Scripts.Events.Publish(new MenuInteractionCompleted(request.Target));return true;
        });
    }
    public override void OnDestroy()
    {
        if(!Entity.Session.IsDisposed)
        {
            foreach(var body in new[]{targetBody,floorBody})if(body is not null)
            {try{body.Destroy();}catch(EngineException exception)when(exception.Key=="Bridge.InvalidBody"){/* 场景对象可能已提前销毁。 */}}
            if(debugChanged)Entity.Session.Physics.SetDebugDrawing(previousDebug);
        }
        targetBody=floorBody=null;debugChanged=false;
    }
}

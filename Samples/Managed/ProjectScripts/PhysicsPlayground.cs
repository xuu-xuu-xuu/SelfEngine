using System.Numerics;
using SelfEngine;

namespace ProjectScripts;

[ScriptId("sample.physics_playground")]
public sealed class PhysicsPlayground : Behaviour
{
    [Expose] public EntityReference ViewEntity,Floor,Crate,LeftWall,RightWall,BackWall;
    [Expose] public float InteractionImpulse=1,InteractionRange=4,LookSensitivity=.003f;
    [Expose] public bool DestroyCrateOnContact;
    private readonly List<RigidBody> bodies=[];
    private CharacterController? character;
    private Entity? view,crate;
    private RigidBody? crateBody;
    private Transform crateStart;
    private Vector3 spawn;
    private float yaw,pitch=-.3f;
    private Vector2 lastPointer;
    private bool looking,debugChanged,previousDebug,drawing;
    public override void OnCreate()
    {
        if(!float.IsFinite(InteractionImpulse)||InteractionImpulse<0||InteractionImpulse>2||!float.IsFinite(InteractionRange)||InteractionRange<=0||InteractionRange>10||!float.IsFinite(LookSensitivity)||LookSensitivity<.001f||LookSensitivity>.02f)
            throw new ArgumentException("Bridge.InvalidArgument");
        var session=Entity.Session;view=ViewEntity.Resolve(session)??throw new ArgumentException("Bridge.InvalidEntity");crate=Crate.Resolve(session)??throw new ArgumentException("Bridge.InvalidEntity");
        spawn=Entity.Transform.Position;crateStart=crate.Transform;yaw=0;pitch=-.3f;looking=false;
        previousDebug=session.Physics.SetDebugDrawing(false);debugChanged=true;drawing=false;
        foreach(var reference in new[]{Floor,LeftWall,RightWall,BackWall})
        {var entity=reference.Resolve(session)??throw new ArgumentException("Bridge.InvalidEntity");bodies.Add(session.Physics.CreateBox(entity,BoxBodyDescription.Static));}
        crateBody=session.Physics.CreateBox(crate,BoxBodyDescription.Dynamic);bodies.Add(crateBody);
        character=new CharacterController(Entity,CharacterDescription.Default);
        Subscribe<PhysicsContact>(contact=>{
            if(!DestroyCrateOnContact||!contact.Began||crate is not {IsAlive:true})return;
            if(contact.EntityA.StableId==Crate.StableId||contact.EntityB.StableId==Crate.StableId)Scripts.Destroy(crate);
        });
        UpdateView();
    }
    private Quaternion Rotation=>Quaternion.CreateFromYawPitchRoll(yaw,pitch,0);
    private void UpdateView(){if(view is not {IsAlive:true})return;var pose=view.Transform;pose.Rotation=Rotation;view.Transform=pose;}
    public override void OnUpdate(double seconds)
    {
        var pointer=Input.Pointer;
        if(pointer.Down(MouseButton.Right))
        {
            if(looking){var delta=pointer.Position-lastPointer;yaw=MathF.IEEERemainder(yaw-delta.X*LookSensitivity,2*MathF.PI);pitch=Math.Clamp(pitch-delta.Y*LookSensitivity,-1.5f,1.5f);}
            lastPointer=pointer.Position;looking=true;
        }
        else looking=false;
        UpdateView();
    }
    public override void OnFixedUpdate(double seconds)
    {
        if(character is null)return;
        if(Input.Pressed('G')){drawing=!drawing;Entity.Session.Physics.SetDebugDrawing(drawing);}
        if(Input.Pressed('R')){character.Teleport(spawn);if(crate is {IsAlive:true})crateBody!.Teleport(crateStart);yaw=0;pitch=-.3f;}
        var local=new Vector3((Input.Down('D')?1:0)-(Input.Down('A')?1:0),0,(Input.Down('S')?1:0)-(Input.Down('W')?1:0));
        character.Move(Vector3.Transform(local,Quaternion.CreateFromAxisAngle(Vector3.UnitY,yaw)),Input.Pressed(32),seconds);UpdateView();
        if(view is null)return;
        var direction=Vector3.Transform(-Vector3.UnitZ,Rotation);var origin=Entity.Transform.Position+view.Transform.Position;
        var hit=Entity.Session.Physics.Raycast(origin,direction,InteractionRange);
        if(hit is not { } target||target.Body.State.Mass<=0)return;
        if(Input.Pressed('E')){target.Body.ApplyImpulse(direction*InteractionImpulse,target.Point);Console.WriteLine($"{Entity.Session.Text("Sample.PhysicsInteract")}: {target.Entity.Name}");}
        if(Input.Pressed('F')){target.Body.ApplyForce(direction*12,target.Point);Console.WriteLine($"{Entity.Session.Text("Sample.PhysicsForce")}: {target.Entity.Name}");}
    }
    public override void OnDestroy()
    {
        if(!Entity.Session.IsDisposed)
        {
            if(character is not null&&Entity.IsAlive)try{character.Destroy();}catch(EngineException error)when(error.Key=="Bridge.InvalidCharacter"){}
            foreach(var body in bodies)if(body.Entity.IsAlive)try{body.Destroy();}catch(EngineException error)when(error.Key=="Bridge.InvalidBody"){}
            if(debugChanged)Entity.Session.Physics.SetDebugDrawing(previousDebug);
        }
        bodies.Clear();character=null;crateBody=null;crate=view=null;debugChanged=false;looking=false;
    }
}

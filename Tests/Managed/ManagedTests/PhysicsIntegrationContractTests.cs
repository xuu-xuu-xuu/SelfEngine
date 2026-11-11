using SelfEngine;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Text.Json;

internal static class PhysicsIntegrationContractTests
{
    private static void Check(bool value,string message){if(!value)throw new InvalidOperationException(message);}
    private static bool Near(double a,double b,double tolerance=0.001)=>Math.Abs(a-b)<=tolerance;
    private static Entity At(EngineSession session,string name,Vector3 position)
    {var entity=session.CreateEntity(name);var pose=Transform.Identity;pose.Position=position;entity.Transform=pose;return entity;}
    public static void Run()
    {
        Check(Marshal.SizeOf<RigidBodyState>()==72,"body state ABI layout");var records=new List<object>();
        double expectedY=20,expectedV=0;const double dt=1.0/60;double damping=Math.Exp(-.02*dt);
        for(int step=0;step<60;++step){expectedV=(expectedV-9.81*dt)*damping;expectedY+=expectedV*dt;}
        float? baseline=null;
        foreach(int fps in new[]{30,60,120,144})
        {
            using var session=new EngineSession();var entity=At(session,"falling body",new(0,20,0));var body=session.Physics.CreateBox(entity,BoxBodyDescription.Dynamic);uint steps=0;
            for(int frame=0;frame<fps;++frame)steps+=session.Physics.Advance(1.0/fps);var state=body.State;
            Check(steps==60&&Near(state.Transform.Position.Y,expectedY)&&Near(state.LinearVelocity.Y,expectedV)&&state.Mass==1&&!state.Sleeping,"cross-ABI freefall agrees with independent semi-implicit/damping reference");
            Check(entity.Transform.Position==state.Transform.Position&&Near(session.Physics.DroppedSeconds,0,1e-9)&&(!baseline.HasValue||baseline.Value==state.Transform.Position.Y),"all display rates advance same 60 steps and synchronize World exactly");baseline=state.Transform.Position.Y;
            records.Add(new{kind="freefall",fps,steps,y=state.Transform.Position.Y,vy=state.LinearVelocity.Y,expectedY,expectedV});
        }
        using(var session=new EngineSession())
        {
            var entity=At(session,"force body",new(0,20,0));var description=BoxBodyDescription.Dynamic;description.Mass=2;var body=session.Physics.CreateBox(entity,description);
            body.ApplyForce(new(12,0,0),new(0,21,0));Check(body.State.LinearVelocity.X==0,"force is accumulated until next fixed step");session.Physics.Step(dt);var first=body.State;
            Check(Near(first.LinearVelocity.X,.1*Math.Exp(-.02*dt),1e-6)&&Near(first.AngularVelocity.Z,-.6*Math.Exp(-.05*dt),1e-5),"mass and box inertia determine linear and off-center angular acceleration");
            session.Physics.Step(dt);var second=body.State;Check(Near(second.LinearVelocity.X,first.LinearVelocity.X*Math.Exp(-.02*dt),1e-6),"force accumulator clears after one step");
            body.ApplyImpulse(new(2,0,0),second.Transform.Position);var impulse=body.State;Check(Near(impulse.LinearVelocity.X,second.LinearVelocity.X+1,1e-6)&&impulse.Transform.Position==second.Transform.Position,"impulse immediately changes velocity without teleporting");
            var target=impulse.Transform;target.Position=new(3,4,5);target.Rotation=Quaternion.CreateFromAxisAngle(Vector3.UnitY,.5f);body.Teleport(target,false);
            Check(body.State.Transform.Position==target.Position&&body.State.LinearVelocity==impulse.LinearVelocity&&entity.Transform.Position==target.Position,"explicit teleport retains velocities and updates World");
            bool owns=false,scale=false;try{entity.Transform=target;}catch(EngineException exception){owns=exception.Key=="Bridge.PhysicsOwnsTransform";}
            var resized=target;resized.Scale=new(2);try{body.Teleport(resized);}catch(EngineException exception){scale=exception.Key=="Bridge.BodyScaleChanged";}
            Check(owns&&scale,"physics owns pose and collider size changes require reconstruction");body.Teleport(target,true);
            Check(body.State.LinearVelocity==Vector3.Zero&&body.State.AngularVelocity==Vector3.Zero,"teleport clear flag resets both velocities");
            records.Add(new{kind="force_impulse_teleport",firstVelocity=first.LinearVelocity.X,firstOmegaZ=first.AngularVelocity.Z,impulseVelocity=impulse.LinearVelocity.X,transformOwned=true,scaleProtected=true});
            var spinner=At(session,"spinner",new(10,1000,0));var spinning=session.Physics.CreateBox(spinner,BoxBodyDescription.Dynamic);spinning.SetVelocity(Vector3.Zero,new(0,2,3));
            for(int step=0;step<360;++step)session.Physics.Step(dt);var spin=spinning.State;
            Check(Near(spin.Transform.Rotation.LengthSquared(),1,1e-5)&&Vector3.Distance(spinner.Transform.Position,spin.Transform.Position)<1e-6,"long rotation remains normalized and physical pose drives render Transform");
            records.Add(new{kind="rotation",steps=360,quaternionNorm=spin.Transform.Rotation.LengthSquared()});
            body.Destroy();bool stale=false;try{_ = body.State;}catch(EngineException exception){stale=exception.Key=="Bridge.InvalidBody";}Check(stale,"destroyed body cannot expose stale state");
        }
        using(var session=new EngineSession())
        {
            var entity=At(session,"budget body",new(0,1000,0));session.Physics.CreateBox(entity,BoxBodyDescription.Dynamic);uint steps=session.Physics.Advance(1);
            Check(steps==8&&session.Physics.DroppedSeconds>.8,"fixed clock caps catchup and reports dropped seconds");
            bool owned=false;using(var scripts=new ScriptSystem(session,new ScriptRegistry())){try{session.Physics.Advance(dt);}catch(InvalidOperationException exception){owned=exception.Message=="Script.PhysicsClockOwned";}}
            Check(owned,"script and external physics clocks cannot advance simultaneously");var resumed=session.Physics.Advance(dt);
            Check(resumed==1,$"budget overflow must not replay a dropped fixed step; resumed={resumed}, dropped={session.Physics.DroppedSeconds}");records.Add(new{kind="clock",maximumCatchup=steps,dropped=session.Physics.DroppedSeconds,singleOwner=true});
        }
        using(var session=new EngineSession())
        {
            using var scripts=new ScriptSystem(session,new ScriptRegistry(),simulatePhysics:false);var probe=scripts.Attach<InputEdgesProbe>(session.CreateEntity("script budget"));scripts.Start();
            session.Step(1);Check(probe.FixedCalls==8&&scripts.DroppedFixedSteps==52,"script clock drops all 52 excess steps at integral boundary");session.Step(0);Check(probe.FixedCalls==8,"zero delta cannot replay a dropped script step");session.Step(dt);Check(probe.FixedCalls==9,"script clock resumes exactly one step");records.Add(new{kind="script_clock",initialSteps=8,dropped=scripts.DroppedFixedSteps,resumedSteps=1});
        }
        using(var session=new EngineSession())
        {
            var entity=At(session,"static body",new(0,10,0));var body=session.Physics.CreateBox(entity,BoxBodyDescription.Static);bool rejected=false;
            try{body.ApplyForce(Vector3.UnitY,body.State.Transform.Position);}catch(EngineException exception){rejected=exception.Key=="Bridge.PhysicsFailed";}
            for(int step=0;step<60;++step)session.Physics.Step(dt);Check(rejected&&body.State.Mass==0&&body.State.Transform.Position==new Vector3(0,10,0),"static mass remains immobile and rejects force");records.Add(new{kind="static",steps=60,mass=body.State.Mass,position=body.State.Transform.Position.Y});
        }
        var directory=Path.Combine(AppContext.BaseDirectory,"Validation","T49",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(directory);
        File.WriteAllText(Path.Combine(directory,"physics-integration.json"),JsonSerializer.Serialize(new{schema=1,success=true,bodyStateBytes=72,records},new JsonSerializerOptions{WriteIndented=true}));Console.WriteLine("Physics integration contract passed; evidence: "+directory);
    }
}

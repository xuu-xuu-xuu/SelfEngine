using System.Diagnostics;
using System.Globalization;
using System.Numerics;
using System.Text.Json;
using SelfEngine;

// 固定场景、全部组合相同的观察器。文件写出只发生在窗口退出后。
CultureInfo.CurrentCulture=CultureInfo.InvariantCulture;
try
{
    var options=new Dictionary<string,string>(StringComparer.Ordinal);
    for(int i=0;i<args.Length;i+=2){if(i+1>=args.Length||!options.TryAdd(args[i],args[i+1]))throw new ArgumentException("Benchmark.InvalidArguments");}
    string Read(string key,string fallback)=>options.GetValueOrDefault(key,fallback);
    bool Flag(string key)=>Read(key,"off") switch{"on"=>true,"off"=>false,_=>throw new ArgumentException("Benchmark.InvalidArguments")};
    string output=Path.GetFullPath(options["--output"]),source=options["--source-id"];
    bool scripting=Flag("--scripts"),physics=Flag("--physics"),ui=Flag("--ui"),visible=!Flag("--hidden");
    double warmup=double.Parse(Read("--warmup","5")),sample=double.Parse(Read("--seconds","20"));
    if(!double.IsFinite(warmup)||!double.IsFinite(sample)||warmup<0||sample<=0||warmup+sample>80||source.Length!=64)throw new ArgumentException("Benchmark.InvalidArguments");
    string? capture=options.GetValueOrDefault("--capture");
    if(Directory.Exists(output)||File.Exists(output))throw new IOException("Bridge.ProfileOutputExists");
    using var session=new EngineSession();session.LoadLocale("en-US");session.Physics.SetDebugDrawing(false);
    var objects=new List<object>();
    object Item(int id,string name,float x,float y,float z,object components)=>new{id=id.ToString(CultureInfo.InvariantCulture),name,enabled=true,parent=(string?)null,transform=new{position=new[]{x,y,z},rotation=new[]{0,0,0,1},scale=new[]{1,1,1}},components};
    objects.Add(Item(1,"Camera",0,2,15,new{camera=new{fov=1.0471976,near=.1,far=100,primary=true}}));
    for(int i=0;i<32;i++)objects.Add(Item(i+2,"Static"+i,(i%8-3.5f)*1.4f,(i/8-1.5f)*1.4f,-2,new{mesh=new{asset="00000000000000000000000000000001",materials=Array.Empty<string>()}}));
    string scene=JsonSerializer.Serialize(new{format="SelfEngine.Scene",version=1,entities=objects});session.ImportScene(scene);
    var probes=new List<Entity>();var bodies=new List<RigidBody>();var behaviours=new List<Probe>();
    // 全部组合保留相同非渲染实体；脚本和物理分别作用于独立探针，避免更改相同变换的所有权。
    for(int i=0;i<65;i++){var entity=session.CreateEntity("Probe"+i);var pose=Transform.Identity;pose.Position=new((i%8)*2,i<32?1+i/8*2:.49f,-40-(i%32/8)*2);entity.Transform=pose;probes.Add(entity);}
    if(physics){var floor=BoxBodyDescription.Static;floor.HalfExtents=new(40,.5f,40);var pose=Transform.Identity;pose.Position=new(0,-.5f,-40);probes[64].Transform=pose;session.Physics.CreateBox(probes[64],floor);for(int i=32;i<64;i++)bodies.Add(session.Physics.CreateBox(probes[i],BoxBodyDescription.Dynamic));}
    if(ui){for(int i=0;i<16;i++){var element=session.Ui.Create(i%2==0?UiKind.Label:UiKind.Button);var style=UiStyle.Default;style.Offset=new(12,12+i*36);style.Size=new(210,30);element.SetStyle(style);element.SetText(session.Text("Sample.BenchmarkControl")+" "+i);}}
    using var scripts=new ScriptSystem(session,new ScriptRegistry(),simulatePhysics:physics);
    scripts.Diagnostic+=d=>throw new InvalidOperationException("Benchmark.ScriptDiagnostic:"+d);
    if(scripting)for(int i=0;i<32;i++)behaviours.Add(scripts.Attach<Probe>(probes[i]));
    scripts.Start();
    var rows=new List<Observation>(6000);var watch=new Stopwatch();long first=0;int starts0=GC.CollectionCount(0),starts1=GC.CollectionCount(1),starts2=GC.CollectionCount(2);long allocated=GC.GetTotalAllocatedBytes();
    session.Update+=dt=>{
        long begin=Stopwatch.GetTimestamp();if(first==0){first=begin;watch.Start();}
        double elapsed=watch.Elapsed.TotalSeconds;
        // 持续唤醒非渲染刚体；成本属于物理组合，记录单独的控制回调时间。
        long control=Stopwatch.GetTimestamp();
        if(physics)for(int i=0;i<bodies.Count;i++)bodies[i].SetVelocity(new((float)Math.Sin(elapsed+i)*.3f,0,0));
        long controlled=Stopwatch.GetTimestamp();
        var clock=session.SimulationClock;
        if(rows.Count>=6000)throw new InvalidOperationException("Benchmark.FrameLimit");
        rows.Add(new(session.StepCount,elapsed,dt,clock.Steps,clock.SimulationSeconds,clock.DroppedSeconds,(controlled-control)*1000.0/Stopwatch.Frequency,(Stopwatch.GetTimestamp()-begin)*1000.0/Stopwatch.Frequency));
        if(capture is null&&elapsed>=warmup+sample)session.RequestWindowClose();
    };
    session.SetWindowProfile(output);
    if(capture is not null)session.SetWindowCapture(capture,30);
    session.RunWindow("en-US",visible,capture is null?0UL:32UL);
    watch.Stop();var clockFinal=session.SimulationClock;
    long updates=behaviours.Sum(x=>x.Updates),fixedUpdates=behaviours.Sum(x=>x.FixedUpdates),contacts=behaviours.Sum(x=>x.Contacts);
    int awake=bodies.Count(x=>!x.State.Sleeping);double motion=bodies.Sum(x=>x.State.LinearVelocity.Length());
    float minimumBodyY=physics?bodies.Min(x=>x.State.Transform.Position.Y):0,maximumBodyY=physics?bodies.Max(x=>x.State.Transform.Position.Y):0;
    using(var writer=new StreamWriter(Path.Combine(output,"managed.csv"))){writer.WriteLine("step,elapsed_s,dt_s,fixed_steps,simulation_s,dropped_s,physics_control_ms,observer_including_control_ms");foreach(var row in rows)writer.WriteLine(FormattableString.Invariant($"{row.Step},{row.Elapsed:R},{row.Dt:R},{row.Fixed},{row.Simulation:R},{row.Dropped:R},{row.Control:R},{row.Observer:R}"));}
    bool valid=rows.Count>0&&clockFinal.Steps>0&&(!scripting||(updates>0&&fixedUpdates>0))&&(!physics||(awake==32&&motion>0&&minimumBodyY>.45f&&maximumBodyY<.6f))&&(!(scripting&&physics)||contacts>0)&&(capture is not null||watch.Elapsed.TotalSeconds>=warmup+sample);
    var report=new{schema=1,success=valid,source_sha256=source,scripting,physics,ui,visible,warmup_seconds=warmup,sample_seconds=sample,capture=capture is not null,draw_objects=32,probe_entities=65,script_count=behaviours.Count,body_count=bodies.Count+(physics?1:0),ui_controls=ui?16:0,observations=rows.Count,updates,fixed_updates=fixedUpdates,contacts,awake_bodies=awake,motion,minimum_body_y=minimumBodyY,maximum_body_y=maximumBodyY,clock=clockFinal,wall_seconds=watch.Elapsed.TotalSeconds,gc0=GC.CollectionCount(0)-starts0,gc1=GC.CollectionCount(1)-starts1,gc2=GC.CollectionCount(2)-starts2,managed_allocated_bytes=GC.GetTotalAllocatedBytes()-allocated,observer="One Stopwatch/clock/count observation per update, no per-frame IO; physics steering included in physics switch"};
    File.WriteAllText(Path.Combine(output,"managed.json"),JsonSerializer.Serialize(report,new JsonSerializerOptions{WriteIndented=true}));
    if(!valid)throw new InvalidOperationException("Benchmark.InvalidWorkload");
    Console.WriteLine("SystemBenchmark passed: "+output);return 0;
}
catch(Exception exception){Console.Error.WriteLine(exception);return 1;}

readonly record struct Observation(ulong Step,double Elapsed,double Dt,ulong Fixed,double Simulation,double Dropped,double Control,double Observer);
[ScriptId("Benchmark.Probe")]
public sealed class Probe:Behaviour
{
    public long Updates,FixedUpdates,Contacts;private double time;
    public override void OnCreate(){Subscribe<PhysicsContact>(_=>++Contacts);}
    public override void OnUpdate(double deltaSeconds){++Updates;time+=deltaSeconds;var pose=Entity.Transform;pose.Position.X=(float)Math.Sin(time);Entity.Transform=pose;}
    public override void OnFixedUpdate(double deltaSeconds){++FixedUpdates;}
}

namespace SelfEngine;

public sealed class ScriptSystem : IDisposable
{
    private sealed class Entry(Behaviour value,ScriptComponentBinding native,SceneHandle scene)
    {
        internal readonly Behaviour Value = value;
        internal readonly ScriptComponentBinding Native=native;
        internal readonly SceneHandle Scene=scene;
        internal bool Created;
        internal bool Active;
        internal bool Faulted;
        internal readonly Dictionary<TickPhase,ulong> Ticks=[];
    }
    private readonly List<Entry> entries = [];
    private readonly Queue<Action> mutations = [];
    private readonly HashSet<ulong> destroying = [];
    private int dispatchDepth;
    private bool draining;
    private bool running;
    private bool stopping;
    private bool disposed;
    private bool disposeRequested;
    private readonly List<(Entity Entity,Action<Entity>? Configure)> pendingSpawns=[];
    private bool simulationPaused;
    private ScriptInput frameInput = ScriptInput.Empty;
    private ScriptInput fixedInput = ScriptInput.Empty;
    public EngineSession Session { get; }
    public ScriptRegistry Registry { get; }
    public ScriptEvents Events { get; }
    public UiActionBindings UiActions {get;}
    public ScriptCommands Commands {get;}
    public ScriptInput Input { get; private set; } = ScriptInput.Empty;
    public double FixedDeltaSeconds { get; }
    public bool AutomaticFixedUpdates { get; }
    public bool SimulatePhysics { get; }
    public int MaxFixedSteps { get; }
    public ulong DroppedFixedSteps { get; private set; }
    public int Count => entries.Count;
    public bool IsRunning => running;
    public bool SimulationPaused
    {
        get => simulationPaused;
        set { if(simulationPaused==value)return;simulationPaused=value;frameInput=fixedInput=Input=ScriptInput.Empty;if(Session.EnvironmentOwner==this&&!Session.IsDisposed)Session.Invoke(handle=>{EngineSession.Check(NativeMethods.EnvironmentPaused(handle,value?1:0));return 0;}); }
    }
    public event Action<ScriptDiagnostic>? Diagnostic;

    public ScriptSystem(EngineSession session, ScriptRegistry registry, bool automaticFixedUpdates = true,
        double fixedDeltaSeconds = 1.0 / 60.0, int maxFixedSteps = 8, bool simulatePhysics = true)
    {
        if (!double.IsFinite(fixedDeltaSeconds) || fixedDeltaSeconds <= 0 || fixedDeltaSeconds > 1 || maxFixedSteps < 1)
            throw new ArgumentException("Script.InvalidClock");
        Session = session; Registry = registry; AutomaticFixedUpdates = automaticFixedUpdates;
        FixedDeltaSeconds = fixedDeltaSeconds; MaxFixedSteps = maxFixedSteps;
        Events = new(this);
        UiActions=new(this);
        Commands=new(this);
        SimulatePhysics = simulatePhysics;
        if (simulatePhysics)
        {
            if (session.Physics.ClockOwner is not null) throw new InvalidOperationException("Script.PhysicsClockOwned");
            session.Physics.ClockOwner = this;
        }
        session.EnvironmentPhaseChanged+=OnPhase;session.ComponentTick+=OnComponentTick;
        session.SceneRetiring+=OnSceneRetiring;
    }
    public T Attach<T>(Entity entity, Action<T>? configure = null) where T : Behaviour, new()
    {
        Registry.Register<T>();
        var value = new T(); configure?.Invoke(value);
        Attach(entity, value);
        return value;
    }
    public void Attach(Entity entity, Behaviour value)
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        if (stopping || entity.Session != Session || (!entity.IsAlive&&!entity.IsPending) || value.Owner is not null)
            throw new ArgumentException("Script.InvalidAttachment");
        value.Entity = entity;
        value.Owner = this;
        Mutate(() =>
        {
            if ((!entity.IsAlive&&!entity.IsPending) || destroying.Contains(entity.Handle)) { value.Owner = null; return; }
            ScriptComponentBinding binding;
            try{binding=new(entity,value.EnabledValue);}catch{value.Owner=null;throw;}
            var entry = new Entry(value,binding,Session.Scenes.Of(entity)?.Handle??default);
            entries.Add(entry);
            if (running) Create(entry);
        });
    }
    public void Start()
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        if (running) return;
        if(Session.EnvironmentOwner is not null&&Session.EnvironmentOwner!=this)throw new InvalidOperationException("Script.EnvironmentClockOwned");
        Session.Invoke(handle=>{EngineSession.Check(NativeMethods.EnvironmentConfigure(handle,FixedDeltaSeconds,checked((uint)MaxFixedSteps),AutomaticFixedUpdates?1:0,SimulatePhysics?1:0));return 0;});
        Session.EnvironmentOwner=this;DroppedFixedSteps=0;running=true;Events.Open();
        try{foreach(var entry in entries.ToArray())Create(entry);FlushMutations();if(simulationPaused)Session.Invoke(handle=>{EngineSession.Check(NativeMethods.EnvironmentPaused(handle,1));return 0;});}
        catch{Stop();throw;}
    }
    public void SetInput(ScriptInput input)
    {
        frameInput = input;
        fixedInput = fixedInput.MergeEdges(input);
    }
    public void Spawn(string name, Action<Entity>? configure = null) => Mutate(() =>
    {
        if (!stopping) { var entity = Session.CreateEntity(name);if(entity.IsPending)pendingSpawns.Add((entity,configure));else configure?.Invoke(entity); }
    });
    public void Destroy(Entity entity)
    {
        if (entity.Session != Session) throw new ArgumentException("Script.InvalidAttachment");
        destroying.Add(entity.Handle);
        if(entity.IsAlive||entity.IsPending)entity.Destroy();
        Mutate(() =>
        {
            foreach (var entry in entries.Where(e => e.Value.Entity.Handle == entity.Handle).ToArray()) Remove(entry);
            if (entity.IsAlive||entity.IsPending) entity.Destroy();
            destroying.Remove(entity.Handle);
            SweepDead();
        });
    }
    public void Remove(Behaviour value) => Mutate(() =>
    {
        var entry = entries.Find(e => e.Value == value);
        if (entry is not null) Remove(entry);
    });
    public void SetEnabled(Behaviour value, bool enabled) => Mutate(() =>
    {
        var entry = entries.Find(e => e.Value == value);
        if (entry is null) return;
        if(entry.Faulted){if(enabled)Report(value,"Script.FaultedCannotEnable","",nameof(Behaviour.OnEnable));return;}
        value.EnabledValue = enabled;
        entry.Native.SetEnabled(enabled);
        RefreshActivation(entry);
    });
    public void FixedUpdate(double deltaSeconds)
    {
        if(AutomaticFixedUpdates)throw new InvalidOperationException("Script.ExternalFixedUpdateDisabled");
        if(!double.IsFinite(deltaSeconds)||deltaSeconds<=0||deltaSeconds>1)throw new ArgumentException("Script.InvalidClock");
        if(!running||SimulationPaused)return;
        Session.Invoke(handle=>{EngineSession.Check(NativeMethods.EnvironmentFixedStep(handle,deltaSeconds));return 0;});
    }
    private void OnPhase(EnvironmentPhase phase,double seconds)
    {
        if(!running||disposed||Session.EnvironmentOwner!=this)return;
        if(phase==EnvironmentPhase.Prepare)
        {
            var pending=pendingSpawns.ToArray();pendingSpawns.Clear();
            foreach(var (entity,configure) in pending)
            {
                if(!running||disposed)break;
                if(entity.IsPending){pendingSpawns.Add((entity,configure));continue;}
                if(!entity.IsAlive)continue;
                try{using var scope=Session.EnterSceneCallbackScope(Session.Scenes.Of(entity)?.Handle??default);configure?.Invoke(entity);}catch(Exception exception){Report(null,"Script.CallbackFailed",exception.GetType().Name+": "+exception.Message,"Spawn");if(entity.IsAlive)entity.Destroy();}
            }
        }
        if(phase==EnvironmentPhase.PrepareFinished)
        {SweepDead();foreach(var entry in entries.ToArray())Create(entry);RefreshActivation();}
        if(phase==EnvironmentPhase.FrameBegin)
        {
            if(Session.IsWindowRunning)SetInput(Session.WindowInput);
            SweepDead();RefreshActivation();
            foreach(var uiEvent in Session.Ui.DrainEvents())
            {if(!running||disposed)break;UiActions.Dispatch(uiEvent);if(!running||disposed)break;Events.Publish(uiEvent);}
            RefreshActivation();Input=frameInput;
        }
        if(phase is EnvironmentPhase.PreTick or EnvironmentPhase.Tick or EnvironmentPhase.PostTick){RefreshActivation();Input=frameInput;}
        if(phase is EnvironmentPhase.FixedPreTick or EnvironmentPhase.FixedPostTick){RefreshActivation();Input=fixedInput;}
        if(phase==EnvironmentPhase.AfterPhysics&&SimulatePhysics)
            foreach(var contact in Session.Physics.DrainContacts())Events.Publish(contact,EventPhase.AfterPhysics);
        if(phase==EnvironmentPhase.FixedEnd){fixedInput=fixedInput.WithoutEdges();Input=frameInput;}
        if(phase==EnvironmentPhase.FrameEnd)
        {
            frameInput=frameInput.WithoutEdges();Input=frameInput;UpdateClockDiagnostics();
        }
        SweepDead();FlushMutations();
    }
    private void UpdateClockDiagnostics()
    {
        if(Session.IsDisposed||Session.EnvironmentOwner!=this)return;
        double count=Math.Round(Session.SimulationClock.DroppedSeconds/FixedDeltaSeconds);
        ulong dropped=count>=ulong.MaxValue?ulong.MaxValue:(ulong)Math.Max(0,count);
        if(dropped>DroppedFixedSteps)Report(null,"Script.FixedStepBudget",(dropped-DroppedFixedSteps).ToString(System.Globalization.CultureInfo.InvariantCulture));
        DroppedFixedSteps=dropped;
    }
    private void RegisterTicks(Entry entry)
    {
        if(!running||entry.Faulted||!entries.Contains(entry)||!entry.Native.IsAlive||entry.Ticks.Count!=0)return;
        var type=entry.Value.GetType();
        void Register(string method,TickPhase phase,bool paused=false)
        {
            if(type.GetMethod(method)!.DeclaringType==typeof(Behaviour))return;
            var token=Session.Invoke(handle=>{EngineSession.Check(NativeMethods.ComponentTickRegister(handle,entry.Native.Handle,(uint)phase,phase is TickPhase.FixedPreTick or TickPhase.FixedPostTick?1:0,paused?1:0,out var task));return task;});
            entry.Ticks.Add(phase,token);
        }
        Register(nameof(Behaviour.OnPreTick),TickPhase.PreTick,entry.Value.UpdateWhenPaused);
        Register(nameof(Behaviour.OnFixedUpdate),TickPhase.FixedPreTick);
        Register(nameof(Behaviour.OnFixedPostUpdate),TickPhase.FixedPostTick);
        Register(nameof(Behaviour.OnUpdate),entry.Value.UpdateWhenPaused?TickPhase.FrameBegin:TickPhase.Tick,entry.Value.UpdateWhenPaused);
        Register(nameof(Behaviour.OnPostTick),TickPhase.PostTick,entry.Value.UpdateWhenPaused);
    }
    private void UnregisterTicks(Entry entry)
    {
        foreach(var token in entry.Ticks.Values)
        {
            if(Session.IsDisposed)break;
            try{Session.Invoke(handle=>{EngineSession.Check(NativeMethods.ComponentTickRemove(handle,token));return 0;});}
            catch(EngineException exception)when(exception.Key=="Tick.InvalidTask"){}
        }
        entry.Ticks.Clear();
    }
    public unsafe void SetTickPrerequisites(Behaviour value,TickPhase phase,params TickDependency[] prerequisites)
    {
        var entry=entries.Find(e=>e.Value==value);
        if(entry is null||!entry.Ticks.TryGetValue(phase,out var task))throw new InvalidOperationException("Tick.InvalidTask");
        var native=new NativeMethods.TickPrerequisite[prerequisites.Length];
        for(int i=0;i<native.Length;++i)
        {
            var prior=entries.Find(e=>e.Value==prerequisites[i].Behaviour);
            if(prior is null||!prior.Ticks.TryGetValue(prerequisites[i].Phase,out var token))throw new InvalidOperationException("Tick.InvalidPrerequisite");
            native[i]=new(){Task=token,Mode=prerequisites[i].RequireFresh?1u:0u,Reserved=0};
        }
        Session.Invoke(handle=>{fixed(NativeMethods.TickPrerequisite* buffer=native)EngineSession.Check(NativeMethods.ComponentTickPrerequisites(handle,task,buffer,checked((uint)native.Length)));return 0;});
    }
    private void OnComponentTick(ulong component,TickPhase phase,double seconds)
    {
        if(!running||disposed)return;
        // 原生调度器已决定暂停任务及编辑器单步；这里仍校验生命周期，不能再次拦截强制单步。
        var entry=entries.Find(e=>e.Native.Handle==component);if(entry is null||!CanRun(entry.Value,true))return;
        Action callback=phase switch
        {
            TickPhase.PreTick=>()=>entry.Value.OnPreTick(seconds),
            TickPhase.FixedPreTick=>()=>entry.Value.OnFixedUpdate(seconds),
            TickPhase.FixedPostTick=>()=>entry.Value.OnFixedPostUpdate(seconds),
            TickPhase.PostTick=>()=>entry.Value.OnPostTick(seconds),
            _=>()=>entry.Value.OnUpdate(seconds)
        };
        Invoke(entry,callback,phase:phase switch{TickPhase.PreTick=>nameof(Behaviour.OnPreTick),TickPhase.FixedPreTick=>nameof(Behaviour.OnFixedUpdate),TickPhase.FixedPostTick=>nameof(Behaviour.OnFixedPostUpdate),TickPhase.PostTick=>nameof(Behaviour.OnPostTick),_=>nameof(Behaviour.OnUpdate)});
        SweepDead();FlushMutations();
    }
    private void Create(Entry entry)
    {
        if (entry.Created || !entry.Value.Entity.IsAlive || !entry.Native.IsAlive || destroying.Contains(entry.Value.Entity.Handle)) return;
        entry.Created = true;
        Invoke(entry, entry.Value.OnCreate, phase:nameof(Behaviour.OnCreate));
        RefreshActivation(entry);
        RegisterTicks(entry);
    }
    private void Remove(Entry entry)
    {
        if (!entries.Remove(entry)) return;
        UnregisterTicks(entry);
        entry.Native.SetEnabled(false);
        if (entry.Active) { entry.Active = false; Invoke(entry, entry.Value.OnDisable, false, nameof(Behaviour.OnDisable)); }
        if (entry.Created) { entry.Created = false; Invoke(entry, entry.Value.OnDestroy, false, nameof(Behaviour.OnDestroy)); }
        Events.Remove(entry.Value);
        UiActions.Remove(entry.Value);
        Commands.Remove(entry.Value);
        entry.Native.Remove();
        entry.Value.Owner = null;
        if(!entries.Any(other=>other.Value.Entity.Handle==entry.Value.Entity.Handle))destroying.Remove(entry.Value.Entity.Handle);
    }
    private void SweepDead()
    {
        if (Session.IsDisposed) return;
        foreach (var entry in entries.ToArray()) if ((!entry.Value.Entity.IsAlive&&!entry.Value.Entity.IsPending)||(!entry.Native.IsAlive&&!entry.Native.Pending)) Remove(entry);
    }
    private void OnSceneRetiring(SceneRetirement retirement)
    {
        if(disposed)return;
        foreach(var entry in entries.ToArray())
        {
            if(retirement.Members.Contains(entry.Value.Entity.Handle))
            {destroying.Add(entry.Value.Entity.Handle);entry.Value.EnabledValue=false;entry.Native.SetEnabled(false);Mutate(()=>Remove(entry));}
            else ScriptFields.ClearSceneReferences(entry.Value,retirement.Invalidated);
        }
    }
    private void RefreshActivation()
    {if(!running||disposed)return;foreach(var entry in entries.ToArray())RefreshActivation(entry);}
    private void RefreshActivation(Entry entry)
    {
        if(!entry.Created||!entries.Contains(entry))return;
        bool active=running&&!entry.Faulted&&entry.Value.EnabledValue&&!destroying.Contains(entry.Value.Entity.Handle)&&entry.Value.Entity.IsAlive&&entry.Native.Active;
        if(active==entry.Active)return;
        entry.Active=active;
        Invoke(entry,active?entry.Value.OnEnable:entry.Value.OnDisable,phase:active?nameof(Behaviour.OnEnable):nameof(Behaviour.OnDisable));
    }
    internal bool CanRun(Behaviour value) => CanRun(value,false);
    private bool CanRun(Behaviour value,bool scheduledTick)
    {
        var entry = entries.Find(e => e.Value == value);
        return running && value.EnabledValue && (scheduledTick||!SimulationPaused||value.UpdateWhenPaused) && entry is { Created: true, Active: true, Faulted: false }
            && !destroying.Contains(value.Entity.Handle) && value.Entity.IsAlive && entry.Native.Active;
    }
    internal ulong ComponentHandle(Behaviour value)=>entries.Find(entry=>entry.Value==value)?.Native.Handle??throw new ArgumentException("Script.NotAttached");
    internal bool InvokeEvent(Behaviour value,Action action,string phase="Event")
    {
        var entry = entries.Find(e => e.Value == value);
        if (entry is null)return false;Invoke(entry, action, phase:phase);return !entry.Faulted;
    }
    private void Invoke(Entry entry, Action action, bool disableOnError = true, string phase = "")
    {
        ++dispatchDepth;
        try {using var scope=Session.EnterSceneCallbackScope(entry.Scene,entry.Native.Handle);action();}
        catch (Exception exception)
        {
            Report(entry.Value, "Script.CallbackFailed", exception.ToString(), phase);
            if (disableOnError)
            {
                entry.Faulted = true;
                entry.Value.EnabledValue = false;
                entry.Native.SetEnabled(false);
                if (entry.Active)
                {
                    entry.Active = false;
                    try { entry.Value.OnDisable(); } catch (Exception disabled) { Report(entry.Value, "Script.CallbackFailed", disabled.GetType().Name+": "+disabled.Message, nameof(Behaviour.OnDisable)); }
                }
            }
        }
        finally { --dispatchDepth; }
    }
    private void Mutate(Action action)
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        mutations.Enqueue(action);
        FlushMutations();
    }
    internal void FlushMutations()
    {
        if (dispatchDepth != 0 || draining) return;
        draining = true;
        try
        {
            int count = 0;
            while (mutations.Count != 0)
            {
                if (++count > 10000) { mutations.Clear(); throw new InvalidOperationException("Script.MutationBudget"); }
                mutations.Dequeue()();
            }
        }
        finally { draining = false; }
    }
    internal void Report(Behaviour? value, string key, string detail, string phase = "")
    {
        try { Diagnostic?.Invoke(new(key, value?.Entity.Handle ?? 0, value is null ? "" : ScriptRegistry.IdOf(value.GetType()), detail, phase)); }
        catch { }
    }
    public void Stop()
    {
        if (stopping || disposed) return;
        if (dispatchDepth != 0) { mutations.Enqueue(Stop); return; }
        stopping = true;
        UpdateClockDiagnostics();
        running = false;
        foreach (var entry in entries.ToArray()) Remove(entry);
        mutations.Clear(); destroying.Clear(); Events.Clear();UiActions.Clear();Commands.Clear();
        foreach(var (entity,_) in pendingSpawns)if(entity.IsPending)entity.Destroy();pendingSpawns.Clear();
        if(Session.EnvironmentOwner==this){if(!Session.IsDisposed)Session.Invoke(handle=>{EngineSession.Check(NativeMethods.EnvironmentRelease(handle));return 0;});Session.EnvironmentOwner=null;}
        frameInput = fixedInput = Input = ScriptInput.Empty;
        simulationPaused = false;
        stopping = false;
        if(disposeRequested)CompleteDispose();
    }
    public void Dispose()
    {
        if (disposed) return;
        // Stop 会取消普通结构修改；终止请求必须独立保存，不能随 mutations 被清空。
        if(dispatchDepth!=0||stopping){disposeRequested=true;if(!stopping)mutations.Enqueue(Dispose);return;}
        Stop();
        CompleteDispose();
    }
    private void CompleteDispose()
    {
        if(disposed)return;
        disposed=true;disposeRequested=false;
        Session.EnvironmentPhaseChanged-=OnPhase;Session.ComponentTick-=OnComponentTick;
        Session.SceneRetiring-=OnSceneRetiring;
        if (Session.Physics.ClockOwner == this) Session.Physics.ClockOwner = null;
    }
    internal IReadOnlyList<Behaviour> Attachments => entries.Select(e => e.Value).ToArray();
}

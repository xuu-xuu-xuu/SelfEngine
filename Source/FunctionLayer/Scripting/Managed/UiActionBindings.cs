namespace SelfEngine;

// 只执行显式注册的动作；持久绑定保存 ID，不保存项目委托或 Type。
public sealed class UiActionBindings
{
    private readonly record struct Key(ulong Entity,string Script,string Action);
    private sealed record Registration(ulong Id,Behaviour Owner,Action<UiEvent> Handler);
    private sealed class Token(UiActionBindings registry,Key key,ulong id):IDisposable
    {
        private UiActionBindings? ownerRegistry=registry;
        public void Dispose(){ownerRegistry?.Release(key,id);ownerRegistry=null;}
    }
    private readonly ScriptSystem system;
    private readonly Dictionary<Key,Registration> registrations=[];
    private readonly HashSet<(uint Epoch,ulong Node,UiBinding Binding)> missing=[];
    private ulong next;
    internal UiActionBindings(ScriptSystem system)=>this.system=system;
    public int Count=>registrations.Count;
    public IDisposable Register(Behaviour owner,string action,Action<UiEvent> handler)
    {
        ArgumentNullException.ThrowIfNull(owner);ArgumentNullException.ThrowIfNull(handler);
        if(owner.Owner!=system||system.Session.IsDisposed||!owner.Entity.IsAlive)throw new ArgumentException("Script.NotAttached");
        var script=ScriptRegistry.IdOf(owner.GetType());
        if(!UiBinding.ValidName(script)||!UiBinding.ValidName(action)||registrations.Count>=65536||next==ulong.MaxValue)throw new ArgumentException("Script.UiActionInvalid");
        var key=new Key(owner.Entity.StableId,script,action);if(registrations.ContainsKey(key))throw new InvalidOperationException("Script.UiActionDuplicate");
        var id=++next;registrations.Add(key,new(id,owner,handler));return new Token(this,key,id);
    }
    private void Release(Key key,ulong id)
    {if(registrations.TryGetValue(key,out var registration)&&registration.Id==id)registrations.Remove(key);}
    internal void Dispatch(UiEvent value)
    {
        if(value.Binding is not { } binding||value.SourceEpoch!=system.Session.Ui.Epoch||system.Session.Ui.Find(value.Target) is null)return;
        if(registrations.TryGetValue(new(binding.Entity,binding.Script,binding.Action),out var registration))
        {
            if(system.CanRun(registration.Owner))
            {system.InvokeEvent(registration.Owner,()=>registration.Handler(value),"UiAction");system.FlushMutations();}
            return;
        }
        if(missing.Count>=1024)missing.Clear();
        if(missing.Add((system.Session.Ui.Epoch,value.Target,binding)))
            system.Report(null,"Script.UiBindingMissing",$"node={value.Target} event={value.Kind} entity={binding.Entity} script={binding.Script} action={binding.Action}","UiAction");
    }
    internal void Remove(Behaviour owner)
    {foreach(var key in registrations.Where(pair=>pair.Value.Owner==owner).Select(pair=>pair.Key).ToArray())registrations.Remove(key);}
    internal void Clear(){registrations.Clear();missing.Clear();}
}

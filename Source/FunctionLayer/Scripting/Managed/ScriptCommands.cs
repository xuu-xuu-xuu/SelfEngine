namespace SelfEngine;

// 需要立即返回结果的控制走单一明确接收者，不广播成已经发生的事实。
public sealed class ScriptCommands
{
    private sealed record Registration(ulong Id,Behaviour Owner,Type Type,Func<object,bool> Handler);
    private sealed class Token(ScriptCommands registry,string key,ulong id):IDisposable
    {
        private readonly WeakReference<ScriptCommands> registry=new(registry);
        public void Dispose(){if(registry.TryGetTarget(out var owner)&&owner.registrations.TryGetValue(key,out var value)&&value.Id==id)owner.registrations.Remove(key);}
    }
    private readonly ScriptSystem system;
    private readonly Dictionary<string,Registration> registrations=new(StringComparer.Ordinal);
    private ulong next;
    internal ScriptCommands(ScriptSystem system)=>this.system=system;
    public int Count=>registrations.Count;
    public IDisposable Register<T>(Behaviour owner,string key,Func<T,bool> handler) where T:notnull
    {
        ArgumentNullException.ThrowIfNull(handler);
        if(owner.Owner!=system||!UiBinding.ValidName(key))throw new ArgumentException("Script.CommandInvalid");
        if(registrations.ContainsKey(key)||registrations.Count>=65536||next==ulong.MaxValue)throw new InvalidOperationException("Script.CommandDuplicate");
        var id=++next;registrations.Add(key,new(id,owner,typeof(T),value=>handler((T)value)));return new Token(this,key,id);
    }
    public bool Execute<T>(string key,T value) where T:notnull
    {
        if(!registrations.TryGetValue(key,out var registration)||registration.Type!=typeof(T)||!system.CanRun(registration.Owner))return false;
        bool accepted=false;var success=system.InvokeEvent(registration.Owner,()=>accepted=registration.Handler(value),"Command");system.FlushMutations();return success&&accepted;
    }
    internal void Remove(Behaviour owner){foreach(var key in registrations.Where(pair=>pair.Value.Owner==owner).Select(pair=>pair.Key).ToArray())registrations.Remove(key);}
    internal void Clear()=>registrations.Clear();
}

namespace SelfEngine;

// 原生只保存受控标识；实际 Behaviour 和委托仍由 ScriptSystem 管理。
internal sealed class ScriptComponentBinding
{
    private static long lastControl;
    private readonly EngineSession session;
    private ulong handle;
    private static ulong NextControl()
    {
        while(true)
        {
            long prior=Volatile.Read(ref lastControl);if(prior==long.MaxValue)throw new InvalidOperationException("World.ComponentHandleExhausted");
            if(Interlocked.CompareExchange(ref lastControl,prior+1,prior)==prior)return (ulong)(prior+1);
        }
    }
    internal ScriptComponentBinding(Entity owner,bool enabled)
    {
        session=owner.Session;var controlled=NextControl();
        handle=session.Invoke(value=>{EngineSession.Check(NativeMethods.ScriptComponentAttach(value,owner.Handle,controlled,enabled?1:0,out var component));return component;});
    }
    private (bool Alive,bool Active) State()
    {
        if(handle==0||session.IsDisposed)return(false,false);
        return session.Invoke(value=>{EngineSession.Check(NativeMethods.ScriptComponentState(value,handle,out var alive,out var active));return(alive!=0,active!=0);});
    }
    internal bool IsAlive=>State().Alive;
    internal ulong Handle=>handle;
    internal bool Pending=>handle!=0&&!session.IsDisposed&&session.Invoke(value=>{EngineSession.Check(NativeMethods.ScriptComponentPending(value,handle,out var pending));return pending!=0;});
    internal bool Active=>State().Active;
    internal void SetEnabled(bool enabled)
    {
        if(!IsAlive&&!Pending)return;
        session.Invoke(value=>{EngineSession.Check(NativeMethods.ScriptComponentEnabled(value,handle,enabled?1:0));return 0;});
    }
    internal void Remove()
    {
        var old=handle;handle=0;if(old==0||session.IsDisposed)return;
        try{session.Invoke(value=>{EngineSession.Check(NativeMethods.ScriptComponentRemove(value,old));return 0;});}
        catch(EngineException exception)when(exception.Key=="Bridge.InvalidComponent"){/* owner 销毁已完成原生清理。 */}
    }
}

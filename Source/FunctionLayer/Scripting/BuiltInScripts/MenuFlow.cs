using SelfEngine;
using System.Numerics;

namespace ProjectScripts;

public enum MenuFlowState { MainMenu, Playing, Paused, Settings }
[EventId("sample.menu_flow_changed")] public readonly record struct MenuFlowChanged(MenuFlowState State);
[EventId("sample.menu_settings_changed")] public readonly record struct MenuSettingsChanged(float MoveSpeed,float LookSensitivity);
public readonly record struct MenuInteractionCommand(EntityReference Target);
[EventId("sample.menu_interaction_completed")] public readonly record struct MenuInteractionCompleted(EntityReference Target);
[EventId("sample.menu_game_started")] public readonly record struct MenuGameStarted;

[ScriptId("sample.menu_flow")]
public sealed class MenuFlow : Behaviour
{
    [Expose] public EntityReference ViewEntity;
    [Expose] public float InteractionRange=4;
    [Expose] public float MoveSpeed=4;
    [Expose] public float LookSensitivity=0.003f;
    public override bool UpdateWhenPaused => true;
    public MenuFlowState State { get; private set; }=MenuFlowState.MainMenu;
    public EntityReference Target { get; private set; }
    public IReadOnlyDictionary<string,ulong> Controls => controls;
    public MenuSettingsChanged Settings => new(MoveSpeed,LookSensitivity);
    private readonly Dictionary<string,ulong> controls=[];
    private readonly List<UiElement> roots=[];
    private UiElement? mainPanel,pausePanel,settingsPanel,hud,prompt,speedSlider,sensitivitySlider;
    private MenuFlowState settingsReturn=MenuFlowState.MainMenu;
    private bool previousPause,created;

    public override void OnCreate()
    {
        if(!float.IsFinite(InteractionRange)||InteractionRange<=0||InteractionRange>100
            ||!ValidSettings(MoveSpeed,LookSensitivity))throw new ArgumentException("MenuFlow.InvalidSettings");
        previousPause=Scripts.SimulationPaused;
        RegisterActions();
        mainPanel=Panel("main",new(280,240),new(24,24));
        Label(mainPanel,"MenuFlow.Title");
        Button(mainPanel,"start","MenuFlow.Start","menu.start");
        Button(mainPanel,"main-settings","MenuFlow.Settings","menu.settings");
        Button(mainPanel,"quit","MenuFlow.Quit","menu.quit");
        pausePanel=Panel("pause",new(280,280),new(24,24));
        Label(pausePanel,"MenuFlow.Paused");
        Button(pausePanel,"resume","MenuFlow.Resume","menu.resume");
        Button(pausePanel,"pause-settings","MenuFlow.Settings","menu.settings");
        Button(pausePanel,"main-menu","MenuFlow.MainMenu","menu.main");
        settingsPanel=Panel("settings",new(320,300),new(24,24));
        Label(settingsPanel,"MenuFlow.SettingsTitle");
        Label(settingsPanel,"MenuFlow.MoveSpeed");speedSlider=Slider(settingsPanel,"speed",1,10,MoveSpeed);
        Label(settingsPanel,"MenuFlow.Sensitivity");sensitivitySlider=Slider(settingsPanel,"sensitivity",0.001,0.02,LookSensitivity);
        speedSlider.Bind(UiEventKind.ValueChanged,this,"menu.speed");
        sensitivitySlider.Bind(UiEventKind.ValueChanged,this,"menu.sensitivity");
        Button(settingsPanel,"back","MenuFlow.Back","menu.back");
        hud=Panel("hud",new(320,72),Vector2.Zero);
        var hudStyle=UiStyle.Default;hudStyle.AnchorMin=Vector2.Zero;hudStyle.AnchorMax=Vector2.One;hudStyle.Pivot=Vector2.Zero;
        hudStyle.Size=Vector2.Zero;hudStyle.Layout=UiLayout.Absolute;hudStyle.Padding=0;hud.SetStyle(hudStyle);
        hud.SetEnabled(false); // HUD 只显示，不拦截角色鼠标输入。
        foreach(var size in new[]{new Vector2(14,2),new Vector2(2,14)})
        {var line=Entity.Session.Ui.Create(UiKind.Panel,hud);var cross=UiStyle.Default;cross.AnchorMin=cross.AnchorMax=cross.Pivot=new(.5f,.5f);cross.Size=size;line.SetStyle(cross);}
        prompt=Label(hud,"MenuFlow.NoTarget");var promptStyle=UiStyle.Default;promptStyle.AnchorMin=promptStyle.AnchorMax=new(.5f,.5f);promptStyle.Pivot=new(.5f,0);promptStyle.Offset=new(0,18);promptStyle.Size=new(320,28);prompt.SetStyle(promptStyle);
        var help=Label(hud,"MenuFlow.GameControls");var helpStyle=UiStyle.Default;helpStyle.AnchorMin=new(0,1);helpStyle.AnchorMax=Vector2.One;helpStyle.Pivot=new(0,1);helpStyle.Offset=new(16,-12);helpStyle.Size=new(-32,48);help.SetStyle(helpStyle);
        created=true;
    }
    public override void OnEnable(){if(created)ApplyState();}
    public override void OnDisable()
    {
        if(!created||Entity.Session.IsDisposed)return;
        foreach(var root in roots)
        {try{root.CancelTransitions();root.SetEnabled(false);root.SetVisible(false);}catch(EngineException exception)when(exception.Key=="Bridge.InvalidUi"){/* 文档已被外部替换，仍须恢复暂停状态。 */}}
        Scripts.SimulationPaused=previousPause;
    }
    public override void OnDestroy()
    {
        if(!Entity.Session.IsDisposed)foreach(var root in roots)
        {try{root.Remove();}catch(EngineException){/* UI 文档可能已被外部场景切换替换。 */}}
        roots.Clear();controls.Clear();created=false;
        mainPanel=pausePanel=settingsPanel=hud=prompt=speedSlider=sensitivitySlider=null;
    }
    public bool StartGame()
    {
        if(State!=MenuFlowState.MainMenu)return false;
        Transition(MenuFlowState.Playing);Scripts.Events.Publish(new MenuGameStarted());return true;
    }
    public bool Pause(){if(State!=MenuFlowState.Playing)return false;Transition(MenuFlowState.Paused);return true;}
    public bool Resume(){if(State!=MenuFlowState.Paused)return false;Transition(MenuFlowState.Playing);return true;}
    public bool OpenSettings()
    {
        if(State!=MenuFlowState.MainMenu&&State!=MenuFlowState.Paused)return false;
        settingsReturn=State;Transition(MenuFlowState.Settings);return true;
    }
    public bool CloseSettings(){if(State!=MenuFlowState.Settings)return false;Transition(settingsReturn);return true;}
    public bool ReturnToMainMenu()
    {
        if(State==MenuFlowState.MainMenu)return false;
        Transition(MenuFlowState.MainMenu);return true;
    }
    private static bool ValidSettings(float speed,float sensitivity)
        =>float.IsFinite(speed)&&speed>=1&&speed<=10&&float.IsFinite(sensitivity)&&sensitivity>=0.001f&&sensitivity<=0.02f;
    public bool SetSettings(float speed,float sensitivity)
    {
        if(!ValidSettings(speed,sensitivity))return false;
        MoveSpeed=speed;LookSensitivity=sensitivity;
        speedSlider?.SetValue(speed);sensitivitySlider?.SetValue(sensitivity);
        Scripts.Events.Publish(Settings);return true;
    }
    private void Transition(MenuFlowState state)
    {
        State=state;Target=default;ApplyState();Scripts.Events.Publish(new MenuFlowChanged(state));
        if(state==MenuFlowState.Playing)Scripts.Events.Publish(Settings);
    }
    private void ApplyState()
    {
        Scripts.SimulationPaused=State!=MenuFlowState.Playing;
        ShowPanel(mainPanel,State==MenuFlowState.MainMenu);ShowPanel(pausePanel,State==MenuFlowState.Paused);
        ShowPanel(settingsPanel,State==MenuFlowState.Settings);hud?.SetVisible(State==MenuFlowState.Playing);
        if(State!=MenuFlowState.Playing)prompt?.SetText(Entity.Session.Text("MenuFlow.NoTarget"));
    }
    private void RegisterActions()
    {
        Scripts.UiActions.Register(this,"menu.start",_=>StartGame());Scripts.UiActions.Register(this,"menu.resume",_=>Resume());
        Scripts.UiActions.Register(this,"menu.settings",_=>OpenSettings());Scripts.UiActions.Register(this,"menu.back",_=>CloseSettings());
        Scripts.UiActions.Register(this,"menu.main",_=>ReturnToMainMenu());Scripts.UiActions.Register(this,"menu.quit",_=>Entity.Session.RequestWindowClose());
        Scripts.UiActions.Register(this,"menu.speed",value=>SetSettings((float)value.Value,LookSensitivity));Scripts.UiActions.Register(this,"menu.sensitivity",value=>SetSettings(MoveSpeed,(float)value.Value));
    }
    private static void ShowPanel(UiElement? panel,bool active)
    {
        if(panel is null)return;panel.SetEnabled(active);if(active)panel.SetVisible(true);
        panel.MoveTo(active?new(24,24):new(56,24),.2);panel.FadeTo(active?1:0,.2);
    }
    private void HideFinishedPanels()
    {
        var visuals=Entity.Session.Ui.Visuals().ToDictionary(value=>value.Id);
        foreach(var item in new[]{(mainPanel,MenuFlowState.MainMenu),(pausePanel,MenuFlowState.Paused),(settingsPanel,MenuFlowState.Settings)})
            if(item.Item1 is { } panel&&State!=item.Item2&&(!visuals.TryGetValue(panel.Id,out var visual)||visual.Opacity==0))panel.SetVisible(false);
    }
    public override void OnUpdate(double seconds)
    {
        _=seconds;
        HideFinishedPanels();
        if(Input.Pressed(27))
        {
            if(State==MenuFlowState.Playing)Pause();else if(State==MenuFlowState.Paused)Resume();else if(State==MenuFlowState.Settings)CloseSettings();
        }
        if(State!=MenuFlowState.Playing)return;
        var view=ViewEntity.StableId==0?Entity:ViewEntity.Resolve(Entity.Session);
        RayHit? hit=null;
        if(view is not null)
        {
            var transform=view.Transform;
            var direction=Vector3.Transform(-Vector3.UnitZ,transform.Rotation);
            hit=Entity.Session.Physics.Raycast(transform.Position,direction,InteractionRange);
        }
        Target=hit is { } found?EntityReference.From(found.Entity):default;
        prompt?.SetText(hit is { } target?Entity.Session.Text("MenuFlow.TargetHint")+" "+target.Entity.Name:Entity.Session.Text("MenuFlow.NoTarget"));
        if(Input.Pressed(69)&&Target.StableId!=0)Scripts.Commands.Execute("menu.interact",new MenuInteractionCommand(Target));
    }
    private UiElement Panel(string name,Vector2 size,Vector2 offset)
    {
        var panel=Entity.Session.Ui.Create(UiKind.Panel);panel.SetPointerBlocking(name!="hud");var style=UiStyle.Default;
        style.Size=size;style.Offset=offset;style.Layout=UiLayout.Vertical;style.Padding=12;style.Spacing=8;panel.SetStyle(style);
        panel.SetColor(name=="hud"?new(0,0,0,0):new(.025f,.04f,.065f,.96f));
        if(name!="hud"){panel.MoveTo(offset+new Vector2(32,0),0);panel.FadeTo(0,0);panel.SetEnabled(false);panel.SetVisible(false);}
        roots.Add(panel);controls[name]=panel.Id;return panel;
    }
    private UiElement Label(UiElement parent,string key)
    {var value=Entity.Session.Ui.Create(UiKind.Label,parent);var style=UiStyle.Default;style.Size=new(240,28);value.SetStyle(style);value.SetText(Entity.Session.Text(key));return value;}
    private void Button(UiElement parent,string id,string key,string action)
    {
        var value=Entity.Session.Ui.Create(UiKind.Button,parent);var style=UiStyle.Default;style.Size=new(240,32);value.SetStyle(style);
        value.SetText(Entity.Session.Text(key));value.Bind(UiEventKind.Click,this,action);controls[id]=value.Id;
    }
    private UiElement Slider(UiElement parent,string id,double minimum,double maximum,double current)
    {
        var value=Entity.Session.Ui.Create(UiKind.Slider,parent);var style=UiStyle.Default;style.Size=new(240,24);value.SetStyle(style);
        value.SetRange(minimum,maximum);value.SetValue(current);controls[id]=value.Id;return value;
    }
}

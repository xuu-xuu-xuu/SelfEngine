using SelfEngine;
using System.Numerics;

namespace ProjectScripts;

[ScriptId("sample.project_menu")]
public sealed class Menu : Behaviour
{
    public override bool UpdateWhenPaused => true;
    private UiElement? panel;
    private UiElement? status;
    public override void OnCreate()
    {
        var session = Entity.Session;
        panel = session.Ui.Create(UiKind.Panel);
        panel.SetPointerBlocking(true);
        var panelStyle = UiStyle.Default; panelStyle.Size = new(280,180); panelStyle.Offset = new(24,24);
        panelStyle.Layout = UiLayout.Vertical; panelStyle.Padding = 12; panelStyle.Spacing = 8; panel.SetStyle(panelStyle);
        status = session.Ui.Create(UiKind.Label, panel); status.SetText(session.Text("Sample.MenuReady"));
        var toggle = session.Ui.Create(UiKind.Button, panel); toggle.SetText(session.Text("Sample.MenuToggle"));
        bool active = false;
        toggle.Subscribe(this, UiEventKind.Click, _ => { active = !active; status.SetText(session.Text(active ? "Sample.MenuActive" : "Sample.MenuReady")); });
        var close = session.Ui.Create(UiKind.Button, panel); close.SetText(session.Text("Sample.MenuClose"));
        close.Subscribe(this, UiEventKind.Click, _ => session.RequestWindowClose());
    }
    public override void OnDestroy() { panel?.Remove(); panel = status = null; }
}

[ScriptId("sample.project_character")]
public sealed class PlayerCharacter : Behaviour
{
    // ViewEntity 是独立顶层相机的数据实体；没有它时仍可使用无视角的角色移动。
    [Expose] public EntityReference ViewEntity;
    [Expose] public float MoveSpeed=4;
    [Expose] public float LookSensitivity=0.003f;
    [Expose] public Vector3 EyeOffset=new(0,0.6f,0);
    private CharacterController? controller;
    private Vector2 previousPointer;
    private bool hasPointer;
    public float Yaw { get; private set; }
    public float Pitch { get; private set; }
    public bool ViewAvailable { get; private set; }
    public CharacterState CharacterState => controller?.State??new(Entity.Transform.Position,0,false);
    public override void OnCreate()
    {
        if(!float.IsFinite(MoveSpeed)||MoveSpeed<1||MoveSpeed>10||!float.IsFinite(LookSensitivity)
            ||LookSensitivity<0.001f||LookSensitivity>0.02f)throw new ArgumentException("MenuFlow.InvalidSettings");
        var description=CharacterDescription.Default;
        // 原生方向输入允许[0,1]幅度；调速仅改变幅度，不重建胶囊/重置竖直状态。
        description.WalkSpeed=10;controller=new(Entity,description);
        Subscribe<MenuSettingsChanged>(settings=>
        {
            MoveSpeed=settings.MoveSpeed;LookSensitivity=settings.LookSensitivity;hasPointer=false;
        });
        var view=ViewEntity.Resolve(Entity.Session);
        if(view is not null&&view.StableId!=Entity.StableId)
        {
            var direction=Vector3.Transform(-Vector3.UnitZ,view.Transform.Rotation);
            Yaw=MathF.Atan2(-direction.X,-direction.Z);Pitch=MathF.Asin(Math.Clamp(direction.Y,-1,1));
        }
    }
    public override void OnEnable()=>hasPointer=false;
    public override void OnDisable()=>hasPointer=false;
    private void UpdateLook()
    {
        var view=ViewEntity.Resolve(Entity.Session);
        if(view is null||view.StableId==Entity.StableId){hasPointer=false;return;}
        var pointer=Input.Pointer;
        if(pointer.Consumed){hasPointer=false;return;}
        if(hasPointer)
        {
            var delta=pointer.Position-previousPointer;
            Yaw-=delta.X*LookSensitivity;
            Yaw=MathF.IEEERemainder(Yaw,MathF.Tau);
            Pitch=Math.Clamp(Pitch-delta.Y*LookSensitivity,-1.5f,1.5f);
        }
        previousPointer=pointer.Position;hasPointer=true;
    }
    private void UpdateView()
    {
        var view=ViewEntity.Resolve(Entity.Session);ViewAvailable=view is not null&&view.StableId!=Entity.StableId;
        if(!ViewAvailable)return;
        var transform=view!.Transform;transform.Position=controller!.State.Position+EyeOffset;
        transform.Rotation=Quaternion.CreateFromYawPitchRoll(Yaw,Pitch,0);view.Transform=transform;
    }
    public override void OnFixedUpdate(double seconds)
    {
        UpdateLook();
        var direction=new Vector3((Input.Down(68)?1:0)-(Input.Down(65)?1:0),0,(Input.Down(83)?1:0)-(Input.Down(87)?1:0));
        if(direction.LengthSquared()>1)direction=Vector3.Normalize(direction);
        direction=Vector3.Transform(direction,Quaternion.CreateFromAxisAngle(Vector3.UnitY,Yaw))*(MoveSpeed/10);
        controller!.Move(direction,Input.Pressed(32),seconds);UpdateView();
    }
    public override void OnUpdate(double seconds){_=seconds;UpdateLook();UpdateView();}
    public override void OnDestroy() { if (Entity.IsAlive) controller?.Destroy(); controller = null; }
}

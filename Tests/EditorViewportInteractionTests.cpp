#include "ToolLayer/Editor/EditorViewportController.h"
#include "ToolLayer/Editor/EditorUi/EditorInputAdapter.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/WindowsInput.h"
#include <Windows.h>
#include <iostream>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* label){if(!value)throw std::runtime_error(label);}
bool Near(Vec3 a,Vec3 b){return Length(a-b)<.002f;}
struct Fixture
{
    EditorSession session;
    AssetStore assets{".","out/validation/editor-viewport-unity/cache"};
    EditorViewportCamera camera;
    EditorViewportController view{camera};
    StableEntityId cube=0;
    ui::UiRect area{101,73,800,600};
    Fixture()
    {
        Check(session.Apply("fixture",[&](World& world){
            const auto e=world.Create("Cube"),c=world.Create("Camera");cube=world.Get(e)->stableId;
            CameraComponent lens;lens.primary=true;
            return world.SetMesh(e,{BuiltinCubeAssetId,{}})&&world.SetCamera(c,lens)&&world.SetTransform(c,{{0,2,6},{},{1,1,1}});
        }),"fixture");
        camera.state={{3,3,6},.46f,-.4f};view.Action("Grid",session,assets);Prepare();
    }
    void Prepare(){view.Prepare(session,assets,area,1);}
    void Event(ui::UiEventType type,float x=400,float y=300,int button=0,int key=0,bool alt=false,bool shift=false,bool allow=true)
    {ui::UiInputEvent e;e.type=type;e.x=x;e.y=y;e.button=static_cast<std::uint8_t>(button);e.key=key;e.alt=alt;e.shift=shift;view.Handle(e,session,assets,allow);}
    void Key(int key,bool shift=false)
    {Event(ui::UiEventType::KeyDown,0,0,0,key,false,shift);Event(ui::UiEventType::KeyUp,0,0,0,key,false,shift);}
    Transform TransformValue(){return *session.EditWorld().GetTransform(session.EditWorld().FindByStableId(cube));}
};
void Navigation()
{
    using enum ui::UiEventType;Fixture f;
    const auto worldCamera=f.session.EditWorld().GetWorldMatrix(f.session.EditWorld().FindByStableId(2));
    const auto initial=f.camera.state;
    f.Event(PointerDown,400,300,1,0,false,false,false);Check(!f.view.Captured(),"UI does not start navigation");
    f.Event(PointerDown,400,300,1);f.Event(KeyDown,0,0,0,'W');f.view.Update(.25,f.session,f.assets);
    Check(std::abs(Length(f.camera.state.position-initial.position)-.75f)<.001f,"W movement uses elapsed time");
    f.Event(KeyUp,0,0,0,'W');const auto y=f.camera.state.position.y;
    f.Event(KeyDown,0,0,0,'Q');f.view.Update(.25,f.session,f.assets);Check(std::abs(f.camera.state.position.y-y+.75f)<.001f,"Unity Q down");
    f.Event(KeyUp,0,0,0,'Q');f.Event(KeyDown,0,0,0,'E');f.view.Update(.25,f.session,f.assets);Check(std::abs(f.camera.state.position.y-y)<.001f,"Unity E up");
    Check(f.view.Tool()==1,"fly keys do not change tools");
    f.Event(PointerMove,430,280);Check(f.camera.state.yaw!=initial.yaw&&f.camera.state.pitch!=initial.pitch,"look changes orientation");
    const auto speed=f.view.Speed();ui::UiInputEvent wheel;wheel.type=PointerWheel;wheel.wheel=1;f.view.Handle(wheel,f.session,f.assets,true);Check(f.view.Speed()>speed,"fly wheel adjusts speed");
    f.Event(PointerUp,430,280,1);const auto stopped=f.camera.state.position;f.view.Update(1,f.session,f.assets);Check(Near(stopped,f.camera.state.position),"release clears movement");
    f.Key('E');Check(f.view.Tool()==2,"E rotate outside flight");f.Key('Q');Check(f.view.Tool()==0,"Q view tool");
    f.Key('W');f.Key('X');Check(f.view.Local(),"X local");f.Event(KeyDown,0,0,0,'X');f.Event(KeyDown,0,0,0,'X');Check(!f.view.Local(),"repeat does not toggle twice");f.Event(KeyUp,0,0,0,'X');
    f.Event(PointerDown,400,300,1);f.Event(KeyDown,0,0,0,'W');f.Event(FocusLost);const auto lost=f.camera.state.position;f.view.Update(1,f.session,f.assets);Check(!f.view.Captured()&&Near(lost,f.camera.state.position),"focus loss cancels");
    Check(f.session.EditWorld().GetWorldMatrix(f.session.EditWorld().FindByStableId(2)).values==worldCamera.values,"navigation leaves game camera untouched");
}
void SelectionAndTools()
{
    using enum ui::UiEventType;Fixture f;
    const auto p=ProjectWorldPoint(*f.view.Camera(),{});Check(bool(p),"project cube");
    f.Event(PointerDown,p->x,p->y);Check(f.session.Selected()==f.cube,"pick rendered cube with viewport offset");f.Prepare();
    Check(std::none_of(f.view.Overlay().commands.begin(),f.view.Overlay().commands.end(),[](const auto& c){
        return c.kind==ui::UiPaintCommandKind::Line&&c.color.r==1&&c.color.g==.6f;
    }),"mesh selection no longer draws a depth-free UI box");
    Check(!f.view.SceneLines().empty(),"transform handles are submitted as world-space lines");
    f.Key('F');f.Prepare();const auto center=ProjectWorldPoint(*f.view.Camera(),{});Check(center&&std::abs(center->x-501)<1&&std::abs(center->y-373)<1,"F frames selected in viewport center");
    const auto beforeOrbit=f.camera.state.position;f.Event(PointerDown,501,373,0,0,true);f.Event(PointerMove,551,393);f.Event(PointerUp,551,393);
    Check(!Near(beforeOrbit,f.camera.state.position),"Alt LMB orbits");f.Prepare();
    const auto beforePan=f.camera.state.position;f.Event(PointerDown,501,373,2);f.Event(PointerMove,521,383);f.Event(PointerUp,521,383,2);Check(!Near(beforePan,f.camera.state.position),"MMB pans");
    for(const char tool:{'W','E','R'})
    {
        f.Key(tool);f.Event(PointerMove,110,80);f.Prepare();const auto before=f.TransformValue();const auto count=f.session.UndoCount();
        std::optional<Vec3> start,end;
        for(const auto& c:f.view.SceneLines())if(c.color.x==.95f)
        {
            auto a=ProjectWorldPoint(*f.view.Camera(),c.a),b=ProjectWorldPoint(*f.view.Camera(),c.b);
            if(a&&b&&(tool=='E'||std::hypot(b->x-a->x,b->y-a->y)>20)){start=a;end=b;break;}
        }
        if(!start||!end)throw std::runtime_error(std::string("visible transform handle: ")+tool);
        const Vec2 mid{(start->x+end->x)*.5f,(start->y+end->y)*.5f};
        const float length=std::hypot(end->x-start->x,end->y-start->y);
        const Vec2 d{(end->x-start->x)/length*35,(end->y-start->y)/length*35};
        f.Event(PointerDown,mid.x,mid.y);Check(f.view.Captured(),"gizmo gets click before scene selection");
        f.Event(PointerMove,mid.x+d.x*.5f,mid.y+d.y*.5f);f.Event(PointerMove,mid.x+d.x,mid.y+d.y);f.Event(PointerUp,mid.x+d.x,mid.y+d.y);
        Check(f.TransformValue().Matrix().values!=before.Matrix().values,"transform changes object");
        Check(f.session.UndoCount()==count+1,"drag is one undo transaction");
        Check(f.session.Undo()&&f.TransformValue().Matrix().values==before.Matrix().values,"undo restores transform");
        Check(f.session.Redo(),"redo transform");Check(f.session.Undo(),"restore fixture");
        f.Prepare();f.Event(PointerDown,mid.x,mid.y);f.Event(PointerMove,mid.x+d.x,mid.y+d.y);f.Key(27);
        Check(!f.view.Captured()&&f.TransformValue().Matrix().values==before.Matrix().values,"Esc cancels drag");
    }
    f.Key('Z');Check(f.view.Center(),"Z center mode");
    f.view.Blur(f.session);f.Key('W');Check(f.view.Tool()==1,"tool shortcuts also work after selecting in Hierarchy");
    Check(f.session.StartPlay(),"play start");f.view.Update(.01,f.session,f.assets);f.Prepare();Check(!f.view.Camera()&&f.view.Overlay().commands.empty()&&f.view.SceneLines().empty(),"Play leaves game camera and hides editor overlays");f.session.StopPlay();f.Prepare();Check(bool(f.view.Camera()),"Stop restores editor camera");
}
void Adapter()
{
    se::ui::editor::EditorInputAdapter adapter(2);ui::UiInputEvent e;
    Check(adapter.Translate(WM_MBUTTONDOWN,0,MAKELPARAM(222,144),e)&&e.button==2&&e.x==111&&e.y==72,"middle button and DPI");
    Check(adapter.Translate(WM_RBUTTONUP,0,0,e)&&e.button==1&&e.type==ui::UiEventType::PointerUp,"right button identity");
    Check(adapter.Translate(WM_SYSKEYDOWN,'X',0,e)&&e.type==ui::UiEventType::KeyDown,"Alt system keys");
    Check(adapter.Translate(WM_CAPTURECHANGED,0,0,e)&&e.type==ui::UiEventType::PointerCaptureLost,"capture loss");
    const auto scan=MapVirtualKeyExW('E',MAPVK_VK_TO_VSC,GetKeyboardLayout(0));
    Check(scan!=0&&adapter.Translate(WM_KEYDOWN,VK_PROCESSKEY,static_cast<LPARAM>(scan<<16),e)&&e.key=='E',"IME process key restores E for editor shortcuts");
    adapter.SetTextInputActive(true);
    Check(adapter.Translate(WM_KEYDOWN,VK_PROCESSKEY,static_cast<LPARAM>(scan<<16),e)&&e.key==VK_PROCESSKEY,"text field preserves IME processing");
    WindowState window;Check(CreateEngineWindow(window,false),"IME message fixture window");
    adapter.SetWindow(window.hwnd);adapter.SetTextInputActive(false);
    Check(reinterpret_cast<UINT_PTR>(GetPropW(window.hwnd,WindowsTextInputModeProperty))==1,"message pump shortcuts context");
    Check(SetPropW(window.hwnd,WindowsImeVirtualKeyProperty,reinterpret_cast<HANDLE>(static_cast<UINT_PTR>('E')))!=FALSE,"captured IME original key");
    Check(adapter.Translate(WM_KEYDOWN,VK_PROCESSKEY,0,e)&&e.key=='E',"message pump original key works without scan code");
    adapter.SetTextInputActive(true);
    Check(reinterpret_cast<UINT_PTR>(GetPropW(window.hwnd,WindowsTextInputModeProperty))==2,"message pump preserves text context");
    adapter.SetWindow(nullptr);Check(GetPropW(window.hwnd,WindowsTextInputModeProperty)==nullptr,"adapter releases text mode property");
    RemovePropW(window.hwnd,WindowsImeVirtualKeyProperty);window.postQuitOnDestroy=false;DestroyEngineWindow(window);
}
void CenterAndFollow()
{
    Fixture f;Check(f.session.Select(f.cube),"center selection");EditorGizmo gizmo;
    const auto before=f.TransformValue();const Vec3 pivot{1,.5f,0};
    Check(gizmo.Begin(f.session,GizmoMode::Rotate,GizmoAxis::Z,{},{1,0},80,GizmoSpace::World,nullptr,&pivot)
        &&gizmo.Update(f.session,{Pi*.5f*80,0})&&gizmo.End(f.session),"rotate around supplied Center");
    Check(Near(f.TransformValue().position,pivot+Rotate(AxisAngle({0,0,1},Pi*.5f),before.position-pivot)),"Center rotation moves origin around center");
    Check(f.session.Undo(),"center rotation undo");
    Check(gizmo.Begin(f.session,GizmoMode::Scale,GizmoAxis::X,{},{1,0},80,GizmoSpace::Local,nullptr,&pivot)
        &&gizmo.Update(f.session,{40,0})&&gizmo.End(f.session),"local scale around Center");
    Check(Near(f.TransformValue().position,{-0.5f,0,0})&&f.session.Undo(),"Center stays fixed under local scaling");
    const auto point=ProjectWorldPoint(*f.view.Camera(),{});f.Event(ui::UiEventType::PointerDown,point->x,point->y);
    f.Key('F',true);const auto camera=f.camera.state.position;auto moved=f.TransformValue();moved.position={1,2,3};
    Check(f.session.SetTransform(f.cube,moved),"follow target moved");f.view.Update(.1,f.session,f.assets);
    Check(Near(f.camera.state.position,camera+Vec3{1,2,3}),"Shift F follows target displacement");
}
}
int main()
{
    try{Navigation();SelectionAndTools();Adapter();CenterAndFollow();std::cout<<"Unity viewport interaction tests passed.\n";return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

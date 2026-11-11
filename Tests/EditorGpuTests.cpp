#include "ToolLayer/Editor/EditorApplication.h"
#include "ToolLayer/Editor/EditorSession.h"
#include "ToolLayer/Editor/EditorSessionStore.h"
#include "ToolLayer/Editor/EditorViewportCamera.h"
#include "CoreLayer/Localization.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "PlatformLayer/Windows/Window.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <imgui/imgui_internal.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace se;
namespace
{
void Check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
class ObservedRenderer:public Renderer
{
public:
    SandboxRenderer gpu{SandboxScene::Scene};HWND window=nullptr;FrameData last;ImGuiContext* context=nullptr;
    const Localization* localization=nullptr;
    struct Target {Vec2 point;ImGuiID id=0;};
    std::filesystem::path root;std::ofstream trace;bool valid=true,expectEditorCamera=true;std::uint64_t frames=0;Vec2 mouse{};
    explicit ObservedRenderer(std::filesystem::path path):root(std::move(path)),trace(root/"input.csv")
    {trace<<"frame,selected_x,camera_x,camera_y,camera_z,viewport_width,viewport_height,gui_vertices,active_id,want_text,mouse_x,mouse_y,left,focused,hovered_window\n";}
    bool Initialize(const RenderSurface& surface,const Localization& text)override{window=surface.window;localization=&text;return gpu.Initialize(surface,text);}
    RenderResult Render(const RenderFrame& frame,const Localization& text)override
    {
        if(!frame.data)return RenderResult::Failed;last=*frame.data;++frames;
        valid=valid&&last.editorGuiEnabled&&!last.editorGui.vertices.empty()&&last.editorCamera.has_value()==expectEditorCamera;
        float x=0;for(const auto& mesh:last.meshes)if(mesh.entity==101)x=mesh.world(0,3);
        const auto pos=last.editorCamera?last.editorCamera->position:Vec3{};
        trace<<frames<<','<<x<<','<<pos.x<<','<<pos.y<<','<<pos.z<<','<<last.sceneViewport.width<<','<<last.sceneViewport.height<<','<<last.editorGui.vertices.size()<<','<<(context?context->ActiveId:0)<<','<<(context?context->IO.WantTextInput:false)<<','<<(context?context->IO.MousePos.x:0)<<','<<(context?context->IO.MousePos.y:0)<<','<<(context?context->IO.MouseDown[0]:false)<<','<<(context?!context->IO.AppFocusLost:false)<<','<<(context&&context->HoveredWindow?context->HoveredWindow->Name:"")<<'\n';trace.flush();
        if(frames==6||frames==11||frames==31||frames==46||frames==53||frames==60||frames==62||frames==63||frames==66)gpu.SetCaptureOutput(root/(std::to_string(frames)+".ppm"),frame.updateIndex);
        return gpu.Render(frame,text);
    }
    EditorRenderView QueryEditorView()const override{return gpu.QueryEditorView();}
    void Shutdown()noexcept override{const auto stats=gpu.Statistics();valid=valid&&stats.debugWarnings==0&&stats.debugErrors==0;gpu.Shutdown();}
    void Move(float x,float y){mouse={x,y};SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(static_cast<int>(std::lround(x)),static_cast<int>(std::lround(y))));}
    void Button(bool down,bool right=false){Move(mouse.x,mouse.y);SendMessageW(window,right?(down?WM_RBUTTONDOWN:WM_RBUTTONUP):(down?WM_LBUTTONDOWN:WM_LBUTTONUP),down?(right?MK_RBUTTON:MK_LBUTTON):0,MAKELPARAM(static_cast<int>(std::lround(mouse.x)),static_cast<int>(std::lround(mouse.y))));}
    void Key(WPARAM key,bool down){SendMessageW(window,down?WM_KEYDOWN:WM_KEYUP,key,static_cast<LPARAM>((MapVirtualKeyW(static_cast<UINT>(key),MAPVK_VK_TO_VSC)<<16)|(down?0:0xc0000000u)));}
    Vec2 Point(Vec3 world)const{Check(last.editorCamera.has_value(),"current editor camera");const auto p=ProjectWorldPoint(*last.editorCamera,world);Check(p.has_value(),"project editor point");return {last.sceneViewport.x+p->x,last.sceneViewport.y+p->y};}
    Vec2 ViewCenter()const{return {last.sceneViewport.x+last.sceneViewport.width*.5f,last.sceneViewport.y+last.sceneViewport.height*.5f};}
    Target InspectorTarget(bool positionX)const
    {
        Check(context!=nullptr,"Inspector context available");
        struct Restore {ImGuiContext* previous=ImGui::GetCurrentContext();~Restore(){ImGui::SetCurrentContext(previous);}} restore;
        ImGui::SetCurrentContext(context);auto* panel=ImGui::FindWindowByID(ImHashStr("###EditorUi.Inspector"));
        Check(panel&&panel->Active&&!panel->Hidden,"Inspector panel available for native input");
        const float height=ImGui::GetFrameHeight(),row=height+ImGui::GetStyle().ItemSpacing.y;
        // 名称、启用、位置按三行排列；使用实际内容原点与当前字体/样式，不绑定窗口绝对坐标。
        Target target{{panel->DC.CursorStartPos.x+height*.5f,panel->DC.CursorStartPos.y+(positionX?2*row:0)+height*.5f},panel->GetID("###EditorUi.Name")};
        if(positionX)
        {
            // DragFloat3 / DragScalarN 依次 PushID(label)、PushID(component)，子控件标签为空。
            const auto group=panel->GetID("###transform.position");const int component=0;
            target.id=ImHashData(&component,sizeof(component),group);
        }
        return target;
    }
    Target ViewportModeTarget(int index)const
    {
        Check(context&&localization&&index>=0&&index<3,"viewport radio target available");
        struct Restore {ImGuiContext* previous=ImGui::GetCurrentContext();~Restore(){ImGui::SetCurrentContext(previous);}} restore;
        ImGui::SetCurrentContext(context);auto* panel=ImGui::FindWindowByID(ImHashStr("###EditorUi.Viewport"));
        Check(panel&&panel->Active&&!panel->Hidden,"viewport panel available for native input");
        const char* keys[]{"EditorUi.Translate","EditorUi.Rotate","EditorUi.ScaleTool"};
        const auto& style=ImGui::GetStyle();const float height=ImGui::GetFrameHeight();float x=panel->DC.CursorStartPos.x;
        for(int prior=0;prior<index;++prior)
            x+=height+style.ItemInnerSpacing.x+ImGui::CalcTextSize(localization->GetText(keys[prior]).c_str()).x+style.ItemSpacing.x;
        return {{x+height*.5f,panel->DC.CursorStartPos.y+height*.5f},panel->GetID((std::string("###")+keys[index]).c_str())};
    }
    void CheckActive(Target target,const char* message)const
    {std::cout<<"native target expected="<<target.id<<" active="<<(context?context->ActiveId:0)<<" x="<<target.point.x<<" y="<<target.point.y<<'\n';Check(context&&context->ActiveId==target.id,message);}
};
float X(const EditorSession& editor){return editor.EditWorld().GetTransform(editor.EditWorld().FindByStableId(101))->position.x;}
struct Ppm {unsigned width=0,height=0;std::vector<unsigned char> pixels;};
Ppm ReadPpm(const std::filesystem::path& path)
{std::ifstream file(path,std::ios::binary);Ppm result;std::string kind;unsigned maximum=0;file>>kind>>result.width>>result.height>>maximum;Check(kind=="P6"&&maximum==255&&result.width&&result.height,"GPU PPM header");file.get();result.pixels.resize(size_t(result.width)*result.height*3);file.read(reinterpret_cast<char*>(result.pixels.data()),static_cast<std::streamsize>(result.pixels.size()));Check(bool(file),"complete GPU PPM");return result;}
void Test(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root/"project/Assets");ObservedRenderer renderer(root);EditorApplicationHooks hooks;std::uint64_t frame=0;float moved=0;std::size_t afterDrag=0;Vec2 dragStart,dragDelta,navigationStart;Vec3 beforeCamera{},beforeText{};Mat4 sceneCamera;
    ObservedRenderer::Target numericTarget,nameTarget,rotateTarget,scaleTarget;UiRect beforeResize;
    hooks.initialize=[&](EditorSession& editor,UiTree&){return editor.Apply("Editor.Test",[&](World& world){auto cube=world.Create("Cube",101),camera=world.Create("SceneCamera",103);const auto t=FreeCameraTransform({{0,2,6},0,-.2f});sceneCamera=t.Matrix();return cube&&camera&&world.SetMesh(cube,{BuiltinCubeAssetId,{}})&&world.SetTransform(camera,t)&&world.SetCamera(camera,{Pi/3,.1f,1000,true});});};
    hooks.inspector=[&](std::uint64_t,bool){renderer.context=ImGui::GetCurrentContext();};
    hooks.drawn=[&](ImGuiContext* context,const EditorFrameResult&){renderer.context=context;};
    hooks.frame=[&](EditorSession& editor,double){
        ++frame;std::cout<<"frame "<<frame<<" selected "<<editor.Selected()<<" x "<<X(editor)<<'\n';
        const auto move=[&](Vec2 p){renderer.Move(p.x,p.y);};
        const auto beginDrag=[&]{const auto center=renderer.Point({X(editor),0,0}),end=renderer.Point({X(editor)+1,0,0});const float length=std::hypot(end.x-center.x,end.y-center.y);dragDelta={(end.x-center.x)/length,(end.y-center.y)/length};dragStart={center.x+dragDelta.x*40,center.y+dragDelta.y*40};move(dragStart);};
        switch(frame)
        {
        case 2:SendMessageW(renderer.window,WM_SETFOCUS,0,0);break;
        case 3:move(renderer.Point({0,0,0}));break;
        case 4:renderer.Button(true);break;
        case 5:renderer.Button(false);break;
        case 6:Check(editor.Selected()==101,"real viewport mouse pick stable ID");break;
        case 7:beginDrag();break;
        case 8:renderer.Button(true);break;
        case 9:move({dragStart.x+dragDelta.x*30,dragStart.y+dragDelta.y*30});break;
        case 10:renderer.Button(false);break;
        case 11:moved=X(editor);afterDrag=editor.UndoCount();Check(moved>.1f&&afterDrag==2,"real gizmo drag is one transaction");break;
        case 12:Check(editor.Undo(),"gizmo undo");break;
        case 13:Check(std::abs(X(editor))<1e-5f&&editor.Redo(),"gizmo undo/redo data");break;
        case 14:Check(std::abs(X(editor)-moved)<1e-5f,"gizmo redo");beginDrag();break;
        case 15:renderer.Button(true);break;
        case 16:move({dragStart.x+dragDelta.x*20,dragStart.y+dragDelta.y*20});break;
        case 17:SendMessageW(renderer.window,WM_KILLFOCUS,0,0);break;
        case 18:Check(std::abs(X(editor)-moved)<1e-5f&&editor.UndoCount()==afterDrag,"focus loss cancels gizmo transaction");SendMessageW(renderer.window,WM_SETFOCUS,0,0);renderer.Button(false);break;
        case 19:beforeCamera=renderer.last.editorCamera->position;navigationStart=renderer.ViewCenter();move(navigationStart);renderer.Button(true,true);break;
        case 20:move({navigationStart.x+15,navigationStart.y+5});renderer.Key('W',true);break;
        case 22:renderer.Key('W',false);renderer.Button(false,true);break;
        case 23:Check(Length(renderer.last.editorCamera->position-beforeCamera)>.005f,"real RMB WASD updates editor camera");Check(editor.EditWorld().GetWorldMatrix(editor.EditWorld().FindByStableId(103)).values==sceneCamera.values,"editor camera never changes scene camera");break;
        // 双击位置 X，实际 WM_CHAR 提交数值，不直接调用 SetTransform。
        case 24:numericTarget=renderer.InspectorTarget(true);move(numericTarget.point);renderer.Button(true);break;
        case 25:renderer.Button(false);break;
        case 26:renderer.Button(true);break;
        case 27:renderer.Button(false);break;
        case 28:renderer.CheckActive(numericTarget,"native numeric input targets position X");Check(renderer.context->IO.WantTextInput,"position X entered native text mode");for(const wchar_t ch:std::wstring(L"2.5"))SendMessageW(renderer.window,WM_CHAR,ch,0);break;
        case 29:renderer.Key(VK_RETURN,true);break;
        case 30:renderer.Key(VK_RETURN,false);break;
        case 31:Check(std::abs(X(editor)-2.5f)<.001f,"native numeric input commits position X");break;
        case 32:nameTarget=renderer.InspectorTarget(false);move(nameTarget.point);renderer.Button(true);break;
        case 33:renderer.Button(false);break;
        case 34:renderer.CheckActive(nameTarget,"native Unicode input targets entity name");beforeText=renderer.last.editorCamera->position;renderer.Button(true,true);break;
        case 35:move(renderer.ViewCenter());renderer.Key('W',true);break;
        case 37:Check(Length(renderer.last.editorCamera->position-beforeText)<1e-6f,"active text input blocks current-frame camera");Check(renderer.context&&renderer.context->IO.WantTextInput,"native text field has keyboard capture");renderer.Key('W',false);renderer.Button(false,true);SendMessageW(renderer.window,WM_UNICHAR,0x4e2d,0);break;
        case 38:renderer.Key(VK_RETURN,true);break;
        case 39:renderer.Key(VK_RETURN,false);break;
        case 42:{Check(editor.EditWorld().Get(editor.EditWorld().FindByStableId(101))->name.find("中")!=std::string::npos,"native Unicode name edit");beforeResize=renderer.last.sceneViewport;auto* state=reinterpret_cast<WindowState*>(GetWindowLongPtrW(renderer.window,GWLP_USERDATA));Check(state&&SetEngineWindowClientSize(*state,1120,640),"real editor window resize");auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(renderer.context);auto* panel=ImGui::FindWindowByID(ImHashStr("###EditorUi.Viewport"));Check(panel!=nullptr,"viewport panel identity");ImGui::SetWindowSize(panel,{550,330});ImGui::SetCurrentContext(previous);break;}
        case 44:Check(renderer.last.sceneViewport.width>=256&&renderer.last.sceneViewport.height>=128&&(renderer.last.sceneViewport.width!=beforeResize.width||renderer.last.sceneViewport.height!=beforeResize.height),"resize updates a usable scene viewport");Check(renderer.last.editorCamera->viewport.width==renderer.last.sceneViewport.width&&renderer.last.editorCamera->viewport.height==renderer.last.sceneViewport.height,"current-frame camera uses panel dimensions");break;
        case 47:rotateTarget=renderer.ViewportModeTarget(1);move(rotateTarget.point);renderer.Button(true);break;
        case 48:renderer.CheckActive(rotateTarget,"native rotate input targets viewport radio");renderer.Button(false);break;
        case 49:beginDrag();break;
        case 50:renderer.Button(true);break;
        case 51:move({dragStart.x+dragDelta.x*20,dragStart.y+dragDelta.y*20});break;
        case 52:renderer.Button(false);break;
        case 53:Check(std::abs(editor.EditWorld().GetTransform(editor.EditWorld().FindByStableId(101))->rotation.x)>.05f,"native rotate radio and gizmo change local quaternion");break;
        case 54:scaleTarget=renderer.ViewportModeTarget(2);move(scaleTarget.point);renderer.Button(true);break;
        case 55:renderer.CheckActive(scaleTarget,"native scale input targets viewport radio");renderer.Button(false);break;
        case 56:beginDrag();break;
        case 57:renderer.Button(true);break;
        case 58:move({dragStart.x+dragDelta.x*24,dragStart.y+dragDelta.y*24});break;
        case 59:renderer.Button(false);break;
        case 60:Check(editor.EditWorld().GetTransform(editor.EditWorld().FindByStableId(101))->scale.x>1.1f,"native scale radio and gizmo change local scale");break;
        case 61:{std::string error;Check(SaveScene(root/"shared.scene.json",editor.EditWorld(),error),"save edited test scene for independent Player");break;}
        case 62:Check(editor.StartPlay(),"editor starts isolated runtime");break;
        case 63:{auto* play=editor.PlayWorld();auto entity=play->FindByStableId(101);auto transform=*play->GetTransform(entity);transform.position.x=-2.5f;Check(play->SetTransform(entity,transform),"runtime edit");break;}
        case 64:Check(std::abs(X(editor)-2.5f)<.001f,"Play changes never affect edit world");editor.StopPlay();break;
        case 65:Check(std::abs(X(editor)-2.5f)<.001f,"Stop restores editor view of unchanged edit world");break;
        default:break;
        }
        renderer.expectEditorCamera=!editor.IsPlaying();return true;
    };
    const int result=RunEditorApp(renderer,"zh-CN",root/"project",true,66,&hooks);
    Check(result==0&&frame==66&&renderer.valid&&renderer.gpu.CaptureSucceeded(),"real editor run completes without GPU diagnostics");
    const auto sessionDirectory=EditorSessionStore::ProjectDirectory(root/"project");EditorSessionSnapshot persisted;std::string sessionError;
    Check(std::filesystem::is_empty(root/"project/Assets"),"editor never implicitly saves the unnamed source scene or creates asset files");
    Check(EditorSessionStore::LoadSession(sessionDirectory,persisted,sessionError)&&std::filesystem::is_regular_file(sessionDirectory/"layout.json")&&!std::filesystem::exists(sessionDirectory/"process.json"),"project session/layout persist and normal host cleanup removes crash marker");
    for(const auto& entry:std::filesystem::recursive_directory_iterator(root/"project"))
        if(entry.is_regular_file())Check(asset_io::Within(sessionDirectory,entry.path()),"new project files stay inside the prescribed editor user-data directory");
    Check(std::filesystem::exists(root/"6.ppm")&&std::filesystem::exists(root/"11.ppm")&&std::filesystem::exists(root/"31.ppm")&&std::filesystem::exists(root/"46.ppm"),"all GPU evidence captures exist");
    World loaded;std::string error;Check(LoadScene(root/"shared.scene.json",loaded,error),"independent scene load");
    SandboxRenderer player(SandboxScene::Scene);player.SetCaptureOutput(root/"player.ppm",3);
    const auto rect=renderer.last.sceneViewport;
    const auto sceneWidth=static_cast<std::uint32_t>(std::lround(rect.width)),sceneHeight=static_cast<std::uint32_t>(std::lround(rect.height));
    Check(sceneWidth>=256&&sceneHeight>=128,"pixel comparison uses a nontrivial actual scene viewport");
    ApplicationServices services;services.maximumFrames=3;services.initialWidth=sceneWidth;services.initialHeight=sceneHeight;
    services.extract=[&](FrameData& data){BuildFrameData(loaded,nullptr,nullptr,data);Check(!data.editorGuiEnabled&&!data.editorDrawData&&!data.editorCamera,"Player snapshot has no Tool UI");};
    Check(RunSandboxApp("en-US",player,false,&services)==0&&player.CaptureSucceeded(),"independent Player host runs same edited scene");
    const auto editorImage=ReadPpm(root/"62.ppm"),playerImage=ReadPpm(root/"player.ppm");
    Check(playerImage.width==sceneWidth&&playerImage.height==sceneHeight&&rect.x>=0&&rect.y>=0&&rect.x+sceneWidth<=editorImage.width&&rect.y+sceneHeight<=editorImage.height,"Player and editor crop use the same actual scene pixel dimensions");
    unsigned maximumDifference=0;for(unsigned y=0;y<playerImage.height;++y)for(unsigned x=0;x<playerImage.width;++x)for(unsigned channel=0;channel<3;++channel)
    {const auto a=editorImage.pixels[((size_t(y)+static_cast<unsigned>(rect.y))*editorImage.width+x+static_cast<unsigned>(rect.x))*3+channel],b=playerImage.pixels[(size_t(y)*playerImage.width+x)*3+channel];maximumDifference=std::max(maximumDifference,static_cast<unsigned>(std::abs(int(a)-b)));}
    Check(maximumDifference<=1,"every Player pixel matches the GUI scene image within one sRGB quantization step");
    std::cout<<"Player/editor scene max RGB difference "<<maximumDifference<<'\n';
}
}
int main(int argc,char** argv)
{
    try{const auto root=std::filesystem::absolute(argc>1?argv[1]:"out/validation/editor-gpu")/asset_io::NewId();std::filesystem::create_directories(root);std::cout<<"evidence "<<root.string()<<'\n';Test(root);std::cout<<"EditorGpuTests passed (native messages, GPU GUI/scene, pick/gizmo/undo/cancel, numeric/Unicode input, capture, camera, resize).\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

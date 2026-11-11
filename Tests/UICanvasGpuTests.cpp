#include "ToolLayer/Editor/EditorApplication.h"
#include "ToolLayer/Editor/ImGuiEditor.h"
#include "FunctionLayer/GameUI/ControlsGallery.h"
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "Scene/SceneDemo.h"
#include <imgui/imgui_internal.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace se;
namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool Near(float a,float b){return std::abs(a-b)<.01f;}
struct Image{unsigned width=0,height=0;std::vector<unsigned char> pixels;};
Image Ppm(const fs::path& path)
{std::ifstream file(path,std::ios::binary);Image image;std::string kind;unsigned max=0;file>>kind>>image.width>>image.height>>max;Check(kind=="P6"&&max==255,"canvas PPM header");file.get();image.pixels.resize(size_t(image.width)*image.height*3);file.read(reinterpret_cast<char*>(image.pixels.data()),static_cast<std::streamsize>(image.pixels.size()));Check(bool(file),"canvas PPM bytes");return image;}
struct Observe:Renderer
{
    SandboxRenderer gpu{SandboxScene::Scene};fs::path output;HWND window=nullptr;unsigned frames=0;bool valid=true,noWorld=true;UiRect viewport{};
    explicit Observe(fs::path path):output(std::move(path)){}
    bool Initialize(const RenderSurface& surface,const Localization& text)override
    {window=surface.window;const bool ok=gpu.Initialize(surface,text);SendMessageW(window,WM_SETFOCUS,0,0);return ok;}
    RenderResult Render(const RenderFrame& frame,const Localization& text)override
    {
        ++frames;Check(frame.data&&frame.data->editorGuiEnabled,"real editor canvas GUI");noWorld=noWorld&&frame.data->meshes.empty()&&frame.data->cameras.empty()&&!frame.data->editorCamera;viewport=frame.data->sceneViewport;
        if(frames==4||frames==8||frames==14||frames==16||frames==23||frames==24||frames==28)gpu.SetCaptureOutput(output/(std::to_string(frames)+".ppm"),frame.updateIndex);
        return gpu.Render(frame,text);
    }
    EditorRenderView QueryEditorView()const override{return gpu.QueryEditorView();}
    void Shutdown()noexcept override{const auto stats=gpu.Statistics();valid=valid&&!stats.debugWarnings&&!stats.debugErrors;gpu.Shutdown();}
    UiPoint Move(UiPoint point)
    {const short x=static_cast<short>(std::lround(point.x)),y=static_cast<short>(std::lround(point.y));SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(x,y));return {float(x),float(y)};}
    void Button(bool down,UiPoint point){point=Move(point);SendMessageW(window,down?WM_LBUTTONDOWN:WM_LBUTTONUP,down?MK_LBUTTON:0,MAKELPARAM(static_cast<short>(point.x),static_cast<short>(point.y)));}
    void Escape(bool down){SendMessageW(window,down?WM_KEYDOWN:WM_KEYUP,VK_ESCAPE,down?1:static_cast<LPARAM>(0xc0000001u));}
};
void Test(const fs::path& root)
{
    const auto project=root/"project";fs::create_directories(project/"Assets");fs::copy_file(GetExecutableDirectory()/"Resources/Textures/checker.png",project/"Assets/checker.png",fs::copy_options::overwrite_existing);AssetId image;std::vector<AssetDiagnostic> diagnostics;
    {AssetStore assets(project/"Assets",project/"Derived");Check(assets.RegisterSource(project/"Assets/checker.png",{},image,diagnostics),"canvas image asset");}
    Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/zh-CN.txt",error),"canvas language");Observe renderer(root);ControlsGallery gallery;UiTree* ui=nullptr;ImGuiEditor* frontend=nullptr;unsigned frame=0;UiStyle initialStyle,movedStyle,resizedStyle;std::string savedDocument;UiPoint down{},moved{},resizeDown{},resizeMoved{};bool configured=false;
    const auto panelPoint=[&]{const auto r=frontend->UiCanvas().ToScreen(ui->Get(gallery.animated)->rectangle);return UiPoint{r.x+8,r.y+r.height-12};};
    EditorApplicationHooks hooks;
    hooks.initialize=[&](EditorSession& editor,UiTree& tree)
    {
        ui=&tree;Check(BuildControlsGallery(tree,text,gallery,image),"canvas complete controls");const std::array<UiEventBinding,1> bindings{{{UiEventKind::Click,88,"sample.menu_flow","menu.start"}}};Check(tree.SetBindings(gallery.button,bindings),"canvas binding description");initialStyle=tree.Get(gallery.animated)->style;
        return editor.Apply("Editor.Test",[](World& world){const auto cube=world.Create("hidden canvas world",88);return cube&&world.SetMesh(cube,{BuiltinCubeAssetId,{}});});
    };
    hooks.configure=[&](ImGuiEditor& editor){frontend=&editor;editor.SetUiCanvasEnabled(true);editor.SetDocumentPaths(root/"unused.scene.json",root/"menu.ui.json");};
    hooks.drawn=[&](ImGuiContext* context,const EditorFrameResult& result)
    {
        Check(result.uiCanvas&&result.uiPreview.size()==(frame==19?26u:28u),"canvas uses real runtime snapshot");
        if(frame==1)
        {
            const auto previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(context);auto* window=ImGui::FindWindowByID(ImHashStr("###EditorUi.UiDesigner"));auto* toolbar=ImGui::FindWindowByID(ImHashStr("###EditorUi.Toolbar"));Check(window&&toolbar,"canvas window and toolbar");const float top=toolbar->Pos.y+toolbar->Size.y;ImGui::SetWindowPos(window,{10,top});ImGui::SetWindowSize(window,{ImGui::GetIO().DisplaySize.x-20,ImGui::GetIO().DisplaySize.y-top-10});ImGui::FocusWindow(window);ImGui::SetCurrentContext(previous);
        }
        if(frame==2||frame==3){Check(frontend->UiCanvas().SetPreview({result.viewport.width,result.viewport.height}),"one-to-one canvas preview");configured=true;}
    };
    hooks.frame=[&](EditorSession&,double)
    {
        ++frame;switch(frame)
        {
        case 5:Check(configured,"configured canvas");down=renderer.Move(panelPoint());renderer.Button(true,down);break;
        case 6:moved=renderer.Move({down.x+40,down.y+24});break;
        case 7:renderer.Button(false,moved);break;
        case 8:{const auto s=ui->Get(gallery.animated)->style;const float scale=ui->CanvasScale()*frontend->UiCanvas().ViewScale();Check(frontend->SelectedUi()==gallery.animated&&Near(s.offset.x,initialStyle.offset.x+(moved.x-down.x)/scale)&&Near(s.offset.y,initialStyle.offset.y+(moved.y-down.y)/scale),"native canvas move and selection");movedStyle=s;break;}
        case 9:Check(frontend->UiUndo(*ui)&&ui->Get(gallery.animated)->style==initialStyle,"native move is one undo command");break;
        case 10:Check(frontend->UiRedo(*ui)&&ui->Get(gallery.animated)->style==movedStyle,"move redo");break;
        case 11:{const auto r=frontend->UiCanvas().ToScreen(ui->Get(gallery.animated)->rectangle);resizeDown=renderer.Move({r.x+r.width-4,r.y+r.height-4});renderer.Button(true,resizeDown);break;}
        case 12:resizeMoved=renderer.Move({resizeDown.x+24,resizeDown.y+16});break;
        case 13:renderer.Button(false,resizeMoved);break;
        case 14:resizedStyle=ui->Get(gallery.animated)->style;Check(resizedStyle.size.x>movedStyle.size.x&&resizedStyle.size.y>movedStyle.size.y&&resizedStyle.offset==movedStyle.offset,"native bottom-right resize keeps zero-pivot origin");Check(frontend->SaveUi(*ui,root/"menu.ui.json"),"save edited UI");savedDocument=ui->SaveJson();break;
        case 15:down=renderer.Move(panelPoint());renderer.Button(true,down);break;
        case 16:moved=renderer.Move({down.x-30,down.y+10});break;
        case 17:renderer.Escape(true);break;
        case 18:renderer.Escape(false);renderer.Button(false,moved);break;
        case 19:Check(ui->SaveJson()==savedDocument,"Escape restores the full design");Check(frontend->EditUi(*ui,[&](UiTree& tree){return tree.Remove(gallery.animated);}),"delete selected subtree");break;
        case 20:Check(frontend->UiUndo(*ui)&&ui->Get(gallery.animatedLabel)&&ui->SaveJson()==savedDocument,"delete undo restores subtree");break;
        case 21:Check(frontend->EditUi(*ui,[&](UiTree& tree){return tree.Reparent(gallery.animatedLabel,gallery.root);}),"change hierarchy");break;
        case 22:Check(frontend->UiUndo(*ui)&&ui->SaveJson()==savedDocument,"hierarchy undo");break;
        case 23:Check(frontend->UiCanvas().SetPreview({640,360}),"small preview");break;
        case 24:Check(ui->SaveJson()==savedDocument&&frontend->UiCanvas().SetPreview({1920,1080}),"large preview preserves document");break;
        case 25:Check(ui->SaveJson()==savedDocument&&frontend->UiCanvas().SetPreview({renderer.viewport.width,renderer.viewport.height}),"restore preview without design mutation");break;
        case 26:renderer.Escape(true);break;
        case 27:renderer.Escape(false);break;
        case 28:Check(frontend->SelectedUi()==0&&ui->SaveJson()==savedDocument&&frontend->SaveUi(*ui,root/"menu.ui.json"),"final clear selection/save");break;
        default:break;
        }
        return true;
    };
    Check(RunEditorApp(renderer,"zh-CN",project,true,28,&hooks)==0&&renderer.frames==28&&renderer.valid&&renderer.noWorld&&renderer.gpu.CaptureSucceeded(),"actual editor canvas run");ui=nullptr;frontend=nullptr;
    UiTree loaded;Check(loaded.LoadJson(savedDocument,error)&&loaded.Get(gallery.button)->bindings.size()==1&&loaded.Get(gallery.button)->bindings[0].action=="menu.start","saved UI binding survives independent load");
    const auto width=static_cast<unsigned>(std::lround(renderer.viewport.width)),height=static_cast<unsigned>(std::lround(renderer.viewport.height));SceneDemo document({},project/"Assets","en-US",root/"menu.ui.json");auto services=document.Services();services.initialWidth=width;services.initialHeight=height;services.maximumFrames=3;SandboxRenderer player(SandboxScene::Clear);player.SetCaptureOutput(root/"player.ppm",3);Check(RunSandboxApp("en-US",player,false,&services)==0&&player.CaptureSucceeded(),"independent Player loads the saved UI file and images");
    const auto editor=Ppm(root/"28.ppm"),play=Ppm(root/"player.ppm");Check(play.width==width&&play.height==height,"same Player canvas pixel size");unsigned maximum=0;size_t pixels=0;
    // 一像素边框是 Tool 的画布标记；内部每个像素都必须与独立 Player 一致。
    const auto left=static_cast<unsigned>(std::lround(renderer.viewport.x)),top=static_cast<unsigned>(std::lround(renderer.viewport.y));
    for(unsigned y=1;y+1<height;++y)for(unsigned x=1;x+1<width;++x){for(unsigned c=0;c<3;++c){const auto a=editor.pixels[((size_t(y)+top)*editor.width+x+left)*3+c],b=play.pixels[(size_t(y)*width+x)*3+c];maximum=std::max(maximum,static_cast<unsigned>(std::abs(int(a)-int(b))));}++pixels;}
    Check(maximum<=1&&pixels>100000,"saved Player and actual editor canvas RGB identity");std::ofstream report(root/"verification.json");report<<nlohmann::json{{"success",true},{"editorFrames",28},{"move",true},{"resize",true},{"cancel",true},{"deleteUndo",true},{"reparentUndo",true},{"previewSizes",3},{"designUnchanged",true},{"savedBindings",true},{"canvasNoWorld",true},{"maximumRgbDifference",maximum},{"comparedPixels",pixels},{"playerWidth",width},{"playerHeight",height},{"manualInputVerified",false}}.dump(2);Check(bool(report),"canvas GPU evidence");
}
}
int main(int argc,char** argv)
{
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_CALL_REPORTFAULT);
#endif
    try{const auto root=fs::absolute(argc>1?argv[1]:"out/validation/ui-canvas-gpu")/asset_io::NewId();fs::create_directories(root);std::cout<<"evidence "<<root<<'\n';Test(root);std::cout<<"UICanvasGpuTests passed (real editor input/controls, move/resize/history, preview isolation and saved Player RGB).\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

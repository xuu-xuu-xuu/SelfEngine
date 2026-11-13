#include "ToolLayer/Editor/EditorApplication.h"
#include "ToolLayer/Editor/EditorFloatingWindows.h"
#include "ToolLayer/Editor/EditorRenderer.h"
#include "ToolLayer/Editor/EditorSession.h"
#include "ToolLayer/Editor/WindowsEditorInput.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

namespace
{
using namespace se;
using namespace se::ui::editor;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Pointer(HWND window,UINT message,float x,float y,WPARAM buttons=0)
{SendMessageW(window,message,buttons,MAKELPARAM(static_cast<int>(std::lround(x)),static_cast<int>(std::lround(y))));}
std::size_t Colors(const std::filesystem::path& file,int& width,int& height)
{
    std::ifstream input(file,std::ios::binary);std::string magic;int maximum=0;input>>magic>>width>>height>>maximum;input.get();
    Check(magic=="P6"&&maximum==255&&width>200&&height>100,"floating capture header");
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width)*height*3);
    input.read(reinterpret_cast<char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()));Check(bool(input),"floating capture pixels");
    std::set<unsigned> colors;
    for(std::size_t i=0;i<pixels.size();i+=3)colors.insert((pixels[i]<<16)|(pixels[i+1]<<8)|pixels[i+2]);
    return colors.size();
}
}
int main(int argc,char** argv)
{
    try
    {
        const auto output=std::filesystem::absolute(argc>1?argv[1]:"out/validation/floating-dock/gpu");
        std::filesystem::create_directories(output);
        const auto project=output/("Project-"+std::to_string(GetCurrentProcessId()));
        std::filesystem::copy("Resources/Templates/BlankScene",project,std::filesystem::copy_options::recursive);
        SetEnvironmentVariableW(L"SELFENGINE_TEST_OFFSCREEN",L"1");SetEnvironmentVariableW(L"USERDOMAIN",L"FlareFloatingGpu");
        WindowsEditorInput::EnableDpiAwareness();
        EditorApplicationHooks hooks;EditorShell* shell=nullptr;unsigned tick=0;std::uint32_t assets=0,viewport=0,last=0;
        hooks.initialize=[](EditorSession& session,UiTree&){std::string error;return session.NewDefaultScene(error);};
        HWND viewportWindow=nullptr,assetWindow=nullptr,lastWindow=nullptr;Vec3 initialCamera{};
        bool navigated=false,returned=false,closed=false;
        hooks.frame=[&](EditorSession&,double){
            if(tick==4)
            {
                SetWindowPos(viewportWindow,nullptr,0,0,860,620,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
                SendMessageW(viewportWindow,WM_EXITSIZEMOVE,0,0);
            }
            return true;
        };
        hooks.toolkitDrawn=[&](EditorShell& ui,EditorSession& session){
            shell=&ui;++tick;
            if(tick==1)
            {
                assets=ui.FloatPanel(EditorPanelId::Assets,-28000,-28000,640,480);
                viewport=ui.FloatPanel(EditorPanelId::Viewport,-27000,-28000,720,540);
                Check(assets&&viewport,"assets and viewport detach");
            }
            if(tick==12)ui.FocusPanel(EditorPanelId::Assets);
            if(tick==13){last=ui.FloatPanel(EditorPanelId::Assets,-28000,-28000);Check(last!=0,"reopened panel can float again");}
            if(tick==18)Check(session.EditWorld().Size()>0,"floating operations keep project world");
        };
        hooks.floatingDrawn=[&](EditorFloatingWindows& host,const FrameData& frame){
            std::cout<<"Floating verification frame "<<tick<<'\n';
            if(tick==2)
            {
                assetWindow=host.Window(assets);viewportWindow=host.Window(viewport);
                Check(assetWindow&&viewportWindow&&assetWindow!=viewportWindow,"two independent native windows");
                host.CaptureNext(assets,output/"assets.ppm");host.CaptureNext(viewport,output/"viewport.ppm");
                Check(frame.editorCamera.has_value(),"floating viewport gets editor camera");initialCamera=frame.editorCamera->position;
                const auto area=shell->Layout().Rect(shell->ViewportPanel());const float dpi=GetDpiForWindow(viewportWindow)/96.0f;
                Pointer(viewportWindow,WM_RBUTTONDOWN,(area.x+100)*dpi,(area.y+100)*dpi,MK_RBUTTON);
                SendMessageW(viewportWindow,WM_KEYDOWN,'W',0);
            }
            if(tick==4)
            {
                SendMessageW(viewportWindow,WM_KEYUP,'W',0);Pointer(viewportWindow,WM_RBUTTONUP,100,130);
                Check(frame.editorCamera&&Length(frame.editorCamera->position-initialCamera)>.0001f,"native input navigates floating viewport");navigated=true;
            }
            if(tick==6)host.CaptureNext(viewport,output/"viewport-resized.ppm");
            if(tick==7)
            {
                const float dpi=GetDpiForWindow(viewportWindow)/96.0f;
                Pointer(viewportWindow,WM_LBUTTONDOWN,20*dpi,8*dpi,MK_LBUTTON);
                POINT target{300,180};ClientToScreen(assetWindow,&target);ScreenToClient(viewportWindow,&target);
                Pointer(viewportWindow,WM_MOUSEMOVE,static_cast<float>(target.x),static_cast<float>(target.y),MK_LBUTTON);
                Pointer(viewportWindow,WM_LBUTTONUP,static_cast<float>(target.x),static_cast<float>(target.y));
            }
            if(tick==8)
            {
                Check(!IsWindow(viewportWindow)&&shell->Dock().SurfaceFor(EditorPanelId::Viewport)==assets,"floating scene merges into another floating window");
                host.CaptureNext(assets,output/"merged-viewport.ppm");
            }
            if(tick==9)
            {
                const auto tab=shell->Layout().Rect(shell->TabButton(EditorPanelId::Viewport));
                const float dpi=GetDpiForWindow(assetWindow)/96.0f;
                Pointer(assetWindow,WM_LBUTTONDOWN,(tab.x+8)*dpi,(tab.y+8)*dpi,MK_LBUTTON);
                POINT target{100,240};ClientToScreen(host.Window(0),&target);ScreenToClient(assetWindow,&target);
                Pointer(assetWindow,WM_MOUSEMOVE,static_cast<float>(target.x),static_cast<float>(target.y),MK_LBUTTON);
                Pointer(assetWindow,WM_LBUTTONUP,static_cast<float>(target.x),static_cast<float>(target.y));
            }
            if(tick==10){Check(shell->Dock().SurfaceFor(EditorPanelId::Viewport)==0&&IsWindow(assetWindow),"viewport returns to main and assets remain floating");returned=true;}
            if(tick==11)SendMessageW(assetWindow,WM_CLOSE,0,0);
            if(tick==12){Check(!IsWindow(assetWindow),"close floating panel destroys its window");closed=true;}
            if(tick==14){lastWindow=host.Window(last);Check(IsWindow(lastWindow),"last auxiliary window ready before editor exit");}
        };
        EditorRenderer renderer;
        const auto result=RunEditorApp(renderer,"zh-CN",project,true,18,&hooks);
        Check(result==0&&navigated&&returned&&closed,"multi-window application lifecycle completes");
        Check(!IsWindow(lastWindow),"main editor shutdown destroys remaining auxiliary windows");
        int aw=0,ah=0,vw=0,vh=0,rw=0,rh=0;
        Check(Colors(output/"assets.ppm",aw,ah)>20,"floating asset text draws");
        Check(Colors(output/"viewport.ppm",vw,vh)>200,"floating scene renders actual shaded pixels");
        Check(Colors(output/"viewport-resized.ppm",rw,rh)>200&&rw>vw&&rh>vh,"floating swapchain and scene resize");
        Check(Colors(output/"merged-viewport.ppm",rw,rh)>200,"scene renders after merging into GUI-only floating window");
        std::ofstream(output/"verification.json")<<"{\"success\":true,\"nativeWindows\":2,\"sceneNavigation\":true,\"resize\":true,\"dockBack\":true,\"closeReopen\":true,\"shutdown\":true}";
        std::cout<<"Floating windows GPU, scene input, resize, dock back and teardown passed.\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<"[FAIL] "<<e.what()<<'\n';return 1;}
}

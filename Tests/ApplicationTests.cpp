#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "FunctionLayer/Rendering/Renderer.h"
#include "PlatformLayer/InputState.h"
#include <Windows.h>
#include <iostream>
#include <stdexcept>
using namespace se;
void Check(bool result,const char* message){if(!result)throw std::runtime_error(message);}
class TestRenderer final : public Renderer
{
public:
    bool failInit=false,throwFrame=false,shutdownWithWindow=false;
    unsigned frames=0,stops=0;HWND window=nullptr;UiTree* tree=nullptr;
    bool Initialize(const RenderSurface& surface,const Localization&) override
    {
        window=surface.window;
        SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(10,10));
        SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(10,10));
        SendMessageW(window,WM_CHAR,L'中',0);SendMessageW(window,WM_CHAR,0xd83d,0);SendMessageW(window,WM_CHAR,0xde00,0);
        tree->SelectText(2,0);SendMessageW(window,WM_CHAR,3,0);SendMessageW(window,WM_CHAR,1,0);
        Check(tree->Get(tree->Focused())->text=="中😀","control WM_CHAR preserves selection text");
        return !failInit;
    }
    RenderResult Render(const RenderFrame& frame,const Localization&) override
    {
        ++frames;if(throwFrame)throw std::runtime_error("injected");
        Check(frame.data&&frame.data->meshes.size()==1,"runtime snapshot delivered");
        if(frame.data->ui.size()!=1||frame.data->ui[0].text!="中😀")
        {std::cerr<<"actual UI count="<<frame.data->ui.size()<<" text="<<(frame.data->ui.empty()?"":frame.data->ui[0].text)<<'\n';throw std::runtime_error("UTF16 Win32 to UI and frame snapshot");}
        return RenderResult::Success;
    }
    void Shutdown() noexcept override{++stops;shutdownWithWindow=IsWindow(window)!=FALSE;}
};
int main()
{
    try
    {
        World world;auto entity=world.Create("mesh");world.SetMesh(entity,{"cpu-asset",{}});
        for(unsigned attempt=0;attempt<4;++attempt)
        {
            UiTree ui;ui.Create(UiKind::TextInput);ApplicationServices services;services.ui=&ui;services.maximumFrames=3;
            unsigned updates=0;bool consumed=false;
            services.input=[&](const InputState&,bool keyboard,bool pointer){consumed=consumed||(keyboard&&pointer);};
            services.update=[&](double dt){Check(dt>=0,"valid delta");++updates;return true;};
            services.extract=[&](FrameData& frame){BuildFrameData(world,&ui,nullptr,frame);};
            TestRenderer renderer;renderer.failInit=attempt==2;renderer.throwFrame=attempt==3;
            renderer.tree=&ui;
            const int result=RunSandboxApp("en-US",renderer,false,&services);
            Check(result==(attempt<2?0:1),"host return code");
            Check(renderer.stops==1&&renderer.shutdownWithWindow,"exactly-once shutdown before HWND destruction");
            if(attempt<2)Check(updates==3&&renderer.frames==3&&consumed,"repeated loops and input consumption");
        }
        FrameData frozen;BuildFrameData(world,nullptr,nullptr,frozen);world.Destroy(entity);Check(frozen.meshes.size()==1,"snapshot owns scene data");
        std::cout<<"ApplicationTests passed (repeated hidden window/UI input/snapshot/failure cleanup)\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<"ApplicationTests failed: "<<e.what()<<'\n';return 1;}
}

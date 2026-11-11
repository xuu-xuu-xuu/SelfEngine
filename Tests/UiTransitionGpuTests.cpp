#include "FunctionLayer/GameUI/UiTransitionDemo.h"
#include "FunctionLayer/Rendering/GameUIRenderer.h"
#include "FunctionLayer/Application/FrameData.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/ImageWriter.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace se;
namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Ok(rhi::Result value,const char* message){if(!value)throw std::runtime_error(std::string(message)+": "+value.Key());}
int Encode(float value){return int(std::lround((value<=.0031308f?value*12.92f:1.055f*std::pow(value,1/2.4f)-.055f)*255));}
bool Same(const rhi::Image& a,const rhi::Image& b)
{if(a.width!=b.width||a.height!=b.height)return false;for(size_t i=0;i<a.rgba8.size();i+=4)for(unsigned c=0;c<3;++c)if(a.rgba8[i+c]!=b.rgba8[i+c])return false;return true;}
void Test(const Localization& text,const fs::path& root)
{
    constexpr unsigned width=960,height=640;WindowState window;window.postQuitOnDestroy=false;Check(CreateEngineWindow(window,false)&&SetEngineWindowClientSize(window,width,height),"transition surface");struct WindowScope{WindowState& w;~WindowScope(){DestroyEngineWindow(w);}} ws{window};rhi::Result result;auto device=rhi::Device::CreateD3D12({window.hwnd,width,height},text,result);Ok(result,"transition device");struct DeviceScope{std::shared_ptr<rhi::Device>& d;~DeviceScope(){d->Shutdown();}} ds{device};
    UiTree ui;UiTransitionDemo ids;Check(BuildUiTransitionDemo(ui,text,ids),"transition example");GameUIRenderer renderer;std::vector<AssetDiagnostic> diagnostics;
    const auto observe=[&]{const auto events=ui.TakeEvents();UpdateUiTransitionDemo(ui,ids,events);ui.Layout(width,height);};
    const auto draw=[&](const char* name)
    {
        FrameData data;data.ui=ui.Visuals();auto commands=device->BeginCommands();Check(bool(commands),"transition commands");Ok(renderer.Configure(*device,*commands,data,width,height,GetExecutableDirectory()/"Resources/Shaders",diagnostics),"transition configure");Ok(commands->BeginPass({rhi::PassKind::Clear}),"transition clear");Ok(commands->EndPass(),"transition clear end");Ok(commands->Close(),"transition close");rhi::CompletionToken token;rhi::Image image;Ok(device->Submit(*commands,token,&image),"transition submit");if(*name)Check(WritePpmImage(root/(std::string(name)+".ppm"),width,height,image.rgba8,diagnostics),"transition image");return image;
    };
    const auto open=draw("open");Check(SetUiTransitionMenuOpen(ui,ids,false),"close menu");ui.Update(.15);observe();const auto half=draw("closing-mid");
    Check(ui.Get(ids.menu)->rectangle.x==440&&ui.Get(ids.menu)->opacity==.5f&&!ui.Get(ids.menu)->enabled,"mid-frame layout/opacity state");
    const float background[]{.015f,.024f,.04f},foreground[]{.04f,.1f,.18f};
    for(unsigned c=0;c<3;++c)
    {
        Check(std::abs(int(half.rgba8[(190*width+450)*4+c])-Encode((background[c]+foreground[c])*.5f))<=2,"actual intermediate linear alpha");
        Check(std::abs(int(half.rgba8[(190*width+410)*4+c])-Encode(background[c]))<=2,"slide vacates old location");
    }
    ui.Update(.15);observe();const auto hidden=draw("hidden");Check(!ui.Get(ids.menu)->visible&&!ui.ActiveTransitions(),"hidden at exact completion");
    for(unsigned c=0;c<3;++c)Check(std::abs(int(hidden.rgba8[(190*width+450)*4+c])-Encode(background[c]))<=2,"hidden menu has no residual GPU pixels");
    SetUiTransitionMenuOpen(ui,ids,true);ui.Update(.15);observe();const auto opening=draw("reopening-mid");
    // 关闭时子按钮禁用、打开时启用，状态皮肤有意不同；面板自身的中间颜色必须一致。
    for(unsigned y=182;y<198;++y)for(unsigned x=442;x<470;++x)for(unsigned c=0;c<3;++c)Check(half.rgba8[(size_t(y)*width+x)*4+c]==opening.rgba8[(size_t(y)*width+x)*4+c],"reverse opening has identical halfway panel pixels");
    ui.Update(.15);observe();const auto reopened=draw("reopened");Check(Same(open,reopened),"completed reopen restores original RGB exactly");
    ui.PointerMove({60,210});observe();ui.Update(.075);const auto hoverMid=draw("hover-mid");ui.Update(.075);const auto hoverEnd=draw("hover-end");size_t changed=0;for(size_t i=0;i<hoverMid.rgba8.size();i+=4)if(hoverMid.rgba8[i]!=hoverEnd.rgba8[i]||hoverMid.rgba8[i+1]!=hoverEnd.rgba8[i+1]||hoverMid.rgba8[i+2]!=hoverEnd.rgba8[i+2])++changed;Check(changed>100,"hover transition changes actual button pixels");
    ui.PointerLeave();observe();ui.Update(.15);Check(Same(open,draw("hover-left")),"hover reversal leaves no residual tint");draw("");draw("");const auto warm=device->Stats();const auto uiWarm=renderer.Statistics();for(unsigned i=0;i<8;++i)draw("");Check(device->Stats().memoryResourceCreations==warm.memoryResourceCreations&&renderer.Statistics().atlasUploads==uiWarm.atlasUploads,"warm transitions reuse GPU/atlas resources");
    SetUiTransitionMenuOpen(ui,ids,false);ui.Update(.1);Check(ui.Remove(ids.menu)&&!ui.ActiveTransitions(),"destroy active menu");observe();draw("deleted");Check(device->Stats().debugWarnings==0&&device->Stats().debugErrors==0,"transition D3D warning/error zero");
    std::ofstream report(root/"verification.json");report<<nlohmann::json{{"success",true},{"width",width},{"height",height},{"linearHalf",true},{"slideVacated",true},{"hiddenClean",true},{"halfPanelRgbIdentity",true},{"reopenRgbIdentity",true},{"hoverPixels",changed},{"hoverRestored",true},{"warmFrames",8},{"destroy",true}}.dump(2);Check(bool(report),"transition GPU evidence");
}
}
int main(int argc,char** argv){try{Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/zh-CN.txt",error),"transition language");const auto root=fs::absolute(argc>1?argv[1]:"out/validation/ui-transition-gpu");fs::create_directories(root);Test(text,root);std::cout<<"UiTransitionGpuTests passed (actual fade/slide, reverse RGB, hover, hidden/deleted state and resource reuse).\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}

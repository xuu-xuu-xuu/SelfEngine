#include "FunctionLayer/GameUI/ControlsGallery.h"
#include "FunctionLayer/GameUI/UiControlGeometry.h"
#include "FunctionLayer/GameUI/UiEventSystem.h"
#include "FunctionLayer/Rendering/GameUIRenderer.h"
#include "FunctionLayer/Application/FrameData.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/ImageWriter.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace se;
namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Ok(rhi::Result value,const char* message){if(!value)throw std::runtime_error(std::string(message)+": "+value.Key());}
UiPoint Center(UiRect r){return {r.x+r.width/2,r.y+r.height/2};}
void Test(const Localization& text,const fs::path& root)
{
    WindowState window;window.postQuitOnDestroy=false;Check(CreateEngineWindow(window,false)&&SetEngineWindowClientSize(window,1280,720),"controls surface");struct WindowScope{WindowState& w;~WindowScope(){DestroyEngineWindow(w);}} ws{window};rhi::Result status;auto device=rhi::Device::CreateD3D12({window.hwnd,1280,720},text,status);Ok(status,"controls device");struct DeviceScope{std::shared_ptr<rhi::Device>& d;~DeviceScope(){d->Shutdown();}} ds{device};
    fs::create_directories(root/"Assets");fs::copy_file(GetExecutableDirectory()/"Resources/Textures/checker.png",root/"Assets/checker.png",fs::copy_options::overwrite_existing);AssetStore assets(root/"Assets",root/"Derived");AssetId image;std::vector<AssetDiagnostic> diagnostics;Check(assets.RegisterSource(root/"Assets/checker.png",{},image,diagnostics),"gallery image asset");
    GameUIRenderer renderer;nlohmann::json records=nlohmann::json::array();
    for(const auto [width,height]:{std::pair{1280u,720u},std::pair{640u,360u},std::pair{1920u,1080u}})
    {
        Ok(device->Resize(width,height),"controls target resize");UiTree tree;ControlsGallery ids;Check(BuildControlsGallery(tree,text,ids,image),"build complete gallery");tree.Layout(float(width),float(height));const float scale=tree.CanvasScale();
        // 子按钮刻意覆盖右侧轨道，用于核对滚动条的最终绘制和命中层级。
        for(const auto id:ids.scrollButtons){auto overlap=tree.Get(id)->style;overlap.size.x=400;tree.SetStyle(id,overlap);}tree.Layout(float(width),float(height));
        const auto observe=[&]{const auto events=tree.TakeEvents();UpdateControlsGallery(tree,ids,events,text);tree.Layout(float(width),float(height));};
        const auto click=[&](UiId id){const auto point=Center(tree.Get(id)->rectangle);Check(tree.PointerDown(point)&&tree.PointerUp(point),"gallery target click");observe();};
        const auto draw=[&](std::string name)
        {
            FrameData f;f.ui=tree.Visuals();f.assets=&assets;auto commands=device->BeginCommands();Check(bool(commands),"gallery commands");Ok(renderer.Configure(*device,*commands,f,width,height,GetExecutableDirectory()/"Resources/Shaders",diagnostics),"gallery configure");Ok(commands->BeginPass({rhi::PassKind::Clear}),"gallery clear");Ok(commands->EndPass(),"gallery clear end");Ok(commands->Close(),"gallery close");rhi::CompletionToken token;rhi::Image imageOut;Ok(device->Submit(*commands,token,&imageOut),"gallery GPU");if(!name.empty())Check(WritePpmImage(root/(std::to_string(width)+"-"+name+".ppm"),width,height,imageOut.rgba8,diagnostics),"gallery capture");return imageOut;
        };
        std::set<UiKind> kinds;for(const auto id:tree.Ids())kinds.insert(tree.Get(id)->kind);Check(kinds.size()==9,"nine independent control kinds");const auto initial=draw("initial");click(ids.checkbox);const auto unchecked=draw("unchecked");
        const auto box=tree.Get(ids.checkbox)->rectangle;size_t changed=0;for(unsigned y=unsigned(box.y);y<unsigned(box.y+box.height);++y)for(unsigned x=unsigned(box.x);x<unsigned(box.x+22*scale);++x){const auto i=(size_t(y)*width+x)*4;if(initial.rgba8[i]!=unchecked.rgba8[i]||initial.rgba8[i+1]!=unchecked.rgba8[i+1]||initial.rgba8[i+2]!=unchecked.rgba8[i+2])++changed;}Check(changed>10,"actual checkbox mark changes");click(ids.checkbox);
        const auto sr=tree.Get(ids.slider)->rectangle;const auto slider=MakeUiSliderGeometry(sr,scale,tree.Get(ids.slider)->value,0,100);UiPoint p{slider.first+(slider.last-slider.first)*.75f,slider.thumb.y+slider.thumb.height/2};Check(tree.PointerDown(p)&&tree.PointerUp(p),"slider interaction");observe();Check(tree.Get(ids.slider)->value==75&&tree.Get(ids.progress)->value==.75,"slider step drives progress");
        const auto values=draw("values");const auto pr=tree.Get(ids.progress)->rectangle;const auto blue=[&](float ratio){const auto x=unsigned(pr.x+pr.width*ratio),y=unsigned(pr.y+pr.height-1);return int(values.rgba8[(size_t(y)*width+x)*4+2]);};Check(blue(.74f)>blue(.76f)+30,"progress pixel boundary matches value");
        const auto scroll=tree.Get(ids.scroll);auto g=MakeUiScrollGeometry(scroll->rectangle,scroll->style.padding*scale,scroll->contentHeight*scale,scroll->scroll*scale,scale);const auto thumb=Center(g.thumb);
        Check(UiEventSystem::HitTarget(tree,thumb)==ids.scroll,"track remains topmost above oversized child");Check(tree.PointerDown(thumb)&&tree.Get(ids.scroll)->scroll==0,"visible thumb grab no jump");tree.PointerUp(thumb);observe();
        tree.Wheel(Center(tree.Get(ids.scroll)->rectangle),-3);observe();UiId selected=0;for(const auto id:ids.scrollButtons){const auto n=tree.Get(id);const auto point=Center(n->rectangle);if(n->clipping.Contains(point)&&UiEventSystem::HitTarget(tree,point)==id){selected=id;click(id);break;}}Check(selected!=0,"scrolled displayed button can be clicked");
        tree.Focus(ids.textInput);tree.EditKey('A',false,true);Check(tree.InsertText("中文 English 😀"),"independent text input sample");observe();const auto final=draw("scrolled");(void)final;
        tree.SetEnabled(ids.button,false);draw("disabled");tree.SetEnabled(ids.button,true);p=Center(tree.Get(ids.button)->rectangle);tree.PointerMove(p);draw("hover");tree.PointerDown(p);draw("pressed");tree.PointerUp(p);observe();Check(ids.clicks==1,"button action remains once");tree.CancelInput();draw("");draw("");const auto warm=device->Stats();const auto uiWarm=renderer.Statistics();
        for(unsigned i=0;i<4;++i)draw("");Check(device->Stats().memoryResourceCreations==warm.memoryResourceCreations&&renderer.Statistics().atlasUploads==uiWarm.atlasUploads,"warm gallery does not create resources");records.push_back({{"width",width},{"height",height},{"kinds",kinds.size()},{"checkboxPixels",changed},{"slider",75},{"progress",.75},{"scroll",tree.Get(ids.scroll)->scroll},{"scrolledTarget",selected},{"buttonClicks",ids.clicks}});
    }
    Check(device->Stats().debugWarnings==0&&device->Stats().debugErrors==0,"controls zero D3D warnings/errors");std::ofstream report(root/"verification.json");report<<nlohmann::json{{"success",true},{"records",records},{"sharedGeometry",true},{"scrollOverlay",true},{"warmFrames",4}}.dump(2);
}
}
int main(int argc,char** argv){try{Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/zh-CN.txt",error),"controls locale");const auto root=fs::absolute(argc>1?argv[1]:"out/validation/ui-controls-gpu");fs::create_directories(root);Test(text,root);std::cout<<"UiControlsGpuTests passed (nine kinds, three windows, checkbox/slider/progress/scroll/text, states and resources).\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}

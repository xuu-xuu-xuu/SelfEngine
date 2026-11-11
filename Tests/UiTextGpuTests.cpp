#include "FunctionLayer/Rendering/GameUIRenderer.h"
#include "FunctionLayer/GameUI/UiTextPresentation.h"
#include "FunctionLayer/Application/FrameData.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Localization.h"
#include "ResourceLayer/Assets/ImageWriter.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace se;
namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Ok(rhi::Result value,const char* message){if(!value)throw std::runtime_error(std::string(message)+": "+value.Key());}
UiId Field(UiTree& tree,float y,std::string text)
{const auto id=tree.Create(UiKind::TextInput);UiStyle s;s.offset={20,y};s.size={240,76};Check(tree.SetStyle(id,s)&&tree.SetText(id,text),"text GPU fixture");return id;}
size_t Different(const rhi::Image& a,const rhi::Image& b)
{Check(a.width==b.width&&a.height==b.height,"text image dimensions");size_t pixels=0;for(size_t i=0;i<a.rgba8.size();i+=4)if(a.rgba8[i]!=b.rgba8[i]||a.rgba8[i+1]!=b.rgba8[i+1]||a.rgba8[i+2]!=b.rgba8[i+2])++pixels;return pixels;}
void Test(const Localization& locale,const fs::path& root)
{
    WindowState window;window.postQuitOnDestroy=false;Check(CreateEngineWindow(window,false)&&SetEngineWindowClientSize(window,640,480),"text GPU window");
    struct WindowScope{WindowState& window;~WindowScope(){DestroyEngineWindow(window);}} ws{window};rhi::Result status;auto device=rhi::Device::CreateD3D12({window.hwnd,640,480},locale,status);Ok(status,"text GPU device");
    struct DeviceScope{std::shared_ptr<rhi::Device>& device;~DeviceScope(){if(device)device->Shutdown();}} ds{device};
    GameUIRenderer renderer;UiTree tree;const auto first=Field(tree,20,"AB中文😀XYZ"),second=Field(tree,125,"");tree.Layout(640,480);tree.Focus(first);tree.SelectText(5,2);
    FrameData frame;std::vector<AssetDiagnostic> diagnostics;
    const auto draw=[&](const char* name,bool dpi=false)
    {
        frame.ui=tree.Visuals();frame.editorGuiEnabled=dpi;frame.editorGui.framebufferScale={2,2};frame.sceneViewport={0,0,640,480};
        auto commands=device->BeginCommands();Check(bool(commands),"text commands");Ok(renderer.Configure(*device,*commands,frame,dpi?1280u:640u,dpi?960u:480u,GetExecutableDirectory()/"Resources/Shaders",diagnostics),"text configure");
        Ok(commands->BeginPass({rhi::PassKind::Clear}),"text clear");Ok(commands->EndPass(),"text clear end");Ok(commands->Close(),"text close");rhi::CompletionToken token;rhi::Image image;Ok(device->Submit(*commands,token,&image),"actual text GPU image");
        if(name)Check(WritePpmImage(root/name,image.width,image.height,image.rgba8,diagnostics),"text evidence PPM");return image;
    };
    const auto selection=draw("selection.ppm");size_t blue=0;for(unsigned y=24;y<80;++y)for(unsigned x=24;x<256;++x){const auto i=(size_t(y)*640+x)*4;if(selection.rgba8[i+2]>selection.rgba8[i]+40&&selection.rgba8[i+1]>selection.rgba8[i]+10)++blue;}
    Check(blue>100,"actual blue selection pixels");const auto committed=tree.Get(first)->text;Check(tree.SetComposition("拼音😀",1),"GPU preedit");const auto composition=draw("composition.ppm");Check(tree.Get(first)->text==committed&&Different(selection,composition)>100,"preedit visible without modifying committed text");
    tree.CancelComposition();tree.Focus(second);const auto caret=draw("empty-caret.ppm");size_t white=0;for(unsigned y=129;y<160;++y){const auto i=(size_t(y)*640+24)*4;if(caret.rgba8[i]>230&&caret.rgba8[i+1]>230&&caret.rgba8[i+2]>230)++white;}Check(white>10,"empty input shows real caret pixels");
    tree.Focus(first);tree.SetText(first,std::string(300,'W'));const auto longText=draw("scroll-clip.ppm");size_t leaked=0;
    for(unsigned y=0;y<480;++y)for(unsigned x=0;x<640;++x)
    {
        const bool inside=(x>=20&&x<260&&y>=20&&y<96)||(x>=20&&x<260&&y>=125&&y<201);if(inside)continue;
        const auto i=(size_t(y)*640+x)*4;for(unsigned c=0;c<3;++c)if(selection.rgba8[i+c]!=longText.rgba8[i+c]){++leaked;break;}
    }
    Check(leaked==0,"text selection/caret/preedit never draws outside input boxes");
    Ok(device->Resize(1280,960),"DPI text target resize");const auto high=draw("dpi-2x.ppm",true);Check(high.width==1280&&high.height==960,"real 2x text framebuffer");draw(nullptr,true);const auto warm=device->Stats();const auto uiWarm=renderer.Statistics();
    for(unsigned i=0;i<8;++i)draw(nullptr,true);const auto after=device->Stats();Check(after.memoryResourceCreations==warm.memoryResourceCreations&&after.uploadPagesCreated==warm.uploadPagesCreated&&renderer.Statistics().atlasUploads==uiWarm.atlasUploads,"stable selection/caret frames reuse GPU resources");
    Check(after.debugWarnings==0&&after.debugErrors==0,"text GPU zero native warnings/errors");std::ofstream report(root/"verification.json");report<<"{\"success\":true,\"selectionPixels\":"<<blue<<",\"emptyCaretPixels\":"<<white<<",\"preeditChangedPixels\":"<<Different(selection,composition)<<",\"clipLeakPixels\":"<<leaked<<",\"dpi2x\":true,\"warmFrames\":8,\"committedUnchanged\":true}\n";
}
}
int main(int argc,char** argv){try{Localization locale;std::string error;Check(locale.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"text locale");const auto root=fs::absolute(argc>1?argv[1]:"out/validation/ui-text-gpu");fs::create_directories(root);Test(locale,root);std::cout<<"UiTextGpuTests passed (actual selection, composition, empty caret, scrolling/clip, DPI and warm resources).\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}

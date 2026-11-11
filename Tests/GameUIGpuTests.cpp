#include "FunctionLayer/Rendering/GameUIRenderer.h"
#include "FunctionLayer/Rendering/ForwardRenderer.h"
#include "FunctionLayer/Rendering/PostProcessRenderer.h"
#include "FunctionLayer/Application/FrameData.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Diagnostics.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/ImageWriter.h"
#include "CoreLayer/Localization.h"
#include <wincodec.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace se;
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Ok(rhi::Result result,const char* message){if(!result)throw std::runtime_error(std::string(message)+": "+result.Key());}
struct Fixture
{
    WindowState window;std::shared_ptr<rhi::Device> device;
    Fixture(const Localization& text){Check(CreateEngineWindow(window,false)&&SetEngineWindowClientSize(window,640,480),"UI surface");int exit=0;Check(PumpWindowMessages(exit),"UI messages");rhi::Result result;device=rhi::Device::CreateD3D12({window.hwnd,640,480},text,result);Ok(result,"UI device");}
    ~Fixture(){if(device)device->Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
};
void AlphaPng(const fs::path& path)
{
    ComPtr<IWICImagingFactory> factory;Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.GetAddressOf()))),"PNG factory");
    ComPtr<IWICStream> stream;ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;
    Check(SUCCEEDED(factory->CreateStream(&stream))&&SUCCEEDED(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE))
        &&SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))&&SUCCEEDED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache))
        &&SUCCEEDED(encoder->CreateNewFrame(&frame,nullptr))&&SUCCEEDED(frame->Initialize(nullptr))&&SUCCEEDED(frame->SetSize(1,1)),"PNG frame");
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;unsigned char pixel[]{0,255,0,128};
    Check(SUCCEEDED(frame->SetPixelFormat(&format))&&format==GUID_WICPixelFormat32bppBGRA&&SUCCEEDED(frame->WritePixels(1,4,4,pixel))&&SUCCEEDED(frame->Commit())&&SUCCEEDED(encoder->Commit()),"PNG alpha fixture");
}
UiVisual Visual(UiKind kind,UiRect rectangle,UiColor color,std::string text={})
{UiVisual v{};v.id=1;v.kind=kind;v.rectangle=rectangle;v.clip={0,0,640,480};v.color=color;v.opacity=1;v.enabled=true;v.fontSize=28;v.text=std::move(text);return v;}
void Quad(GuiGeometry& g)
{g.displaySize={640,480};g.vertices={{{0,0},{0,0}},{{640,0},{1,0}},{{640,480},{1,1}},{{0,480},{0,1}}};g.indices={0,1,2,0,2,3};g.commands={{{0,0,640,480},1,0,6,0,false}};}
bool SameRgb(const rhi::Image& a,const rhi::Image& b)
{if(a.width!=b.width||a.height!=b.height)return false;for(size_t i=0;i<a.rgba8.size();i+=4)for(unsigned c=0;c<3;++c)if(a.rgba8[i+c]!=b.rgba8[i+c])return false;return true;}
float Decode(float x){return x<=.04045f?x/12.92f:std::pow((x+.055f)/1.055f,2.4f);}
int Encode(float x){return int(std::lround((x<=.0031308f?x*12.92f:1.055f*std::pow(x,1/2.4f)-.055f)*255));}
void Test(const Localization& text,const fs::path& root)
{
    Fixture fixture(text);auto& d=*fixture.device;GameUIRenderer renderer;std::vector<AssetDiagnostic> diagnostics;
    AlphaPng(root/"alpha.png");AssetStore assets(root,root/"cache");AssetId imageId;Check(assets.RegisterSource(root/"alpha.png",{},imageId,diagnostics),"register alpha asset");
    FrameData f;f.assets=&assets;
    f.ui.push_back(Visual(UiKind::Panel,{10,10,620,240},{.1f,.2f,.3f,1}));
    auto clipped=Visual(UiKind::Panel,{30,30,80,70},{1,0,0,.5f});clipped.clip={50,40,40,40};f.ui.push_back(clipped);
    auto image=Visual(UiKind::Image,{130,30,80,70},{1,1,1,1});image.imageAsset=imageId;f.ui.push_back(image);
    f.ui.push_back(Visual(UiKind::Label,{20,125,600,50},{1,1,1,1},"中文字体 / English text"));
    f.ui.push_back(Visual(UiKind::Label,{20,185,600,50},{1,1,1,1},"Missing glyph: \xf4\x8f\xbf\xbf"));
    auto draw=[&](bool editor=false,rhi::PipelineHandle output={},rhi::PipelineHandle sky={},float exposure=1)
    {
        auto commands=d.BeginCommands();Check(bool(commands),"UI commands");
        rhi::Texture scene,composite;rhi::Pipeline gui;
        if(editor)
        {
            Ok(d.CreateRenderTarget({640,480,true,false},scene),"UI editor scene");Ok(d.CreateRenderTarget({640,480,false,true},composite),"UI editor output");
            rhi::PipelineDescription p{rhi::PipelineKind::Gui,GetExecutableDirectory()/"Resources/Shaders/Gui.hlsl",rhi::GuiLayoutVersion};p.colorFormat=rhi::ColorFormat::Rgba8Srgb;p.blend=rhi::BlendMode::Alpha;Ok(d.CreatePipeline(p,gui),"UI editor pipeline");
            GuiGeometry g;Quad(g);const std::array refs{*scene};Ok(commands->ConfigureGui(*scene,*composite,*gui,g,refs),"UI editor scene sample");
        }
        Ok(renderer.Configure(d,*commands,f,640,480,GetExecutableDirectory()/"Resources/Shaders",diagnostics),"UI renderer configure");
        if(output){rhi::ForwardLighting lighting;lighting.exposure=exposure;lighting.sky={.2f,.4f,.8f};Ok(RecordForwardFrame(*commands,lighting,output,sky,{}),"UI forward output");}
        else {Ok(commands->BeginPass({rhi::PassKind::Clear}),"UI clear");Ok(commands->EndPass(),"UI clear end");Ok(commands->Close(),"UI close");}
        rhi::CompletionToken token;rhi::Image result;Ok(d.Submit(*commands,token,&result),"UI actual GPU capture");return result;
    };
    const auto initial=draw();const auto pixel=[&](unsigned x,unsigned y,unsigned c){return int(initial.rgba8[(size_t(y)*640+x)*4+c]);};
    const float alpha=128/255.0f;
    for(unsigned c=0;c<3;++c)
    {
        const float base=Decode(pixel(20,20,c)/255.0f);
        Check(std::abs(pixel(60,50,c)-Encode(base*(1-alpha)+(c==0?alpha:0)))<=2,"linear translucent panel oracle");
        Check(std::abs(pixel(160,50,c)-Encode(base*(1-alpha)+(c==1?alpha:0)))<=2,"sRGB image linear alpha oracle");
        Check(pixel(49,50,c)==pixel(20,20,c)&&pixel(90,50,c)==pixel(20,20,c)&&pixel(60,39,c)==pixel(20,20,c)&&pixel(60,80,c)==pixel(20,20,c),"exact clip boundaries");
    }
    size_t textPixels=0;for(unsigned y=125;y<230;++y)for(unsigned x=20;x<620;++x)if(pixel(x,y,0)>200&&pixel(x,y,1)>200&&pixel(x,y,2)>200)++textPixels;
    Check(textPixels>1000,"real CJK English and missing glyph pixels");Check(WritePpmImage(root/"text-images-clip.ppm",640,480,initial.rgba8,diagnostics),"UI evidence");
    std::string growth;for(unsigned i=0;i<40;++i)growth+="W"+std::to_string(i)+" ";
    auto large=Visual(UiKind::Label,{0,260,640,220},{1,1,1,1},growth);large.fontSize=180;f.ui.push_back(large);draw();Check(renderer.Statistics().fontPages>1,"GPU atlas expands to multiple pages");f.ui.pop_back();
    const auto restored=draw();Check(SameRgb(initial,restored),"all old glyph and image pixels preserved after atlas growth");
    const auto warm=d.Stats();const auto uiWarm=renderer.Statistics();
    for(unsigned i=0;i<12;++i)Check(SameRgb(initial,draw()),"repeat frame pixel identity");
    const auto steady=d.Stats();Check(steady.memoryResourceCreations==warm.memoryResourceCreations&&steady.uploadPagesCreated==warm.uploadPagesCreated&&steady.pipelineCreations==warm.pipelineCreations
        &&steady.uploadedBytes==warm.uploadedBytes&&renderer.Statistics().atlasUploads==uiWarm.atlasUploads&&renderer.Statistics().imageUploads==uiWarm.imageUploads,"warm frames create no textures pipelines upload pages or asset uploads");
    Check(SameRgb(initial,draw(true)),"editor scene UI and Player RGB identical");
    rhi::Pipeline output,sky;Ok(d.CreatePipeline({rhi::PipelineKind::FinalOutput,GetExecutableDirectory()/"Resources/Shaders/FinalOutput.hlsl",3},output),"tone map output");
    rhi::PipelineDescription skyDescription{rhi::PipelineKind::Sky,GetExecutableDirectory()/"Resources/Shaders/Sky.hlsl",9};skyDescription.colorFormat=rhi::ColorFormat::Rgba16Float;Ok(d.CreatePipeline(skyDescription,sky),"HDR sky");
    const auto dark=draw(false,*output,*sky,.25f),bright=draw(false,*output,*sky,4);
    for(unsigned y=10;y<250;++y)for(unsigned x=10;x<630;++x)for(unsigned c=0;c<3;++c)Check(dark.rgba8[(size_t(y)*640+x)*4+c]==bright.rgba8[(size_t(y)*640+x)*4+c],"UI after tone mapping independent of exposure");
    Check(dark.rgba8[(450*640+300)*4]!=bright.rgba8[(450*640+300)*4],"scene exposure really changes");
    Check(WritePpmImage(root/"exposure-dark.ppm",640,480,dark.rgba8,diagnostics)&&WritePpmImage(root/"exposure-bright.ppm",640,480,bright.rgba8,diagnostics),"exposure evidence");
    PostProcessRenderer post;rhi::Pipeline shadowDepth,shadowOutput;
    Check(post.Initialize(d,GetExecutableDirectory()/"Resources/Shaders"),"UI full post processing pipelines");
    Ok(d.CreatePipeline({rhi::PipelineKind::ShadowDepth,GetExecutableDirectory()/"Resources/Shaders/ShadowDepth.hlsl",4},shadowDepth),"UI post depth pipeline");
    Ok(d.CreatePipeline({rhi::PipelineKind::ShadowOutput,GetExecutableDirectory()/"Resources/Shaders/ShadowOutput.hlsl",4},shadowOutput),"UI post output pipeline");
    const auto drawPost=[&](float exposure)
    {
        auto c=d.BeginCommands();Check(bool(c),"UI full post commands");Ok(renderer.Configure(d,*c,f,640,480,GetExecutableDirectory()/"Resources/Shaders",diagnostics),"UI full post configure");
        rhi::ForwardLighting lighting;lighting.exposure=exposure;lighting.sky={.2f,.4f,.8f};rhi::ShadowSettings shadows;shadows.enabled=false;rhi::PostProcessSettings settings;const auto pipelines=post.Bindings();
        Ok(RecordShadowedFrame(*c,lighting,shadows,*shadowDepth,*shadowOutput,*sky,{},{},nullptr,&settings,&pipelines,{}),"UI bloom tone mapping and FXAA");
        rhi::CompletionToken completed;rhi::Image result;Ok(d.Submit(*c,completed,&result),"UI full post capture");return result;
    };
    const auto postDark=drawPost(.25f),postBright=drawPost(4);
    for(unsigned y=10;y<250;++y)for(unsigned x=10;x<630;++x)for(unsigned c=0;c<3;++c)Check(postDark.rgba8[(size_t(y)*640+x)*4+c]==postBright.rgba8[(size_t(y)*640+x)*4+c],"UI after complete post chain independent of exposure");
    Check(postDark.rgba8[(450*640+300)*4]!=postBright.rgba8[(450*640+300)*4],"complete post scene exposure changes");
    Check(WritePpmImage(root/"post-ui.ppm",640,480,postBright.rgba8,diagnostics),"complete post evidence");
    const auto saved=f.ui;f.ui.resize(2);f.ui[0].rectangle={0,0,320,240};f.editorGuiEnabled=true;f.editorGui.framebufferScale={2,2};f.sceneViewport={0,0,320,240};
    const auto dpi=draw();const auto at=[&](unsigned x,unsigned y){return dpi.rgba8[(size_t(y)*640+x)*4];};
    Check(at(99,100)==at(20,20)&&at(100,100)>at(20,20)&&at(179,100)>at(20,20)&&at(180,100)==at(20,20)&&at(120,79)==at(20,20)&&at(120,160)==at(20,20),"game UI raster and scissor at 2x framebuffer scale");
    Check(WritePpmImage(root/"dpi-2x.ppm",640,480,dpi.rgba8,diagnostics),"DPI evidence");f.ui=saved;f.editorGuiEnabled=false;
    // 直接命令边界：坏输入不占用配置；typed 句柄与提交后的 Fence 继续保护所有者。
    rhi::Texture white;rhi::Pipeline gui;const std::array<unsigned char,4> rgba{255,255,255,255};Ok(d.CreateTexture({1,1},rgba,white),"UI lifetime white");
    rhi::PipelineDescription p{rhi::PipelineKind::Gui,GetExecutableDirectory()/"Resources/Shaders/Gui.hlsl",rhi::GuiLayoutVersion};p.colorFormat=rhi::ColorFormat::Rgba8Srgb;p.blend=rhi::BlendMode::Alpha;Ok(d.CreatePipeline(p,gui),"UI lifetime pipeline");
    GuiGeometry g;Quad(g);auto commands=d.BeginCommands();auto stale=*white;++stale.generation;const std::array bad{stale},good{*white};
    Check(commands->ConfigureGameUi(*gui,g,bad).error==rhi::Error::InvalidHandle,"game UI stale handle rejected");Ok(commands->ConfigureGameUi(*gui,g,good),"game UI recovers after invalid input");Check(commands->ConfigureGameUi(*gui,g,good).error==rhi::Error::InvalidState,"game UI configured once");
    Ok(commands->BeginPass({rhi::PassKind::Clear}),"pending UI clear");Ok(commands->EndPass(),"pending UI clear end");Ok(commands->Close(),"pending UI close");
    ComPtr<ID3D12Fence> gate;Check(SUCCEEDED(rhi::D3D12DiagnosticsAccess::NativeDevice(d)->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.GetAddressOf()))),"UI gate");
    struct GateRelease{ID3D12Fence* fence;~GateRelease(){fence->Signal(1);}} release{gate.Get()};
    Check(SUCCEEDED(rhi::D3D12DiagnosticsAccess::Queue(d)->Wait(gate.Get(),1)),"UI gate queued");
    const auto life=rhi::D3D12DiagnosticsAccess::Lifetime(d,*white);rhi::CompletionToken token;Ok(d.Submit(*commands,token),"UI in flight");commands.reset();white.reset();gui.reset();renderer.Clear();d.Collect();
    Check(!life.expired()&&!d.IsComplete(token),"game UI owner retained by actual pending Fence");Check(SUCCEEDED(gate->Signal(1)),"UI open gate");Ok(d.WaitIdle(),"UI final drain");d.Collect();Check(life.expired(),"game UI owner released after Fence");
    Check(d.Stats().debugWarnings==0&&d.Stats().debugErrors==0,"game UI zero native warnings/errors");
    std::ofstream report(root/"verification.json");report<<"{\"textPixels\":"<<textPixels<<",\"fontPagesAfterGrowth\":"<<uiWarm.fontPages<<",\"repeatFrames\":12,\"rgbIdentity\":true,\"linearBlend\":true,\"clip\":true,\"dpi2x\":true,\"completePostChain\":true,\"exposureIndependent\":true,\"fenceLifetime\":true}\n";
}
}
int main(int argc,char** argv)
{
    try{const auto hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);Check(SUCCEEDED(hr),"UI COM");struct ComScope{~ComScope(){CoUninitialize();}} com;
        Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"UI localization");const auto root=fs::absolute(argc>1?argv[1]:"out/validation/game-ui");fs::create_directories(root);Test(text,root);std::cout<<"GameUIGpuTests passed (real text/images/clip, linear alpha, atlas growth, warm reuse, editor identity, post-tone-map exposure, typed handles and Fence).\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

#include "PlatformLayer/Graphics/D3D12/D3D12Diagnostics.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Presentation.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/ImageWriter.h"
#include "CoreLayer/Localization.h"
#include <iostream>
#include <stdexcept>
using namespace se;
using Microsoft::WRL::ComPtr;
namespace
{
void Check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void Ok(rhi::Result result,const char* message){if(!result)throw std::runtime_error(std::string(message)+": "+result.Key());}
struct Fixture
{
    WindowState window;std::shared_ptr<rhi::Device> device;
    Fixture(const Localization& text){Check(CreateEngineWindow(window,false)&&SetEngineWindowClientSize(window,320,240),"GUI surface");int exitCode=0;Check(PumpWindowMessages(exitCode),"GUI messages");rhi::Result result;device=rhi::Device::CreateD3D12({window.hwnd,320,240},text,result);Ok(result,"GUI device");}
    ~Fixture(){if(device)device->Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
};
void Quad(GuiGeometry& g,Vec4 rectangle,Vec4 clip,std::uint32_t color)
{
    const auto v=static_cast<unsigned>(g.vertices.size()),i=static_cast<unsigned>(g.indices.size());
    g.vertices.insert(g.vertices.end(),{{{rectangle.x,rectangle.y},{0,0},color},{{rectangle.z,rectangle.y},{1,0},color},{{rectangle.z,rectangle.w},{1,1},color},{{rectangle.x,rectangle.w},{0,1},color}});
    g.indices.insert(g.indices.end(),{0,1,2,0,2,3});g.commands.push_back({clip,1,i,6,v});
}
std::unique_ptr<rhi::CommandContext> Commands(rhi::Device& d,rhi::TextureHandle scene,rhi::TextureHandle output,rhi::PipelineHandle pipeline,const GuiGeometry& g,std::span<const rhi::TextureHandle> textures)
{
    auto c=d.BeginCommands();Check(bool(c),"GUI commands");Ok(c->ConfigureGui(scene,output,pipeline,g,textures),"GUI configure");Ok(c->BeginPass({rhi::PassKind::Clear}),"GUI clear");Ok(c->EndPass(),"GUI clear end");Ok(c->Close(),"GUI close");return c;
}
void Test(const Localization& text,const std::filesystem::path& root)
{
    Fixture f(text);auto& d=*f.device;rhi::Texture scene,output,white;rhi::Pipeline pipeline;
    Ok(d.CreateRenderTarget({73,41,true,false},scene),"scene target");Ok(d.CreateRenderTarget({320,240,false,true},output),"linear blend target");
    const std::array<std::uint8_t,4> pixel{255,255,255,255};Ok(d.CreateTexture({1,1},pixel,white),"GUI white");
    rhi::PipelineDescription p{rhi::PipelineKind::Gui,GetExecutableDirectory()/"Resources/Shaders/Gui.hlsl",rhi::GuiLayoutVersion};p.blend=rhi::BlendMode::Alpha;p.colorFormat=rhi::ColorFormat::Rgba8Srgb;Ok(d.CreatePipeline(p,pipeline),"reflected GUI pipeline");
    GuiGeometry g;g.displayPosition={10,20};g.displaySize={160,120};g.framebufferScale={2,2};
    Quad(g,{10,20,170,140},{10,20,170,140},0xff808080);Quad(g,{10,20,170,140},{40,40,80,80},0x80000080);
    const std::array textures{*white,*white};auto c=Commands(d,*scene,*output,*pipeline,g,textures);rhi::CompletionToken token;rhi::Image image;Ok(d.Submit(*c,token,&image),"actual GUI pixels");
    const auto at=[&](unsigned x,unsigned y,unsigned channel){return int(image.rgba8[(size_t(y)*image.width+x)*4+channel]);};
    Check(std::abs(at(10,10,0)-128)<=1&&std::abs(at(10,10,1)-128)<=1,"sRGB vertex roundtrip");
    Check(std::abs(at(80,60,0)-128)<=1&&std::abs(at(80,60,1)-92)<=2&&std::abs(at(80,60,2)-92)<=2,"linear alpha blend oracle");
    Check(at(59,60,1)==128&&at(160,60,1)==128&&at(80,39,1)==128&&at(80,120,1)==128,"scaled display offset scissor boundary");
    std::vector<AssetDiagnostic> diagnostics;Check(WritePpmImage(root/"linear-clip.ppm",image.width,image.height,image.rgba8,diagnostics),"GUI evidence image");
    auto validate=[&](const GuiGeometry& bad,std::span<const rhi::TextureHandle> refs){auto next=d.BeginCommands();return next->ConfigureGui(*scene,*output,*pipeline,bad,refs);};
    auto bad=g;bad.indices[0]=UINT32_MAX;Check(validate(bad,textures).error==rhi::Error::InvalidArgument,"invalid index before native recording");
    bad=g;bad.commands[1].vertexOffset=UINT32_MAX;Check(!validate(bad,textures),"invalid base vertex");
    bad=g;bad.commands[0].clip.x=std::numeric_limits<float>::quiet_NaN();Check(!validate(bad,textures),"NaN clip rejected");
    bad=g;bad.displaySize.x=159;Check(!validate(bad,textures),"framebuffer dimension mismatch");
    auto wrong=textures;wrong[1].generation+=1;Check(validate(g,wrong).error==rhi::Error::InvalidHandle,"stale texture generation");
    wrong=textures;wrong[1]=*output;Check(validate(g,wrong).error==rhi::Error::Unsupported,"GUI output feedback rejected");
    Check(validate(g,textures).error==rhi::Error::None,"recovery after invalid commands");
    // 场景 Clear 的编码字节经过 sRGB 采样、sRGB RTV 输出后逐通道保持。
    GuiGeometry sceneImage;sceneImage.displaySize={320,240};Quad(sceneImage,{0,0,320,240},{0,0,320,240},0xffffffff);
    const std::array sceneTexture{*scene};c=Commands(d,*scene,*output,*pipeline,sceneImage,sceneTexture);Ok(d.Submit(*c,token,&image),"sample offscreen scene");
    Check(std::abs(int(image.rgba8[0])-20)<=1&&std::abs(int(image.rgba8[1])-46)<=1&&std::abs(int(image.rgba8[2])-82)<=1,"scene one sRGB output");
    // 队列阻塞时释放 CPU token 和命令数据，目标/描述符/上传页仍保持至 Fence。
    c.reset();d.Collect();auto sceneLife=rhi::D3D12DiagnosticsAccess::Lifetime(d,*scene),outputLife=rhi::D3D12DiagnosticsAccess::Lifetime(d,*output),fontLife=rhi::D3D12DiagnosticsAccess::Lifetime(d,*white);
    ComPtr<ID3D12Fence> gate;Check(SUCCEEDED(rhi::D3D12DiagnosticsAccess::NativeDevice(d)->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.GetAddressOf()))),"GUI gate");
    struct GateRelease{ID3D12Fence* fence;~GateRelease(){fence->Signal(1);}} release{gate.Get()};
    Check(SUCCEEDED(rhi::D3D12DiagnosticsAccess::Queue(d)->Wait(gate.Get(),1)),"GUI queued gate");
    const auto waits=d.Stats().globalWaits;
    c=Commands(d,*scene,*output,*pipeline,g,textures);Ok(d.Submit(*c,token),"GUI pending submission");c.reset();scene.reset();output.reset();d.Collect();
    rhi::Texture resizedScene,resizedOutput;Ok(d.CreateRenderTarget({111,61,true,false},resizedScene),"resize in-flight scene target");Ok(d.CreateRenderTarget({320,240,false,true},resizedOutput),"replace in-flight composite");
    c=Commands(d,*resizedScene,*resizedOutput,*pipeline,g,textures);rhi::CompletionToken nextToken;Ok(d.Submit(*c,nextToken),"second GUI frame retains its own upload pages");c.reset();resizedScene.reset();resizedOutput.reset();white.reset();d.Collect();
    Check(d.Stats().globalWaits==waits&&!fontLife.expired()&&!d.IsComplete(nextToken),"target resize and GUI submission never global-drain");
    Check(!d.IsComplete(token)&&!sceneLife.expired()&&!outputLife.expired()&&d.Stats().retiring>0,"targets stay alive while in flight");
    Check(SUCCEEDED(gate->Signal(1)),"open GUI gate");Ok(d.WaitIdle(),"GUI final drain");d.Collect();
    Check(sceneLife.expired()&&outputLife.expired()&&fontLife.expired(),"targets and font retire after real fence");
    Check(d.Stats().debugWarnings==0&&d.Stats().debugErrors==0,"GUI zero D3D warnings and errors");
}
}
int main(int argc,char** argv)
{
    try{Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"GUI localization");const auto root=std::filesystem::absolute(argc>1?argv[1]:"out/validation/gui");std::filesystem::create_directories(root);Test(text,root);std::cout<<"GuiGpuTests passed (actual pixels, linear blending, scissor/DPI, scene sampling, invalid recovery, Fence lifetime).\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

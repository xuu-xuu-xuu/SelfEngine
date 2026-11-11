#include "PlatformLayer/Graphics/Rhi.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "CoreLayer/Localization.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace rhi=se::rhi;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Ok(rhi::Result value,const char* message){if(!value)throw std::runtime_error(std::string(message)+": "+value.Key());}
struct Fixture
{
    WindowState window;
    std::shared_ptr<rhi::Device> device;
    ~Fixture(){if(device)device->Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
};
void Size(Fixture& f,UINT width,UINT height)
{
    RECT rect{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
    AdjustWindowRectEx(&rect,static_cast<DWORD>(GetWindowLongPtrW(f.window.hwnd,GWL_STYLE)),FALSE,0);
    Check(SetWindowPos(f.window.hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"set RHI window size");
    int code=0;Check(PumpWindowMessages(code)&&f.window.width==width&&f.window.height==height,"actual RHI surface dimensions");
}
struct Resources {rhi::Buffer vertices,indices;rhi::Texture texture;rhi::Pipeline pipeline;};
Resources Create(rhi::Device& device)
{
    const auto cube=se::MakeUnitCube();std::vector<rhi::Vertex> vertices;
    for(const auto& v:cube.vertices)vertices.push_back({v.position,v.normal,v.uv,v.tangent});
    Resources resources;
    Ok(device.CreateBuffer({rhi::BufferUsage::Vertex,sizeof(rhi::Vertex),"Cube.vertices"},std::as_bytes(std::span(vertices)),resources.vertices),"create vertex handle");
    Ok(device.CreateBuffer({rhi::BufferUsage::Index,sizeof(std::uint32_t),"Cube.indices"},std::as_bytes(std::span(cube.indices)),resources.indices),"create index handle");
    const std::array<std::uint8_t,4> white{255,255,255,255};
    Ok(device.CreateTexture({1,1},white,resources.texture),"create texture handle");
    Ok(device.CreatePipeline({rhi::PipelineKind::TexturedMesh,GetExecutableDirectory()/"Resources/Shaders/TexturedMesh.hlsl",1},resources.pipeline),"create pipeline handle");
    return resources;
}
rhi::DrawIndexed Draw(const Resources& r)
{
    return {*r.vertices,*r.indices,*r.texture,*r.pipeline,{},
        *se::PerspectiveRH(se::Pi/3,4.0f/3,0.1f,100)*se::Translation({0,0,-3}),{1,1,1,1},false,true};
}
void BeginOpaque(rhi::CommandContext& commands)
{
    Ok(commands.BeginPass({rhi::PassKind::Clear}),"begin clear");Ok(commands.EndPass(),"end clear");
    Ok(commands.BeginPass({rhi::PassKind::Opaque}),"begin opaque");
}
void End(rhi::CommandContext& commands){Ok(commands.EndPass(),"end opaque");Ok(commands.Close(),"close commands");}
void Pixel(const rhi::Image& image)
{
    Check(image.width==320&&image.height==240,"RHI capture dimensions");
    const size_t offset=(size_t(image.height/2)*image.width+image.width/2)*4;
    Check(image.rgba8[offset]==255&&image.rgba8[offset+1]==255&&image.rgba8[offset+2]==255,"synthetic cube center from no World data");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Fixture f;Check(CreateEngineWindow(f.window,false),"create hidden RHI surface");Size(f,320,240);
        Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"RHI strings");
        rhi::Result result;f.device=rhi::Device::CreateD3D12({f.window.hwnd,320,240},text,result);Ok(result,"create actual RHI device");
        auto& device=*f.device;auto resources=Create(device);auto draw=Draw(resources);
        {
            rhi::Texture output;const std::array<std::uint8_t,4> pixel{255,255,255,255};
            Check(device.CreateTexture({0,1},pixel,output).error==rhi::Error::InvalidArgument&&!output,"invalid dimensions are transactional");
            Check(device.CreateTexture({1,1,2},pixel,output).error==rhi::Error::Unsupported&&!output,"unsupported mip declaration rejected");
            rhi::Pipeline pipeline;
            Check(device.CreatePipeline({rhi::PipelineKind::TexturedMesh,GetExecutableDirectory()/"Resources/Shaders/TexturedMesh.hlsl",2},pipeline).error==rhi::Error::Unsupported&&!pipeline,"binding layout version checked");
        }
        auto commands=device.BeginCommands();Check(bool(commands),"create self-owned command packet");
        rhi::CompletionToken token;
        Check(device.Submit(*commands,token).error==rhi::Error::InvalidState&&!token.value,"open commands cannot submit");
        Check(commands->Draw(draw).error==rhi::Error::InvalidState,"draw requires a pass");
        Check(commands->BeginPass({rhi::PassKind::Opaque}).error==rhi::Error::InvalidState,"clear must precede opaque");
        Check(commands->BeginPass({rhi::PassKind::Clear,rhi::ResourceUse::TextureRead}).error==rhi::Error::Unsupported,"bad output declaration rejected");
        BeginOpaque(*commands);
        Check(commands->Close().error==rhi::Error::InvalidState,"cannot close an open pass");
        auto foreign=draw;++foreign.texture.device;
        Check(commands->Draw(foreign).error==rhi::Error::InvalidHandle,"foreign device texture rejected");
        auto wrong=draw;wrong.vertices=draw.indices;
        Check(commands->Draw(wrong).error==rhi::Error::InvalidArgument,"vertex/index usage is checked");
        Ok(commands->Draw(draw),"record synthetic world-free draw");
        const auto oldVertex=*resources.vertices;
        resources={};device.Collect();
        Check(device.Stats().buffers==0&&device.Stats().textures==0&&device.Stats().pipelines==0
            &&device.Stats().retiring>=4,"logical owners released while recorded commands retain resources");
        End(*commands);rhi::Image image;
        Ok(device.Submit(*commands,token,&image),"submit after original CPU owners release");Pixel(image);
        Check(device.IsComplete(token)&&!device.IsComplete({token.device+1,token.queue,token.value})&&!device.IsComplete({}),"queue/device completion identity");
        Check(device.Submit(*commands,token).error==rhi::Error::InvalidState,"commands submit exactly once");
        device.Collect();Check(device.Stats().retiring==0&&device.Stats().inFlightBatches==0,"completed resources actually reclaimed");
        if(argc>1)
        {
            const auto path=std::filesystem::path(argv[1]);std::filesystem::create_directories(path.parent_path());
            std::ofstream file(path,std::ios::binary);file<<"P6\n320 240\n255\n";
            for(size_t i=0;i<image.rgba8.size();i+=4)file.write(reinterpret_cast<const char*>(image.rgba8.data()+i),3);
            Check(bool(file),"save RHI screenshot");
        }
        resources=Create(device);draw=Draw(resources);
        Check(resources.vertices->index==oldVertex.index&&resources.vertices->generation!=oldVertex.generation,"slot reuse advances generation");
        commands=device.BeginCommands();BeginOpaque(*commands);
        auto stale=draw;stale.vertices=oldVertex;
        Check(commands->Draw(stale).error==rhi::Error::InvalidHandle,"old generation cannot resolve new resource");
        const auto before=device.Stats().submittedFrames;commands.reset();device.Collect();
        Check(device.Stats().submittedFrames==before,"cancelled packet submits no GPU work");
        const auto waits=device.Stats().globalWaits;
        for(int frame=0;frame<64;++frame)
        {
            commands=device.BeginCommands();BeginOpaque(*commands);draw.world=se::Translation({float(frame%3)*0.01f,0,0});
            Ok(commands->Draw(draw),"record ordinary RHI frame");End(*commands);Ok(device.Submit(*commands,token),"submit ordinary RHI frame");
        }
        Check(device.Stats().globalWaits==waits&&device.Stats().submittedFrames==before+64,"64 ordinary frames have no global flush");
        Size(f,512,256);Ok(device.Resize(512,256),"resize through RHI");
        Size(f,320,240);Ok(device.Resize(320,240),"restore RHI dimensions");
        resources={};Ok(device.WaitIdle(),"drain last resource usage");device.Collect();
        const auto stats=device.Stats();
        Check(stats.buffers==0&&stats.textures==0&&stats.pipelines==0&&stats.retiring==0,"no stale resource ownership");
        Check(stats.debugWarnings==0&&stats.debugErrors==0,"RHI GPU warning/error/corruption zero");
        auto cancelled=device.BeginCommands();Ok(cancelled->BeginPass({rhi::PassKind::Clear}),"partial command at shutdown");
        device.Shutdown();device.Shutdown();
        Check(cancelled->EndPass().error==rhi::Error::InvalidState&&!device.BeginCommands(),"shutdown invalidates operations and supports cancellation");
        std::cout<<"RHI: typed generations, pass contracts, recorded leases, GPU pixels, no-flush frames and shutdown passed; InfoQueue warning/error/corruption=0.\n";
        return 0;
    }
    catch(const std::exception& error){std::cerr<<"RhiTests: "<<error.what()<<'\n';return 1;}
}

#include "PlatformLayer/Graphics/D3D12/D3D12Diagnostics.h"
#include "PlatformLayer/Graphics/D3D12/D3D12PipelineCache.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Presentation.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "PlatformLayer/Windows/Window.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "CoreLayer/Hash.h"
#include "CoreLayer/Localization.h"
#include <atomic>
#include <algorithm>
#include <iterator>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace rhi=se::rhi;
namespace fs=std::filesystem;
using Microsoft::WRL::ComPtr;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Hr(HRESULT value,const char* message){Check(SUCCEEDED(value),message);}
void Ok(rhi::Result value,const char* message){if(!value)throw std::runtime_error(std::string(message)+": "+value.Key());}
std::string Read(const fs::path& path)
{std::ifstream input(path,std::ios::binary);Check(bool(input),"read shader fixture");return {std::istreambuf_iterator<char>(input),{}};}
void Write(const fs::path& path,const std::string& text)
{std::ofstream output(path,std::ios::binary);output.write(text.data(),static_cast<std::streamsize>(text.size()));Check(bool(output),"write shader fixture");}
void Replace(std::string& text,const std::string& before,const std::string& after)
{const auto position=text.find(before);Check(position!=std::string::npos,"known shader fixture fragment");text.replace(position,before.size(),after);}
DWORD Build(const fs::path& shader,const fs::path& artifact)
{
    const auto executable=GetExecutableDirectory()/"ShaderBuilder.exe";
    auto command=L"\""+executable.wstring()+L"\" mesh \""+shader.wstring()+L"\" \""+artifact.wstring()+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    Check(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,GetExecutableDirectory().c_str(),&startup,&process)!=FALSE,"run actual ShaderBuilder tool");
    struct Guard{PROCESS_INFORMATION& p;~Guard(){CloseHandle(p.hThread);CloseHandle(p.hProcess);}} guard{process};
    const auto wait=WaitForSingleObject(process.hProcess,30000);
    if(wait!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,1);WaitForSingleObject(process.hProcess,30000);Check(false,"ShaderBuilder own child timeout");}
    DWORD code=0;Check(GetExitCodeProcess(process.hProcess,&code)!=FALSE,"ShaderBuilder exit code");return code;
}
struct Fixture
{
    WindowState window;std::shared_ptr<rhi::Device> device;
    explicit Fixture(const Localization& text)
    {
        Check(CreateEngineWindow(window,false),"shader hidden window");RECT rect{0,0,320,240};
        AdjustWindowRectEx(&rect,static_cast<DWORD>(GetWindowLongPtrW(window.hwnd,GWL_STYLE)),FALSE,0);
        Check(SetWindowPos(window.hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"shader window size");
        int code=0;Check(PumpWindowMessages(code),"shader window messages");rhi::Result result;
        device=rhi::Device::CreateD3D12({window.hwnd,320,240},text,result);Ok(result,"shader GPU device");
    }
    ~Fixture(){if(device)device->Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
};
void NoProblems(rhi::Device& device)
{const auto stats=device.Stats();Check(stats.debugWarnings==0&&stats.debugErrors==0,"shader GPU warning/error/corruption zero");}
struct Geometry{rhi::Buffer vertices,indices;rhi::Texture texture;};
Geometry Create(rhi::Device& device)
{
    Geometry result;const auto cube=se::MakeUnitCube();std::vector<rhi::Vertex> vertices;
    for(const auto& vertex:cube.vertices)vertices.push_back({vertex.position,vertex.normal,vertex.uv,vertex.tangent});
    Ok(device.BeginUploadBatch(),"shader geometry upload begin");
    Ok(device.CreateBuffer({rhi::BufferUsage::Vertex,sizeof(rhi::Vertex),"Shader.vertices"},std::as_bytes(std::span(vertices)),result.vertices),"shader vertex buffer");
    Ok(device.CreateBuffer({rhi::BufferUsage::Index,4,"Shader.indices"},std::as_bytes(std::span(cube.indices)),result.indices),"shader index buffer");
    const std::array<std::uint8_t,4> gray{128,128,128,255};Ok(device.CreateTexture({1,1},gray,result.texture),"shader gray texture");
    rhi::CompletionToken token;Ok(device.EndUploadBatch(token),"shader geometry upload end");return result;
}
std::unique_ptr<rhi::CommandContext> Record(rhi::Device& device,const Geometry& geometry,rhi::PipelineHandle pipeline)
{
    auto commands=device.BeginCommands();Check(bool(commands),"shader command packet");
    Ok(commands->BeginPass({rhi::PassKind::Clear}),"shader clear");Ok(commands->EndPass(),"shader clear end");Ok(commands->BeginPass({rhi::PassKind::Opaque}),"shader opaque");
    const auto camera=*se::PerspectiveRH(se::Pi/3,4.0f/3,0.1f,100)*se::Translation({0,0,-3});
    Ok(commands->Draw({*geometry.vertices,*geometry.indices,*geometry.texture,pipeline,{},camera,{1,1,1,1},false,true}),"prepared shader draw");
    Ok(commands->EndPass(),"shader pass end");Ok(commands->Close(),"shader packet close");return commands;
}
rhi::Image Capture(rhi::Device& device,const Geometry& geometry,rhi::PipelineHandle pipeline,int expected)
{
    auto commands=Record(device,geometry,pipeline);rhi::CompletionToken token;rhi::Image image;
    Ok(device.Submit(*commands,token,&image),"shader capture");const auto offset=(size_t(120)*320+160)*4;
    Check(image.width==320&&image.height==240,"shader image dimensions");
    for(UINT channel=0;channel<3;++channel)Check(std::abs(int(image.rgba8[offset+channel])-expected)<=1,"shader dependency changes visible pixels");return image;
}
void Cpu(const fs::path& shader,const Localization& text)
{
    rhi::PipelineDescription description;description.shader=shader;D3D12ShaderSource source;
    Check(ReadD3D12ShaderSource(description,source,text)&&source.key.size()==64&&source.dependencies.size()==2,"actual root and include dependency hashes");
    auto first=description;first.macros={{"B","2"},{"A","1"}};D3D12ShaderSource ab,ba;
    Check(ReadD3D12ShaderSource(first,ab,text),"macro source");std::reverse(first.macros.begin(),first.macros.end());
    Check(ReadD3D12ShaderSource(first,ba,text)&&ab.key==ba.key&&ab.macros[0].name=="A","macro order normalized");
    first.macros.front().value="3";D3D12ShaderSource changed;
    Check(ReadD3D12ShaderSource(first,changed,text)&&changed.key!=ba.key,"macro value changes source identity");
    auto invalid=description;invalid.macros={{"A","1"},{"A","2"}};const auto previous=source.key;
    Check(!ReadD3D12ShaderSource(invalid,source,text)&&source.key==previous,"duplicate macro rejected without replacing source");
    const auto escape=shader.parent_path()/"Escape.hlsl";Write(shader.parent_path().parent_path()/"outside.hlsli","float x=1;\n");Write(escape,"#include \"../outside.hlsli\"\n");
    invalid=description;invalid.shader=escape;Check(!ReadD3D12ShaderSource(invalid,source,text),"include cannot escape declared shader root");
    const auto warning=shader.parent_path()/"Warning.hlsl";Write(warning,"#define DUPLICATE 1\n#define DUPLICATE 2\n"+Read(shader));
    invalid=description;invalid.shader=warning;Check(!ReadD3D12ShaderSource(invalid,source,text),"preprocessor warnings fail strict builds too");
}
void CacheAndLifetime(const fs::path& shader,const Localization& text,const fs::path& output)
{
    Fixture f(text);auto& device=*f.device;rhi::PipelineDescription description;description.shader=shader;
    rhi::Pipeline first,duplicate;Ok(device.CreatePipeline(description,first),"load built shader artifact");
    const auto initial=device.Stats();
    if(!(initial.shaderArtifactLoads==1&&initial.shaderProgramCompilations==0
        &&initial.pipelineCreations+initial.pipelineLibraryHits==1))
        std::cerr<<"shader artifact stats: loads="<<initial.shaderArtifactLoads<<" compilations="
            <<initial.shaderProgramCompilations<<" pso_creations="<<initial.pipelineCreations
            <<" library_hits="<<initial.pipelineLibraryHits<<'\n';
    // Release 的持久 PSO 库可能已经存有同一键；此时是库命中而不是新建，二者都只发生一次。
    Check(initial.shaderArtifactLoads==1&&initial.shaderProgramCompilations==0
        &&initial.pipelineCreations+initial.pipelineLibraryHits==1,"build artifact actually used at runtime");
    Ok(device.CreatePipeline(description,duplicate),"identical pipeline");
    const auto original=rhi::D3D12DiagnosticsAccess::PipelineState(device,*first);
    Check(original==rhi::D3D12DiagnosticsAccess::PipelineState(device,*duplicate)&&device.Stats().pipelineCacheHits==1,"identical configuration shares actual PSO");
    std::vector<rhi::Pipeline> variants;
    for(UINT variant=0;variant<4;++variant)
    {
        auto changed=description;if(variant==0)changed.depth=rhi::DepthMode::Disabled;
        if(variant==1)changed.depth=rhi::DepthMode::ReadOnlyLessEqual;if(variant==2)changed.blend=rhi::BlendMode::Alpha;
        if(variant==3)changed.colorFormat=rhi::ColorFormat::Rgba16Float;
        rhi::Pipeline pipeline;Ok(device.CreatePipeline(changed,pipeline),"prepare distinct fixed state");
        Check(rhi::D3D12DiagnosticsAccess::PipelineState(device,*pipeline)!=original,"depth/blend/target never incorrectly hit original PSO");variants.push_back(std::move(pipeline));
    }
    auto explicitDefault=description;explicitDefault.depth=rhi::DepthMode::ReadWriteLess;rhi::Pipeline normalized;
    Ok(device.CreatePipeline(explicitDefault,normalized),"normalized default depth");Check(rhi::D3D12DiagnosticsAccess::PipelineState(device,*normalized)==original,"default and explicit effective states share PSO");normalized.reset();
    auto geometry=Create(device);const auto reference=Capture(device,geometry,*first,128);
    {
        auto commands=device.BeginCommands();Ok(commands->BeginPass({rhi::PassKind::Clear}),"HDR rejection clear");Ok(commands->EndPass(),"HDR rejection clear end");Ok(commands->BeginPass({rhi::PassKind::Opaque}),"HDR rejection opaque");
        const auto camera=*se::PerspectiveRH(se::Pi/3,4.0f/3,0.1f,100)*se::Translation({0,0,-3});
        Check(commands->Draw({*geometry.vertices,*geometry.indices,*geometry.texture,*variants.back(),{},camera}).error==rhi::Error::Unsupported,"HDR PSO cannot bind to current RGBA8 pass");
    }
    const auto prepared=rhi::D3D12DiagnosticsAccess::Prepared(device,*first);auto commands=Record(device,geometry,*first);
    ComPtr<ID3D12Fence> gate;Hr(rhi::D3D12DiagnosticsAccess::NativeDevice(device)->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.GetAddressOf())),"PSO gate");
    struct Gate
    {
        ComPtr<ID3D12Fence> fence;HANDLE event;std::atomic<bool> timedOut{false};std::thread worker;
        explicit Gate(ComPtr<ID3D12Fence> value):fence(std::move(value)),event(CreateEventW(nullptr,TRUE,FALSE,nullptr))
        {Check(event!=nullptr,"PSO gate event");worker=std::thread([this]{if(WaitForSingleObject(event,3000)==WAIT_TIMEOUT){timedOut=true;fence->Signal(1);}});}
        void Release(){Hr(fence->Signal(1),"release PSO gate");SetEvent(event);}
        ~Gate(){fence->Signal(1);SetEvent(event);if(worker.joinable())worker.join();CloseHandle(event);}
    } release{gate};
    Hr(rhi::D3D12DiagnosticsAccess::Queue(device)->Wait(gate.Get(),1),"hold shared PSO in flight");rhi::CompletionToken token;
    Ok(device.Submit(*commands,token),"submit cached PSO behind gate");Check(!device.IsComplete(token)&&!release.timedOut,"shared PSO GPU work incomplete");
    first.reset();duplicate.reset();device.Collect();Check(!prepared.expired(),"cache and logical owners do not free GPU PSO early");
    rhi::Pipeline inFlightHit;Ok(device.CreatePipeline(description,inFlightHit),"cache hit retained by GPU work");
    Check(rhi::D3D12DiagnosticsAccess::PipelineState(device,*inFlightHit)==original&&!release.timedOut,"in-flight prepared object remains reusable");inFlightHit.reset();
    release.Release();Ok(device.WaitIdle(),"PSO retirement drain");device.Collect();Check(prepared.expired(),"weak cache does not permanently retain prepared PSO/root");
    const auto creations=device.Stats();Ok(device.CreatePipeline(description,first),"recreate retired prepared PSO");
    const auto recreated=device.Stats();
    // 退役后重建必须得到新的 prepared 对象；Release 的持久库可以直接恢复 PSO，因此“新建或库命中”二选一。
    Check(recreated.pipelineCreations+recreated.pipelineLibraryHits
            ==creations.pipelineCreations+creations.pipelineLibraryHits+1
        &&recreated.pipelineCacheMisses==creations.pipelineCacheMisses+1,"retired key produces a new prepared object");
    const auto before=device.Stats();for(UINT frame=0;frame<32;++frame){commands=Record(device,geometry,*first);Ok(device.Submit(*commands,token),"ordinary prepared pipeline frame");}
    const auto after=device.Stats();Check(after.pipelineCreations==before.pipelineCreations&&after.shaderProgramCompilations==before.shaderProgramCompilations,"draw/submit never compile Shader or create PSO");
    Check(Capture(device,geometry,*first,128).rgba8==reference.rgba8,"cache and state refactor preserves complete image");
    std::ofstream csv(output/"pipeline-cache.csv");const auto stats=device.Stats();
    csv<<"artifact_loads,program_compile_attempts,pso_hits,pso_misses,pso_creations,live_prepared\n"<<stats.shaderArtifactLoads<<','<<stats.shaderProgramCompilations<<','<<stats.pipelineCacheHits<<','<<stats.pipelineCacheMisses<<','<<stats.pipelineCreations<<','<<stats.livePreparedPipelines<<'\n';
    Check(bool(csv),"pipeline statistics");NoProblems(device);
}
void FailAndRepair(const fs::path& shader,const Localization& text)
{
    const auto include=shader.parent_path()/"MeshBindings.hlsli";const auto originalShader=Read(shader),originalInclude=Read(include);
    const auto artifact=fs::path(shader.wstring()+L".sebin");Check(Build(shader,artifact)==0,"standalone ShaderBuilder writes valid artifact");const auto originalArtifact=Read(artifact);
    Fixture f(text);auto& device=*f.device;auto geometry=Create(device);rhi::PipelineDescription description;description.shader=shader;rhi::Pipeline old;
    Ok(device.CreatePipeline(description,old),"repair baseline pipeline");const auto reference=Capture(device,geometry,*old,128);
    Write(shader,"float4 VSMain( : SV_Position { invalid }\n");rhi::Pipeline invalid;
    Check(!device.CreatePipeline(description,invalid)&&!invalid,"syntax error rejected");Check(Build(shader,artifact)!=0&&Read(artifact)==originalArtifact,"failed tool build preserves prior artifact bytes");
    Check(Capture(device,geometry,*old,128).rgba8==reference.rgba8,"old pipeline survives broken source");Write(shader,originalShader);
    auto half=originalInclude;Replace(half,"return lerp(","return 0.5*lerp(");const auto oldTime=fs::last_write_time(include);Write(include,half);fs::last_write_time(include,oldTime);
    rhi::Pipeline changed;Ok(device.CreatePipeline(description,changed),"include content invalidates old program even with same timestamp");Capture(device,geometry,*changed,64);
    const auto goodCreations=device.Stats().pipelineCreations;
    auto bad=half;Replace(bad,"column_major float4x4 clipFromLocal","row_major float4x4 clipFromLocal");Write(include,bad);
    Check(!device.CreatePipeline(description,invalid)&&!invalid&&device.Stats().pipelineCreations==goodCreations,"row-major mismatch rejected before PSO creation");
    bad=half;Replace(bad,"register(t0)","register(t1)");Write(include,bad);
    Check(!device.CreatePipeline(description,invalid)&&device.Stats().pipelineCreations==goodCreations,"wrong register rejected before GPU");
    Write(include,half);rhi::Pipeline repaired;Ok(device.CreatePipeline(description,repaired),"fixing source restores valid cached program");Capture(device,geometry,*repaired,64);
    auto macros=description;macros.macros={{"B","2"},{"A","1"}};rhi::Pipeline macroA,macroB;
    Ok(device.CreatePipeline(macros,macroA),"prepare macro variant");std::reverse(macros.macros.begin(),macros.macros.end());Ok(device.CreatePipeline(macros,macroB),"reordered macro variant");
    Check(rhi::D3D12DiagnosticsAccess::PipelineState(device,*macroA)==rhi::D3D12DiagnosticsAccess::PipelineState(device,*macroB),"macro ordering does not duplicate PSO");
    Write(include,originalInclude);Write(artifact,"damaged artifact");
    // D3D12 同一适配器在进程内共享 Device；先结束原来唯一活动队列，再验证新 cache。
    NoProblems(device);device.Shutdown();
    {Fixture fresh(text);rhi::Pipeline fallback;Ok(fresh.device->CreatePipeline(description,fallback),"fresh cache recovers damaged artifact from source");
        Check(fresh.device->Stats().shaderArtifactRejections==1&&fresh.device->Stats().shaderProgramCompilations==1,"artifact corruption recovery actually compiled");NoProblems(*fresh.device);}
    Check(Build(shader,artifact)==0,"repair valid artifact after corruption");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply shader test output directory");const auto output=fs::absolute(argv[1]);fs::create_directories(output);
        const auto source=output/(L"着色器-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));fs::create_directories(source);
        const auto resources=GetExecutableDirectory()/"Resources/Shaders";
        for(const auto* name:{L"TexturedMesh.hlsl",L"MeshBindings.hlsli",L"TexturedMesh.hlsl.sebin",L"TexturedMesh.hlsl.sebin.json"})fs::copy_file(resources/name,source/name);
        const auto shader=source/"TexturedMesh.hlsl";Localization text;std::string error;
        Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"shader strings");
        Cpu(shader,text);CacheAndLifetime(shader,text,output);FailAndRepair(shader,text);
        std::cout<<"ShaderTests passed (built artifacts, exact layout, effective PSO keys, Fence retirement, dependencies, diagnostics and repair); GPU warnings/errors=0.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"ShaderTests: "<<error.what()<<'\n';return 1;}
}

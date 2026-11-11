#include "PlatformLayer/Graphics/Rhi.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/ImageWriter.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Profiler.h"
#include "CoreLayer/Hash.h"
#include "CoreLayer/Log.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace
{
using namespace se;namespace fs=std::filesystem;using Json=nlohmann::json;using Clock=std::chrono::steady_clock;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Ok(rhi::Result value,const char* message){if(!value)throw std::runtime_error(std::string(message)+": "+value.Key());}
double Ms(Clock::time_point begin){return std::chrono::duration<double,std::milli>(Clock::now()-begin).count();}
struct Config
{
    bool committed=false,batch=true,psoCache=true,light=false,pix=false;
    std::uint32_t cycles=24,warmup=4;fs::path output,capture;std::string source;
};
struct CpuWorkload
{
    UINT width=257,height=193;
    std::vector<std::vector<rhi::Vertex>> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<std::vector<std::uint8_t>> images;
    std::uint32_t pipelineRequests=16;
    std::string digest;
    explicit CpuWorkload(bool light)
    {
        const UINT meshCount=light?2:6,textureCount=light?4:24;if(light){width=31;height=17;pipelineRequests=4;}
        const auto cube=MakeUnitCube();indices=cube.indices;Json hashes=Json::array();
        for(UINT mesh=0;mesh<meshCount;++mesh)
        {
            std::vector<rhi::Vertex> data;for(const auto& v:cube.vertices){rhi::Vertex vertex{v.position,v.normal,v.uv,v.tangent};vertex.position[1]*=0.75f+float(mesh)*0.1f;data.push_back(vertex);}
            const auto bytes=std::as_bytes(std::span(data));hashes.push_back(Sha256({reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()}));vertices.push_back(std::move(data));
        }
        for(UINT texture=0;texture<textureCount;++texture)
        {
            std::vector<std::uint8_t> data(size_t(width)*height*4);
            for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const size_t at=(size_t(y)*width+x)*4;const UINT checker=((x/7+y/5+texture)&1)?190:65;
                data[at]=static_cast<std::uint8_t>((checker+texture*7)%256);data[at+1]=static_cast<std::uint8_t>((checker+texture*19)%256);data[at+2]=static_cast<std::uint8_t>((checker+texture*31)%256);data[at+3]=255;}
            hashes.push_back(Sha256(data));images.push_back(std::move(data));
        }
        const auto indexBytes=std::as_bytes(std::span(indices));hashes.push_back(Sha256({reinterpret_cast<const std::uint8_t*>(indexBytes.data()),indexBytes.size()}));
        digest=Sha256(Json{{"algorithm","SelfEngine.ResourceWorkload.1"},{"size",{width,height}},{"meshes",meshCount},{"textures",textureCount},{"pipeline_requests",pipelineRequests},{"hashes",hashes}}.dump());
        Check(!digest.empty(),"workload identity");
    }
};
struct GpuResources
{
    std::vector<rhi::Buffer> vertices,indices;std::vector<rhi::Texture> textures;std::vector<rhi::Pipeline> pipelines;
};
void Snapshot(Json& row,const char* prefix,const rhi::Statistics& s)
{
    const std::string p=prefix;
    row[p+"buffers"]=s.buffers;row[p+"textures"]=s.textures;row[p+"pipelines"]=s.pipelines;row[p+"retiring"]=s.retiring;row[p+"in_flight"]=s.inFlightBatches;
    row[p+"memory_resources"]=s.memoryResources;row[p+"memory_used"]=s.memoryUsedBytes;row[p+"memory_reserved"]=s.memoryReservedBytes;
    row[p+"placed_heaps"]=s.memoryHeaps;row[p+"committed_resources"]=s.memoryCommittedResources;row[p+"srv_used"]=s.persistentDescriptors;row[p+"srv_capacity"]=s.descriptorCapacity;
    row[p+"frame_descriptors"]=s.frameDescriptors;row[p+"frame_capacity"]=s.frameDescriptorCapacity;row[p+"upload_reserved"]=s.uploadReservedBytes;row[p+"upload_pages"]=s.uploadPagesCreated;
    row[p+"tracked_resources"]=s.trackedResources;row[p+"live_prepared"]=s.livePreparedPipelines;row[p+"live_programs"]=s.liveShaderPrograms;
}
bool SaveJson(const fs::path& path,const Json& data)
{const auto text=data.dump(2)+"\n";return asset_io::WriteAtomic(path,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});}
void SaveCycles(const fs::path& path,const Json& rows)
{
    Check(!rows.empty(),"cycle rows");std::ofstream stream(path,std::ios::binary);stream.imbue(std::locale::classic());stream<<std::setprecision(17);bool first=true;
    for(const auto& item:rows[0].items()){if(!first)stream<<',';stream<<item.key();first=false;}stream<<'\n';
    for(const auto& row:rows){first=true;for(const auto& item:rows[0].items()){if(!first)stream<<',';const auto& value=row.at(item.key());if(value.is_string())stream<<value.get<std::string>();else stream<<value.dump();first=false;}stream<<'\n';}
    Check(bool(stream),"cycle CSV write");
}
int Run(const Config& config,const Localization& text)
{
    Check(!fs::exists(config.output),"output.exists");fs::create_directories(config.output);CpuWorkload workload(config.light);
    WindowState window;std::shared_ptr<rhi::Device> device;
    struct Cleanup{WindowState& window;std::shared_ptr<rhi::Device>& device;~Cleanup(){if(device)device->Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}} cleanup{window,device};
    Check(CreateEngineWindow(window,false)&&SetEngineWindowClientSize(window,320,240),"resource window");int exit=0;Check(PumpWindowMessages(exit),"resource messages");rhi::Result result;
    rhi::MemorySettings memory;memory.mode=config.committed?rhi::MemoryMode::CommittedBaseline:rhi::MemoryMode::Placed;
    device=rhi::Device::CreateD3D12({window.hwnd,320,240},text,result,memory);Ok(result,"resource device");Ok(device->EnableGpuTiming(true),"resource queries");
    Profiler profiler;profiler.SetEnabled(true);device->SetCpuProfiler(&profiler);
    struct UnbindProfiler{rhi::Device& device;~UnbindProfiler(){device.SetCpuProfiler(nullptr);}} unbind{*device};
    const auto baseline=device->Stats();const auto adapter=device->Adapter();Json rows=Json::array();
    const auto projection=*OrthographicRH(7,5.25f,0.1f,100)*Translation({0,0,-6});const UINT total=config.capture.empty()?config.warmup+config.cycles:1;
    for(UINT cycle=0;cycle<total;++cycle)
    {
        Check(PumpWindowMessages(exit),"resource cycle messages");Check(profiler.BeginFrame(cycle+1),"resource CPU frame");const auto before=device->Stats();Json row{{"cycle",cycle+1},{"phase",cycle<config.warmup&&config.capture.empty()?"warmup":"sample"}};
        GpuResources resources;const auto loadBegin=Clock::now();
        {
            CpuScope scope(profiler,"CPU.ResourceLoad");rhi::PipelineDescription description{rhi::PipelineKind::TexturedMesh,GetExecutableDirectory()/"Resources/Shaders/TexturedMesh.hlsl",1};description.cachePrepared=config.psoCache;
            for(UINT index=0;index<workload.pipelineRequests;++index){rhi::Pipeline pipeline;Ok(device->CreatePipeline(description,pipeline),"resource prepared request");resources.pipelines.push_back(std::move(pipeline));}
            if(config.batch)Ok(device->BeginUploadBatch(),"resource batch begin");
            for(size_t mesh=0;mesh<workload.vertices.size();++mesh)
            {
                rhi::Buffer vertices,indices;Ok(device->CreateBuffer({rhi::BufferUsage::Vertex,sizeof(rhi::Vertex),"Lifecycle.VB"},std::as_bytes(std::span(workload.vertices[mesh])),vertices),"resource VB");
                Ok(device->CreateBuffer({rhi::BufferUsage::Index,sizeof(std::uint32_t),"Lifecycle.IB"},std::as_bytes(std::span(workload.indices)),indices),"resource IB");resources.vertices.push_back(std::move(vertices));resources.indices.push_back(std::move(indices));
            }
            for(size_t texture=0;texture<workload.images.size();++texture){rhi::Texture value;Ok(device->CreateTexture({workload.width,workload.height,1,1,rhi::TextureFormat::Rgba8,"Lifecycle.Texture"},workload.images[texture],value),"resource texture");resources.textures.push_back(std::move(value));}
            if(config.batch){rhi::CompletionToken token;Ok(device->EndUploadBatch(token),"resource batch submit");}
            row["load_calls_ms"]=Ms(loadBegin);Ok(device->WaitIdle(),"resource ready fence");row["load_ready_ms"]=Ms(loadBegin);
        }
        const auto loaded=device->Stats();
        Check(loaded.pipelineCreations-before.pipelineCreations==(config.psoCache?1u:workload.pipelineRequests)&&loaded.livePreparedPipelines==(config.psoCache?1u:workload.pipelineRequests),"actual cache policy/native prepared count");
        Snapshot(row,"loaded_",loaded);row["upload_submissions"]=loaded.uploadSubmissions-before.uploadSubmissions;row["load_global_waits"]=loaded.globalWaits-before.globalWaits;
        row["pso_creations"]=loaded.pipelineCreations-before.pipelineCreations;row["pso_hits"]=loaded.pipelineCacheHits-before.pipelineCacheHits;row["heap_creations"]=loaded.memoryHeapCreations-before.memoryHeapCreations;
        row["committed_creations"]=loaded.memoryCommittedCreations-before.memoryCommittedCreations;row["shader_compilations"]=loaded.shaderProgramCompilations-before.shaderProgramCompilations;
        const auto recordBegin=Clock::now();rhi::Image image;
        {
            CpuScope scope(profiler,"CPU.ResourceDraw");auto commands=device->BeginCommands(cycle+1);Check(bool(commands),"resource commands");Ok(commands->BeginPass({rhi::PassKind::Clear}),"resource clear");Ok(commands->EndPass(),"resource clear end");Ok(commands->BeginPass({rhi::PassKind::Opaque}),"resource opaque");
            for(size_t index=0;index<resources.textures.size();++index)
            {
                const size_t mesh=index%resources.vertices.size();const auto world=Translation({-2.8f+float(index%6)*1.1f,-1.6f+float(index/6)*1.05f,0})*Rotation(AxisAngle({0,1,0},float(index)*0.17f))*Scaling({0.6f,0.6f,0.6f});
                rhi::DrawIndexed draw{*resources.vertices[mesh],*resources.indices[mesh],*resources.textures[index],*resources.pipelines[0],world,projection,{1,1,1,1},false,true};Ok(commands->Draw(draw),"resource actual draw");
            }
            Ok(commands->EndPass(),"resource opaque end");Ok(commands->Close(),"resource close");rhi::CompletionToken token;Ok(device->Submit(*commands,token,config.capture.empty()?nullptr:&image),"resource submit");
        }
        row["draw_submit_ms"]=Ms(recordBegin);const auto retireBegin=Clock::now();
        {
            CpuScope scope(profiler,"CPU.ResourceRetire");resources={};Snapshot(row,"drop_",device->Stats());Ok(device->WaitIdle(),"resource retirement fence");device->Collect();
        }
        row["retire_ms"]=Ms(retireBegin);const auto drained=device->Stats();Snapshot(row,"drained_",drained);row["debug_warnings"]=drained.debugWarnings;row["debug_errors"]=drained.debugErrors;
        Check(drained.buffers==baseline.buffers&&drained.textures==baseline.textures&&drained.pipelines==baseline.pipelines&&!drained.retiring&&!drained.inFlightBatches,"resource logical/Fence retirement");
        Check(drained.memoryUsedBytes==baseline.memoryUsedBytes&&drained.memoryResources==baseline.memoryResources&&drained.persistentDescriptors==baseline.persistentDescriptors&&!drained.livePreparedPipelines&&!drained.liveShaderPrograms,"resource physical/cache retirement");
        Check(!drained.debugWarnings&&!drained.debugErrors,"resource GPU diagnostics");Check(profiler.EndFrame(),"end resource CPU frame");rows.push_back(std::move(row));
        if(!config.capture.empty()){std::vector<AssetDiagnostic> diagnostics;Check(WritePpmImage(config.capture,image.width,image.height,image.rgba8,diagnostics),"resource capture write");}
        if(config.pix)Sleep(16);
    }
    device->SetCpuProfiler(nullptr);Ok(device->WaitIdle(),"final resource query drain");const auto final=device->Stats();Ok(device->TrimMemory(),"resource empty heap Trim");const auto trimmed=device->Stats();
    Check(trimmed.memoryUsedBytes==baseline.memoryUsedBytes&&trimmed.memoryReservedBytes==baseline.memoryReservedBytes,"Trim releases empty resource heaps to window baseline");
    SaveCycles(config.output/"cycles.csv",rows);Check(SaveJson(config.output/"cycles.json",rows),"resource cycle JSON");std::string error;
    Check(profiler.ExportFramesCsv(config.output/"cpu.csv",error)&&profiler.ExportSummaryCsv(config.output/"cpu-summary.csv",error),"resource CPU export");Ok(device->ExportGpuTimingsCsv(config.output/"gpu.csv"),"resource GPU export");
    Json report{{"schema",1},{"success",true},{"mode",config.pix?"pix":config.capture.empty()?"measurement":"capture"},{"measurement_complete",config.capture.empty()&&!config.pix},
        {"source_sha256",config.source},{"workload_sha256",workload.digest},{"workload_algorithm","SelfEngine.ResourceWorkload.1"},{"light",config.light},{"memory",config.committed?"committed":"placed"},{"upload",config.batch?"batch":"individual"},{"pso_cache",config.psoCache},
        {"cycles",config.cycles},{"warmup_cycles",config.warmup},{"total_cycles",total},{"texture_count",workload.images.size()},{"mesh_count",workload.vertices.size()},{"pipeline_requests",workload.pipelineRequests},{"texture_size",{workload.width,workload.height}},
        {"adapter",{{"name",adapter.name},{"vendor_id",adapter.vendorId},{"device_id",adapter.deviceId},{"software",adapter.software}}},{"present_sync_interval",1},{"debug_warnings",final.debugWarnings},{"debug_errors",final.debugErrors}};
    Json snapshots;Snapshot(snapshots,"baseline_",baseline);Snapshot(snapshots,"final_",final);Snapshot(snapshots,"trimmed_",trimmed);report["snapshots"]=snapshots;
    std::wstring executable(32768,L'\0');const DWORD executableLength=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));Check(executableLength&&executableLength<executable.size(),"resource executable path");executable.resize(executableLength);
    std::vector<std::uint8_t> program;Check(asset_io::Read(fs::path(executable),program),"resource program identity");report["executable_sha256"]=Sha256(program);
#ifdef NDEBUG
    report["configuration"]="Release";
#else
    report["configuration"]="Debug";
#endif
    Check(SaveJson(config.output/"run.json",report),"resource report");Log(LogLevel::Info,text.GetText("ResourceBenchmark.Complete"));return 0;
}
}
int wmain(int argc,wchar_t** argv)
{
    Localization text;std::string error;if(!text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error)){std::cerr<<"ResourceLifecycleBenchmark localization unavailable\n";return 1;}
    try
    {
        Config config;
#if defined(SE_RESOURCE_CAPTURE)
        config.pix=true;config.batch=SE_RESOURCE_CAPTURE==1;config.cycles=360;config.warmup=0;
        config.output=GetExecutableDirectory()/"Cache/ResourcePix"/(std::string(config.batch?"batch-":"individual-")+std::to_string(GetTickCount64()));
#endif
        for(int index=1;index<argc;++index)
        {
            const std::wstring argument=argv[index];if(argument==L"--light")config.light=true;
            else if(argument==L"--output"&&index+1<argc)config.output=fs::absolute(argv[++index]);
            else if(argument==L"--capture"&&index+1<argc)config.capture=fs::absolute(argv[++index]);
            else if(argument==L"--source-id"&&index+1<argc){const std::wstring value=argv[++index];Check(value.size()==64&&value.find_first_not_of(L"0123456789abcdefABCDEF")==std::wstring::npos,"source-id");for(wchar_t c:value)config.source+=static_cast<char>(c);}
            else if((argument==L"--cycles"||argument==L"--warmup-cycles")&&index+1<argc){const std::wstring value=argv[++index];size_t used=0;const auto count=std::stoul(value,&used);Check(used==value.size()&&count<=1000,"cycles");if(argument==L"--cycles")config.cycles=count;else config.warmup=count;}
            else if((argument==L"--memory"||argument==L"--upload"||argument==L"--pso-cache")&&index+1<argc)
            {const std::wstring value=argv[++index];if(argument==L"--memory"){Check(value==L"placed"||value==L"committed","memory");config.committed=value==L"committed";}
                else if(argument==L"--upload"){Check(value==L"batch"||value==L"individual","upload");config.batch=value==L"batch";}else{Check(value==L"on"||value==L"off","cache");config.psoCache=value==L"on";}}
            else throw std::invalid_argument("arguments");
        }
        Check(!config.output.empty()&&config.cycles&&config.warmup<=64,"configuration");return Run(config,text);
    }
    catch(const std::exception& exception){Log(LogLevel::Error,text.GetText("ResourceBenchmark.Failed")+": "+exception.what());return 2;}
}

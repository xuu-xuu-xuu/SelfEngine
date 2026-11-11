#include "Benchmark/BenchmarkScene.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include "CoreLayer/Profiler.h"
#include <nlohmann/json.hpp>
#include <intrin.h>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <cstring>

namespace
{
namespace fs=std::filesystem;
using namespace se;
std::string CpuBrand()
{
    int data[4]{};__cpuid(data,static_cast<int>(0x80000000u));char brand[49]{};
    if(static_cast<unsigned>(data[0])<0x80000004u)return "unavailable";
    for(unsigned index=0;index<3;++index){__cpuid(data,static_cast<int>(0x80000002u+index));std::memcpy(brand+index*16,data,16);}return brand;
}
std::string UserObjectName(HANDLE object)
{
    wchar_t name[512]{};DWORD needed=0;
    if(!GetUserObjectInformationW(object,UOI_NAME,name,sizeof(name),&needed))return "unavailable";
    return asset_io::Utf8(std::filesystem::path(name));
}
std::string Quote(const std::string& value)
{std::string result="\"";for(char c:value){if(c=='\"')result+='\"';result+=c;}return result+'\"';}
class MeasurementRenderer final:public Renderer
{
public:
    MeasurementRenderer(BenchmarkSession& session,Profiler& profiler,const fs::path& output):session_(session),profiler_(profiler),delegate_(SandboxScene::Benchmark)
    {
        cpu_.open(output/"cpu.csv",std::ios::binary);gpu_.open(output/"gpu.csv",std::ios::binary);frames_.open(output/"frames.csv",std::ios::binary);
        for(auto* stream:{&cpu_,&gpu_,&frames_}){stream->imbue(std::locale::classic());*stream<<std::setprecision(17);}
        cpu_<<"frame,scope,calls,cpu_ms\n";gpu_<<"source_frame,submission,frequency_hz,frame_begin,geometry_begin,geometry_end,frame_end,draws,gpu_frame_ms,gpu_geometry_ms\n";
        frames_<<"frame,sample_time_s,phase,raw_delta_ms,occluded,camera_x,camera_y,camera_z,camera_yaw,camera_pitch,candidates,visible,draws,culled,bounds_updates,global_waits,memory_resources,memory_reserved_bytes,memory_used_bytes,memory_peak_used_bytes,persistent_descriptors,frame_descriptors,upload_pages,upload_bytes,pipeline_creations,shader_compilations,debug_warnings,debug_errors,opaque_pipeline_binds,opaque_root_binds,opaque_table_binds,opaque_vertex_binds,opaque_index_binds,opaque_constant_binds,opaque_instance_buffer_binds,instances,instance_batches,instance_capacity,instance_bytes,draw_constant_bytes,upload_reserved_bytes,surface_width,surface_height,window_visible,window_foreground\n";
        failed_=!cpu_||!gpu_||!frames_;
    }
    ~MeasurementRenderer() override{Shutdown();}
    bool Initialize(const RenderSurface& surface,const Localization& text) override
    {
        started_=true;if(failed_||!delegate_.Initialize(surface,text))return false;
        adapter_=delegate_.Adapter();session_.started=std::chrono::steady_clock::now();session_.running=true;return true;
    }
    RenderResult Render(const RenderFrame& frame,const Localization& text) override
    {
        if(frame.surface.width!=session_.config.width||frame.surface.height!=session_.config.height){failed_=true;return RenderResult::Failed;}
        const auto result=delegate_.Render(frame,text);if(result==RenderResult::Failed){failed_=true;return result;}
        CpuScope measurement(frame.profiler,"CPU.BenchmarkCollect");const auto stats=delegate_.Statistics();const auto draws=delegate_.DrawStatistics();const auto& camera=session_.camera;
        const char* phase=session_.sampleTime<0?"warmup":session_.sampleTime<session_.config.sampleSeconds?"sample":"tail";
        frames_<<frame.updateIndex<<','<<session_.sampleTime<<','<<phase<<','<<frame.rawDeltaSeconds*1000<<','<<(result==RenderResult::Occluded?1:0)<<','<<camera.position.x<<','<<camera.position.y<<','<<camera.position.z<<','<<camera.yaw<<','<<camera.pitch
            <<','<<draws.candidates<<','<<draws.visible<<','<<draws.draws<<','<<draws.culled<<','<<draws.boundsUpdates<<','<<stats.globalWaits<<','<<stats.memoryResources<<','<<stats.memoryReservedBytes<<','<<stats.memoryUsedBytes<<','<<stats.memoryPeakUsedBytes
            <<','<<stats.persistentDescriptors<<','<<stats.frameDescriptors<<','<<stats.uploadPagesCreated<<','<<stats.uploadedBytes<<','<<stats.pipelineCreations<<','<<stats.shaderProgramCompilations<<','<<stats.debugWarnings<<','<<stats.debugErrors
            <<','<<stats.opaqueBindings.pipelines<<','<<stats.opaqueBindings.rootSignatures<<','<<stats.opaqueBindings.descriptorTables<<','<<stats.opaqueBindings.vertexBuffers<<','<<stats.opaqueBindings.indexBuffers<<','<<stats.opaqueBindings.constants
            <<','<<stats.opaqueBindings.instanceBuffers<<','<<draws.instances<<','<<draws.instanceBatches<<','<<draws.instanceCapacity<<','<<stats.frameInstanceBytes<<','<<stats.frameDrawConstantBytes<<','<<stats.uploadReservedBytes
            <<','<<frame.surface.width<<','<<frame.surface.height<<','<<(IsWindowVisible(frame.surface.window)?1:0)<<','<<(GetForegroundWindow()==frame.surface.window?1:0)<<'\n';
        ++rendered_;Pull();finalStats_=stats;
        if(!frames_||stats.debugErrors){failed_=true;return RenderResult::Failed;}
        if(!session_.captureMode&&session_.sampleTime>=session_.config.sampleSeconds)
        {
            complete_=true;if(!PostMessageW(static_cast<HWND>(frame.surface.window),WM_CLOSE,0,0)){failed_=true;return RenderResult::Failed;}
            Log(LogLevel::Info,text.GetText("Sample.BenchmarkComplete"));
        }
        return result;
    }
    void Shutdown() noexcept override
    {
        if(finished_)return;finished_=true;
        try
        {
            if(started_){if(!delegate_.WaitIdle())failed_=true;Pull();finalStats_=delegate_.Statistics();}
            elapsed_=session_.running?std::chrono::duration<double>(std::chrono::steady_clock::now()-session_.started).count():0;session_.running=false;
            delegate_.Shutdown();for(auto* stream:{&cpu_,&gpu_,&frames_}){stream->flush();if(!*stream)failed_=true;stream->close();}
        }
        catch(...){failed_=true;delegate_.Shutdown();}
    }
    void SetCapture(const fs::path& path,std::uint64_t frame){delegate_.SetCaptureOutput(path,frame);}
    bool Success() const{return !failed_&&delegate_.CaptureSucceeded()&&(session_.captureMode||complete_)&&gpuRows_==rendered_&&cpuFrames_==rendered_;}
    nlohmann::json Report() const
    {
        SYSTEM_POWER_STATUS power{};const bool gotPower=GetSystemPowerStatus(&power)!=FALSE;
        const auto& c=session_.config;return {{"schema",1},{"mode",session_.captureMode?"capture":"measurement"},{"scene",c.kind==BenchmarkKind::Mixed?"mixed":"repeated"},{"object_count",c.objects},{"seed",c.seed},
            {"layout_algorithm",BenchmarkLayoutAlgorithm(c.visibility)},{"layout_sha256",session_.layout.digest},{"visibility_case",BenchmarkVisibilityName(c.visibility)},{"size",{c.width,c.height}},{"warmup_seconds",c.warmupSeconds},{"sample_seconds",c.sampleSeconds},
            {"camera_path",{{"period_seconds",c.periodSeconds},{"radius_multiplier",c.radiusMultiplier},{"height_multiplier",c.heightMultiplier}}},{"elapsed_seconds",elapsed_},{"measurement_complete",complete_},
            {"rendered_frames",rendered_},{"cpu_frames_streamed",cpuFrames_},{"gpu_frames_streamed",gpuRows_},{"profiler_history_dropped",profiler_.DroppedFrames()},
            {"adapter",{{"name",adapter_.name},{"vendor_id",adapter_.vendorId},{"device_id",adapter_.deviceId},{"dedicated_video_memory",adapter_.dedicatedVideoMemory},{"software",adapter_.software}}},
            {"cpu_brand",CpuBrand()},{"hardware_threads",std::thread::hardware_concurrency()},{"power_status_available",gotPower},{"ac_line_status",gotPower?int(power.ACLineStatus):-1},
            {"present_sync_interval",1},{"culling",c.frustumCulling},{"sorting",c.sortOpaque},{"binding_cache",c.cacheOpaqueBindings},{"sort_algorithm","SelfEngine.OpaqueSort.1"},{"instancing",c.instancing},{"observer_scope","CPU.BenchmarkCollect"},{"debug_warnings",finalStats_.debugWarnings},{"debug_errors",finalStats_.debugErrors},
            {"surface_size_guard",true},{"window_station",UserObjectName(GetProcessWindowStation())},{"thread_desktop",UserObjectName(GetThreadDesktop(GetCurrentThreadId()))},
            {"outside_frame_sleep_ms",{{"occluded_or_nondrawable",16},{"drawable",1}}},
            {"final_memory_used_bytes",finalStats_.memoryUsedBytes},{"final_memory_reserved_bytes",finalStats_.memoryReservedBytes}};
    }
private:
    void Pull()
    {
        for(const auto& frame:profiler_.FramesSince(lastCpu_))
        {for(const auto& [scope,sample]:frame.scopes)cpu_<<frame.frame<<','<<Quote(scope)<<','<<sample.calls<<','<<sample.milliseconds<<'\n';lastCpu_=frame.frame;++cpuFrames_;}
        for(const auto& sample:delegate_.GpuTimingsSince(lastGpu_))
        {gpu_<<sample.sourceFrame<<','<<sample.submission<<','<<sample.frequency;for(auto tick:sample.ticks)gpu_<<','<<tick;
            gpu_<<','<<sample.draws<<','<<sample.frameMilliseconds<<','<<sample.geometryMilliseconds<<'\n';lastGpu_=sample.submission;++gpuRows_;}
        failed_|=!cpu_||!gpu_;
    }
    BenchmarkSession& session_;Profiler& profiler_;SandboxRenderer delegate_;std::ofstream cpu_,gpu_,frames_;
    rhi::AdapterInformation adapter_;rhi::Statistics finalStats_;
    std::uint64_t lastCpu_=0,lastGpu_=0,rendered_=0,cpuFrames_=0,gpuRows_=0;
    double elapsed_=0;bool failed_=false,started_=false,finished_=false,complete_=false;
};
}
int wmain(int argc,wchar_t** argv)
{
    using namespace se;Localization text;std::string error;
    if(!text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error)){std::cerr<<"BenchmarkRunner localization unavailable\n";return 1;}
    try
    {
        fs::path configPath=GetExecutableDirectory()/"Resources/Benchmark/repeated.json",output,capture;std::string sourceId;std::optional<double> warmup,seconds;std::optional<std::uint32_t> count;std::optional<bool> culling,sorting,bindingCache,instancing;bool show=false;std::uint64_t captureFrame=3;
        for(int index=1;index<argc;++index)
        {
            const std::wstring argument=argv[index];
            if(argument==L"--config"&&index+1<argc)configPath=argv[++index];
            else if(argument==L"--output"&&index+1<argc)output=argv[++index];
            else if(argument==L"--capture"&&index+1<argc)capture=argv[++index];
            else if(argument==L"--source-id"&&index+1<argc){const std::wstring value=argv[++index];if(value.size()!=64||value.find_first_not_of(L"0123456789abcdefABCDEF")!=std::wstring::npos)throw std::invalid_argument("source-id");for(wchar_t c:value)sourceId+=static_cast<char>(c);}
            else if(argument==L"--warmup"&&index+1<argc){const std::wstring value=argv[++index];size_t used=0;warmup=std::stod(value,&used);if(used!=value.size())throw std::invalid_argument("warmup");}
            else if(argument==L"--seconds"&&index+1<argc){const std::wstring value=argv[++index];size_t used=0;seconds=std::stod(value,&used);if(used!=value.size())throw std::invalid_argument("seconds");}
            else if(argument==L"--count"&&index+1<argc){const std::wstring value=argv[++index];size_t used=0;const auto number=std::stoull(value,&used);if(used!=value.size()||number>50000)throw std::invalid_argument("count");count=static_cast<std::uint32_t>(number);}
            else if(argument==L"--show")show=true;
            else if(argument==L"--culling"&&index+1<argc){const std::wstring value=argv[++index];if(value==L"on")culling=true;else if(value==L"off")culling=false;else throw std::invalid_argument("culling");}
            else if((argument==L"--sorting"||argument==L"--binding-cache"||argument==L"--instancing")&&index+1<argc)
            {const std::wstring value=argv[++index];auto& setting=argument==L"--sorting"?sorting:argument==L"--binding-cache"?bindingCache:instancing;if(value==L"on")setting=true;else if(value==L"off")setting=false;else throw std::invalid_argument("draw ordering");}
            else throw std::invalid_argument("arguments");
        }
        BenchmarkSession session;std::vector<AssetDiagnostic> diagnostics;
        if(!LoadBenchmarkConfig(configPath,session.config,diagnostics)){for(const auto& diagnostic:diagnostics)Log(LogLevel::Error,text.GetText(diagnostic.key)+": "+diagnostic.detail);return 2;}
        if(warmup)session.config.warmupSeconds=*warmup;if(seconds)session.config.sampleSeconds=*seconds;if(count)session.config.objects=*count;if(culling)session.config.frustumCulling=*culling;
        if(sorting)session.config.sortOpaque=*sorting;if(bindingCache)session.config.cacheOpaqueBindings=*bindingCache;
        if(instancing)session.config.instancing=*instancing;
        if(output.empty()||!ValidateBenchmarkConfig(session.config)){Log(LogLevel::Error,text.GetText("Sample.BenchmarkConfigInvalid"));return 2;}
        output=fs::absolute(output);if(fs::exists(output)){Log(LogLevel::Error,text.GetText("Sample.BenchmarkOutputExists"));return 2;}fs::create_directories(output);
        session.captureMode=!capture.empty();Profiler profiler;profiler.SetEnabled(true);BenchmarkScene scene(session);MeasurementRenderer renderer(session,profiler,output);
        if(session.captureMode)renderer.SetCapture(fs::absolute(capture),captureFrame);auto services=scene.Services();services.profiler=&profiler;if(session.captureMode)services.maximumFrames=captureFrame;
        const auto result=RunSandboxApp("en-US",renderer,show,&services);renderer.Shutdown();auto report=renderer.Report();report["source_sha256"]=sourceId;report["window_visible"]=show;
        std::vector<std::uint8_t> bytes;if(asset_io::Read(GetExecutableDirectory()/"BenchmarkRunner.exe",bytes))report["executable_sha256"]=asset_io::Hash(bytes);
        report["configuration"]=
#ifdef NDEBUG
            "Release";
#else
            "Debug";
#endif
        const bool success=!result&&renderer.Success();report["success"]=success;const auto encoded=report.dump(2)+"\n";
        if(!asset_io::WriteAtomic(output/"run.json",{reinterpret_cast<const std::uint8_t*>(encoded.data()),encoded.size()}))return 1;
        if(!success){Log(LogLevel::Error,text.GetText("Sample.BenchmarkFailed"));return 1;}return 0;
    }
    catch(const std::exception& exception){Log(LogLevel::Error,text.GetText("Sample.BenchmarkFailed")+": "+exception.what());return 2;}
}

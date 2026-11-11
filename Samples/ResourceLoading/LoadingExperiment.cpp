#include "LoadingExperiment.h"
#include "FunctionLayer/Runtime/Camera.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/ImageWriter.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Profiler.h"
#include "CoreLayer/Hash.h"
#include "CoreLayer/Utf.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace se
{
namespace
{
using Json=nlohmann::json;using Clock=std::chrono::steady_clock;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Ok(rhi::Result value,const char* message){if(!value)throw std::runtime_error(std::string(message)+": "+value.Key());}
double Ms(Clock::time_point begin){return std::chrono::duration<double,std::milli>(Clock::now()-begin).count();}
bool Save(const std::filesystem::path& path,const Json& data)
{const auto text=data.dump(2)+"\n";return asset_io::WriteAtomic(path,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});}
void SaveRows(const std::filesystem::path& path,const Json& rows)
{
    Check(!rows.empty(),"loading.rows");std::ofstream stream(path,std::ios::binary);stream.imbue(std::locale::classic());stream<<std::setprecision(17);
    bool first=true;for(const auto& item:rows[0].items()){if(!first)stream<<',';stream<<item.key();first=false;}stream<<'\n';
    for(const auto& row:rows){first=true;for(const auto& item:rows[0].items()){if(!first)stream<<',';stream<<row.at(item.key()).dump();first=false;}stream<<'\n';}Check(bool(stream),"loading.csv");
}
class LoadingView
{
public:
    WindowState window;std::shared_ptr<rhi::Device> device;std::uint32_t width=640,height=480;
    LoadingView(const Localization& text,std::size_t count,bool show):count_(count)
    {
        Check(CreateEngineWindow(window,show)&&SetEngineWindowClientSize(window,width,height),"loading.window");int exit=0;Check(PumpWindowMessages(exit),"loading.messages");rhi::Result result;device=rhi::Device::CreateD3D12({window.hwnd,width,height},text,result);Ok(result,"loading.device");
        const auto cube=MakeUnitCube();std::vector<rhi::Vertex> vertices;for(const auto& v:cube.vertices)vertices.push_back({v.position,v.normal,v.uv,v.tangent});
        Ok(device->CreateBuffer({},std::as_bytes(std::span(vertices)),vertices_),"loading.vertices");Ok(device->CreateBuffer({rhi::BufferUsage::Index,4},std::as_bytes(std::span(cube.indices)),indices_),"loading.indices");
        const auto& placeholder=DefaultTextureImage(DefaultTextureKind::Fallback);Ok(device->CreateTexture({placeholder.width,placeholder.height},placeholder.rgba8,fallback_),"loading.placeholder");
        Ok(device->CreatePipeline({rhi::PipelineKind::TexturedMesh,GetExecutableDirectory()/"Resources/Shaders/TexturedMesh.hlsl",1},pipeline_),"loading.pipeline");ResetWorld();
    }
    ~LoadingView(){if(device)device->Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
    void ResetWorld()
    {
        World candidate;camera_=candidate.Create("LoadingCamera",1);Check(bool(camera_)&&candidate.SetCamera(camera_,{Pi/3,.1f,100,true}),"loading.camera");
        std::uint32_t columns=1;while(size_t(columns)*columns<count_)++columns;const auto rows=(count_+columns-1)/columns;const float distance=std::max(float(columns),float(rows))*.85f+2;
        Check(candidate.SetTransform(camera_,{{0,0,distance},{},{1,1,1}}),"loading.camera transform");std::vector<EntityHandle> entities;
        for(std::size_t index=0;index<count_;++index)
        {
            const auto entity=candidate.Create("LoadingCube",100+index);const Vec3 position{(float(index%columns)-float(columns-1)*.5f)*1.15f,(float(index/columns)-float(rows-1)*.5f)*1.15f,0};
            Check(bool(entity)&&candidate.SetTransform(entity,{position,{},{.78f,.78f,.78f}})&&candidate.SetMesh(entity,{BuiltinCubeAssetId,{}}),"loading.scene cube");entities.push_back(entity);
        }
        world_=std::move(candidate);entities_=std::move(entities);
    }
    void Update(std::uint32_t frame)
    {
        for(std::size_t index=0;index<entities_.size();++index){auto transform=*world_.GetTransform(entities_[index]);transform.rotation=AxisAngle({0,1,0},.35f+float(frame)*.008f+float(index)*.11f)*AxisAngle({1,0,0},.12f);Check(world_.SetTransform(entities_[index],transform),"loading.world update");}
    }
    rhi::Texture Fallback()const{return fallback_;}
    void Render(std::span<const rhi::Texture> textures,std::uint32_t frame,rhi::Image* capture)
    {
        CameraSnapshot camera;CameraError error;Check(BuildCameraSnapshot({1,world_.GetWorldMatrix(camera_),*world_.GetCamera(camera_),true},{0,0,float(width),float(height)},frame,camera,error),"loading.camera snapshot");
        auto commands=device->BeginCommands(frame);Check(bool(commands),"loading.commands");Ok(commands->BeginPass({rhi::PassKind::Clear}),"loading.clear");Ok(commands->EndPass(),"loading.clear end");Ok(commands->BeginPass({rhi::PassKind::Opaque}),"loading.opaque");
        for(std::size_t index=0;index<entities_.size();++index){rhi::DrawIndexed draw{*vertices_,*indices_,*textures[index],*pipeline_,world_.GetWorldMatrix(entities_[index]),camera.viewProjection,{1,1,1,1},false,true};Ok(commands->Draw(draw),"loading.draw");}
        Ok(commands->EndPass(),"loading.opaque end");Ok(commands->Close(),"loading.close");rhi::CompletionToken token;Ok(device->Submit(*commands,token,capture),"loading.present");
    }
private:
    std::size_t count_;World world_;EntityHandle camera_;std::vector<EntityHandle> entities_;
    rhi::Buffer vertices_,indices_;rhi::Texture fallback_;rhi::Pipeline pipeline_;
};
}
int RunLoadingExperiment(const LoadingExperimentConfig& config,const Localization& text)
{
    Check(config.frames>config.triggerFrame+16&&config.frames<=5000&&config.triggerFrame&&config.frameMilliseconds<=100&&!config.output.empty(),"loading.configuration");
    Check((!config.cancelFrame&&!config.restartFrame)||(config.cancelFrame>config.triggerFrame&&config.restartFrame>config.cancelFrame&&config.restartFrame<config.frames),"loading.scenario");
    Check(!std::filesystem::exists(config.output),"output.exists");std::filesystem::create_directories(config.output);std::vector<AssetDiagnostic> diagnostics;LoadingWorkload workload;
    Check(ReadLoadingWorkload(config.workload,workload,diagnostics),"loading.workload verification");AssetStore assets(workload.root/"Assets",config.output/"Cache");Check(assets.Scan(diagnostics),"loading.catalog");
    LoadingView view(text,workload.images.size(),config.show);auto& device=*view.device;Profiler profiler(config.frames);profiler.SetEnabled(true);device.SetCpuProfiler(&profiler);
    struct Unbind{rhi::Device& device;~Unbind(){device.SetCpuProfiler(nullptr);}} unbind{device};
    TaskQueue tasks(2,128);AsyncAssetLoader resources(assets,tasks);AssetUploadScheduler uploads(device,resources,config.budget,&profiler);Ok(device.EnableGpuTiming(true),"loading.gpu queries");
    std::vector<AssetTicket> tickets;std::vector<rhi::Texture> synchronous;std::vector<std::shared_ptr<const ImageData>> syncCpu;std::vector<std::string> syncKeys;
    bool active=false,interrupted=false,modified=false,allReady=false;std::uint32_t epochs=0,readyFrame=0;double readyMs=0;Clock::time_point loadBegin{};Json rows=Json::array();rhi::Image image;
    const auto cancel=[&]{for(const auto& ticket:tickets)ticket.Cancel();tickets.clear();synchronous.clear();syncCpu.clear();syncKeys.clear();resources.CollectUnused();active=false;allReady=false;readyFrame=0;readyMs=0;};
    const auto start=[&]
    {
        cancel();view.ResetWorld();loadBegin=Clock::now();active=true;++epochs;
        if(config.asynchronous){for(const auto& id:workload.images)tickets.push_back(resources.Request(id,AsyncAssetKind::Image,AssetTarget::Renderable));}
        else
        {
            CpuScope scope(profiler,"CPU.LoadingSynchronous");
            for(const auto& id:workload.images)
            {
                auto decoded=assets.LoadTextureImage(id,diagnostics);Check(bool(decoded),"loading.synchronous decode");const auto key=ImageContentKey(*decoded);Check(!key.empty(),"loading.content key");rhi::Texture texture;
                Ok(device.CreateTexture({decoded->width,decoded->height,1,1,rhi::TextureFormat::Rgba8,id},decoded->rgba8,texture),"loading.synchronous upload");syncKeys.push_back(key);syncCpu.push_back(std::move(decoded));synchronous.push_back(std::move(texture));
            }
        }
    };
    std::cout<<text.GetText("Sample.LoadingControls")<<'\n';const auto experimentBegin=Clock::now();auto previous=experimentBegin;auto due=experimentBegin;const auto initial=device.Stats();
    for(std::uint32_t frame=1;frame<=config.frames;++frame)
    {
        int exit=0;if(!PumpWindowMessages(exit)){interrupted=true;break;}
        Check(profiler.BeginFrame(frame),"loading.cpu frame");const auto now=Clock::now();const auto before=device.Stats();const double delta=std::chrono::duration<double,std::milli>(now-previous).count();previous=now;
        if(view.window.width&&view.window.height&&(view.window.width!=view.width||view.window.height!=view.height))
        {view.width=view.window.width;view.height=view.window.height;Ok(device.Resize(view.width,view.height),"loading.resize");modified=true;}
        for(const auto& key:view.window.input.keyEvents)if(key.type==KeyEventType::Pressed&&key.keyCode=='R'){modified=true;if(active)cancel();else start();}
        if(frame==config.triggerFrame)start();if(frame==config.cancelFrame)cancel();if(frame==config.restartFrame)start();
        {CpuScope scope(profiler,"CPU.LoadingWorld");view.Update(frame);}
        {CpuScope scope(profiler,"CPU.AssetPublish");resources.Pump();}
        AssetUploadStep step;if(config.asynchronous)Ok(uploads.Tick(step),"loading.budgeted upload");
        std::vector<rhi::Texture> textures(workload.images.size(),view.Fallback());std::size_t ready=0;
        if(active)
        {
            if(config.asynchronous)
            {
                for(std::size_t index=0;index<tickets.size();++index)
                {
                    const auto state=resources.Query(tickets[index]);Check(state.state!=AssetLoadState::Failed,"loading.resource failed");
                    if(state.state==AssetLoadState::Ready){auto gpu=std::dynamic_pointer_cast<const RenderImageAsset>(state.gpu);Check(bool(gpu)&&bool(gpu->texture),"loading.typed image lease");textures[index]=gpu->texture;++ready;}
                }
            }
            else{textures=synchronous;ready=textures.size();}
        }
        if(active&&ready==workload.images.size()&&!allReady){allReady=true;readyFrame=frame;readyMs=Ms(loadBegin);}
        std::string title=text.GetText(config.asynchronous?"Sample.LoadingAsync":"Sample.LoadingSync")+" | "+text.GetText("Sample.LoadingProgress")+" "+std::to_string(ready)+"/"+std::to_string(workload.images.size());
        std::u32string decodedTitle;Check(DecodeUtf8(title,decodedTitle),"loading.title UTF8");const auto wide=EncodeUtf16(decodedTitle);SetWindowTextW(view.window.hwnd,std::wstring(wide.begin(),wide.end()).c_str());
        view.Render(textures,frame,!config.capture.empty()&&frame==config.frames?&image:nullptr);view.window.input.ClearEvents();const auto after=device.Stats();const auto resourceStats=resources.Stats();
        std::uint64_t cpuBytes=resourceStats.cpuBytes;if(!config.asynchronous)for(const auto& cpu:syncCpu)cpuBytes+=cpu->rgba8.size();
        rows.push_back({{"frame",frame},{"phase",frame<config.triggerFrame?"warmup":"loading_window"},{"raw_delta_ms",delta},{"wall_ms",Ms(experimentBegin)},{"ready",ready},{"draws",workload.images.size()},{"epoch",epochs},
            {"staged_payload_bytes",step.payloadBytes},{"staged_footprint_bytes",step.footprintBytes},{"copy_tasks",step.tasks},{"resource_creations",step.creations},{"pending_uploads",step.pending},{"receipts",step.completed},
            {"upload_submissions",after.uploadSubmissions-before.uploadSubmissions},{"device_upload_payload",after.uploadedBytes-before.uploadedBytes},{"native_staged_footprint",after.stagedFootprintBytes-before.stagedFootprintBytes},{"native_staged_copies",after.stagedCopies-before.stagedCopies},
            {"global_waits",after.globalWaits-before.globalWaits},{"cpu_payload_bytes",cpuBytes},{"loading_cpu",resourceStats.loadingCpu},{"upload_queued",resourceStats.queuedUploads},{"uploading_gpu",resourceStats.uploadingGpu},
            {"memory_used",after.memoryUsedBytes},{"memory_reserved",after.memoryReservedBytes},{"retiring",after.retiring},{"upload_reserved",after.uploadReservedBytes},{"srv_used",after.persistentDescriptors},
            {"pipeline_creations",after.pipelineCreations-before.pipelineCreations},{"shader_compilations",after.shaderProgramCompilations-before.shaderProgramCompilations},{"debug_warnings",after.debugWarnings},{"debug_errors",after.debugErrors},{"occluded",device.Occluded()}});
        Check(profiler.EndFrame(),"loading.cpu frame end");
        if(config.capture.empty()&&config.frameMilliseconds){due+=std::chrono::milliseconds(config.frameMilliseconds);std::this_thread::sleep_until(due);}
    }
    const bool complete=!interrupted&&rows.size()==config.frames&&allReady;
    const bool successful=complete||(config.show&&interrupted);
    uploads.Shutdown();resources.Shutdown();tasks.Shutdown();synchronous.clear();syncCpu.clear();syncKeys.clear();profiler.SetEnabled(false);Ok(device.WaitIdle(),"loading.final query drain");device.Collect();
    const auto final=device.Stats();SaveRows(config.output/"frames.csv",rows);Check(Save(config.output/"frames.json",rows),"loading.frame JSON");std::string error;
    Check(final.memoryUsedBytes==initial.memoryUsedBytes&&final.memoryResources==initial.memoryResources&&final.persistentDescriptors==initial.persistentDescriptors&&!final.retiring&&!final.inFlightBatches,"loading.retirement baseline");
    Check(profiler.ExportFramesCsv(config.output/"cpu.csv",error)&&profiler.ExportSummaryCsv(config.output/"cpu-summary.csv",error),"loading.CPU export");Ok(device.ExportGpuTimingsCsv(config.output/"gpu.csv"),"loading.GPU export");
    if(!config.capture.empty()){Check(complete&&!modified&&!image.rgba8.empty(),"loading.capture completion");Check(WritePpmImage(config.capture,image.width,image.height,image.rgba8,diagnostics),"loading.capture write");}
    const auto adapter=device.Adapter();SYSTEM_POWER_STATUS power{};const bool powerAvailable=GetSystemPowerStatus(&power)!=FALSE;
    std::wstring executable(32768,L'\0');const DWORD length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));Check(length&&length<executable.size(),"loading.program path");executable.resize(length);std::vector<std::uint8_t> program;Check(asset_io::Read(std::filesystem::path(executable),program),"loading.program digest");
    Json report{{"schema",1},{"success",successful},{"mode",config.capture.empty()?"measurement":"capture"},{"measurement_complete",complete&&!modified&&!config.cancelFrame&&config.capture.empty()},{"strategy",config.asynchronous?"async":"sync"},{"source_sha256",config.sourceIdentity},{"executable_sha256",Sha256(program)},
        {"workload_sha256",workload.digest},{"workload_algorithm","SelfEngine.ResourceLoading.1"},{"light",workload.light},{"objects",workload.images.size()},{"texture_size",{workload.width,workload.height}},{"frames",rows.size()},{"trigger_frame",config.triggerFrame},{"frame_interval_ms",config.capture.empty()?config.frameMilliseconds:0},{"requested_frame_interval_ms",config.frameMilliseconds},
        {"budget_bytes",config.budget.bytes},{"budget_tasks",config.budget.tasks},{"all_ready_frame",readyFrame},{"total_ready_ms",readyMs},{"epochs",epochs},{"cancel_frame",config.cancelFrame},{"restart_frame",config.restartFrame},{"modified",modified},{"interrupted",interrupted},{"size",{view.width,view.height}},
        {"present_sync_interval",1},{"gpu_timing_scope","draw submission only; upload batches are not GPU-timestamped"},{"input_files_preverified",true},{"application_sleep_outside_cpu_frame",true},{"power_available",powerAvailable},{"ac_line_status",power.ACLineStatus},
        {"adapter",{{"name",adapter.name},{"vendor_id",adapter.vendorId},{"device_id",adapter.deviceId},{"dedicated_bytes",adapter.dedicatedVideoMemory},{"software",adapter.software}}},{"debug_warnings",final.debugWarnings},{"debug_errors",final.debugErrors},{"logical_textures_after_shutdown",final.textures},{"staged_copies",final.stagedCopies-initial.stagedCopies},{"upload_payload_bytes",final.uploadedBytes-initial.uploadedBytes},
        {"baseline_memory_used",initial.memoryUsedBytes},{"final_memory_used",final.memoryUsedBytes},{"baseline_memory_resources",initial.memoryResources},{"final_memory_resources",final.memoryResources},{"baseline_srv_used",initial.persistentDescriptors},{"final_srv_used",final.persistentDescriptors},{"final_retiring",final.retiring},{"final_in_flight",final.inFlightBatches}};
#ifdef NDEBUG
    report["configuration"]="Release";
#else
    report["configuration"]="Debug";
#endif
    Check(Save(config.output/"run.json",report),"loading.report");Check(successful,"loading.incomplete");std::cout<<text.GetText("Sample.LoadingComplete")<<'\n';return 0;
}
}

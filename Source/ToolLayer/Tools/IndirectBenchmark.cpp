#include "IndirectExperiment.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/ImageWriter.h"
#include "CoreLayer/Hash.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>

namespace fs=std::filesystem;
using namespace se;
using Json=nlohmann::json;
namespace
{
using Clock=std::chrono::steady_clock;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
double Ms(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
std::string HashFile(const fs::path& path)
{
    std::ifstream file(path,std::ios::binary);Check(bool(file),"read input hash");
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});return Sha256(bytes);
}
void WriteJson(const fs::path& path,const Json& value)
{
    std::ofstream file(path,std::ios::binary);file<<value.dump(2)<<'\n';file.close();Check(bool(file),"write indirect JSON");
}
void SaveImage(const fs::path& path,const rhi::Image& image)
{
    std::vector<AssetDiagnostic> diagnostics;Check(WritePpmImage(path,image.width,image.height,image.rgba8,diagnostics),"write indirect PPM");
}
Json RuntimeHashes()
{
    Json result=Json::object();const auto root=GetExecutableDirectory();
    for(const auto* directory:{"Resources/Shaders","Resources/Localization","Experiments"})
        for(const auto& entry:fs::recursive_directory_iterator(root/directory))
            if(entry.is_regular_file())result[entry.path().lexically_relative(root).generic_string()]=HashFile(entry.path());
    return result;
}
// 逐字段 little-endian 编码；不散列 C++ padding 或指针。场景生成/散列不计入逐帧准备。
std::string SceneHash(const IndirectScene& scene)
{
    std::vector<std::uint8_t> bytes;bytes.reserve(168+scene.bounds.size()*140);
    const auto word=[&](std::uint32_t v){for(unsigned shift=0;shift<32;shift+=8)bytes.push_back(static_cast<std::uint8_t>(v>>shift));};
    const auto scalar=[&](float v){word(std::bit_cast<std::uint32_t>(v));};
    word(1);word(static_cast<std::uint32_t>(scene.instances.size()));
    for(float v:scene.viewProjection.values)scalar(v);for(const auto& plane:scene.frustum.planes)for(float v:plane)scalar(v);
    for(size_t i=0;i<scene.instances.size();++i)
    {
        const auto& b=scene.bounds[i];const auto& p=scene.instances[i];
        scalar(b.minimum.x);scalar(b.minimum.y);scalar(b.minimum.z);word(b.id);
        scalar(b.maximum.x);scalar(b.maximum.y);scalar(b.maximum.z);word(b.reserved);
        for(float v:p.world.values)scalar(v);scalar(p.tint.x);scalar(p.tint.y);scalar(p.tint.z);scalar(p.tint.w);
        scalar(p.metallic);scalar(p.roughness);scalar(p.normalScale);
        word(p.normalMapping);word(p.unlit);word(p.doubleSided);word(p.receivesShadow);
    }
    return Sha256(bytes);
}
std::uint32_t Number(const wchar_t* argument,std::uint32_t maximum)
{
    const std::wstring value=argument;Check(!value.empty()&&value.find_first_not_of(L"0123456789")==std::wstring::npos,"unsigned decimal argument");
    const auto result=std::stoull(value);Check(result<=maximum,"argument exceeds limit");return static_cast<std::uint32_t>(result);
}
void Released(const Json& stats)
{
    Check(stats["stopped"]&&stats["warnings"]==0&&stats["errors"]==0,"indirect shutdown diagnostics");
    for(const char* key:{"default_used_bytes","default_reserved_bytes","upload_reserved_bytes","readback_allocation_bytes","tracked_resources"})
        Check(stats[key]==0,"indirect shutdown resources");
}
void ExportFrames(const fs::path& directory,const IndirectExperiment& host,unsigned warmup)
{
    const auto frames=host.Frames();const auto cpu=host.CpuFrames();Check(frames.size()==cpu.size(),"matched CPU and GPU frame histories");
    std::ofstream csv(directory/"frames.csv",std::ios::binary),scopes(directory/"cpu-scopes.csv",std::ios::binary);
    csv.imbue(std::locale::classic());scopes.imbue(std::locale::classic());csv<<std::setprecision(17);scopes<<std::setprecision(17);
    csv<<"sequence,phase,path,input_count,cpu_visible,cpu_validation_ms,cpu_cull_ms,cpu_gather_ms,cpu_render_ms,cpu_frame_ms,cpu_command_record_ms,cpu_submit_present_ms,cpu_fence_wait_ms,cpu_query_read_ms,cpu_indirect_query_read_ms,cpu_mesh_validation_ms,cpu_instance_packing_ms,frequency_hz,frame_begin,geometry_begin,geometry_end,frame_end,scan_begin,local_end,group_end,gather_end,draw_end,gpu_frame_ms,gpu_geometry_ms,gpu_local_ms,gpu_group_ms,gpu_gather_ms,gpu_indirect_draw_ms\n";
    scopes<<"sequence,phase,scope,calls,milliseconds\n";
    for(size_t i=0;i<frames.size();++i)
    {
        const auto& f=frames[i];const auto& c=cpu[i];const auto& d=f.drawing;Check(c.frame==f.sequence&&d.sourceFrame==f.sequence&&d.frequency>0,"frame timing identity");
        const auto phase=f.sequence<=warmup?"warmup":"sample";
        csv<<f.sequence<<','<<phase<<','<<(f.gpu?"gpu":"cpu")<<','<<f.inputCount<<','<<f.cpuVisible<<','<<f.cpuValidationMs<<','<<f.cpuCullMs<<','<<f.cpuGatherMs<<','<<f.cpuRenderMs<<','<<f.cpuFrameMs;
        for(const char* scope:{"CPU.CommandRecord","CPU.SubmitPresent","CPU.GpuFenceWait","CPU.QueryRead","CPU.IndirectQueryRead","CPU.MeshValidation","CPU.InstancePacking"})
        {
            const auto found=c.scopes.find(scope);csv<<','<<(found==c.scopes.end()?0:found->second.milliseconds);
        }
        csv<<','<<d.frequency;for(auto tick:d.ticks)csv<<','<<tick;for(auto tick:f.indirectTicks)csv<<','<<tick;
        csv<<','<<d.frameMilliseconds<<','<<d.geometryMilliseconds;
        for(unsigned part=0;part<4;++part)csv<<','<<(f.gpu?double(f.indirectTicks[part+1]-f.indirectTicks[part])*1000.0/double(d.frequency):0.0);
        csv<<'\n';for(const auto& [name,sample]:c.scopes)scopes<<f.sequence<<','<<phase<<','<<name<<','<<sample.calls<<','<<sample.milliseconds<<'\n';
    }
    csv.close();scopes.close();Check(bool(csv)&&bool(scopes),"write indirect raw samples");
}
void Benchmark(const Localization& text,const fs::path& output,const std::string& source,bool smoke)
{
    Check(!output.empty()&&!source.empty()&&!fs::exists(output),"new output directory and source identity required");fs::create_directories(output);
    const auto runtime=RuntimeHashes();const unsigned repeats=smoke?1:3,warmup=smoke?8:64,samples=smoke?16:256;
    const std::array<std::uint32_t,3> sizes{32,1024,65536};
    Json protocol={{"schema",2},{"success",false},{"source_sha256",source},{"executable_sha256",HashFile(GetExecutableDirectory()/"IndirectBenchmark.exe")},
        {"runtime_sha256",runtime},{"layout","SelfEngine.IndirectGrid.1"},{"sizes",sizes},{"repetitions",repeats},{"warmup_iterations",warmup},{"sample_iterations",samples},{"smoke",smoke},
        {"configuration",
#ifdef NDEBUG
        "Release"
#else
        "Debug"
#endif
        },{"present_sync_interval",1},{"window_visible",false},{"width",320},{"height",240},{"normal_count_readback",false},{"cpu_gpu_timings_additive",false},
        {"candidate_scene_generation_timed",false},{"per_frame_matrix_validation_packing_timed",true},
        {"timestamp_protocol","native frame/geometry; local setup+scan, dependency+group scan, dependency+gather, transitions+bindings+ExecuteIndirect"}};
    WriteJson(output/"protocol.json",protocol);Json checks=Json::array(),runs=Json::array();fs::create_directory(output/"validation");
    for(auto count:sizes)for(unsigned rate=0;rate<3;++rate)
    {
        const auto scene=BuildIndirectScene(count,rate);IndirectExperiment host(text,count);const auto started=Clock::now();
        const auto cpu=host.Render(scene,false,true),gpu=host.Render(scene,true,true);host.Drain();const auto wall=Ms(started);
        Check(cpu.width==gpu.width&&cpu.height==gpu.height&&cpu.rgba8==gpu.rgba8,"CPU and indirect pixels differ");
        const auto name=std::to_string(count)+"-"+std::to_string(rate);const auto before=host.Statistics();
        for(const auto& [path,pixels]:std::array<std::pair<const char*,const rhi::Image*>,2>{{{"cpu",&cpu},{"gpu",&gpu}}})
        {
            std::ofstream raw(output/"validation"/(name+"-"+path+".rgba"),std::ios::binary);
            raw.write(reinterpret_cast<const char*>(pixels->rgba8.data()),static_cast<std::streamsize>(pixels->rgba8.size()));raw.close();Check(bool(raw),"write exact RGBA witness");
        }
        SaveImage(output/"validation"/(name+".ppm"),cpu);host.Shutdown();const auto after=host.Statistics();Released(after);
        checks.push_back({{"count",count},{"rate",rate},{"input_sha256",SceneHash(scene)},{"rgba_sha256",Sha256(cpu.rgba8)},
            {"width",cpu.width},{"height",cpu.height},{"wall_including_initial_gpu_setup_and_readbacks_ms",wall},{"statistics",before},{"after_shutdown",after}});
    }
    WriteJson(output/"validation.json",checks);
    for(unsigned round=0;round<repeats;++round)
    {
        std::array<unsigned,9> order{};std::iota(order.begin(),order.end(),0);if(round%2)std::reverse(order.begin(),order.end());
        for(unsigned condition:order)
        {
            const auto count=sizes[condition/3];const unsigned rate=condition%3;const auto scene=BuildIndirectScene(count,rate);const auto inputHash=SceneHash(scene);
            for(unsigned pair=0;pair<2;++pair)
            {
                const bool gpu=(pair+(round+condition)%2)%2!=0;const std::string path=gpu?"gpu":"cpu";
                const auto name=std::to_string(count)+"-"+std::to_string(rate)+"-"+std::to_string(round+1)+"-"+path;
                const auto directory=output/name;fs::create_directory(directory);IndirectExperiment host(text,count);Json initial;
                const auto started=Clock::now();
                for(unsigned frame=0;frame<warmup+samples;++frame)
                {
                    bool toggle=false;Check(host.Pump(toggle)&&!toggle,"benchmark window input interrupted protocol");host.Render(scene,gpu);
                    if(frame+1==warmup)initial=host.Statistics();
                }
                host.Drain();const double loop=Ms(started);const auto stats=host.Statistics();
                Check(stats["warnings"]==0&&stats["errors"]==0&&stats["image_readbacks"]==0&&stats["result_readbacks"]==0,"normal benchmark diagnostics");
                Check(host.Frames().size()==warmup+samples,"complete benchmark history");
                for(const char* key:{"default_used_bytes","default_reserved_bytes","upload_reserved_bytes","readback_allocation_bytes","tracked_resources"})
                    Check(stats[key]==initial[key],"steady benchmark allocation changed");
                ExportFrames(directory,host,warmup);host.Shutdown();const auto after=host.Statistics();Released(after);
                Json run={{"run",name},{"count",count},{"rate",rate},{"round",round+1},{"path",path},{"input_sha256",inputHash},
                    {"loop_including_messages_and_final_drain_ms",loop},{"warmup_statistics",initial},{"statistics",stats},{"after_shutdown",after}};
                WriteJson(directory/"run.json",run);runs.push_back(run);
            }
        }
    }
    Check(RuntimeHashes()==runtime,"runtime inputs changed during measurement");protocol["success"]=true;protocol["runs"]=runs;protocol["validation_cases"]=checks.size();WriteJson(output/"protocol.json",protocol);
}
}
int wmain(int argc,wchar_t** argv)
{
    Localization text;std::string error;
    if(!text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error)){std::cerr<<"IndirectBenchmark localization unavailable\n";return 1;}
    try
    {
        fs::path output,capture;std::string source;std::wstring language=L"zh-CN";bool benchmark=false,smoke=false,hidden=false,gpu=true,demoOptions=false;
        std::uint32_t count=1024,rate=2,limit=0;
        for(int i=1;i<argc;++i)
        {
            const std::wstring arg=argv[i];
            if(arg==L"--benchmark")benchmark=true;
            else if(arg==L"--smoke")smoke=true;
            else if(arg==L"--output"&&i+1<argc)output=fs::absolute(argv[++i]);
            else if(arg==L"--source-id"&&i+1<argc){const std::wstring id=argv[++i];Check(id.size()==64&&id.find_first_not_of(L"0123456789abcdef")==std::wstring::npos,"source identity");source.clear();for(auto digit:id)source.push_back(static_cast<char>(digit));}
            else if(arg==L"--language"&&i+1<argc){language=argv[++i];Check(language==L"zh-CN"||language==L"en-US","supported language");}
            else if(arg==L"--path"&&i+1<argc){const std::wstring path=argv[++i];Check(path==L"cpu"||path==L"gpu","cpu or gpu path");gpu=path==L"gpu";demoOptions=true;}
            else if(arg==L"--count"&&i+1<argc){count=Number(argv[++i],MaximumVisibilityItems);demoOptions=true;}
            else if(arg==L"--rate"&&i+1<argc){const std::wstring value=argv[++i];Check(value==L"none"||value==L"sparse"||value==L"all","visibility rate");rate=value==L"none"?0:value==L"sparse"?1:2;demoOptions=true;}
            else if(arg==L"--frames"&&i+1<argc){limit=Number(argv[++i],1000000);demoOptions=true;}
            else if(arg==L"--hidden"){hidden=true;demoOptions=true;}
            else if(arg==L"--capture"&&i+1<argc){capture=fs::absolute(argv[++i]);demoOptions=true;}
            else throw std::invalid_argument("indirect arguments");
        }
        Check(text.Load(GetExecutableDirectory()/"Resources/Localization"/(language+L".txt"),error),"load indirect language");
        if(benchmark){Check(!demoOptions,"benchmark does not accept demonstration overrides");Benchmark(text,output,source,smoke);}
        else
        {
            Check(output.empty()&&source.empty()&&!smoke&&(!hidden||limit>0)&&(capture.empty()||limit>0),"demo requires bounded hidden/capture run");
            Check(capture.empty()||!fs::exists(capture),"capture destination already exists");
            const auto scene=BuildIndirectScene(count,rate);IndirectExperiment host(text,count,!hidden);
            Log(LogLevel::Info,text.GetText("Sample.IndirectControls"));Log(LogLevel::Info,text.GetText(gpu?"Sample.IndirectGpu":"Sample.IndirectCpu"));
            for(std::uint32_t frame=0;!limit||frame<limit;++frame)
            {
                bool toggle=false;if(!host.Pump(toggle))break;if(toggle){gpu=!gpu;Log(LogLevel::Info,text.GetText(gpu?"Sample.IndirectGpu":"Sample.IndirectCpu"));}
                const bool save=!capture.empty()&&frame+1==limit;const auto image=host.Render(scene,gpu,save);if(save)SaveImage(capture,image);
            }
            host.Shutdown();Released(host.Statistics());
        }
        Log(LogLevel::Info,text.GetText("Sample.IndirectComplete"));return 0;
    }
    catch(const std::exception& exception){Log(LogLevel::Error,text.GetText("Sample.IndirectFailed")+": "+exception.what());return 1;}
}

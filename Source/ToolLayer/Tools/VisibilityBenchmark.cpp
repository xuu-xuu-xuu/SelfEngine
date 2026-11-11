#include "GpuVisibilityExperiment.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include "CoreLayer/Hash.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>

namespace fs=std::filesystem;
using namespace se;
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
VisibilityFrustum FrustumData()
{
    VisibilityFrustum f;f.planes={std::array<float,4>{1,0,0,1},{-1,0,0,1},{0,1,0,1},{0,-1,0,1},{0,0,1,1},{0,0,-1,1}};return f;
}
std::vector<VisibilityItem> Items(UINT count,unsigned visibleRate)
{
    std::vector<VisibilityItem> result;result.reserve(count);
    for(UINT i=0;i<count;++i)
    {
        const float x=visibleRate==0||(visibleRate==1&&i%10!=0)?10+float(i%19):float(i%11)*.05f-.25f;
        result.push_back({{x-.05f,-.1f,-.1f},i,{x+.05f,.1f,.1f},0});
    }
    return result;
}
nlohmann::json Stats(const GpuVisibilityStatistics& s)
{
    return {{"adapter",s.adapter},{"default_reserved_bytes",s.memory.reserved},{"default_used_bytes",s.memory.used},
        {"default_resource_count",s.memory.resources},{"upload_reserved_bytes",s.uploadReservedBytes},{"readback_allocation_bytes",s.readbackAllocationBytes},
        {"tracked_resources",s.states.resources},{"uav_barriers",s.states.uavBarriers},{"submissions",s.submissions},{"reuse_waits",s.reuseWaits},
        {"result_readbacks",s.resultReadbacks},{"result_readback_resources",s.resultReadbackResources},{"pending",s.pending},{"drains",s.drains},
        {"initialization_ms",s.initializationMs},{"debug_info_queue",s.debugInfoQueue},{"warnings",s.warnings},{"errors",s.errors},{"stopped",s.stopped}};
}
void WriteJson(const fs::path& path,const nlohmann::json& value)
{
    std::ofstream file(path,std::ios::binary);file<<value.dump(2)<<'\n';file.close();Check(bool(file),"write benchmark JSON");
}
}
int wmain(int argc,wchar_t** argv)
{
    Localization text;std::string error;
    if(!text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error)){std::cerr<<"VisibilityBenchmark localization unavailable\n";return 1;}
    try
    {
        fs::path output;std::string source;bool smoke=false;
        for(int i=1;i<argc;++i)
        {
            const std::wstring argument=argv[i];
            if(argument==L"--output"&&i+1<argc)output=fs::absolute(argv[++i]);
            else if(argument==L"--source-id"&&i+1<argc)
            {
                const std::wstring id=argv[++i];Check(id.size()==64&&id.find_first_not_of(L"0123456789abcdef")==std::wstring::npos,"source identity");
                source.reserve(id.size());for(wchar_t digit:id)source.push_back(static_cast<char>(digit));
            }
            else if(argument==L"--smoke")smoke=true;
            else throw std::invalid_argument("visibility benchmark arguments");
        }
        Check(!output.empty()&&!source.empty()&&!fs::exists(output),"new output directory and source identity required");
        fs::create_directories(output);const auto shader=GetExecutableDirectory()/"Experiments/Visibility.hlsl";
        const unsigned repeats=smoke?1:3,warmup=smoke?8:128,samples=smoke?16:1000;
        const auto frustum=FrustumData();const std::array<UINT,3> sizes{32,1024,65536};
        nlohmann::json metadata={{"schema",2},{"success",false},{"source_sha256",source},{"executable_sha256",HashFile(GetExecutableDirectory()/"VisibilityBenchmark.exe")},
            {"shader_sha256",HashFile(shader)},{"predicate_sha256",HashFile(shader.parent_path()/"VisibilityPredicate.hlsli")},
            {"smoke",smoke},{"repetitions",repeats},{"warmup_iterations",warmup},{"sample_iterations",samples},
            {"layout","SelfEngine.VisibilitySynthetic.1"},{"frustum_planes",frustum.planes},{"sizes",sizes},{"configuration",
#ifdef NDEBUG
            "Release"
#else
            "Debug"
#endif
            },{"timestamp_protocol","setup+reset, UAV dependency+cull, diagnostic copy separately"},{"normal_count_readback",false},
            {"cpu_reference_output_capacity_reused",true},{"present_used",false},{"cpu_gpu_timings_additive",false}};
        WriteJson(output/"protocol.json",metadata);
        nlohmann::json validations=nlohmann::json::array(),runs=nlohmann::json::array();
        for(unsigned size=0;size<3;++size)for(unsigned rate=0;rate<3;++rate)
        {
            const UINT count=sizes[size];const auto items=Items(count,rate);std::vector<UINT> reference;
            Check(CullVisibilityReference(items,frustum,reference),"reference validation");
            GpuVisibilityExperiment diagnostic(text,shader,true,count);
            const auto started=Clock::now();diagnostic.Submit(items,frustum,count);diagnostic.Drain();const auto validationWall=Ms(started);
            auto measured=diagnostic.TakeResults();Check(measured.size()==1,"diagnostic submission");auto& m=measured[0];std::sort(m.ids.begin(),m.ids.end());
            Check(m.ids==reference&&m.guards&&!m.overflow,"independent full-capacity diagnostic agrees with reference");
            const auto before=diagnostic.Statistics();Check(before.warnings==0&&before.errors==0,"diagnostic GPU messages");diagnostic.Shutdown();
            const auto after=diagnostic.Statistics();Check(after.states.resources==0&&after.memory.used==0,"diagnostic resources released");
            validations.push_back({{"count",count},{"rate",rate},{"visible",reference.size()},{"guards",m.guards},{"cpu_wall_including_completion_ms",validationWall},
                {"cpu_result_read_ms",m.readMs},{"gpu_copy_ms",m.readbackCopyMs},{"gpu_setup_reset_ms",m.setupResetMs},{"gpu_dependency_cull_ms",m.dependencyCullMs},
                {"frequency_hz",m.frequency},{"ticks",m.ticks},{"statistics",Stats(before)},{"after_shutdown",Stats(after)}});
        }
        WriteJson(output/"validation.json",validations);
        for(unsigned round=0;round<repeats;++round)
        {
            std::array<unsigned,9> order{};std::iota(order.begin(),order.end(),0);if(round%2)std::reverse(order.begin(),order.end());
            for(unsigned condition:order)
            {
                const UINT count=sizes[condition/3];const unsigned rate=condition%3;
                const auto items=Items(count,rate);const std::string name=std::to_string(count)+"-"+std::to_string(rate)+"-"+std::to_string(round+1);
                const auto directory=output/name;fs::create_directory(directory);
                std::vector<UINT> reference;std::vector<double> cpuReference(warmup+samples+1);std::uint64_t cpuChecksum=0;
                GpuVisibilityExperiment experiment(text,shader,false,count);GpuVisibilityStatistics warmStats;
                const auto loopStart=Clock::now();
                for(unsigned iteration=0;iteration<warmup+samples;++iteration)
                {
                    const auto started=Clock::now();Check(CullVisibilityReference(items,frustum,reference),"CPU benchmark reference");const auto referenceMs=Ms(started);
                    const auto visible=rate==0?0:rate==1?(count+9)/10:count;Check(reference.size()==visible,"fixed workload CPU count");cpuChecksum+=reference.size();
                    const auto sequence=experiment.Submit(items,frustum,count);cpuReference.at(static_cast<std::size_t>(sequence))=referenceMs;
                    if(iteration+1==warmup)warmStats=experiment.Statistics();
                }
                experiment.Drain();const auto loopMs=Ms(loopStart);auto measurements=experiment.TakeResults();const auto stats=experiment.Statistics();
                Check(measurements.size()==warmup+samples&&stats.warnings==0&&stats.errors==0&&stats.resultReadbacks==0&&stats.resultReadbackResources==0,"normal benchmark protocol");
                Check(stats.memory.used==warmStats.memory.used&&stats.memory.reserved==warmStats.memory.reserved&&stats.uploadReservedBytes==warmStats.uploadReservedBytes,"steady allocations after warmup");
                experiment.Shutdown();const auto after=experiment.Statistics();Check(after.states.resources==0&&after.memory.used==0,"normal resources released");
                std::ofstream csv(directory/"frames.csv",std::ios::binary);csv.imbue(std::locale::classic());csv<<std::setprecision(17);
                csv<<"sequence,phase,input_count,capacity,cpu_reference_ms,cpu_frame_ms,cpu_prepare_ms,cpu_record_ms,cpu_submit_ms,cpu_reuse_wait_ms,cpu_query_read_ms,frequency_hz,setup_begin,reset_end,cull_end,copy_end,gpu_setup_reset_ms,gpu_dependency_cull_ms\n";
                for(const auto& m:measurements)
                {
                    Check(!m.diagnostic&&m.ids.empty(),"normal result readback absent");
                    csv<<m.sequence<<','<<(m.sequence<=warmup?"warmup":"sample")<<','<<m.inputCount<<','<<m.capacity<<','<<cpuReference.at(static_cast<std::size_t>(m.sequence))<<','
                        <<m.cpuFrameMs<<','<<m.prepareMs<<','<<m.recordMs<<','<<m.submitMs<<','<<m.reuseWaitMs<<','<<m.readMs<<','<<m.frequency;
                    for(auto tick:m.ticks)csv<<','<<tick;
                    csv<<','<<m.setupResetMs<<','<<m.dependencyCullMs<<'\n';
                }
                csv.close();Check(bool(csv),"write visibility samples");
                const auto inputHash=Sha256({reinterpret_cast<const std::uint8_t*>(items.data()),items.size()*sizeof(VisibilityItem)});
                nlohmann::json run={{"run",name},{"count",count},{"rate",rate},{"round",round+1},{"input_sha256",inputHash},{"input_bytes",items.size()*sizeof(VisibilityItem)},
                    {"cpu_reference_capacity_bytes",reference.capacity()*sizeof(UINT)},{"cpu_visible_checksum",cpuChecksum},{"loop_including_reference_ms",loopMs},
                    {"warmup_statistics",Stats(warmStats)},{"statistics",Stats(stats)},{"after_shutdown",Stats(after)}};
                WriteJson(directory/"run.json",run);runs.push_back(run);
            }
        }
        metadata["success"]=true;metadata["runs"]=runs;metadata["validation_cases"]=validations.size();WriteJson(output/"protocol.json",metadata);
        Log(LogLevel::Info,text.GetText("Sample.GpuVisibilityComplete"));return 0;
    }
    catch(const std::exception& error){Log(LogLevel::Error,text.GetText("Sample.GpuVisibilityFailed")+": "+error.what());return 1;}
}

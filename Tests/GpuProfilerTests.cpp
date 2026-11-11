#include "PlatformLayer/Graphics/Rhi.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Profiler.h"
#include "CoreLayer/Localization.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace rhi=se::rhi;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Ok(rhi::Result value,const char* message){if(!value)throw std::runtime_error(std::string(message)+": "+value.Key());}
struct Fixture
{
    WindowState window;std::shared_ptr<rhi::Device> device;
    ~Fixture(){if(device)device->Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
};
void Size(Fixture& f,UINT width,UINT height)
{
    RECT rect{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
    AdjustWindowRectEx(&rect,static_cast<DWORD>(GetWindowLongPtrW(f.window.hwnd,GWL_STYLE)),FALSE,0);
    Check(SetWindowPos(f.window.hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"resize profile window");
    int code=0;Check(PumpWindowMessages(code)&&f.window.width==width&&f.window.height==height,"profile actual client dimensions");
}
double Mean(const std::vector<se::CpuFrame>& frames,std::string_view scope,std::uint64_t first,std::uint64_t last)
{
    double sum=0;std::size_t count=0;
    for(const auto& frame:frames)if(frame.frame>=first&&frame.frame<=last)
    {const auto found=frame.scopes.find(std::string(scope));Check(found!=frame.scopes.end(),"benchmark scope exists");sum+=found->second.milliseconds;++count;}
    Check(count==64,"benchmark has 64 ordinary frames per mode");return sum/double(count);
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply GPU profiler output directory");const auto output=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(output);
        Fixture f;Check(CreateEngineWindow(f.window,false),"create profiler window");Size(f,320,240);
        Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"profile strings");
        rhi::Result result;f.device=rhi::Device::CreateD3D12({f.window.hwnd,320,240},text,result);Ok(result,"create profiler GPU");
        rhi::Pipeline pipeline;
        Ok(f.device->CreatePipeline({rhi::PipelineKind::Triangle,GetExecutableDirectory()/"Resources/Shaders/Triangle.hlsl",1},pipeline),"create measured draw");
        se::Profiler cpu;cpu.SetEnabled(true);f.device->SetCpuProfiler(&cpu);
        std::uint64_t frame=0;bool timing=false;std::vector<std::uint64_t> expectedSources;
        const auto submit=[&](bool capture=false){
            ++frame;Check(cpu.BeginFrame(frame),"begin measured CPU frame");
            const auto source=1000+frame;
            std::unique_ptr<rhi::CommandContext> commands;
            {
                se::CpuScope preparation(cpu,"CPU.CommandPrepare");commands=f.device->BeginCommands(source);
                Ok(commands->BeginPass({rhi::PassKind::Clear}),"profile clear");Ok(commands->EndPass(),"profile clear end");
                Ok(commands->BeginPass({rhi::PassKind::Triangle}),"profile geometry pass");Ok(commands->Triangle(*pipeline,{}),"profile actual Draw");
                Ok(commands->EndPass(),"profile geometry end");Ok(commands->Close(),"profile command close");
            }
            rhi::Image image;rhi::CompletionToken token;
            Ok(f.device->Submit(*commands,token,capture?&image:nullptr),"submit measured Draw");
            if(timing)expectedSources.push_back(source);
            Check(cpu.EndFrame(),"end measured CPU frame");return image;
        };
        const auto plain=submit(true);
        Check(f.device->GpuTimings().empty(),"timing off is a real baseline");
        const auto offFirst=frame+1;for(int i=0;i<64;++i)submit();const auto offLast=frame;
        Ok(f.device->EnableGpuTiming(true),"enable real timestamp heap/readback");timing=true;
        const auto measured=submit(true);
        Check(plain.rgba8==measured.rgba8,"timestamp instrumentation preserves full image");
        const auto waits=f.device->Stats().globalWaits;
        const auto onFirst=frame+1;for(int i=0;i<64;++i)submit();const auto onLast=frame;
        Check(f.device->Stats().globalWaits==waits,"query result collection adds no ordinary-frame global wait");
        Check(f.device->GpuTimings().size()<=expectedSources.size(),"poll exposes only completed issued queries");
        Ok(f.device->WaitIdle(),"explicit end-of-series drain");
        Check(f.device->GpuTimings().size()==expectedSources.size(),"last two context samples collected");
        Size(f,512,256);Ok(f.device->Resize(512,256),"profile resize");submit();submit();
        Size(f,320,240);Ok(f.device->Resize(320,240),"profile restore drains pending samples");
        Check(f.device->GpuTimings().size()==expectedSources.size(),"resize preserves all samples");
        Ok(f.device->EnableGpuTiming(false),"disable timing after fence-safe drain");timing=false;
        const auto disabled=submit(true);Check(plain.rgba8==disabled.rgba8,"disabling queries preserves image");
        Check(f.device->GpuTimings().size()==expectedSources.size(),"disabled frame has no synthetic GPU timing");
        Ok(f.device->EnableGpuTiming(true),"re-enable fresh queries");timing=true;submit();submit();
        Ok(f.device->ExportGpuTimingsCsv(output/"gpu.csv"),"export with final pending samples");
        const auto samples=f.device->GpuTimings();Check(samples.size()==expectedSources.size()&&samples.size()==69,"exactly 69 issued timed frames");
        const auto cursorWaits=f.device->Stats().globalWaits;const auto suffix=f.device->GpuTimingsSince(samples[34].submission);
        Check(suffix.size()==34&&suffix.front().submission==samples[35].submission&&suffix.back().submission==samples.back().submission
            &&f.device->GpuTimingsSince(samples.back().submission).empty()&&f.device->Stats().globalWaits==cursorWaits,"incremental GPU cursor retains exact suffix without global wait");
        std::uint64_t previous=0;
        for(size_t i=0;i<samples.size();++i)
        {
            const auto& sample=samples[i];Check(sample.sourceFrame==expectedSources[i]&&sample.submission>previous,"source-frame correspondence and sorted submission order");
            previous=sample.submission;
            Check(sample.frequency>0&&sample.draws==1&&sample.ticks[0]<=sample.ticks[1]&&sample.ticks[1]<=sample.ticks[2]
                &&sample.ticks[2]<=sample.ticks[3]&&sample.frameMilliseconds>0&&sample.geometryMilliseconds>=0
                &&sample.geometryMilliseconds<=sample.frameMilliseconds,"valid GPU query intervals/frequency/draw count");
            Check(std::abs(sample.frameMilliseconds-double(sample.ticks[3]-sample.ticks[0])*1000.0/double(sample.frequency))<1e-12,"raw tick frequency conversion");
        }
        const auto cpuFrames=cpu.Frames();
        const double recordOff=Mean(cpuFrames,"CPU.CommandRecord",offFirst,offLast),recordOn=Mean(cpuFrames,"CPU.CommandRecord",onFirst,onLast);
        std::ostringstream benchmark;benchmark<<std::setprecision(17)<<"mode,frames,mean_command_record_cpu_ms,mean_command_prepare_cpu_ms,mean_submit_present_cpu_ms\n";
        benchmark<<"queries_off,64,"<<recordOff<<','<<Mean(cpuFrames,"CPU.CommandPrepare",offFirst,offLast)<<','<<Mean(cpuFrames,"CPU.SubmitPresent",offFirst,offLast)<<'\n';
        benchmark<<"queries_on,64,"<<recordOn<<','<<Mean(cpuFrames,"CPU.CommandPrepare",onFirst,onLast)<<','<<Mean(cpuFrames,"CPU.SubmitPresent",onFirst,onLast)<<'\n';
        Check(se::WriteProfileCsv(output/"overhead.csv",benchmark.str(),error)&&cpu.ExportFramesCsv(output/"cpu.csv",error)
            &&cpu.ExportSummaryCsv(output/"cpu-summary.csv",error),"export real CPU boundaries and overhead samples");
        std::ifstream csv(output/"gpu.csv");std::string line;size_t lines=0;while(std::getline(csv,line))++lines;
        Check(lines==samples.size()+1,"GPU CSV has actual samples and header only");
        const auto stats=f.device->Stats();Check(stats.debugWarnings==0&&stats.debugErrors==0,"timestamp InfoQueue warning/error/corruption zero");
        f.device->SetCpuProfiler(nullptr);
        std::cout<<"GPU timestamp frequency="<<samples.front().frequency<<", samples="<<samples.size()<<", no ordinary-frame flush; "
            <<"CPU record off/on means="<<recordOff<<'/'<<recordOn<<" ms (single-run noise applies); InfoQueue warning/error/corruption=0.\n";
        return 0;
    }
    catch(const std::exception& error){std::cerr<<"GpuProfilerTests: "<<error.what()<<'\n';return 1;}
}

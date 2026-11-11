#include "CoreLayer/Profiler.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
using namespace se;
void Check(bool condition,const char* text){if(!condition)throw std::runtime_error(text);}
int main(int argc,char** argv)
{
    try
    {
        Profiler profiler(120);Check(!profiler.BeginFrame(0),"disabled by default");{CpuScope inactive(profiler,"Inactive");}
        profiler.SetEnabled(true);
        for(std::uint64_t frame=1;frame<=100;++frame)
        {
            Check(profiler.BeginFrame(frame),"begin frame");
            Check(profiler.RecordCpuDuration("Fixed",static_cast<double>(frame)),"known duration");
            if(frame%2==0)profiler.RecordCpuDuration("Sparse",2);
            Check(profiler.EndFrame(),"end frame");
        }
        const auto stats=profiler.Statistics();const auto fixed=std::find_if(stats.begin(),stats.end(),[](const auto& s){return s.scope=="Fixed";});
        Check(fixed!=stats.end()&&fixed->frames==100&&fixed->averageMilliseconds==50.5&&fixed->p50Milliseconds==50&&fixed->p95Milliseconds==95&&fixed->p99Milliseconds==99,"nearest rank percentiles");
        profiler.BeginFrame(101);
        {CpuScope scope(profiler,"RAII");std::thread worker([&]{for(int i=0;i<10;++i)Check(profiler.RecordCpuDuration("Worker",0.5),"worker recording");});worker.join();}
        Check(!profiler.RecordCpuDuration("CPU.Frame",1)&&!profiler.RecordCpuDuration("NaN",std::numeric_limits<double>::quiet_NaN()),"reserved and invalid samples rejected");
        profiler.RecordCpuDuration("quoted,\"name",0);profiler.EndFrame();
        Check(profiler.Frames().back().scopes.at("Worker").calls==10&&profiler.Frames().back().scopes.at("RAII").milliseconds>=0,"RAII and worker samples");
        profiler.BeginFrame(102);
        {CpuScope late(profiler,"Late");profiler.EndFrame();profiler.BeginFrame(103);}
        profiler.EndFrame();Check(!profiler.Frames().back().scopes.contains("Late"),"late scope cannot enter another frame");
        const auto output=std::filesystem::path(argc>1?argv[1]:"out/validation/profiler");std::filesystem::create_directories(output);std::string error;
        Check(profiler.ExportFramesCsv(output/"frames.csv",error)&&profiler.ExportSummaryCsv(output/"summary.csv",error),"CSV export");
        std::ifstream input(output/"frames.csv");std::string text((std::istreambuf_iterator<char>(input)),{});
        Check(text.find("\"quoted,\"\"name\"")!=std::string::npos&&text.find("gpu")==std::string::npos,"CSV quoting and CPU only labeling");
        profiler.SetEnabled(false);Check(!profiler.BeginFrame(104)&&profiler.Frames().size()==103,"disabled retains history without capture");
        profiler.Clear();Check(profiler.Frames().empty(),"clear");Profiler bounded(2);bounded.SetEnabled(true);
        for(int i=1;i<=3;++i){bounded.BeginFrame(i);bounded.EndFrame();}
        Check(bounded.Frames().size()==2&&bounded.DroppedFrames()==1,"bounded history reports discarded frames");
        Check(bounded.FramesSince(2).size()==1&&bounded.FramesSince(2)[0].frame==3&&bounded.FramesSince(3).empty(),"incremental CPU cursor excludes already consumed frames");
        Profiler stream(2);stream.SetEnabled(true);std::uint64_t cursor=0,streamed=0;
        for(std::uint64_t frame=1;frame<=16;++frame){stream.BeginFrame(frame);stream.RecordCpuDuration("Stream",1);stream.EndFrame();for(const auto& sample:stream.FramesSince(cursor)){Check(sample.frame==++streamed,"stream has no gaps or duplicates");cursor=sample.frame;}}
        Check(streamed==16&&stream.DroppedFrames()==14,"stream preserves all frames beyond bounded history");
        std::cout<<"ProfilerTests passed\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<"ProfilerTests failed: "<<e.what()<<'\n';return 1;}
}

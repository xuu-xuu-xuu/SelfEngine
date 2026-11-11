#include "ToolLayer/Tools/IndirectExperiment.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Context.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Presentation.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Hash.h"
#include "ResourceLayer/Assets/ImageWriter.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace se;
namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Save(const fs::path& path,const rhi::Image& image)
{std::vector<AssetDiagnostic> diagnostics;Check(WritePpmImage(path,image.width,image.height,image.rgba8,diagnostics),"write indirect reference image");}
void Equal(const rhi::Image& cpu,const rhi::Image& gpu)
{Check(cpu.width==gpu.width&&cpu.height==gpu.height&&cpu.rgba8==gpu.rgba8,"CPU and ExecuteIndirect images exactly match");}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply indirect render evidence directory");const fs::path output=fs::absolute(argv[1]);fs::create_directories(output);
        Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"indirect render localization");
        IndirectExperiment experiment(text,MaximumVisibilityItems);nlohmann::json cases=nlohmann::json::array();std::string emptyHash,fullHash;
        for(UINT count:{0u,1u,63u,64u,65u,1024u,65536u})for(UINT rate:{0u,1u,2u})
        {
            const auto scene=BuildIndirectScene(count,rate);const auto cpu=experiment.Render(scene,false,true);
            if(count==0&&rate==0)Check(!experiment.Statistics()["gpu_resources_created"].get<bool>(),"CPU-only baseline creates no indirect resources");
            const auto gpu=experiment.Render(scene,true,true);Equal(cpu,gpu);
            const auto hash=Sha256(cpu.rgba8);if(count==0&&rate==0)emptyHash=hash;if(count==1024&&rate==2)fullHash=hash;
            cases.push_back({{"count",count},{"rate",rate},{"cpu_rgba_sha256",hash},{"gpu_rgba_sha256",Sha256(gpu.rgba8)},{"pixels_equal",true}});
            if(count==1024)Save(output/("rate-"+std::to_string(rate)+".ppm"),gpu);
        }
        Check(emptyHash!=fullHash,"nonempty test actually draws colored geometry");
        auto overlap=BuildIndirectScene(65,2,true);const auto first=experiment.Render(overlap,false,true);Equal(first,experiment.Render(overlap,true,true));Save(output/"equal-depth-forward.ppm",first);
        std::reverse(overlap.instances.begin(),overlap.instances.end());const auto reversed=experiment.Render(overlap,false,true);Equal(reversed,experiment.Render(overlap,true,true));
        Check(first.rgba8!=reversed.rgba8,"equal-depth colors witness input draw order");Save(output/"equal-depth-reversed.ppm",reversed);
        auto invalidBounds=BuildIndirectScene(65,1);invalidBounds.bounds[1].minimum.x=std::numeric_limits<float>::quiet_NaN();
        invalidBounds.bounds[2].minimum.x=2;invalidBounds.bounds[2].maximum.x=1;
        Equal(experiment.Render(invalidBounds,false,true),experiment.Render(invalidBounds,true,true));
        const auto submitted=experiment.Presentation().submittedFrames;auto bad=BuildIndirectScene(1,2);bad.bounds.clear();
        bool rejected=false;try{experiment.Render(bad,true);}catch(const std::exception&){rejected=true;}
        Check(rejected&&experiment.Presentation().submittedFrames==submitted,"mismatched input rejected without submission");
        bad=BuildIndirectScene(1,2);bad.bounds[0].id=55;rejected=false;try{experiment.Render(bad,true);}catch(const std::exception&){rejected=true;}
        Check(rejected&&experiment.Presentation().submittedFrames==submitted,"invalid index association rejected without submission");
        auto normal=BuildIndirectScene(1024,1);const auto imagesBefore=experiment.Statistics()["image_readbacks"].get<UINT64>();
        for(UINT i=0;i<20;++i){bool toggle=false;Check(experiment.Pump(toggle),"normal messages");experiment.Render(normal,i%2!=0);}
        experiment.Drain();const auto ordinary=experiment.Statistics();Check(ordinary["result_readbacks"]==0&&ordinary["image_readbacks"]==imagesBefore,"ordinary direct/indirect switching has no result/image readbacks");
        experiment.Resize(400,300);Equal(experiment.Render(normal,false,true),experiment.Render(normal,true,true));
        HWND window=nullptr;Check(SUCCEEDED(experiment.Presentation().swapChain->GetHwnd(&window)),"sample window handle");
        Check(PostMessageW(window,WM_KEYDOWN,VK_F1,0)!=FALSE,"queue path toggle");bool toggle=false;
        Check(experiment.Pump(toggle)&&toggle,"F1 requests path toggle");
        Check(PostMessageW(window,WM_KEYDOWN,VK_F1,LPARAM{1}<<30)!=FALSE,"queue key repeat");
        Check(experiment.Pump(toggle)&&!toggle,"held F1 does not repeatedly toggle");
        Check(SetWindowPos(window,nullptr,0,0,480,360,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"external window resize");
        Check(experiment.Pump(toggle),"resize message handling");RECT client{};Check(GetClientRect(window,&client)!=FALSE,"resized client extent");
        Check(experiment.Presentation().width==static_cast<UINT>(client.right)&&experiment.Presentation().height==static_cast<UINT>(client.bottom),"presentation follows client extent");
        Equal(experiment.Render(normal,false,true),experiment.Render(normal,true,true));
        Check(PostMessageW(window,WM_KEYDOWN,VK_ESCAPE,0)!=FALSE&&!experiment.Pump(toggle),"Esc closes sample loop");
        experiment.Drain();const auto measured=experiment.Frames();for(const auto& frame:measured)
        {
            Check(frame.drawing.frequency>0&&frame.drawing.sourceFrame==frame.sequence,"every draw has native raw timestamps");
            if(frame.gpu)Check(frame.indirectTicks[0]&&std::is_sorted(frame.indirectTicks.begin(),frame.indirectTicks.end()),"indirect compute and draw intervals complete");
        }
        const auto before=experiment.Statistics();Check(before["warnings"]==0&&before["errors"]==0&&before["dropped_frames"]==0,"draw debug messages and complete frame history");
#ifndef NDEBUG
        Check(before["debug_info_queue"].get<bool>(),"draw Debug InfoQueue present");
#endif
        experiment.Shutdown();const auto after=experiment.Statistics();
        for(const char* key:{"default_used_bytes","default_reserved_bytes","upload_reserved_bytes","readback_allocation_bytes","tracked_resources"})Check(after[key]==0,"draw shutdown releases tracked allocations");
        Check(after["warnings"]==0&&after["errors"]==0,"shutdown debug messages clean");
        IndirectExperiment empty(text,0);const auto emptyScene=BuildIndirectScene(0,0);Equal(empty.Render(emptyScene,false,true),empty.Render(emptyScene,true,true));
        empty.Shutdown();const auto emptyStats=empty.Statistics();
        Check(emptyStats["warnings"]==0&&emptyStats["errors"]==0&&emptyStats["tracked_resources"]==0,"physical zero-capacity draw and release");
        std::ofstream file(output/"verification.json",std::ios::binary);
        file<<nlohmann::json{{"success",true},{"draw_execution_tested",true},{"pairs",cases},{"equal_depth_order_witness",true},{"invalid_bounds_conservative",true},
            {"normal_switches",20},{"resize",true},{"keyboard_and_client_resize",true},{"empty_physical_capacity",true},{"measured_frames",measured.size()},{"statistics",before},{"after_shutdown",after}}.dump(2)<<'\n';file.close();Check(bool(file),"write draw verification");
        std::cout<<"Indirect rendering: 21 image pairs, equal-depth order, zero/max inputs, switching and resize passed\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

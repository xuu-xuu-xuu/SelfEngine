// G06.05：depth prepass + HZB 遮挡接线验证。关闭/开启遮挡的整帧图像必须逐字节一致；
// 首帧与 resize 后按门控跳过一次，随后恢复遮挡；记录 HZB/整体 GPU 时间。
#include "ToolLayer/Tools/IndirectExperiment.h"
#include "FunctionLayer/Rendering/HiZ.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Localization.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace se;
namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void SavePpm(const rhi::Image& image,const fs::path& path)
{
    std::ofstream file(path,std::ios::binary);
    file<<"P6\n"<<image.width<<' '<<image.height<<"\n255\n";
    for(std::size_t i=0;i<image.rgba8.size();i+=4){const char rgb[3]={char(image.rgba8[i]),char(image.rgba8[i+1]),char(image.rgba8[i+2])};file.write(rgb,3);}
    Check(bool(file),"write occlusion render image");
}
double Median(std::vector<double> values)
{
    Check(!values.empty(),"median needs samples");std::sort(values.begin(),values.end());
    return values[values.size()/2];
}
// G06.06：HUD 计数条。绿色=可见、红色=剔除；数字由 stdout/verification.json 给出，
// 图像里只画比例，避免测试宿主依赖字体资源。
void DrawOcclusionHud(rhi::Image& image,const IndirectOcclusionCounters& counters)
{
    if(image.width<260||image.height<48)return;
    const int left=8,top=8,width=224,height=18;
    const auto put=[&](int x,int y,std::uint8_t red,std::uint8_t green,std::uint8_t blue)
    {
        if(x<0||y<0||x>=int(image.width)||y>=int(image.height))return;
        auto* pixel=image.rgba8.data()+(static_cast<std::size_t>(y)*image.width+static_cast<std::size_t>(x))*4;
        pixel[0]=red;pixel[1]=green;pixel[2]=blue;pixel[3]=255;
    };
    for(int y=-2;y<height+2;++y)for(int x=-2;x<width+2;++x)put(left+x,top+y,24,24,24);
    const auto visible=counters.candidates
        ?int(std::lround(double(width)*double(counters.visible)/double(counters.candidates))):0;
    for(int y=0;y<height;++y)for(int x=0;x<width;++x)
    {
        if(x<visible)put(left+x,top+y,60,180,60);
        else put(left+x,top+y,190,48,48);
    }
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply GPU occlusion render evidence directory");
        const auto output=fs::absolute(argv[1]);fs::create_directories(output);
        Localization text;std::string error;
        Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"occlusion render localization");
        const auto scene=BuildOcclusionScene();
        const auto dense=BuildDenseOcclusionScene();
        IndirectExperiment experiment(text,static_cast<std::uint32_t>(std::max(scene.instances.size(),dense.instances.size())));
        const auto off=experiment.Render(scene,true,true,false);
        Check(off.width==320&&off.height==240&&!off.rgba8.empty(),"occlusion off image");
        const auto first=experiment.Render(scene,true,true,true);
        Check(first.rgba8==off.rgba8,"first occluded frame is gated off and matches");
        std::vector<std::uint8_t> lastOn;
        for(unsigned i=0;i<6;++i)
        {
            const auto on=experiment.Render(scene,true,true,true);
            Check(on.rgba8==off.rgba8,"occluded frame matches no-occlusion image byte for byte");
            lastOn=on.rgba8;
        }
        SavePpm(off,output/"off.ppm");SavePpm(rhi::Image{off.width,off.height,lastOn},output/"on.ppm");
        for(unsigned i=0;i<12;++i)experiment.Render(scene,true,false,false);
        for(unsigned i=0;i<12;++i)experiment.Render(scene,true,false,true);
        experiment.Drain();
        const auto frames=experiment.Frames();
        std::vector<double> offTimes,onTimes;std::size_t occludedFrames=0;bool hizTimed=false;
        for(const auto& frame:frames)
        {
            if(frame.occlusion)
            {
                ++occludedFrames;
                onTimes.push_back(frame.drawing.frameMilliseconds);
                if(frame.hizTicks[0]>0&&std::is_sorted(frame.hizTicks.begin(),frame.hizTicks.end()))hizTimed=true;
            }
            else if(frame.gpu)offTimes.push_back(frame.drawing.frameMilliseconds);
        }
        Check(occludedFrames>=10,"occlusion frames recorded");
        Check(hizTimed,"HZB GPU timestamps recorded for occluded frames");
        Check(!offTimes.empty()&&!onTimes.empty(),"both paths have frame samples");
        const auto offMedian=Median(offTimes),onMedian=Median(onTimes);
        experiment.Resize(320,180);
        const auto smallOff=experiment.Render(scene,true,true,false);
        const auto smallFirst=experiment.Render(scene,true,true,true);
        Check(smallFirst.rgba8==smallOff.rgba8,"first frame after resize is gated off and matches");
        const auto smallOn=experiment.Render(scene,true,true,true);
        Check(smallOn.rgba8==smallOff.rgba8,"occluded frame after resize matches");
        SavePpm(smallOff,output/"resized-off.ppm");SavePpm(smallOn,output/"resized-on.ppm");
        // 大分辨率下隐藏实例覆盖的像素足够多，才能观察到净帧时间收益。
        experiment.Resize(1280,720);
        const auto largeOff=experiment.Render(scene,true,true,false);
        const auto largeFirst=experiment.Render(scene,true,true,true);
        Check(largeFirst.rgba8==largeOff.rgba8,"first large frame is gated off and matches");
        const auto largeOn=experiment.Render(scene,true,true,true);
        Check(largeOn.rgba8==largeOff.rgba8,"large occluded frame matches");
        SavePpm(largeOff,output/"large-off.ppm");SavePpm(largeOn,output/"large-on.ppm");
        const auto largeStart=experiment.Frames().size();
        for(unsigned i=0;i<12;++i)experiment.Render(scene,true,false,false);
        for(unsigned i=0;i<12;++i)experiment.Render(scene,true,false,true);
        experiment.Drain();
        const auto allFrames=experiment.Frames();
        std::vector<double> largeOffTimes,largeOnTimes;
        for(std::size_t i=largeStart;i<allFrames.size();++i)
        {
            const auto& frame=allFrames[i];
            if(frame.occlusion)largeOnTimes.push_back(frame.drawing.frameMilliseconds);
            else if(frame.gpu)largeOffTimes.push_back(frame.drawing.frameMilliseconds);
        }
        Check(!largeOffTimes.empty()&&!largeOnTimes.empty(),"large resolution samples");
        const auto largeOffMedian=Median(largeOffTimes),largeOnMedian=Median(largeOnTimes);
        // G06.06：重着色场景净收益。400 个隐藏实例铺满遮挡体投影、400 个前方实例，
        // 关闭遮挡时后方实例先着色再被覆盖，开启后整批剔除；图像必须逐字节一致。
        const auto denseOff=experiment.Render(dense,true,true,false);
        const auto denseFirst=experiment.Render(dense,true,true,true);
        Check(denseFirst.rgba8==denseOff.rgba8,"dense first occluded frame matches no-occlusion image");
        const auto denseOn=experiment.Render(dense,true,true,true);
        Check(denseOn.rgba8==denseOff.rgba8,"dense occluded frame matches no-occlusion image");
        SavePpm(denseOff,output/"dense-off.ppm");SavePpm(denseOn,output/"dense-on.ppm");
        const auto denseStart=experiment.Frames().size();
        for(unsigned i=0;i<12;++i)experiment.Render(dense,true,false,false);
        for(unsigned i=0;i<12;++i)experiment.Render(dense,true,false,true);
        experiment.Drain();
        const auto denseFrames=experiment.Frames();
        std::vector<double> denseOffTimes,denseOnTimes;
        for(std::size_t i=denseStart;i<denseFrames.size();++i)
        {
            const auto& frame=denseFrames[i];
            if(frame.occlusion)denseOnTimes.push_back(frame.drawing.frameMilliseconds);
            else if(frame.gpu)denseOffTimes.push_back(frame.drawing.frameMilliseconds);
        }
        Check(denseOffTimes.size()>=10&&denseOnTimes.size()>=10,"dense scene has both path samples");
        const auto denseOffMedian=Median(denseOffTimes),denseOnMedian=Median(denseOnTimes);
        experiment.Drain();
        const auto denseVisible=experiment.LastVisibleCount();
        const auto denseOcclusionRatio=double(dense.instances.size()-denseVisible)/double(dense.instances.size());
        std::cout<<"Dense occlusion: off="<<denseOffMedian<<" ms, on="<<denseOnMedian<<" ms, visible="
            <<denseVisible<<"/"<<dense.instances.size()<<", ratio="<<denseOcclusionRatio<<'\n';
        Check(denseOnMedian<denseOffMedian,"dense occlusion yields a net frame-time improvement");
        Check(denseVisible>=401&&denseVisible<=500,"dense HZB culls most hidden instances");
        Check(denseOcclusionRatio>=0.30&&denseOcclusionRatio<=0.60,"dense scene culling ratio is 30%-60%");
        // G06.06：HUD 计数与 HZB 可视化。计数来自间接参数读回，HZB mip0 读回后重建 CPU 链并落盘。
        const auto counters=experiment.LastOcclusionCounters();
        Check(counters.enabled&&counters.candidates==dense.instances.size()&&counters.visible==denseVisible
            &&counters.culled==counters.candidates-counters.visible,"dense HUD counters match the indirect readback");
        Check(std::abs(counters.occlusionRate-float(denseOcclusionRatio))<1e-6f,"dense HUD ratio matches the readback");
        std::vector<float> hizDepth;std::uint32_t hizWidth=0,hizHeight=0;
        Check(experiment.ReadHiz(hizDepth,hizWidth,hizHeight)&&hizWidth==1280&&hizHeight==720
            &&hizDepth.size()==std::size_t{hizWidth}*hizHeight,"HZB mip0 readback for visualization");
        HiZBuffer hizReference;Check(hizReference.Build(hizWidth,hizHeight,hizDepth),"HZB readback rebuilds the CPU chain");
        HiZPreview hizPreview;Check(BuildHiZPreview(hizReference,hizPreview)&&hizPreview.Ready()
            &&hizPreview.levels==hizReference.MipCount(),"HZB readback colorizes into a preview");
        SavePpm(rhi::Image{hizPreview.width,hizPreview.height,hizPreview.mip0},output/"hiz-preview.ppm");
        auto hud=denseOn;DrawOcclusionHud(hud,counters);SavePpm(hud,output/"dense-hud.ppm");
        std::cout<<"HUD: candidates="<<counters.candidates<<", visible="<<counters.visible
            <<", culled="<<counters.culled<<", rate="<<counters.occlusionRate*100.0f
            <<"%, hiz_mips="<<hizPreview.levels<<'\n';
        experiment.Drain();
        const auto statistics=experiment.Statistics();
        Check(statistics.at("warnings").get<std::uint64_t>()==0&&statistics.at("errors").get<std::uint64_t>()==0,
            "occlusion render has no D3D12 warnings or errors");
        const auto after=experiment.Frames();
        std::size_t resizedOccluded=0;
        for(const auto& frame:after)if(frame.occlusion&&frame.hizTicks[0]>0)++resizedOccluded;
        Check(resizedOccluded>=1,"HZB rebuilt after resize");
        experiment.Shutdown();
        const auto shutdown=experiment.Statistics();
        Check(shutdown.at("stopped").get<bool>(),"occlusion render shutdown");
        const nlohmann::json report={
            {"success",true},{"off_equals_on",true},{"off_bytes",off.rgba8.size()},
            {"frames",frames.size()},{"occluded_frames",occludedFrames},{"resized_occluded_frames",resizedOccluded},
            {"off_median_frame_ms",offMedian},{"on_median_frame_ms",onMedian},
            {"frame_delta_ms",onMedian-offMedian},
            {"large_off_median_frame_ms",largeOffMedian},{"large_on_median_frame_ms",largeOnMedian},
            {"large_frame_delta_ms",largeOnMedian-largeOffMedian},
            {"large_net_improved",largeOnMedian<=largeOffMedian},
            {"dense_total_instances",dense.instances.size()},{"dense_visible_instances",denseVisible},
            {"dense_occlusion_ratio",denseOcclusionRatio},
            {"dense_hud_candidates",counters.candidates},{"dense_hud_visible",counters.visible},
            {"dense_hud_culled",counters.culled},{"dense_hud_rate",counters.occlusionRate},
            {"hiz_preview_levels",hizPreview.levels},{"hiz_preview_min_depth",hizPreview.levelInfo.empty()?0:hizPreview.levelInfo[0].minimumDepth},
            {"hiz_preview_max_depth",hizPreview.levelInfo.empty()?0:hizPreview.levelInfo[0].maximumDepth},
            {"dense_off_median_frame_ms",denseOffMedian},{"dense_on_median_frame_ms",denseOnMedian},
            {"dense_frame_delta_ms",denseOnMedian-denseOffMedian},
            {"dense_net_improved",denseOnMedian<denseOffMedian},
            {"adapter",statistics.value("adapter",std::string{})},{"warnings",statistics.at("warnings")},{"errors",statistics.at("errors")},
            {"resources_after_shutdown",shutdown.value("tracked_resources",std::uint64_t{0})}};
        std::ofstream file(output/"verification.json",std::ios::binary);file<<report.dump(2)<<'\n';file.close();Check(bool(file),"write occlusion render verification");
        std::cout<<"GPU occlusion render: "<<frames.size()<<" frames, byte-identical off/on, resize gate, HZB timings passed\n";
        return 0;
    }
    catch(const std::exception& exception){std::cerr<<exception.what()<<'\n';return 1;}
}

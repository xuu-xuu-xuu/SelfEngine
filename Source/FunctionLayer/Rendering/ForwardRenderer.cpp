#include "ForwardRenderer.h"
#include "PostProcessRenderer.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <algorithm>
#include <string>

namespace se
{
rhi::ForwardLighting PrepareForwardLighting(std::span<const LightInstance> lights,float exposure,Vec3 sky,
    std::vector<AssetDiagnostic>& diagnostics)
{
    rhi::ForwardLighting frame;bool clamped=false;
    const auto limited=[&](double value,double low,double high){
        const double safe=std::isfinite(value)?std::clamp(value,low,high):low;clamped|=safe!=value;return static_cast<float>(safe);};
    frame.exposure=limited(exposure,1.0/1024,64);frame.sky={limited(sky.x,0,10000),limited(sky.y,0,10000),limited(sky.z,0,10000)};
    std::vector<const LightInstance*> ordered;for(const auto& light:lights)ordered.push_back(&light);
    std::stable_sort(ordered.begin(),ordered.end(),[](const auto* a,const auto* b){return a->entity<b->entity;});
    std::size_t directional=0,points=0;
    for(const auto* instance:ordered)
    {
        const auto& light=instance->light;
        const Vec3 radiance{limited(double(light.color.x)*light.intensity,0,10000),limited(double(light.color.y)*light.intensity,0,10000),limited(double(light.color.z)*light.intensity,0,10000)};
        if(light.type==LightType::Directional)
        {
            ++directional;if(frame.directionalEnabled)continue;
            const auto direction=TransformVector(instance->world,{0,0,1});
            const double length=std::sqrt(double(direction.x)*direction.x+double(direction.y)*direction.y+double(direction.z)*direction.z);
            if(!IsFinite(direction)||!std::isfinite(length)||length<1e-8)
            {asset_io::Diagnostic(diagnostics,"Rendering.LightInvalid",std::to_string(instance->entity),true);continue;}
            frame.directionalEnabled=true;frame.directionToLight={float(direction.x/length),float(direction.y/length),float(direction.z/length)};
            frame.directionalRadiance=radiance;
        }
        else if(light.type==LightType::Point)
        {
            if(++points>rhi::MaximumPointLights)continue;
            auto& point=frame.points[frame.pointCount++];const auto position=TransformPoint(instance->world,{});
            point.position={limited(position.x,-1e6,1e6),limited(position.y,-1e6,1e6),limited(position.z,-1e6,1e6)};
            point.radiance=radiance;point.range=limited(light.range,0.05,10000);
        }
        else asset_io::Diagnostic(diagnostics,"Rendering.LightInvalid",std::to_string(instance->entity),true);
    }
    if(directional>1||points>rhi::MaximumPointLights)asset_io::Diagnostic(diagnostics,"Rendering.LightLimit",std::to_string(directional)+"/"+std::to_string(points),true);
    if(clamped)asset_io::Diagnostic(diagnostics,"Rendering.LightClamped",{},true);return frame;
}
rhi::Result RecordForwardFrame(rhi::CommandContext& commands,const rhi::ForwardLighting& lighting,
    rhi::PipelineHandle output,rhi::PipelineHandle sky,std::span<const rhi::DrawIndexed> draws,bool cacheBindings)
{
    auto result=commands.ConfigureForward(lighting,output,sky);
    if(result)result=commands.BeginPass({rhi::PassKind::Clear,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,"Clear"});
    if(result)result=commands.EndPass();
    if(result)result=commands.BeginPass({rhi::PassKind::Sky,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,"Sky"});
    if(result)result=commands.EndPass();
    if(result)result=commands.BeginPass({rhi::PassKind::Opaque,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,"Opaque",rhi::ResourceUse::None,cacheBindings});
    if(result)for(const auto& draw:draws){result=commands.Draw(draw);if(!result)break;}
    if(result)result=commands.EndPass();
    if(result)result=commands.BeginPass({rhi::PassKind::Output,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,"Output",rhi::ResourceUse::ColorRead});
    if(result)result=commands.EndPass();if(result)result=commands.Close();return result;
}
rhi::Result RecordShadowedFrame(rhi::CommandContext& commands,const rhi::ForwardLighting& lighting,
    const rhi::ShadowSettings& shadows,rhi::PipelineHandle depth,rhi::PipelineHandle output,rhi::PipelineHandle sky,
    std::span<const rhi::DrawIndexed> visible,std::span<const rhi::DrawIndexed> casters,const rhi::EnvironmentBindings* environment,
    const rhi::PostProcessSettings* post,const rhi::PostProcessPipelines* postPipelines,std::span<const rhi::OverlayQuad> overlays,bool cacheBindings)
{
    auto result=commands.ConfigureForward(lighting,output,sky);if(result)result=commands.ConfigureShadows(shadows,depth);
    if(result&&environment)result=commands.ConfigureEnvironment(*environment);
    if(result&&post){if(!postPipelines)return {rhi::Error::InvalidArgument};result=commands.ConfigurePostProcess(*post,*postPipelines);}
    if(result)result=commands.BeginPass({rhi::PassKind::Clear,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,"Clear"});
    if(result)result=commands.EndPass();
    if(result)result=commands.BeginPass({rhi::PassKind::Shadow,rhi::ResourceUse::None,rhi::ResourceUse::DepthWrite,"Shadow"});
    if(result&&shadows.enabled)for(const auto& draw:casters)if(draw.castsShadow)
    {result=commands.DrawShadow(draw.vertices,draw.indices,draw.world);if(!result)break;}
    if(result)result=commands.EndPass();
    if(result)result=commands.BeginPass({rhi::PassKind::Sky,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,"Sky"});
    if(result)result=commands.EndPass();
    if(result)result=commands.BeginPass({rhi::PassKind::Opaque,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,"Opaque",rhi::ResourceUse::DepthRead,cacheBindings});
    if(result)for(const auto& draw:visible){result=commands.Draw(draw);if(!result)break;}
    if(result)result=commands.EndPass();
    if(result&&post)result=RecordPostProcessPasses(commands,*post,overlays);
    if(result)result=commands.BeginPass({rhi::PassKind::Output,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,"Output",rhi::ResourceUse::ColorRead});
    if(result)result=commands.EndPass();if(result)result=commands.Close();return result;
}
namespace
{
// G13：执行体编号只在本文件内解释；Graph 本身只保存数字。
enum class GraphBody : std::uint32_t
{
    Clear=1,Shadow,Sky,Opaque,BloomExtract,BloomHorizontal,BloomVertical,ToneMap,Fxaa,Ui,Output
};
std::uint32_t AddGraphPass(FrameGraph& graph,std::string_view name,rhi::PassKind kind,
    rhi::ResourceUse color,rhi::ResourceUse depthUse,rhi::ResourceUse input,bool cacheBindings,
    bool sideEffect,GraphBody body)
{
    return graph.AddPass({name,kind,color,depthUse,input,cacheBindings,sideEffect,static_cast<std::uint32_t>(body)});
}
rhi::Error GraphFailure(FrameGraphStatus status)
{
    return status==FrameGraphStatus::CapacityExceeded?rhi::Error::CapacityExceeded:rhi::Error::InvalidArgument;
}
template<class ShadowBody,class OpaqueBody,class UiBody>
rhi::Result ExecuteFrameGraph(rhi::CommandContext& commands,FrameGraph& graph,
    ShadowBody shadowBody,OpaqueBody opaqueBody,UiBody uiBody)
{
    rhi::Result result;
    for(const auto& pass:graph.Passes())
    {
        result=commands.BeginPass({pass.kind,pass.color,pass.depth,std::string(pass.name),pass.input,pass.cacheBindings});
        if(!result)break;
        switch(static_cast<GraphBody>(pass.execute))
        {
        case GraphBody::Shadow: result=shadowBody(); break;
        case GraphBody::Opaque: result=opaqueBody(); break;
        case GraphBody::Ui: result=uiBody(); break;
        default: break;
        }
        if(result)result=commands.EndPass();
        if(!result)break;
    }
    if(result)result=commands.Close();
    return result;
}
}
rhi::Result RecordForwardFrameGraph(rhi::CommandContext& commands,const rhi::ForwardLighting& lighting,
    rhi::PipelineHandle output,rhi::PipelineHandle sky,std::span<const rhi::DrawIndexed> draws,bool cacheBindings,
    FrameGraph& graph,std::uint32_t width,std::uint32_t height)
{
    auto result=commands.ConfigureForward(lighting,output,sky);
    if(!result)return result;
    graph.Reset();
    width=(std::max)(1u,width);height=(std::max)(1u,height);
    const auto sceneColor=graph.AddTexture("SceneColor",{width,height,1,rhi::TextureFormat::Rgba16Float},true);
    const auto sceneDepth=graph.AddTexture("SceneDepth",{width,height,1,rhi::TextureFormat::Rgba8,true},true);
    const auto backbuffer=graph.AddTexture("Backbuffer",{width,height,1,rhi::TextureFormat::Rgba8},true);
    const auto clear=AddGraphPass(graph,"Clear",rhi::PassKind::Clear,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,
        rhi::ResourceUse::None,false,false,GraphBody::Clear);
    graph.Write(clear,sceneColor);graph.Write(clear,sceneDepth);
    const auto skyPass=AddGraphPass(graph,"Sky",rhi::PassKind::Sky,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,
        rhi::ResourceUse::None,false,false,GraphBody::Sky);
    graph.Read(skyPass,sceneColor);graph.Write(skyPass,sceneColor);
    const auto opaque=AddGraphPass(graph,"Opaque",rhi::PassKind::Opaque,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,
        rhi::ResourceUse::None,cacheBindings,false,GraphBody::Opaque);
    graph.Read(opaque,sceneColor);graph.Read(opaque,sceneDepth);
    graph.Write(opaque,sceneColor);graph.Write(opaque,sceneDepth);
    const auto outputPass=AddGraphPass(graph,"Output",rhi::PassKind::Output,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,
        rhi::ResourceUse::ColorRead,false,true,GraphBody::Output);
    graph.Read(outputPass,sceneColor);graph.Write(outputPass,backbuffer);
    const auto status=graph.Compile();
    if(status!=FrameGraphStatus::Success)return {GraphFailure(status)};
    return ExecuteFrameGraph(commands,graph,[]{return rhi::Result{};},
        [&]{rhi::Result record;for(const auto& draw:draws){record=commands.Draw(draw);if(!record)break;}return record;},
        []{return rhi::Result{};});
}
rhi::Result RecordShadowedFrameGraph(rhi::CommandContext& commands,const rhi::ForwardLighting& lighting,
    const rhi::ShadowSettings& shadows,rhi::PipelineHandle depth,rhi::PipelineHandle output,rhi::PipelineHandle sky,
    std::span<const rhi::DrawIndexed> visible,std::span<const rhi::DrawIndexed> casters,const rhi::EnvironmentBindings* environment,
    const rhi::PostProcessSettings* post,const rhi::PostProcessPipelines* postPipelines,std::span<const rhi::OverlayQuad> overlays,
    bool cacheBindings,FrameGraph& graph,std::uint32_t width,std::uint32_t height)
{
    auto result=commands.ConfigureForward(lighting,output,sky);
    if(result)result=commands.ConfigureShadows(shadows,depth);
    if(result&&environment)result=commands.ConfigureEnvironment(*environment);
    if(result&&post){if(!postPipelines)return {rhi::Error::InvalidArgument};result=commands.ConfigurePostProcess(*post,*postPipelines);}
    if(!result)return result;
    graph.Reset();
    width=(std::max)(1u,width);height=(std::max)(1u,height);
    const auto shadowResolution=(std::max)(1u,shadows.resolution);
    const auto sceneColor=graph.AddTexture("SceneColor",{width,height,1,rhi::TextureFormat::Rgba16Float},true);
    const auto sceneDepth=graph.AddTexture("SceneDepth",{width,height,1,rhi::TextureFormat::Rgba8,true},true);
    const auto shadowDepth=graph.AddTexture("ShadowDepth",{shadowResolution,shadowResolution,1,rhi::TextureFormat::Rgba8,true},true);
    const auto backbuffer=graph.AddTexture("Backbuffer",{width,height,1,rhi::TextureFormat::Rgba8},true);
    const auto clear=AddGraphPass(graph,"Clear",rhi::PassKind::Clear,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,
        rhi::ResourceUse::None,false,false,GraphBody::Clear);
    graph.Write(clear,sceneColor);graph.Write(clear,sceneDepth);
    const auto shadowPass=AddGraphPass(graph,"Shadow",rhi::PassKind::Shadow,rhi::ResourceUse::None,rhi::ResourceUse::DepthWrite,
        rhi::ResourceUse::None,false,false,GraphBody::Shadow);
    graph.Write(shadowPass,shadowDepth);
    const auto skyPass=AddGraphPass(graph,"Sky",rhi::PassKind::Sky,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,
        rhi::ResourceUse::None,false,false,GraphBody::Sky);
    graph.Read(skyPass,sceneColor);graph.Write(skyPass,sceneColor);
    const auto opaque=AddGraphPass(graph,"Opaque",rhi::PassKind::Opaque,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,
        rhi::ResourceUse::DepthRead,cacheBindings,false,GraphBody::Opaque);
    graph.Read(opaque,sceneColor);graph.Read(opaque,sceneDepth);graph.Read(opaque,shadowDepth);
    graph.Write(opaque,sceneColor);graph.Write(opaque,sceneDepth);
    std::uint32_t ldr=FrameGraphInvalid,bloomA=FrameGraphInvalid;
    if(post)
    {
        ldr=graph.AddTexture("LdrColor",{width,height,1,rhi::TextureFormat::Rgba8});
        const auto bloomWidth=(std::max)(1u,(width+1)/2),bloomHeight=(std::max)(1u,(height+1)/2);
        const auto bloomB=post->bloom?graph.AddTexture("BloomB",{bloomWidth,bloomHeight,1,rhi::TextureFormat::Rgba16Float}):FrameGraphInvalid;
        if(post->bloom)bloomA=graph.AddTexture("BloomA",{bloomWidth,bloomHeight,1,rhi::TextureFormat::Rgba16Float});
        if(post->bloom)
        {
            const auto extract=AddGraphPass(graph,"BloomExtract",rhi::PassKind::BloomExtract,rhi::ResourceUse::ColorWrite,
                rhi::ResourceUse::None,rhi::ResourceUse::ColorRead,false,false,GraphBody::BloomExtract);
            graph.Read(extract,sceneColor);graph.Write(extract,bloomA);
            const auto horizontal=AddGraphPass(graph,"BloomHorizontal",rhi::PassKind::BloomHorizontal,
                rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,rhi::ResourceUse::ColorRead,false,false,GraphBody::BloomHorizontal);
            graph.Read(horizontal,bloomA);graph.Write(horizontal,bloomB);
            const auto vertical=AddGraphPass(graph,"BloomVertical",rhi::PassKind::BloomVertical,
                rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,rhi::ResourceUse::ColorRead,false,false,GraphBody::BloomVertical);
            graph.Read(vertical,bloomB);graph.Write(vertical,bloomA);
        }
        const auto toneMap=AddGraphPass(graph,"ToneMap",rhi::PassKind::ToneMap,rhi::ResourceUse::ColorWrite,
            rhi::ResourceUse::None,rhi::ResourceUse::ColorRead,false,false,GraphBody::ToneMap);
        graph.Read(toneMap,sceneColor);if(post->bloom)graph.Read(toneMap,bloomA);graph.Write(toneMap,ldr);
        const auto fxaa=AddGraphPass(graph,"FXAA",rhi::PassKind::Fxaa,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,
            rhi::ResourceUse::ColorRead,false,false,GraphBody::Fxaa);
        graph.Read(fxaa,ldr);graph.Write(fxaa,ldr);
        const auto ui=AddGraphPass(graph,"UI",rhi::PassKind::Ui,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,
            rhi::ResourceUse::None,false,false,GraphBody::Ui);
        graph.Read(ui,ldr);graph.Write(ui,ldr);
    }
    const auto outputPass=AddGraphPass(graph,"Output",rhi::PassKind::Output,rhi::ResourceUse::ColorWrite,
        rhi::ResourceUse::None,rhi::ResourceUse::ColorRead,false,true,GraphBody::Output);
    graph.Read(outputPass,post?ldr:sceneColor);graph.Write(outputPass,backbuffer);
    const auto status=graph.Compile();
    if(status!=FrameGraphStatus::Success)return {GraphFailure(status)};
    return ExecuteFrameGraph(commands,graph,
        [&]{rhi::Result record;if(!shadows.enabled)return record;
            for(const auto& draw:casters){if(!draw.castsShadow)continue;record=commands.DrawShadow(draw.vertices,draw.indices,draw.world);if(!record)break;}
            return record;},
        [&]{rhi::Result record;for(const auto& draw:visible){record=commands.Draw(draw);if(!record)break;}return record;},
        [&]{rhi::Result record;for(const auto& quad:overlays){record=commands.DrawOverlay(quad);if(!record)break;}return record;});
}
}

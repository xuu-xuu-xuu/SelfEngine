#include "PostProcessRenderer.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <algorithm>

namespace se
{
bool PostProcessRenderer::Initialize(rhi::Device& device,const std::filesystem::path& shaders,bool acesToneMap)
{
    if(pipelines_[0])return false;std::array<rhi::Pipeline,6> candidate;
    const rhi::PipelineKind kinds[]{rhi::PipelineKind::BloomExtract,rhi::PipelineKind::BloomBlur,rhi::PipelineKind::ToneMap,rhi::PipelineKind::Fxaa,rhi::PipelineKind::UiOverlay};
    const char* names[]{"BloomExtract.hlsl","BloomBlur.hlsl","ToneMap.hlsl","Fxaa.hlsl","UiOverlay.hlsl"};
    for(size_t index=0;index<5;++index)
    {
        rhi::PipelineDescription description{kinds[index],shaders/names[index],rhi::BindingLayout(kinds[index])};
        description.colorFormat=index<2?rhi::ColorFormat::Rgba16Float:index==2?rhi::ColorFormat::Rgba8Unorm:rhi::ColorFormat::Rgba8Srgb;
        if(index==4)description.blend=rhi::BlendMode::Alpha;
        if(!device.CreatePipeline(description,candidate[index]))return false;
    }
    if(acesToneMap)
    {
        rhi::PipelineDescription description{rhi::PipelineKind::ToneMap,shaders/"ToneMap.hlsl",rhi::BindingLayout(rhi::PipelineKind::ToneMap)};
        description.colorFormat=rhi::ColorFormat::Rgba8Unorm;
        description.macros.push_back({"SE_TONEMAP_ACES","1"});
        if(!device.CreatePipeline(description,candidate[5]))return false;
    }
    pipelines_=std::move(candidate);return true;
}
rhi::PostProcessPipelines PostProcessRenderer::Bindings() const
{
    if(!pipelines_[0]||!pipelines_[1]||!pipelines_[2]||!pipelines_[3]||!pipelines_[4])return {};
    return {*pipelines_[0],*pipelines_[1],pipelines_[5]?*pipelines_[5]:*pipelines_[2],*pipelines_[3],*pipelines_[4]};
}
rhi::PostProcessSettings PreparePostProcessSettings(const rhi::PostProcessSettings& source,std::vector<AssetDiagnostic>& diagnostics)
{
    auto settings=source;bool limited=false;
    const auto limit=[&](float value,float maximum,float fallback){if(!IsFinite(value)){limited=true;return fallback;}const auto next=std::clamp(value,0.0f,maximum);limited|=next!=value;return next;};
    settings.bloomThreshold=limit(settings.bloomThreshold,65504,1);settings.bloomStrength=limit(settings.bloomStrength,4,0.08f);
    // G15：曝光/LUT 参数与 CPU 参考保持同一合法域；非法值回退默认且只报告一次。
    settings.exposureDeltaSeconds=limit(settings.exposureDeltaSeconds,0.5f,1.0f/60.0f);
    settings.exposureLowPercentile=std::min(limit(settings.exposureLowPercentile,1,0.60f),1.0f);
    settings.exposureHighPercentile=std::min(limit(settings.exposureHighPercentile,1,0.80f),1.0f);
    settings.exposureBrightAdaptation=limit(settings.exposureBrightAdaptation,256,3.0f);
    settings.exposureDarkAdaptation=limit(settings.exposureDarkAdaptation,256,1.0f);
    if(!IsFinite(settings.exposureMinimum)||settings.exposureMinimum<=0||settings.exposureMinimum>1e6)
    {settings.exposureMinimum=1.0f/1024.0f;limited=true;}
    if(!IsFinite(settings.exposureMaximum)||settings.exposureMaximum<settings.exposureMinimum||settings.exposureMaximum>1e6)
    {settings.exposureMaximum=std::max(settings.exposureMinimum,64.0f);limited=true;}
    if(!IsFinite(settings.exposureKey)||settings.exposureKey<=0||settings.exposureKey>1e6){settings.exposureKey=0.18f;limited=true;}
    for(size_t index=0;index<3;++index)
    {
        if(!IsFinite(settings.lutLift[index])||std::abs(settings.lutLift[index])>1e4){settings.lutLift[index]=0;limited=true;}
        if(!IsFinite(settings.lutGamma[index])||settings.lutGamma[index]<1e-4f||settings.lutGamma[index]>1e4f){settings.lutGamma[index]=1;limited=true;}
        if(!IsFinite(settings.lutGain[index])||std::abs(settings.lutGain[index])>1e4){settings.lutGain[index]=1;limited=true;}
    }
    // G14：TAA 参数；矩阵非法时关闭 TAA 而不是产生 NaN 速度。
    if(!IsFinite(settings.taaCurrentClipFromWorld)||!IsFinite(settings.taaPreviousClipFromWorld)){settings.taa=false;limited=true;}
    settings.taaStaticBlend=limit(settings.taaStaticBlend,1,0.1f);settings.taaDynamicBlend=limit(settings.taaDynamicBlend,1,0.2f);
    if(!IsFinite(settings.taaDepthThreshold)||settings.taaDepthThreshold<=0||settings.taaDepthThreshold>1){settings.taaDepthThreshold=0.05f;limited=true;}
    if(!IsFinite(settings.taaVelocityThreshold)||settings.taaVelocityThreshold<=0||settings.taaVelocityThreshold>1){settings.taaVelocityThreshold=0.01f;limited=true;}
    if(!IsFinite(settings.taaSharpen)||settings.taaSharpen<0||settings.taaSharpen>2){settings.taaSharpen=0;limited=true;}
    if(limited)asset_io::Diagnostic(diagnostics,"Rendering.PostProcessClamped",{},true);return settings;
}
std::vector<rhi::OverlayQuad> PreparePostProcessOverlays(std::span<const UiVisual> visuals,std::vector<AssetDiagnostic>& diagnostics)
{
    std::vector<rhi::OverlayQuad> output;
    for(const auto& visual:visuals)
    {
        if(visual.kind!=UiKind::Panel)continue;
        const auto& r=visual.rectangle;const auto& c=visual.clip;const auto& color=visual.color;
        rhi::OverlayQuad quad{{r.x,r.y,r.width,r.height},{color.r,color.g,color.b,color.a*visual.opacity},{c.x,c.y,c.width,c.height}};
        bool valid=IsFinite(quad.rectangle)&&IsFinite(quad.color)&&IsFinite(quad.clip)&&r.width>=0&&r.height>=0&&c.width>=0&&c.height>=0;
        for(float value:{r.x,r.y,r.width,r.height,c.x,c.y,c.width,c.height})valid&=std::abs(value)<=1e6;
        if(!valid){asset_io::Diagnostic(diagnostics,"Rendering.OverlayInvalid",std::to_string(visual.id),true);continue;}
        quad.color={std::clamp(quad.color.x,0.0f,1.0f),std::clamp(quad.color.y,0.0f,1.0f),std::clamp(quad.color.z,0.0f,1.0f),std::clamp(quad.color.w,0.0f,1.0f)};
        output.push_back(quad);
    }
    return output;
}
rhi::Result RecordPostProcessPasses(rhi::CommandContext& commands,const rhi::PostProcessSettings& settings,std::span<const rhi::OverlayQuad> overlays)
{
    rhi::Result result;const rhi::PassKind passes[]{rhi::PassKind::BloomExtract,rhi::PassKind::BloomHorizontal,rhi::PassKind::BloomVertical,rhi::PassKind::ToneMap,rhi::PassKind::Fxaa,rhi::PassKind::Ui};
    const char* names[]{"BloomExtract","BloomHorizontal","BloomVertical","ToneMap","FXAA","UI"};
    for(size_t index=0;index<std::size(passes);++index)
    {
        if(index<3&&!settings.bloom)continue;
        result=commands.BeginPass({passes[index],rhi::ResourceUse::ColorWrite,rhi::ResourceUse::None,names[index],index==5?rhi::ResourceUse::None:rhi::ResourceUse::ColorRead});
        if(result&&index==5)for(const auto& quad:overlays){result=commands.DrawOverlay(quad);if(!result)break;}
        if(result)result=commands.EndPass();if(!result)break;
    }
    return result;
}
}

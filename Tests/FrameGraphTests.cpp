#include "FunctionLayer/Rendering/ForwardRenderer.h"
#include "FunctionLayer/Rendering/PostProcessRenderer.h"
#include "FunctionLayer/Rendering/ShadowSystem.h"
#include "FunctionLayer/Rendering/StaticModelRender.h"
#include "FunctionLayer/Rendering/ColorGrading.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Diagnostics.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "CoreLayer/Localization.h"
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <algorithm>
#include <cmath>

using namespace se;
using Microsoft::WRL::ComPtr;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Ok(rhi::Result result,const char* message){if(!result)throw std::runtime_error(std::string(message)+": "+result.Key());}
struct Captured{rhi::Image image;rhi::HdrImage hdr;};
struct Fixture
{
    WindowState window;std::shared_ptr<rhi::Device> device;
    explicit Fixture(const Localization& text)
    {
        Check(CreateEngineWindow(window,false),"frame graph hidden window");
        SetSize(320,240);
        rhi::Result result;device=rhi::Device::CreateD3D12({window.hwnd,320,240},text,result);Ok(result,"frame graph device");
    }
    void SetSize(std::uint32_t width,std::uint32_t height)
    {
        RECT rect{0,0,LONG(width),LONG(height)};
        AdjustWindowRectEx(&rect,static_cast<DWORD>(GetWindowLongPtrW(window.hwnd,GWL_STYLE)),FALSE,0);
        Check(SetWindowPos(window.hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"frame graph client size");
        int code=0;Check(PumpWindowMessages(code),"frame graph window messages");
    }
    ~Fixture(){if(device)device->Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
};
void Gpu(const Localization& text,const std::filesystem::path& output)
{
    Fixture fixture(text);auto& device=*fixture.device;
    rhi::Pipeline mesh,depth,final,sky;PostProcessRenderer post;
    Check(post.Initialize(device,GetExecutableDirectory()/"Resources/Shaders"),"frame graph post PSOs");
    const auto postPipelines=post.Bindings();
    rhi::PipelineDescription description{rhi::PipelineKind::ShadowedMesh,GetExecutableDirectory()/"Resources/Shaders/ShadowedForward.hlsl",4};
    description.colorFormat=rhi::ColorFormat::Rgba16Float;Ok(device.CreatePipeline(description,mesh),"frame graph HDR PSO");
    Ok(device.CreatePipeline({rhi::PipelineKind::ShadowDepth,GetExecutableDirectory()/"Resources/Shaders/ShadowDepth.hlsl",4},depth),"frame graph depth PSO");
    Ok(device.CreatePipeline({rhi::PipelineKind::ShadowOutput,GetExecutableDirectory()/"Resources/Shaders/ShadowOutput.hlsl",4},final),"frame graph reference output");
    description={rhi::PipelineKind::Sky,GetExecutableDirectory()/"Resources/Shaders/Sky.hlsl",9};description.colorFormat=rhi::ColorFormat::Rgba16Float;
    Ok(device.CreatePipeline(description,sky),"frame graph sky PSO");
    rhi::Pipeline forwardMesh,forwardOutput;
    description={rhi::PipelineKind::ForwardMesh,GetExecutableDirectory()/"Resources/Shaders/Forward.hlsl",3};description.colorFormat=rhi::ColorFormat::Rgba16Float;
    Ok(device.CreatePipeline(description,forwardMesh),"frame graph forward PSO");
    Ok(device.CreatePipeline({rhi::PipelineKind::FinalOutput,GetExecutableDirectory()/"Resources/Shaders/FinalOutput.hlsl",3},forwardOutput),"frame graph forward output PSO");
    StaticModelRenderCache models{true};auto source=std::make_shared<StaticModel>();source->meshes.push_back(MakeUnitCube());
    source->meshes[0].material=0;source->materials.emplace_back();source->materials[0].baseColor={0.8f,0.7f,0.5f,1};
    ModelNode node;node.name="Panel";node.meshes={0};source->nodes.push_back(node);
    std::vector<AssetDiagnostic> diagnostics;std::shared_ptr<const StaticRenderModel> cube;
    Check(models.Upload(source,{},std::filesystem::current_path(),device,cube,diagnostics),"frame graph geometry");
    std::vector<rhi::DrawIndexed> draws;
    AppendPbrModelDraws(*cube,Rotation(AxisAngle({0,0,1},0.32f))*Scaling({1.6f,1,0.01f}),{},draws,*mesh,
        *OrthographicRH(4,3,0.1f,100)*Translation({0,0,-3}),{0,0,3},{0,0,1},{});
    std::vector<rhi::DrawIndexed> forwardDraws;
    AppendPbrModelDraws(*cube,Rotation(AxisAngle({0,0,1},0.32f))*Scaling({1.6f,1,0.01f}),{},forwardDraws,*forwardMesh,
        *OrthographicRH(4,3,0.1f,100)*Translation({0,0,-3}),{0,0,3},{0,0,1},{});
    rhi::ForwardLighting lighting;lighting.directionalEnabled=true;lighting.directionalRadiance={24,24,24};lighting.sky={0.02f,0.03f,0.05f};
    DirectionalShadowView shadowView;shadowView.enabled=false;shadowView.resolution=256;rhi::ShadowSettings shadows;
    Check(PrepareDirectionalShadow(shadowView,lighting,shadows,diagnostics),"frame graph shadow prefix");
    rhi::PostProcessSettings settings;settings.fxaa=true;std::vector<rhi::OverlayQuad> overlays;
    std::uint64_t frame=0;FrameGraph graph;
    const auto record=[&](bool throughGraph,std::uint32_t width,std::uint32_t height){
        auto commands=device.BeginCommands(++frame);
        if(throughGraph)Ok(RecordShadowedFrameGraph(*commands,lighting,shadows,*depth,*final,*sky,draws,draws,nullptr,
            &settings,&postPipelines,overlays,false,graph,width,height),"frame graph pass order");
        else Ok(RecordShadowedFrame(*commands,lighting,shadows,*depth,*final,*sky,draws,draws,nullptr,
            &settings,&postPipelines,overlays),"hand-written pass order");
        return commands;
    };
    const auto capture=[&](bool throughGraph,std::uint32_t width,std::uint32_t height){
        auto commands=record(throughGraph,width,height);Captured result;rhi::CompletionToken token;
        Ok(device.Submit(*commands,token,&result.image,&result.hdr),"frame graph capture");return result;
    };
    const auto compare=[&](const char* message)
    {
        const auto direct=capture(false,320,240),viaGraph=capture(true,320,240);
        Check(direct.hdr.rgba32==viaGraph.hdr.rgba32&&direct.image.rgba8==viaGraph.image.rgba8,message);
    };
    // Bloom 开启：11 个 Pass（Clear/Shadow/Sky/Opaque/Extract/H/V/Tone/FXAA/UI/Output）。
    settings.bloom=true;
    const auto firstFrame=capture(true,320,240);
    const auto directAfterFirst=capture(false,320,240);
    Check(firstFrame.hdr.rgba32==directAfterFirst.hdr.rgba32&&firstFrame.image.rgba8==directAfterFirst.image.rgba8,
        "frame graph first frame after device creation matches hand-written pixels");
    const auto graphAfterFirst=capture(true,320,240);
    Check(directAfterFirst.hdr.rgba32==graphAfterFirst.hdr.rgba32&&directAfterFirst.image.rgba8==graphAfterFirst.image.rgba8,
        "frame graph with Bloom matches hand-written order byte-for-byte");
    Check(graph.Stats().executedPasses==11&&graph.Stats().aliasGroups==3&&graph.Stats().culledPasses==0,
        "frame graph reports 11 executed passes and three transient targets");
    const auto bloom=graph.Stats();
    // Bloom 关闭：8 个 Pass，版本链回退验证。
    settings.bloom=false;compare("frame graph without Bloom matches hand-written order byte-for-byte");
    Check(graph.Stats().executedPasses==8,"frame graph drops Bloom passes when disabled");
    settings.bloom=true;
    // UI/Overlay 与曝光不影响图执行顺序。
    overlays={{{8,8,20,16},{0.2f,0.5f,0.8f,1},{0,0,1e6f,1e6f}},{{40,8,20,16},{0.8f,0.2f,0.1f,0.5f},{0,0,1e6f,1e6f}}};
    compare("frame graph UI overlay pass matches hand-written order");
    overlays.clear();
    // resize：图每帧按新尺寸重新声明，无持久资源/悬空引用。
    for(const auto size:{std::pair{257u,193u},std::pair{320u,240u}})
    {
        fixture.SetSize(size.first,size.second);Ok(device.Resize(size.first,size.second),"frame graph resize");
        const auto direct=capture(false,size.first,size.second),viaGraph=capture(true,size.first,size.second);
        Check(direct.hdr.rgba32==viaGraph.hdr.rgba32&&direct.image.rgba8==viaGraph.image.rgba8,
            "frame graph rebuild after resize matches hand-written pixels");
        Check(graph.Stats().executedPasses==11,"resize keeps the full 11-pass graph");
    }
    // DOT/JSON 导出与真实图一致。
    const auto dot=output/"frame.dot",json=output/"frame.json";
    Check(graph.WriteDot(dot)&&graph.WriteJson(json),"frame graph debug export");
    std::ifstream dotStream(dot),jsonStream(json);
    std::string dotText((std::istreambuf_iterator<char>(dotStream)),{}),jsonText((std::istreambuf_iterator<char>(jsonStream)),{});
    Check(dotText.find("digraph FrameGraph")!=std::string::npos&&dotText.find("BloomExtract")!=std::string::npos
        &&jsonText.find("\"BloomA\"")!=std::string::npos&&jsonText.find("\"executedPasses\":11")!=std::string::npos,
        "frame graph debug export contains Bloom passes and resources");
    // 前向（无阴影/后处理）路径：Clear/Sky/Opaque/Output 4 个 Pass 同样逐字节一致。
    const auto captureForward=[&](bool throughGraph){
        auto commands=device.BeginCommands(++frame);
        if(throughGraph)Ok(RecordForwardFrameGraph(*commands,lighting,*forwardOutput,*sky,forwardDraws,false,graph,320,240),"forward graph pass order");
        else Ok(RecordForwardFrame(*commands,lighting,*forwardOutput,*sky,forwardDraws),"hand-written forward pass order");
        Captured result;rhi::CompletionToken token;Ok(device.Submit(*commands,token,&result.image,&result.hdr),"forward capture");return result;};
    const auto forwardDirect=captureForward(false),forwardGraph=captureForward(true);
    Check(forwardDirect.hdr.rgba32==forwardGraph.hdr.rgba32&&forwardDirect.image.rgba8==forwardGraph.image.rgba8,
        "forward frame graph matches hand-written pass order byte-for-byte");
    Check(graph.Stats().executedPasses==4,"frame graph compiles the 4-pass forward chain");
    // G15：ACES fitted Shader 与 CPU 参考逐像素对照；默认 Reinhard 路径保持可选。
    settings.bloom=false;settings.fxaa=false;settings.toneMapping=true;
    rhi::Pipeline acesToneMap;
    rhi::PipelineDescription acesDescription{rhi::PipelineKind::ToneMap,GetExecutableDirectory()/"Resources/Shaders/ToneMap.hlsl",rhi::BindingLayout(rhi::PipelineKind::ToneMap)};
    acesDescription.colorFormat=rhi::ColorFormat::Rgba8Unorm;
    acesDescription.macros.push_back({"SE_TONEMAP_ACES","1"});
    Ok(device.CreatePipeline(acesDescription,acesToneMap),"G15 ACES tone map PSO");
    auto acesPipelines=postPipelines;acesPipelines.toneMap=*acesToneMap;
    const auto capturePipelines=[&](const rhi::PostProcessPipelines& pipelines){
        auto commands=device.BeginCommands(++frame);
        Ok(RecordShadowedFrame(*commands,lighting,shadows,*depth,*final,*sky,draws,draws,nullptr,&settings,&pipelines,overlays),"G15 tone map pass order");
        Captured result;rhi::CompletionToken token;Ok(device.Submit(*commands,token,&result.image,&result.hdr),"G15 tone map capture");return result;};
    const auto acesImage=capturePipelines(acesPipelines);
    const auto reinhardImage=capturePipelines(postPipelines);
    ColorGradingSettings neutral;
    std::size_t checked=0;int maximumDifference=0,changedChannels=0;
    for(std::uint32_t y=0;y<acesImage.image.height;y+=3)for(std::uint32_t x=0;x<acesImage.image.width;x+=3)
    {
        const auto index=(std::size_t(y)*acesImage.image.width+x)*4;
        const Vec3 hdr{acesImage.hdr.rgba32[index],acesImage.hdr.rgba32[index+1],acesImage.hdr.rgba32[index+2]};
        const auto expected=PresentSrgb(hdr,neutral);
        const float channels[3]{expected.x,expected.y,expected.z};
        for(std::size_t channel=0;channel<3;++channel)
        {
            const auto actual=int(acesImage.image.rgba8[index+channel]);
            const auto reference=int(std::lround(std::clamp(channels[channel],0.0f,1.0f)*255.0f));
            maximumDifference=std::max(maximumDifference,std::abs(actual-reference));
            if(std::abs(actual-int(reinhardImage.image.rgba8[index+channel]))>2)++changedChannels;
        }
        ++checked;
    }
    Check(checked>1000&&maximumDifference<=2,"G15 ACES shader matches CPU ACES/sRGB within two LSB");
    Check(changedChannels>100,"G15 ACES is observably different from the default Reinhard path");
    std::ofstream report(output/"validation.json");
    report<<"{\"bloomCompileMs\":"<<bloom.compileMilliseconds<<",\"bloomPasses\":"<<bloom.executedPasses
        <<",\"aliasGroups\":"<<bloom.aliasGroups<<",\"aliasSavedBytes\":"<<bloom.savedBytes
        <<",\"acesMaxLsb\":"<<maximumDifference<<",\"acesChangedChannels\":"<<changedChannels
        <<",\"debugWarnings\":"<<device.Stats().debugWarnings<<",\"debugErrors\":"<<device.Stats().debugErrors<<"}\n";
    Check(bool(report),"frame graph evidence output");
    if(const auto stats=device.Stats();stats.debugWarnings||stats.debugErrors)
    {
        std::cerr<<"frame graph GPU diagnostics: warnings="<<stats.debugWarnings<<" errors="<<stats.debugErrors<<'\n';
        auto* native=rhi::D3D12DiagnosticsAccess::NativeDevice(device);ComPtr<ID3D12InfoQueue> queue;native->QueryInterface(IID_PPV_ARGS(queue.GetAddressOf()));
        for(UINT64 index=0;queue&&index<queue->GetNumStoredMessages();++index)
        {
            SIZE_T bytes=0;queue->GetMessage(index,nullptr,&bytes);std::vector<std::byte> storage(bytes);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if(SUCCEEDED(queue->GetMessage(index,message,&bytes))&&message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING)
                std::cerr<<"D3D12 "<<message->ID<<": "<<message->pDescription<<'\n';
        }
    }
    Check(device.Stats().debugWarnings==0&&device.Stats().debugErrors==0,"frame graph GPU warnings/errors zero");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply frame graph output directory");
        const auto output=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(output);
        Localization text;std::string error;
        Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"frame graph strings");
        Gpu(text,output);
        std::cout<<"FrameGraphTests passed (hand-written vs graph pixels, Bloom/no-Bloom, UI, resize, DOT/JSON, ACES shader oracle, zero GPU diagnostics).\n";
        return 0;
    }
    catch(const std::exception& error){std::cerr<<"FrameGraphTests: "<<error.what()<<'\n';return 1;}
}

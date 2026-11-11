#include "D3D12PipelineCache.h"
#include "D3D12Triangle.h"
#include "D3D12Result.h"
#include "PlatformLayer/Graphics/MaterialLayout.h"
#include "CoreLayer/Hash.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include <algorithm>
#include <thread>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace rhi=se::rhi;
using Microsoft::WRL::ComPtr;
struct D3D12PipelineCache::Impl
{
    ID3D12Device* device;
    const std::thread::id owner=std::this_thread::get_id();
    std::unordered_map<std::string,std::weak_ptr<const D3D12ShaderProgram>> programs;
    std::unordered_map<std::string,std::weak_ptr<const D3D12PreparedPipeline>> pipelines;
    std::vector<std::weak_ptr<const D3D12PreparedPipeline>> livePipelines;
    D3D12PipelineCacheStatistics stats;
    ComPtr<ID3D12Device1> device1;
    ComPtr<ID3D12PipelineLibrary> library;
    std::vector<char> libraryBlob; // CreatePipelineLibrary 期间保活输入缓存。
    std::filesystem::path libraryPath;
    bool libraryDirty=false;
    explicit Impl(ID3D12Device& value):device(&value){}
};
D3D12PipelineCache::D3D12PipelineCache(ID3D12Device& device):impl_(std::make_unique<Impl>(device))
{
#ifndef NDEBUG
    return; // Debug 层与 PSO 库同时启用会产生校验告警；持久库只在 Release 生效。
#endif
    auto& p=*impl_;
    if(FAILED(device.QueryInterface(IID_PPV_ARGS(p.device1.GetAddressOf()))))return;
    std::error_code error;
    const auto directory=std::filesystem::current_path(error)/".cache"/"pso";
    if(error)return;
    std::filesystem::create_directories(directory,error);
    if(error)return;
    const auto luid=device.GetAdapterLuid();
    // G15.09：库文件名带格式版本，布局变更（如 29 号输出 LUT）后提升版本，使旧条目失效。
    // G02.04：根签名序列化哈希已进入 PSO 键；v3 同时让旧 v2 库整体失效，避免历史条目混用。
    static constexpr const char* libraryVersion="v3";
    std::ostringstream name;name<<"library-"<<libraryVersion<<"-"<<std::hex<<luid.HighPart<<"-"<<luid.LowPart<<".bin";
    p.libraryPath=directory/name.str();
    std::vector<char> blob;
    if(std::filesystem::exists(p.libraryPath,error)&&!error)
    {
        std::ifstream input(p.libraryPath,std::ios::binary);
        p.libraryBlob.assign(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
        if(!p.libraryBlob.empty()&&SUCCEEDED(p.device1->CreatePipelineLibrary(p.libraryBlob.data(),p.libraryBlob.size(),IID_PPV_ARGS(p.library.GetAddressOf()))))
            p.stats.libraryLoaded=true;
    }
    if(!p.library)p.device1->CreatePipelineLibrary(nullptr,0,IID_PPV_ARGS(p.library.GetAddressOf()));
}
D3D12PipelineCache::~D3D12PipelineCache()
{
#ifndef NDEBUG
    return;
#endif
    auto& p=*impl_;
    if(!p.library||!p.libraryDirty||p.libraryPath.empty())return;
    try
    {
        const SIZE_T size=p.library->GetSerializedSize();
        if(!size)return;
        std::vector<char> blob(size);
        if(FAILED(p.library->Serialize(blob.data(),blob.size())))return;
        std::ofstream output(p.libraryPath,std::ios::binary|std::ios::trunc);
        output.write(blob.data(),static_cast<std::streamsize>(blob.size()));
    }
    catch(...){}
}
std::shared_ptr<const D3D12PreparedPipeline> D3D12PipelineCache::Prepare(const rhi::PipelineDescription& description,const Localization& text)
{
    auto& p=*impl_;if(p.owner!=std::this_thread::get_id())return {};
    if((description.bindingLayoutVersion!=rhi::BindingLayout(description.kind)
            &&!(rhi::IsOutput(description.kind)&&description.bindingLayoutVersion==rhi::OutputLutLayoutVersion))
        ||!rhi::ValidPipelineKind(description.kind)
        ||(description.shaderInput!=rhi::ShaderInput::Source&&description.shaderInput!=rhi::ShaderInput::Artifact)
        ||(description.depth!=rhi::DepthMode::Default&&description.depth!=rhi::DepthMode::Disabled&&description.depth!=rhi::DepthMode::ReadWriteLess&&description.depth!=rhi::DepthMode::ReadOnlyLessEqual)
        ||(description.blend!=rhi::BlendMode::Opaque&&description.blend!=rhi::BlendMode::Alpha)
        ||(description.colorFormat!=rhi::ColorFormat::Rgba8Unorm&&description.colorFormat!=rhi::ColorFormat::Rgba16Float&&description.colorFormat!=rhi::ColorFormat::Rgba8Srgb)
        ||(description.vertexLayoutVersion!=rhi::VertexLayoutVersionLegacy&&description.vertexLayoutVersion!=rhi::VertexLayoutVersionCompressed))return {};
    std::erase_if(p.programs,[](const auto& item){return item.second.expired();});std::erase_if(p.pipelines,[](const auto& item){return item.second.expired();});
    std::erase_if(p.livePipelines,[](const auto& item){return item.expired();});
    std::shared_ptr<const D3D12ShaderProgram> program;
    if(description.shaderInput==rhi::ShaderInput::Artifact)
    {
        // 每次准备先验证当前文件；缓存不能掩盖被替换/损坏的产物，普通Draw不走这里。
        const auto candidate=LoadD3D12UserPbrPackage(description,p.stats.shaders,text);if(!candidate)return {};
        const auto programKey="artifact:"+candidate->source.key+":"+candidate->bytecodeKey;
        const auto found=p.programs.find(programKey);if(found!=p.programs.end())program=found->second.lock();
        if(program)++p.stats.programHits;
        else {program=candidate;p.programs.insert_or_assign(programKey,program);}
    }
    else
    {
        D3D12ShaderSource source;if(!ReadD3D12ShaderSource(description,source,text))return {};
        const auto foundProgram=p.programs.find(source.key);if(foundProgram!=p.programs.end())program=foundProgram->second.lock();
        if(program)++p.stats.programHits;
        else
        {
            program=LoadD3D12ShaderProgram(std::move(source),p.stats.shaders,text);if(!program)return {};
            p.programs.insert_or_assign(program->source.key,program);
        }
    }
    const bool pbr=rhi::UsesLitLayout(description.kind),forward=rhi::IsForward(description.kind),output=rhi::IsOutput(description.kind),shadow=rhi::IsShadowed(description.kind),depthOnly=description.kind==rhi::PipelineKind::ShadowDepth;
    const bool environment=description.kind==rhi::PipelineKind::EnvironmentMesh,environmentSky=description.kind==rhi::PipelineKind::EnvironmentSky;
    const bool post=rhi::IsPostProcess(description.kind),overlay=description.kind==rhi::PipelineKind::UiOverlay;
    const bool instanced=rhi::IsInstanced(description.kind);
    // G06.10：Forward 实例化同时需要 b1 前向常量与 t7 实例 SRV，根签名扩到 4 个参数。
    const bool forwardInstanced=description.kind==rhi::PipelineKind::PbrForwardInstancedMesh;
    const bool gui=description.kind==rhi::PipelineKind::Gui;
    const bool fullscreen=output||rhi::IsSky(description.kind)||post;
    const bool mesh=pbr||description.kind==rhi::PipelineKind::TexturedMesh;
    const bool compressed=description.vertexLayoutVersion==rhi::VertexLayoutVersionCompressed;
    const auto depth=description.depth==rhi::DepthMode::Default?(mesh||depthOnly?rhi::DepthMode::ReadWriteLess:rhi::DepthMode::Disabled):description.depth;
    // G02.04：先构建根签名描述并序列化，再把序列化字节的 SHA-256 纳入 PSO 键。
    // 只靠 kind/layout/格式无法区分根参数/描述符表/静态采样器不同的技术；旧键会让
    // 后创建者覆盖同键条目，下一次创建者加载到根签名不匹配的 PSO。
    D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=environment?7:shadow?4:description.kind==rhi::PipelineKind::ShadowOutput?2:pbr?3:1;range.OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    // G15：输出 LUT 变体在 t2 追加 strip，根签名表固定扩到 t0..t2 三槽：
    // ShadowOutput 为场景/阴影预览/LUT，FinalOutput 的 t1 不读取但仍占位以保持寄存器连续。
    if(rhi::IsOutput(description.kind)&&description.bindingLayoutVersion==rhi::OutputLutLayoutVersion)
        range.NumDescriptors=3;
    if(post)range.NumDescriptors=description.bindingLayoutVersion==rhi::ToneMapLayoutVersion?4u:2u;
    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType=mesh?D3D12_ROOT_PARAMETER_TYPE_CBV:D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].ShaderVisibility=mesh||environmentSky||overlay?D3D12_SHADER_VISIBILITY_ALL:fullscreen?D3D12_SHADER_VISIBILITY_PIXEL:D3D12_SHADER_VISIBILITY_VERTEX;
    if(!mesh)parameters[0].Constants={0,0,description.kind==rhi::PipelineKind::Sky?40u:post?12u:environmentSky?20u:fullscreen?4u:16u};
    if(gui)parameters[0].Constants={0,0,4};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[1].DescriptorTable={1,&range};parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[2].Descriptor={1,0};parameters[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    if(instanced&&!forwardInstanced){parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[2].Descriptor={7,0};parameters[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;}
    if(forwardInstanced){parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[3].Descriptor={7,0};parameters[3].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;}
    D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxAnisotropy=1;sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.BorderColor=D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samplers[]{sampler,sampler,sampler};
    samplers[1].ShaderRegister=1;samplers[1].Filter=D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;samplers[1].ComparisonFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].AddressU=samplers[1].AddressV=samplers[1].AddressW=D3D12_TEXTURE_ADDRESS_MODE_BORDER;samplers[1].BorderColor=D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[2].ShaderRegister=2;samplers[2].Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[2].AddressU=samplers[2].AddressV=samplers[2].AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    if(post){samplers[0]=samplers[2];samplers[0].ShaderRegister=0;}
    D3D12_ROOT_SIGNATURE_DESC root{};root.NumParameters=forwardInstanced?4u:instanced?3u:overlay?1u:post?2u:forward?3u:mesh||output||environmentSky?2u:1u;root.pParameters=parameters;
    root.NumStaticSamplers=environment?3u:shadow?2u:mesh||environmentSky?1u:0u;root.pStaticSamplers=environmentSky?&samplers[2]:mesh?samplers:nullptr;root.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if(post){root.NumStaticSamplers=description.kind==rhi::PipelineKind::ToneMap||description.kind==rhi::PipelineKind::Fxaa?1u:0u;root.pStaticSamplers=samplers;}
    if(gui){root.NumParameters=2;root.NumStaticSamplers=1;samplers[0]=samplers[2];samplers[0].ShaderRegister=0;root.pStaticSamplers=samplers;}
    ComPtr<ID3DBlob> serialized,diagnostics;
    if(!CheckGraphicsResult(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,serialized.GetAddressOf(),diagnostics.GetAddressOf()),text,"D3D12.SerializeRootSignatureFailed"))return {};
    const auto rootHash=se::Sha256(std::span<const std::uint8_t>(
        static_cast<const std::uint8_t*>(serialized->GetBufferPointer()),serialized->GetBufferSize()));
    if(rootHash.empty())return {};
    const auto key=se::Sha256(program->source.key+":"+program->bytecodeKey+":"+std::to_string(static_cast<int>(description.kind))
        +":"+std::to_string(description.bindingLayoutVersion)+":"+std::to_string(description.vertexLayoutVersion)+":"+std::to_string(static_cast<int>(depth))+":"+std::to_string(static_cast<int>(description.blend))
        +":"+std::to_string(static_cast<int>(description.colorFormat))+":root:"+rootHash+":triangle-list:cull-none:one-sample:d32-if-depth:v1");
    if(key.empty())return {};
    const std::wstring libraryName(key.begin(),key.end());
    if(description.cachePrepared){const auto found=p.pipelines.find(key);if(found!=p.pipelines.end())if(auto cached=found->second.lock()){++p.stats.pipelineHits;return cached;}}
    ++p.stats.pipelineMisses;
    auto prepared=std::make_shared<D3D12PreparedPipeline>();prepared->program=std::move(program);prepared->key=key;
    prepared->depth=depth;prepared->blend=description.blend;prepared->colorFormat=description.colorFormat;
    if(!CheckGraphicsResult(p.device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(prepared->rootSignature.GetAddressOf())),text,"D3D12.CreateRootSignatureFailed"))return {};
    ++p.stats.rootCreations;
    const D3D12_INPUT_ELEMENT_DESC meshInputs[]{
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,offsetof(rhi::Vertex,position),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,offsetof(rhi::Vertex,normal),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,offsetof(rhi::Vertex,uv),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TANGENT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,offsetof(rhi::Vertex,tangent),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    const D3D12_INPUT_ELEMENT_DESC compressedInputs[]{
        {"POSITION",0,DXGI_FORMAT_R16G16B16A16_UNORM,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R8G8_UNORM,0,6,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R16G16_FLOAT,0,8,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TANGENT",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    const D3D12_INPUT_ELEMENT_DESC triangleInputs[]{
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,offsetof(D3D12TriangleVertex,position),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,offsetof(D3D12TriangleVertex,color),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    const D3D12_INPUT_ELEMENT_DESC guiInputs[]{
        {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,offsetof(se::GuiVertex,position),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,offsetof(se::GuiVertex,uv),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,offsetof(se::GuiVertex,color),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=prepared->rootSignature.Get();
    pipeline.VS={prepared->program->vertex->GetBufferPointer(),prepared->program->vertex->GetBufferSize()};
    pipeline.PS={prepared->program->pixel->GetBufferPointer(),prepared->program->pixel->GetBufferSize()};
    if(depthOnly)pipeline.PS={};
    auto& blend=pipeline.BlendState.RenderTarget[0];blend.BlendEnable=description.blend==rhi::BlendMode::Alpha;
    blend.SrcBlend=blend.BlendEnable?D3D12_BLEND_SRC_ALPHA:D3D12_BLEND_ONE;blend.DestBlend=blend.BlendEnable?D3D12_BLEND_INV_SRC_ALPHA:D3D12_BLEND_ZERO;
    blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=blend.BlendEnable?D3D12_BLEND_INV_SRC_ALPHA:D3D12_BLEND_ZERO;
    blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.SampleMask=UINT_MAX;pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pipeline.RasterizerState.DepthClipEnable=TRUE;
    pipeline.RasterizerState.FrontCounterClockwise=pbr?TRUE:FALSE;
    pipeline.DepthStencilState.DepthEnable=depth!=rhi::DepthMode::Disabled;
    pipeline.DepthStencilState.DepthWriteMask=depth==rhi::DepthMode::ReadWriteLess?D3D12_DEPTH_WRITE_MASK_ALL:D3D12_DEPTH_WRITE_MASK_ZERO;
    pipeline.DepthStencilState.DepthFunc=depth==rhi::DepthMode::ReadOnlyLessEqual?D3D12_COMPARISON_FUNC_LESS_EQUAL:depth==rhi::DepthMode::ReadWriteLess?D3D12_COMPARISON_FUNC_LESS:D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline.DepthStencilState.FrontFace=pipeline.DepthStencilState.BackFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
    pipeline.InputLayout=fullscreen?D3D12_INPUT_LAYOUT_DESC{}:depthOnly?(compressed?D3D12_INPUT_LAYOUT_DESC{compressedInputs,1}:D3D12_INPUT_LAYOUT_DESC{meshInputs,1}):mesh?(compressed?D3D12_INPUT_LAYOUT_DESC{compressedInputs,4}:D3D12_INPUT_LAYOUT_DESC{meshInputs,pbr?4u:3u}):D3D12_INPUT_LAYOUT_DESC{triangleInputs,2};pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if(gui)pipeline.InputLayout={guiInputs,3};
    pipeline.NumRenderTargets=depthOnly?0:1;pipeline.RTVFormats[0]=depthOnly?DXGI_FORMAT_UNKNOWN:description.colorFormat==rhi::ColorFormat::Rgba8Unorm?DXGI_FORMAT_R8G8B8A8_UNORM:description.colorFormat==rhi::ColorFormat::Rgba8Srgb?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R16G16B16A16_FLOAT;
    pipeline.DSVFormat=mesh||pipeline.DepthStencilState.DepthEnable?DXGI_FORMAT_D32_FLOAT:DXGI_FORMAT_UNKNOWN;pipeline.SampleDesc={1,0};
    // G02.04：本机 Release 实测受光照/阴影/环境的 HDR 管线库条目经 LoadGraphicsPipeline
    // 加载后，即使 GetCachedBlob 与新建 PSO 完全一致，也会使后续采样错 1 像素。
    // 这三类 kind 跳过库加载并新建 PSO；其余 kind 继续命中持久库。
    const bool libraryLoadAllowed=description.kind!=rhi::PipelineKind::ShadowedMesh
        &&description.kind!=rhi::PipelineKind::EnvironmentMesh
        &&description.kind!=rhi::PipelineKind::EnvironmentSky;
    if(p.library&&libraryLoadAllowed&&!prepared->pipeline
        &&SUCCEEDED(p.library->LoadGraphicsPipeline(libraryName.c_str(),&pipeline,IID_PPV_ARGS(prepared->pipeline.GetAddressOf()))))
        ++p.stats.libraryHits;
    if(!prepared->pipeline)
    {
        if(!CheckGraphicsResult(p.device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(prepared->pipeline.GetAddressOf())),text,"D3D12.CreatePipelineFailed"))return {};
        ++p.stats.pipelineCreations;
        if(p.library&&SUCCEEDED(p.library->StorePipeline(libraryName.c_str(),prepared->pipeline.Get()))){++p.stats.libraryStores;p.libraryDirty=true;}
    }
    prepared->rootSignature->SetName(L"SelfEngine.PreparedRootSignature");prepared->pipeline->SetName(L"SelfEngine.PreparedPipeline");
    p.livePipelines.push_back(prepared);if(description.cachePrepared)p.pipelines.insert_or_assign(key,prepared);return prepared;
}
D3D12PipelineCacheStatistics D3D12PipelineCache::Stats() const
{
    auto result=impl_->stats;
    for(const auto& [_,value]:impl_->programs)if(!value.expired())++result.livePrograms;
    for(const auto& value:impl_->livePipelines)if(!value.expired())++result.livePipelines;return result;
}

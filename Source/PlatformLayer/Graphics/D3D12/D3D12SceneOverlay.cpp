#include "D3D12SceneOverlay.h"
#include "D3D12Presentation.h"
#include "D3D12Shader.h"
#include "D3D12Result.h"
#include "D3D12StateTracker.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace
{
struct LineVertex {se::Vec4 position,color;float hiddenAlpha;};
struct Constants {se::Mat4 clipFromLocal;se::Vec4 options;se::Mat4 worldFromClip;};
static_assert(sizeof(Constants)==36*sizeof(float));
// 齐次空间裁剪，保留透视正确的深度；避免跨近面或超远网格产生巨型三角形。
bool Clip(se::Vec4& a,se::Vec4& b)
{
    const auto distance=[](se::Vec4 p,int plane)
    {
        switch(plane){case 0:return p.w-.0001f;case 1:return p.z;case 2:return p.w-p.z;
        case 3:return p.w+p.x;case 4:return p.w-p.x;case 5:return p.w+p.y;default:return p.w-p.y;}
    };
    for(int plane=0;plane<7;++plane)
    {
        const float x=distance(a,plane),y=distance(b,plane);
        if(x<0&&y<0)return false;
        if((x<0)!=(y<0))
        {
            const float t=x/(x-y);
            const se::Vec4 p{a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t,a.w+(b.w-a.w)*t};
            if(x<0)a=p;else b=p;
        }
    }
    return true;
}
std::vector<LineVertex> Vertices(const D3D12FrameContent& c,UINT width,UINT height)
{
    std::vector<LineVertex> result;result.reserve(c.sceneLines.size()*6);
    for(const auto& line:c.sceneLines)
    {
        auto a=c.viewProjection*se::Vec4{line.a.x,line.a.y,line.a.z,1};
        auto b=c.viewProjection*se::Vec4{line.b.x,line.b.y,line.b.z,1};
        if(!Clip(a,b))continue;
        const float dx=(b.x/b.w-a.x/a.w)*width,dy=(b.y/b.w-a.y/a.w)*height;
        const float length=std::hypot(dx,dy);if(length<.001f)continue;
        const float x=-dy/length*line.width*c.sceneLineScale/width,y=dx/length*line.width*c.sceneLineScale/height;
        const auto vertex=[&](se::Vec4 p,float side)
        {
            p.x+=side*x*p.w;p.y+=side*y*p.w;
            // 微量偏移只解决网格与地面共面，不写入场景深度。
            p.z-=1e-6f*p.w;
            return LineVertex{p,line.color,line.occludedAlpha};
        };
        const auto p=vertex(a,-1),q=vertex(a,1),r=vertex(b,1),s=vertex(b,-1);
        result.insert(result.end(),{p,q,r,p,r,s});
    }
    return result;
}
}
bool D3D12SceneOverlay::Initialize(ID3D12Device& device,const std::filesystem::path& shader,const Localization& text)
{
    if(root_)return true;
    const auto check=[&](HRESULT hr){return CheckGraphicsResult(hr,text,"D3D12.SceneOverlayFailed");};
    D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants={0,0,36};parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable={1,&range};parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rootDescription{2,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> serialized,error;ComPtr<ID3D12RootSignature> root;
    if(!check(D3D12SerializeRootSignature(&rootDescription,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error))
        ||!check(device.CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root))))return false;
    const D3D12_INPUT_ELEMENT_DESC lineInputs[]{
        {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32_FLOAT,0,32,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    std::array<ComPtr<ID3D12PipelineState>,PipelineCount> pipelines;
    for(UINT i=0;i<pipelines.size();++i)
    {
        const bool fullMask=i==MaskFull||i==MaskFullPacked;
        const bool mask=i==MaskVisible||i==MaskVisiblePacked||fullMask,line=i==LinesVisible||i==LinesHidden;
        const bool depthEnabled=i!=Outline&&!fullMask;
        ComPtr<ID3DBlob> vs,ps;
        if(!CompileD3D12Shader(shader,mask?"VSMask":line?"VSLine":"VSOutline","vs_5_1",vs,text,D3DCOMPILE_IEEE_STRICTNESS)
            ||!CompileD3D12Shader(shader,mask?"PSMask":line?"PSLine":i==GroundGrid?"PSGrid":"PSOutline","ps_5_1",ps,text,D3DCOMPILE_IEEE_STRICTNESS))return false;
        const D3D12_INPUT_ELEMENT_DESC position{"POSITION",0,i==MaskVisiblePacked||i==MaskFullPacked?DXGI_FORMAT_R16G16B16A16_UNORM:DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature=root.Get();desc.VS={vs->GetBufferPointer(),vs->GetBufferSize()};desc.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        if(mask)desc.InputLayout={&position,1};else if(line)desc.InputLayout={lineInputs,3};
        desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;desc.NumRenderTargets=1;
        desc.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;desc.DSVFormat=depthEnabled?DXGI_FORMAT_D32_FLOAT:DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc={1,0};desc.SampleMask=UINT_MAX;
        desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;desc.RasterizerState.DepthClipEnable=TRUE;
        if(mask&&!fullMask){desc.RasterizerState.DepthBias=-1;desc.RasterizerState.SlopeScaledDepthBias=-.1f;}
        desc.DepthStencilState.DepthEnable=depthEnabled;desc.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthStencilState.DepthFunc=i==LinesHidden?D3D12_COMPARISON_FUNC_GREATER:D3D12_COMPARISON_FUNC_LESS_EQUAL;
        desc.DepthStencilState.FrontFace=desc.DepthStencilState.BackFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
        auto& blend=desc.BlendState.RenderTarget[0];
        blend.RenderTargetWriteMask=static_cast<UINT8>(mask?(fullMask?D3D12_COLOR_WRITE_ENABLE_RED:D3D12_COLOR_WRITE_ENABLE_GREEN):D3D12_COLOR_WRITE_ENABLE_ALL);
        blend.BlendEnable=!mask;blend.SrcBlend=D3D12_BLEND_SRC_ALPHA;blend.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;blend.BlendOp=D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
        if(!check(device.CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&pipelines[i]))))return false;
    }
    root_=std::move(root);pipelines_=std::move(pipelines);return true;
}
bool D3D12SceneOverlay::Record(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& c,
    D3D12StateRecording& states,const Localization& text)
{
    if(!c.sceneLdr||!c.sceneHdr||!c.sceneHdr->depth||!c.sceneHdr->dsv)return false;
    const UINT width=c.sceneLdr->color.width,height=c.sceneLdr->color.height;
    const bool selected=std::any_of(c.meshes.begin(),c.meshes.end(),[](const auto& d){return d.outline;});
    const auto vertices=Vertices(c,width,height);if(vertices.empty()&&!selected&&!c.sceneGrid)return true;
    ComPtr<ID3D12Device> device;if(FAILED(p.queue->GetDevice(IID_PPV_ARGS(&device)))||!Initialize(*device.Get(),c.sceneOverlayShader,text))return false;
    auto& commands=*p.commandList.Get();
    const auto check=[&](HRESULT hr){return CheckGraphicsResult(hr,text,"D3D12.SceneOverlayFailed");};
    const auto use=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES state){return check(states.Use(commands,*r,state,"SceneOverlay"));};
    const auto depth=c.sceneHdr->dsv->Cpu(),color=c.sceneLdr->rtv->Cpu();
    if(!use(c.sceneHdr->depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE)||!use(c.sceneLdr->color.resource.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET))return false;
    const D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};const D3D12_RECT scissor{0,0,LONG(width),LONG(height)};
    commands.RSSetViewports(1,&viewport);commands.RSSetScissorRects(1,&scissor);
    commands.SetGraphicsRootSignature(root_.Get());commands.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Constants constants;constants.options={0,std::clamp(2*c.sceneLineScale,1.f,8.f),0,0};
    if(selected)
    {
        if(!frame.sceneOverlay)frame.sceneOverlay=std::make_shared<D3D12SceneOverlayFrame>();
        auto& f=*frame.sceneOverlay;
        if(f.mask.color.width!=width||f.mask.color.height!=height)
        {
            D3D12GuiTarget replacement;const se::Vec4 clear{};
            if(!CreateD3D12GuiTarget(p,{width,height,false,false,se::rhi::TextureFormat::Rgba8},replacement,text,&clear))return false;
            f.mask=std::move(replacement);
        }
        const auto maskRtv=f.mask.rtv->Cpu();
        if(!use(f.mask.color.resource.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET))return false;
        const float clear[4]{};commands.ClearRenderTargetView(maskRtv,clear,0,nullptr);
        // 完整投影写 R，不受遮挡影响；实际可见像素写 G，复用场景深度且不写深度。
        // 两遍只写各自通道，遮挡物造成的缺口不会成为模型自身的轮廓。
        for(bool visible:{false,true})
        {
            commands.OMSetRenderTargets(1,&maskRtv,FALSE,visible?&depth:nullptr);
            for(const auto& draw:c.meshes)if(draw.outline)
            {
                if(!use(draw.mesh->vertices.Get(),D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)
                    ||!use(draw.mesh->indices.Get(),D3D12_RESOURCE_STATE_INDEX_BUFFER))return false;
                constants.clipFromLocal=c.viewProjection*draw.world;
                const auto slot=visible?(draw.compressedVertices?MaskVisiblePacked:MaskVisible):(draw.compressedVertices?MaskFullPacked:MaskFull);
                commands.SetPipelineState(pipelines_[slot].Get());commands.SetGraphicsRoot32BitConstants(0,36,&constants,0);
                commands.IASetVertexBuffers(0,1,&draw.mesh->vertexView);commands.IASetIndexBuffer(&draw.mesh->indexView);
                commands.DrawIndexedInstanced(draw.mesh->indexCount,1,0,0,0);
            }
        }
    }
    commands.OMSetRenderTargets(1,&color,FALSE,&depth);
    if(c.sceneGrid)
    {
        const auto inverse=se::Inverse(c.viewProjection);if(!inverse)return false;
        constants.clipFromLocal=c.viewProjection;constants.worldFromClip=*inverse;
        constants.options={c.sceneLineScale,0,1.f/width,1.f/height};
        commands.SetPipelineState(pipelines_[GroundGrid].Get());
        commands.SetGraphicsRoot32BitConstants(0,36,&constants,0);commands.DrawInstanced(3,1,0,0);
        constants.options={0,std::clamp(2*c.sceneLineScale,1.f,8.f),0,0};
    }
    if(!vertices.empty())
    {
        D3D12UploadSlice upload;
        if(!check(frame.uploads.Allocate(vertices.size()*sizeof(LineVertex),16,upload)))return false;
        std::memcpy(upload.cpu,vertices.data(),vertices.size()*sizeof(LineVertex));
        if(!use(upload.resource,D3D12_RESOURCE_STATE_GENERIC_READ))return false;
        const D3D12_VERTEX_BUFFER_VIEW vb{upload.Gpu(),UINT(upload.size),sizeof(LineVertex)};
        commands.IASetVertexBuffers(0,1,&vb);
        for(const auto slot:{LinesHidden,LinesVisible})
        {
            constants.options.x=slot==LinesHidden?1.f:0.f;commands.SetPipelineState(pipelines_[slot].Get());
            commands.SetGraphicsRoot32BitConstants(0,36,&constants,0);commands.DrawInstanced(UINT(vertices.size()),1,0,0);
        }
    }
    if(selected)
    {
        auto& f=*frame.sceneOverlay;
        if(!use(f.mask.color.resource.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))return false;
        const D3D12_CPU_DESCRIPTOR_HANDLE views[]{f.mask.color.views->Cpu(1)};
        if(!check(f.descriptors.Prepare(*device.Get(),views,1)))return false;
        ID3D12DescriptorHeap* heaps[]{f.descriptors.Heap()};commands.SetDescriptorHeaps(1,heaps);
        commands.OMSetRenderTargets(1,&color,FALSE,nullptr);commands.SetPipelineState(pipelines_[Outline].Get());
        commands.SetGraphicsRoot32BitConstants(0,36,&constants,0);commands.SetGraphicsRootDescriptorTable(1,f.descriptors.Draw(0));
        commands.DrawInstanced(3,1,0,0);
    }
    return true;
}

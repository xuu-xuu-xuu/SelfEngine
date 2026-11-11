#include "D3D12Mesh.h"
#include "D3D12Context.h"
#include "D3D12Presentation.h"
#include "D3D12PipelineCache.h"
#include "D3D12Result.h"
#include "D3D12Upload.h"
#include "D3D12Buffer.h"
#include "D3D12Indirect.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

using Microsoft::WRL::ComPtr;
namespace
{
constexpr UINT64 maxBytes=256ull*1024*1024;
using MeshConstants=se::rhi::MeshDrawConstants;

bool ValidGeometry(std::span<const D3D12MeshVertex> vertices,std::span<const std::uint32_t> indices)
{
    if(vertices.empty()||indices.empty()||indices.size()%3||vertices.size_bytes()>maxBytes||indices.size_bytes()>maxBytes)return false;
    for(const auto index:indices)if(index>=vertices.size())return false;
    for(const auto& vertex:vertices)
    {
        for(const auto value:vertex.position)if(!se::IsFinite(value))return false;
        for(const auto value:vertex.normal)if(!se::IsFinite(value))return false;
        for(const auto value:vertex.uv)if(!se::IsFinite(value))return false;
        for(const auto value:vertex.tangent)if(!se::IsFinite(value))return false;
        const float length=se::LengthSquared(se::Vec3{vertex.normal[0],vertex.normal[1],vertex.normal[2]});
        if(!se::IsFinite(length)||length<1e-12f)return false;
    }
    return true;
}
}

bool InitializeD3D12MeshPipeline(D3D12MeshPipeline& output,const D3D12Context& context,
    const std::filesystem::path& shader,const Localization& text)
{
    se::rhi::PipelineDescription description;description.shader=shader;
    return InitializeD3D12MeshPipeline(output,context,description,text);
}
bool InitializeD3D12MeshPipeline(D3D12MeshPipeline& output,const D3D12Context& context,
    const se::rhi::PipelineDescription& description,const Localization& text)
{
    if(output.rootSignature||output.pipeline||output.prepared||!context.device||!context.pipelines||description.shader.empty()
        ||(description.kind!=se::rhi::PipelineKind::TexturedMesh&&!se::rhi::UsesLitLayout(description.kind)))
    {Log(LogLevel::Error,text.GetText("D3D12.MeshInputsInvalid"));return false;}
    const auto prepared=context.pipelines->Prepare(description,text);if(!prepared)return false;
    output.prepared=prepared;output.rootSignature=prepared->rootSignature;output.pipeline=prepared->pipeline;return true;
}

bool InitializeD3D12Mesh(D3D12Mesh& output,const D3D12Context& context,D3D12Presentation& presentation,
    std::span<const D3D12MeshVertex> vertices,std::span<const std::uint32_t> indices,
    D3D12TexturePixels pixels,const Localization& text)
{
    if(output.vertices||output.indices||output.texture||output.textureViews||!ValidGeometry(vertices,indices)
        ||!ValidD3D12TexturePixels(pixels))
    {Log(LogLevel::Error,text.GetText("D3D12.MeshInputsInvalid"));return false;}
    D3D12Texture texture;
    return InitializeD3D12Texture(texture,context,presentation,pixels,text)
        &&InitializeD3D12Mesh(output,context,presentation,vertices,indices,texture,text);
}

bool InitializeD3D12Mesh(D3D12Mesh& output,const D3D12Context& context,D3D12Presentation& p,
    std::span<const D3D12MeshVertex> vertices,std::span<const std::uint32_t> indices,
    const D3D12Texture& texture,const Localization& text)
{
    if(output.vertices||output.indices||output.texture||output.textureViews||!CanUploadD3D12(context,p)
        ||!ValidGeometry(vertices,indices)||!texture.resource||!texture.views||!texture.width||!texture.height
        ||texture.cube||texture.arrayLayers!=1||texture.format!=se::rhi::TextureFormat::Rgba8
        ||texture.views->Count()!=2||texture.views->Device()!=context.device.Get())
    {Log(LogLevel::Error,text.GetText("D3D12.MeshInputsInvalid"));return false;}
    ComPtr<ID3D12Device> textureDevice;
    if(FAILED(texture.resource->GetDevice(IID_PPV_ARGS(textureDevice.GetAddressOf())))||textureDevice.Get()!=context.device.Get())
    {Log(LogLevel::Error,text.GetText("D3D12.MeshInputsInvalid"));return false;}
    D3D12Mesh candidate;
    D3D12UploadOperation operation(p,text);if(!operation)return false;
    if(!UploadD3D12Buffer(candidate.vertices,context,p,std::as_bytes(vertices),D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,text)
        ||!UploadD3D12Buffer(candidate.indices,context,p,std::as_bytes(indices),D3D12_RESOURCE_STATE_INDEX_BUFFER,text)
        ||!operation.Finish())return false;
    candidate.vertexView={candidate.vertices->GetGPUVirtualAddress(),static_cast<UINT>(vertices.size_bytes()),sizeof(D3D12MeshVertex)};
    candidate.indexView={candidate.indices->GetGPUVirtualAddress(),static_cast<UINT>(indices.size_bytes()),DXGI_FORMAT_R32_UINT};
    candidate.indexCount=static_cast<UINT>(indices.size());
    candidate.texture=texture.resource;candidate.textureViews=texture.views;
    candidate.textureWidth=texture.width;candidate.textureHeight=texture.height;
    output=std::move(candidate);
    return true;
}

bool ValidateD3D12MeshDraws(const D3D12MeshPipeline& pipeline,std::span<const D3D12MeshDraw> draws,
    const se::Mat4& viewProjection,const Localization& text,bool allowEmptyInstanced)
{
    const bool defaultForward=se::rhi::IsForward(pipeline.prepared?pipeline.prepared->program->source.kind:se::rhi::PipelineKind::TexturedMesh);
    if(!pipeline.rootSignature||!pipeline.pipeline||!pipeline.prepared||pipeline.prepared->colorFormat!=(defaultForward?se::rhi::ColorFormat::Rgba16Float:se::rhi::ColorFormat::Rgba8Unorm)
        ||draws.size()>D3D12MeshMaxDraws||!se::IsFinite(viewProjection))
    {Log(LogLevel::Error,text.GetText("D3D12.MeshDrawsInvalid"));return false;}
    // 同批绘制必须同 pass 类别；Pipeline 可以按材质不同，但类别、颜色格式与实例化模式必须一致。
    const auto defaultKind=pipeline.prepared->program->source.kind;
    const bool defaultShadowed=se::rhi::IsShadowed(defaultKind);
    const bool defaultEnvironment=defaultKind==se::rhi::PipelineKind::EnvironmentMesh;
    size_t instanceCount=0;
    for(const auto& draw:draws)
    {
        const auto& active=draw.pipeline?*draw.pipeline:pipeline;
        if(!active.rootSignature||!active.pipeline||!active.prepared
            ||se::rhi::IsForward(active.prepared->program->source.kind)!=defaultForward
            ||se::rhi::IsShadowed(active.prepared->program->source.kind)!=defaultShadowed
            ||(active.prepared->program->source.kind==se::rhi::PipelineKind::EnvironmentMesh)!=defaultEnvironment
            ||active.prepared->colorFormat!=(defaultForward?se::rhi::ColorFormat::Rgba16Float:se::rhi::ColorFormat::Rgba8Unorm))
        {Log(LogLevel::Error,text.GetText("D3D12.MeshDrawsInvalid"));return false;}
        const bool instanced=se::rhi::IsInstanced(active.prepared->program->source.kind);
        const bool instanceDraw=!draw.instances.empty()||(allowEmptyInstanced&&draws.size()==1&&instanced);
        if(instanced!=instanceDraw||draw.instances.size()>se::rhi::MaximumFrameInstances||instanceCount>se::rhi::MaximumFrameInstances-draw.instances.size())
        {Log(LogLevel::Error,text.GetText("D3D12.MeshDrawsInvalid"));return false;}
        instanceCount+=draw.instances.size();
        for(const auto& instance:draw.instances)
        {
            const auto& world=instance.world;const auto color=instance.tint;
            if(!se::IsFinite(world)||!se::Inverse(world)||!se::IsFinite(viewProjection*world)||!se::IsFinite(color)
                ||std::abs(world(3,0))>1e-5f||std::abs(world(3,1))>1e-5f||std::abs(world(3,2))>1e-5f||std::abs(world(3,3)-1)>1e-5f
                ||color.x<0||color.y<0||color.z<0||color.w<0||color.x>1||color.y>1||color.z>1||color.w>1
                ||!std::isfinite(instance.metallic)||instance.metallic<0||instance.metallic>1||!std::isfinite(instance.roughness)||instance.roughness<0||instance.roughness>1
                ||!std::isfinite(instance.normalScale)||instance.normalScale<-8||instance.normalScale>8)
            {Log(LogLevel::Error,text.GetText("D3D12.MeshDrawsInvalid"));return false;}
        }
        const auto* mesh=draw.mesh;
        const auto inverse=se::IsFinite(draw.world)?se::Inverse(draw.world):std::nullopt;
        const auto color=draw.tint;
        if(!mesh||!mesh->vertices||!mesh->indices||!mesh->texture||!mesh->textureViews||mesh->textureViews->Count()!=2||!mesh->indexCount
            ||!inverse||!se::IsFinite(viewProjection*draw.world)||!se::IsFinite(color)
            ||std::abs(draw.world(3,0))>1e-5f||std::abs(draw.world(3,1))>1e-5f
            ||std::abs(draw.world(3,2))>1e-5f||std::abs(draw.world(3,3)-1)>1e-5f
            ||color.x<0||color.y<0||color.z<0||color.w<0||color.x>1||color.y>1||color.z>1||color.w>1)
        {Log(LogLevel::Error,text.GetText("D3D12.MeshDrawsInvalid"));return false;}
        if(se::rhi::UsesLitLayout(active.prepared->program->source.kind))
        {
            const auto& p=draw.pbr;const auto length=se::LengthSquared(p.directionToLight);
            const auto validTexture=[](const D3D12Texture& t){return t.resource&&t.views&&t.views->Count()==2&&!t.cube&&t.arrayLayers==1
                &&(t.format==se::rhi::TextureFormat::Rgba8||t.format==se::rhi::TextureFormat::Bc1Unorm
                    ||t.format==se::rhi::TextureFormat::Bc1UnormSrgb||t.format==se::rhi::TextureFormat::Bc5Unorm
                    ||t.format==se::rhi::TextureFormat::Bc4Unorm);};
            if(draw.textureIsData||!validTexture(mesh->pbrTextures[0])||!validTexture(mesh->pbrTextures[1])
                ||!se::IsFinite(p.cameraPosition)||!se::IsFinite(p.directionToLight)||!se::IsFinite(length)||length<1e-12f||!se::IsFinite(p.radiance)
                ||p.radiance.x<0||p.radiance.y<0||p.radiance.z<0||p.radiance.x>10000||p.radiance.y>10000||p.radiance.z>10000
                ||!std::isfinite(p.metallic)||p.metallic<0||p.metallic>1||!std::isfinite(p.roughness)||p.roughness<0||p.roughness>1
                ||!std::isfinite(p.normalScale)||p.normalScale<-8||p.normalScale>8)
            {Log(LogLevel::Error,text.GetText("D3D12.MeshDrawsInvalid"));return false;}
        }
    }
    return true;
}

void RecordD3D12Meshes(const D3D12MeshPipeline& pipeline,std::span<const D3D12MeshDraw> draws,
    const se::Mat4& viewProjection,D3D12FrameContext& frame,ID3D12GraphicsCommandList& commands,UINT width,UINT height,
    bool cacheBindings,se::rhi::DrawBindingStatistics* statistics,std::span<const D3D12IndirectCommand> indirect)
{
    se::rhi::DrawBindingStatistics binds;
    const D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};
    const D3D12_RECT scissor{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
    const D3D12MeshPipeline* activePipeline=nullptr;
    const auto bindPipeline=[&](const D3D12MeshPipeline& program)
    {
        activePipeline=&program;
        commands.SetPipelineState(program.pipeline.Get());++binds.pipelines;
        commands.SetGraphicsRootSignature(program.rootSignature.Get());++binds.rootSignatures;
        if(se::rhi::IsForward(program.prepared->program->source.kind))
        {commands.SetGraphicsRootConstantBufferView(2,frame.forwardConstants.Gpu());++binds.constants;}
        commands.RSSetViewports(1,&viewport);commands.RSSetScissorRects(1,&scissor);
        commands.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    };
    bindPipeline(pipeline);
    if(!draws.empty()){ID3D12DescriptorHeap* heaps[]{frame.descriptors.Heap()};commands.SetDescriptorHeaps(1,heaps);}
    size_t index=0;bool firstBinding=true;D3D12_GPU_DESCRIPTOR_HANDLE lastTable{};D3D12_VERTEX_BUFFER_VIEW lastVertex{};D3D12_INDEX_BUFFER_VIEW lastIndex{};
    for(const auto& draw:draws)
    {
        // 同帧可混合默认/用户 PBR：只有 Pipeline 变化时重设 PSO 与根签名，并强制重绑根参数。
        const auto& program=draw.pipeline?*draw.pipeline:pipeline;
        if(activePipeline!=&program){bindPipeline(program);firstBinding=true;lastTable={};lastVertex={};lastIndex={};}
        const auto& slice=frame.drawConstants[index++];
        std::memset(slice.cpu,0,static_cast<size_t>(slice.size));
        if(se::rhi::UsesLitLayout(program.prepared->program->source.kind))
        {
            const auto& p=draw.pbr;
            const se::Vec3 x{draw.world(0,0),draw.world(1,0),draw.world(2,0)},y{draw.world(0,1),draw.world(1,1),draw.world(2,1)},z{draw.world(0,2),draw.world(1,2),draw.world(2,2)};
            const float sign=se::Dot(se::Cross(x,y),z)<0?-1.0f:1.0f;
            const se::rhi::PbrDrawConstants constants{viewProjection*draw.world,draw.world,se::Transpose(*se::Inverse(draw.world)),draw.tint,
                {p.cameraPosition.x,p.cameraPosition.y,p.cameraPosition.z,1},{p.directionToLight.x,p.directionToLight.y,p.directionToLight.z,0},
                {p.radiance.x,p.radiance.y,p.radiance.z,0},{p.metallic,p.roughness,p.normalScale,sign},
                {p.normalMapping?1.0f:0.0f,draw.unlit?1.0f:0.0f,p.doubleSided?1.0f:0.0f,p.lodDither!=0.0f?p.lodDither:(p.receivesShadow?1.0f:0.0f)}};
            std::memcpy(slice.cpu,&constants,sizeof(constants));
        }
        else
        {
            const MeshConstants constants{viewProjection*draw.world,se::Transpose(*se::Inverse(draw.world)),draw.tint,
                {draw.unlit?1.0f:0.0f,draw.impostorView,draw.impostorNext,draw.impostorBlend}};
            std::memcpy(slice.cpu,&constants,sizeof(constants));
        }
        commands.SetGraphicsRootConstantBufferView(0,slice.Gpu());++binds.constants;
        frame.constantCursor+=static_cast<UINT>(slice.size);
        const auto table=frame.descriptors.Draw(static_cast<UINT>(index-1));const auto& vertex=draw.mesh->vertexView;const auto& indices=draw.mesh->indexView;
        if(!cacheBindings||firstBinding||table.ptr!=lastTable.ptr){commands.SetGraphicsRootDescriptorTable(1,table);++binds.descriptorTables;lastTable=table;}
        if(!cacheBindings||firstBinding||vertex.BufferLocation!=lastVertex.BufferLocation||vertex.SizeInBytes!=lastVertex.SizeInBytes||vertex.StrideInBytes!=lastVertex.StrideInBytes)
        {commands.IASetVertexBuffers(0,1,&vertex);++binds.vertexBuffers;lastVertex=vertex;}
        if(!cacheBindings||firstBinding||indices.BufferLocation!=lastIndex.BufferLocation||indices.SizeInBytes!=lastIndex.SizeInBytes||indices.Format!=lastIndex.Format)
        {commands.IASetIndexBuffer(&indices);++binds.indexBuffers;lastIndex=indices;}
        firstBinding=false;
        if(!draw.instances.empty()||!indirect.empty())
        {
            // G06.10：Forward 实例化的实例 SRV 在 b1 之后的参数 3；非 Forward 仍在参数 2。
            const UINT instanceParameter=se::rhi::IsInstanced(program.prepared->program->source.kind)
                &&se::rhi::IsForward(program.prepared->program->source.kind)?3u:2u;
            commands.SetGraphicsRootShaderResourceView(instanceParameter,frame.instanceSlices[index-1].Gpu());++binds.instanceBuffers;
        }
        if(!indirect.empty())commands.ExecuteIndirect(indirect[index-1].signature,1,indirect[index-1].arguments,0,indirect[index-1].arguments,D3D12Indirect::commandCountOffset);
        else commands.DrawIndexedInstanced(draw.mesh->indexCount,draw.instances.empty()?1u:static_cast<UINT>(draw.instances.size()),0,0,0);
    }
    if(statistics)*statistics=binds;
}

#include "D3D12Gui.h"
#include "D3D12Presentation.h"
#include "D3D12Memory.h"
#include "D3D12Result.h"
#include <algorithm>
#include <cmath>
#include <cstring>

bool CreateD3D12GuiTarget(D3D12Presentation& p,const se::rhi::RenderTargetDescription& d,D3D12GuiTarget& out,const Localization& text,const se::Vec4* optimizedClear)
{
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if(FAILED(p.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf()))))return false;
    const bool hdr=d.format==se::rhi::TextureFormat::Rgba16Float;
    const DXGI_FORMAT colorFormat=hdr?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_TYPELESS;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=d.width;desc.Height=d.height;
    desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc={1,0};desc.Format=colorFormat;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};clear.Format=hdr?DXGI_FORMAT_R16G16B16A16_FLOAT:d.srgb?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM;clear.Color[3]=1;
    // 前向 HDR 实际清为黑色；GUI 合成和普通 LDR 保留各自默认清屏色。
    clear.Color[0]=hdr?0:d.srgb?0.015f:0.08f;clear.Color[1]=hdr?0:d.srgb?0.015f:0.18f;clear.Color[2]=hdr?0:d.srgb?0.02f:0.32f;
    if(optimizedClear){clear.Color[0]=optimizedClear->x;clear.Color[1]=optimizedClear->y;clear.Color[2]=optimizedClear->z;clear.Color[3]=optimizedClear->w;}
    if(!CheckGraphicsResult(p.memory->Create(desc,D3D12_RESOURCE_STATE_RENDER_TARGET,&clear,out.color.resource,"GuiTarget"),text,"D3D12.GuiFailed")
        ||!CheckGraphicsResult(p.srvPool->Allocate(2,out.color.views),text,"D3D12.DescriptorCapacityExceeded")
        ||!CheckGraphicsResult(p.rtvPool->Allocate(1,out.rtv),text,"D3D12.DescriptorCapacityExceeded"))return false;
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};rtv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;rtv.Format=clear.Format;
    device->CreateRenderTargetView(out.color.resource.Get(),&rtv,out.rtv->Cpu());
    if(!d.srgb&&!hdr)
    {
        if(!CheckGraphicsResult(p.rtvPool->Allocate(1,out.srgbRtv),text,"D3D12.DescriptorCapacityExceeded"))return false;
        rtv.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        device->CreateRenderTargetView(out.color.resource.Get(),&rtv,out.srgbRtv->Cpu());
    }
    else out.srgbRtv=out.rtv;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
    for(UINT i=0;i<2;++i){srv.Format=hdr?DXGI_FORMAT_R16G16B16A16_FLOAT:i?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;device->CreateShaderResourceView(out.color.resource.Get(),&srv,out.color.views->Cpu(i));}
    out.color.width=d.width;out.color.height=d.height;out.color.format=d.format;out.srgb=d.srgb;
    if(d.depth)
    {
        desc.Format=DXGI_FORMAT_D32_FLOAT;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        clear={};clear.Format=DXGI_FORMAT_D32_FLOAT;clear.DepthStencil.Depth=1;
        if(!CheckGraphicsResult(p.memory->Create(desc,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,out.depth,"GuiSceneDepth"),text,"D3D12.GuiFailed")
            ||!CheckGraphicsResult(p.dsvPool->Allocate(1,out.dsv),text,"D3D12.DescriptorCapacityExceeded"))return false;
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};dsv.Format=DXGI_FORMAT_D32_FLOAT;dsv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(out.depth.Get(),&dsv,out.dsv->Cpu());
    }
    return true;
}
bool PrepareD3D12Gui(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12GuiLayer& c,D3D12FrameDescriptors& descriptors,D3D12GuiUpload& u,const Localization& text)
{
    const auto& g=*c.geometry;
    Microsoft::WRL::ComPtr<ID3D12Device> device;if(FAILED(p.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf()))))return false;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> views;views.reserve(g.commands.size());
    for(size_t i=0;i<g.commands.size();++i)views.push_back(c.textures[i]->views->Cpu(g.commands[i].textureIsData?1:0));
    if(!CheckGraphicsResult(descriptors.Prepare(*device.Get(),views,1),text,"D3D12.DescriptorCapacityExceeded"))return false;
    if(g.indices.empty())return true;
    if(!CheckGraphicsResult(frame.uploads.Allocate(g.vertices.size()*sizeof(se::GuiVertex),16,u.vertices),text,"D3D12.UploadCapacityExceeded")
        ||!CheckGraphicsResult(frame.uploads.Allocate(g.indices.size()*sizeof(std::uint32_t),16,u.indices),text,"D3D12.UploadCapacityExceeded"))return false;
    std::memcpy(u.vertices.cpu,g.vertices.data(),g.vertices.size()*sizeof(se::GuiVertex));
    std::memcpy(u.indices.cpu,g.indices.data(),g.indices.size()*sizeof(std::uint32_t));return true;
}
bool RecordD3D12GuiLayer(D3D12Presentation& p,const D3D12GuiLayer& c,D3D12FrameDescriptors& descriptors,const D3D12GuiUpload& u,D3D12StateRecording& states,UINT width,UINT height,const Localization& text)
{
    auto& commands=*p.commandList.Get();const auto& g=*c.geometry;
    const auto use=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES state){return CheckGraphicsResult(states.Use(commands,*resource,state,"Gui"),text,"D3D12.StateTrackingFailed");};
    if(!g.indices.empty())
    {
        if(!use(u.vertices.resource,D3D12_RESOURCE_STATE_GENERIC_READ)||!use(u.indices.resource,D3D12_RESOURCE_STATE_GENERIC_READ))return false;
        for(const auto* texture:c.textures)if(!use(texture->resource.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))return false;
        commands.SetPipelineState(c.pipeline->pipeline.Get());commands.SetGraphicsRootSignature(c.pipeline->rootSignature.Get());
        ID3D12DescriptorHeap* heaps[]{descriptors.Heap()};commands.SetDescriptorHeaps(1,heaps);
        const se::rhi::GuiConstants constants{{2/g.displaySize.x,-2/g.displaySize.y,-1-g.displayPosition.x*2/g.displaySize.x,1+g.displayPosition.y*2/g.displaySize.y}};
        commands.SetGraphicsRoot32BitConstants(0,4,&constants,0);
        const D3D12_VERTEX_BUFFER_VIEW vb{u.vertices.Gpu(),static_cast<UINT>(g.vertices.size()*sizeof(se::GuiVertex)),sizeof(se::GuiVertex)};
        const D3D12_INDEX_BUFFER_VIEW ib{u.indices.Gpu(),static_cast<UINT>(g.indices.size()*sizeof(std::uint32_t)),DXGI_FORMAT_R32_UINT};
        commands.IASetVertexBuffers(0,1,&vb);commands.IASetIndexBuffer(&ib);commands.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};commands.RSSetViewports(1,&viewport);
        const auto clipX=[&](float x){return std::clamp((x-g.displayPosition.x)*g.framebufferScale.x,0.0f,float(width));};
        const auto clipY=[&](float y){return std::clamp((y-g.displayPosition.y)*g.framebufferScale.y,0.0f,float(height));};
        for(size_t i=0;i<g.commands.size();++i)
        {
            const auto& draw=g.commands[i];const D3D12_RECT clip{LONG(std::floor(clipX(draw.clip.x))),LONG(std::floor(clipY(draw.clip.y))),LONG(std::ceil(clipX(draw.clip.z))),LONG(std::ceil(clipY(draw.clip.w)))};
            if(!draw.indexCount||clip.right<=clip.left||clip.bottom<=clip.top)continue;
            commands.RSSetScissorRects(1,&clip);commands.SetGraphicsRootDescriptorTable(1,descriptors.Draw(static_cast<UINT>(i)));
            commands.DrawIndexedInstanced(draw.indexCount,1,draw.indexOffset,static_cast<INT>(draw.vertexOffset),0);
        }
    }
    return true;
}
bool RecordD3D12Gui(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12GuiContent& c,const D3D12GuiUpload& u,D3D12StateRecording& states,UINT backBuffer,const Localization& text)
{
    auto& commands=*p.commandList.Get();
    const auto use=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES state){return CheckGraphicsResult(states.Use(commands,*resource,state,"Gui"),text,"D3D12.StateTrackingFailed");};
    if(!use(c.composite->color.resource.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET))return false;
    const auto rtv=c.composite->rtv->Cpu();constexpr float clear[]{0.015f,0.015f,0.02f,1};commands.ClearRenderTargetView(rtv,clear,0,nullptr);
    commands.OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    if(!RecordD3D12GuiLayer(p,c.layer,frame.guiDescriptors,u,states,p.width,p.height,text))return false;
    if(!use(c.composite->color.resource.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE)||!use(p.backBuffers[backBuffer].Get(),D3D12_RESOURCE_STATE_COPY_DEST))return false;
    commands.CopyResource(p.backBuffers[backBuffer].Get(),c.composite->color.resource.Get());return true;
}

#pragma once
#include "D3D12Texture.h"
#include "D3D12UploadArena.h"
struct D3D12Presentation;
struct D3D12FrameContext;
struct D3D12MeshPipeline;
class D3D12StateRecording;
struct D3D12GuiTarget
{
    D3D12Texture color;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth;
    std::shared_ptr<D3D12DescriptorRange> rtv,srgbRtv,dsv;
    bool srgb=false;
};
struct D3D12GuiLayer
{
    const D3D12MeshPipeline* pipeline=nullptr;
    const se::GuiGeometry* geometry=nullptr;
    std::span<const D3D12Texture* const> textures;
};
struct D3D12GuiContent
{
    const D3D12GuiTarget* scene=nullptr;
    const D3D12GuiTarget* composite=nullptr;
    D3D12GuiLayer layer;
};
struct D3D12GuiUpload {D3D12UploadSlice vertices,indices;};
bool CreateD3D12GuiTarget(D3D12Presentation& p,const se::rhi::RenderTargetDescription& description,
    D3D12GuiTarget& output,const Localization& text);
bool PrepareD3D12Gui(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12GuiLayer& content,D3D12FrameDescriptors& descriptors,
    D3D12GuiUpload& upload,const Localization& text);
bool RecordD3D12GuiLayer(D3D12Presentation& p,const D3D12GuiLayer& content,D3D12FrameDescriptors& descriptors,
    const D3D12GuiUpload& upload,D3D12StateRecording& states,UINT width,UINT height,const Localization& text);
bool RecordD3D12Gui(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12GuiContent& content,
    const D3D12GuiUpload& upload,D3D12StateRecording& states,UINT backBuffer,const Localization& text);

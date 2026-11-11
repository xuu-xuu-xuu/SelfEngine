#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <span>
#include "D3D12Descriptors.h"
#include "PlatformLayer/Graphics/Rhi.h"

class Localization;
struct D3D12Context;
struct D3D12Presentation;
struct D3D12TexturePixels
{
    UINT width=0,height=0;
    std::span<const std::uint8_t> rgba8;
};
// 上传完成后不可变；RGBA8 提供 sRGB/data 视图，线性格式的两视图同义。
struct D3D12Texture
{
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    std::shared_ptr<D3D12DescriptorRange> views;
    UINT width=0,height=0;
    UINT mipLevels=1,arrayLayers=1;
    UINT mostDetailedMip=0; // G04.06：SRV 可见的最细 mip，由驻留预算钳制。
    bool cube=false;
    se::rhi::TextureFormat format=se::rhi::TextureFormat::Rgba8;
};
bool ValidD3D12TexturePixels(D3D12TexturePixels pixels);
bool ValidD3D12TextureDescription(const se::rhi::TextureDescription& description);
bool AllocateD3D12Texture(D3D12Texture& output,const D3D12Context& context,
    const se::rhi::TextureDescription& description,const Localization& text);
// 分配两个 SRV（sRGB/线性）并写入 MostDetailedMip；替换时旧 Range 由 shared_ptr 释放。
bool AllocateD3D12TextureViews(D3D12Texture& output,const D3D12Context& context,
    std::uint32_t mostDetailedMip,const Localization& text);
bool ValidD3D12TextureData(const se::rhi::TextureDescription& description,std::span<const se::rhi::TextureSubresource> subresources);
// G04.07：只覆盖 [firstMip, firstMip+subresources) 的连续 mip 区间；仅 2D 单层。
bool ValidD3D12TextureSubrange(const se::rhi::TextureDescription& description,std::uint32_t firstMip,
    std::span<const se::rhi::TextureSubresource> subresources);
// 向已有纹理补传 mip 区间（资源必须已按完整 mip 链分配）。
bool UploadD3D12TextureMips(D3D12Texture& texture,const D3D12Context& context,D3D12Presentation& presentation,
    std::uint32_t firstMip,std::span<const se::rhi::TextureSubresource> subresources,const Localization& text);
bool InitializeD3D12Texture(D3D12Texture& output,const D3D12Context& context,
    D3D12Presentation& presentation,D3D12TexturePixels pixels,const Localization& text);
bool InitializeD3D12Texture(D3D12Texture& output,const D3D12Context& context,D3D12Presentation& presentation,
    const se::rhi::TextureDescription& description,std::span<const se::rhi::TextureSubresource> subresources,const Localization& text);

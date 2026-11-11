#pragma once
#include "D3D12Texture.h"
#include <cstddef>
bool D3D12BufferUploadFootprint(UINT64 payloadBytes,se::rhi::UploadFootprint& output);
bool D3D12TextureUploadFootprint(const D3D12Context& context,const se::rhi::TextureDescription& description,
    UINT rows,se::rhi::UploadFootprint& output,D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layout=nullptr);
bool RecordD3D12BufferRange(ID3D12Resource& destination,const D3D12Context& context,D3D12Presentation& presentation,
    UINT64 offset,std::span<const std::byte> bytes,D3D12_RESOURCE_STATES finalUsage,bool complete,
    const se::rhi::UploadFootprint& footprint,const Localization& text);
bool RecordD3D12TextureRows(D3D12Texture& destination,const D3D12Context& context,D3D12Presentation& presentation,
    UINT firstRow,UINT rows,std::span<const std::uint8_t> rgba8,bool complete,
    const se::rhi::UploadFootprint& footprint,const Localization& text);

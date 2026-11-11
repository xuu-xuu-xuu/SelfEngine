#include "D3D12StagedUpload.h"
#include "D3D12Upload.h"
#include <cstring>

namespace
{
constexpr UINT64 alignment=D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
UINT64 Aligned(UINT64 bytes){return (bytes+alignment-1)/alignment*alignment;}
}
bool D3D12BufferUploadFootprint(UINT64 payloadBytes,se::rhi::UploadFootprint& output)
{
    if(!payloadBytes||payloadBytes>256ull*1024*1024)return false;
    output={payloadBytes,Aligned(payloadBytes),0,alignment};return true;
}
bool D3D12TextureUploadFootprint(const D3D12Context& context,const se::rhi::TextureDescription& description,
    UINT rows,se::rhi::UploadFootprint& output,D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layout)
{
    if(!context.device||!ValidD3D12TextureDescription(description)||description.format!=se::rhi::TextureFormat::Rgba8
        ||description.mipLevels!=1||description.arrayLayers!=1||description.cube||!rows||rows>description.height
        ||UINT64(description.width)*description.height*4>256ull*1024*1024)return false;
    D3D12_RESOURCE_DESC image{};image.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;image.Width=description.width;image.Height=rows;
    image.DepthOrArraySize=image.MipLevels=1;image.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;image.SampleDesc={1,0};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed{};UINT actualRows=0;UINT64 rowBytes=0,total=0;
    context.device->GetCopyableFootprints(&image,0,1,0,&placed,&actualRows,&rowBytes,&total);
    if(actualRows!=rows||rowBytes!=UINT64(description.width)*4||!placed.Footprint.RowPitch||total==UINT64_MAX)return false;
    const UINT64 allocated=Aligned(UINT64(placed.Footprint.RowPitch)*rows);
    if(allocated>256ull*1024*1024)return false;
    output={rowBytes*rows,allocated,placed.Footprint.RowPitch,alignment};if(layout)*layout=placed;return true;
}
bool RecordD3D12BufferRange(ID3D12Resource& destination,const D3D12Context& context,D3D12Presentation& p,
    UINT64 offset,std::span<const std::byte> bytes,D3D12_RESOURCE_STATES finalUsage,bool complete,
    const se::rhi::UploadFootprint& footprint,const Localization& text)
{
    if(!p.uploadStates||!CanUploadD3D12(context,p)||bytes.size()!=footprint.payloadBytes)return false;
    auto& frame=p.frames[p.frameCursor];D3D12UploadSlice upload;
    if(!CheckGraphicsResult(frame.uploads.Allocate(footprint.stagingBytes,footprint.alignment,upload),text,"D3D12.UploadCapacityExceeded"))return false;
    std::memset(upload.cpu,0,static_cast<size_t>(footprint.stagingBytes));std::memcpy(upload.cpu,bytes.data(),bytes.size());frame.uploadDestinations.emplace_back(&destination);
    auto& states=*p.uploadStates;
    if(!CheckGraphicsResult(states.Use(*p.commandList.Get(),destination,D3D12_RESOURCE_STATE_COPY_DEST,"StagedBuffer"),text,"D3D12.StateTrackingFailed")
        ||!CheckGraphicsResult(states.Use(*p.commandList.Get(),*upload.resource,D3D12_RESOURCE_STATE_COPY_SOURCE,"StagedBuffer"),text,"D3D12.StateTrackingFailed"))return false;
    p.commandList->CopyBufferRegion(&destination,offset,upload.resource,upload.offset,bytes.size());p.uploadedBytes+=bytes.size();
    return !complete||CheckGraphicsResult(states.Use(*p.commandList.Get(),destination,finalUsage,"StagedBufferComplete"),text,"D3D12.StateTrackingFailed");
}
bool RecordD3D12TextureRows(D3D12Texture& destination,const D3D12Context& context,D3D12Presentation& p,
    UINT firstRow,UINT rows,std::span<const std::uint8_t> rgba8,bool complete,
    const se::rhi::UploadFootprint& footprint,const Localization& text)
{
    if(!p.uploadStates||!CanUploadD3D12(context,p)||rgba8.size()!=footprint.payloadBytes)return false;
    se::rhi::TextureDescription description{destination.width,destination.height};se::rhi::UploadFootprint verified;D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    if(!D3D12TextureUploadFootprint(context,description,rows,verified,&layout)||verified.stagingBytes!=footprint.stagingBytes)return false;
    auto& frame=p.frames[p.frameCursor];D3D12UploadSlice upload;
    if(!CheckGraphicsResult(frame.uploads.Allocate(footprint.stagingBytes,footprint.alignment,upload),text,"D3D12.UploadCapacityExceeded"))return false;
    std::memset(upload.cpu,0,static_cast<size_t>(footprint.stagingBytes));const auto rowBytes=size_t(destination.width)*4;
    for(UINT row=0;row<rows;++row)std::memcpy(upload.cpu+size_t(row)*layout.Footprint.RowPitch,rgba8.data()+size_t(row)*rowBytes,rowBytes);
    frame.uploadDestinations.push_back(destination.resource);auto& states=*p.uploadStates;
    if(!CheckGraphicsResult(states.Use(*p.commandList.Get(),*destination.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"StagedTexture"),text,"D3D12.StateTrackingFailed")
        ||!CheckGraphicsResult(states.Use(*p.commandList.Get(),*upload.resource,D3D12_RESOURCE_STATE_COPY_SOURCE,"StagedTexture"),text,"D3D12.StateTrackingFailed"))return false;
    layout.Offset=upload.offset;D3D12_TEXTURE_COPY_LOCATION source{},target{};source.pResource=upload.resource;source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=layout;
    target.pResource=destination.resource.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    p.commandList->CopyTextureRegion(&target,0,firstRow,0,&source,nullptr);p.uploadedBytes+=rgba8.size();
    return !complete||CheckGraphicsResult(states.Use(*p.commandList.Get(),*destination.resource.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"StagedTextureComplete"),text,"D3D12.StateTrackingFailed");
}

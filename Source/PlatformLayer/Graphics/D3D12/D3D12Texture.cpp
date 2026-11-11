#include "D3D12Texture.h"
#include "D3D12Upload.h"
#include "D3D12Memory.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include <cstddef>
#include <cstring>
#include <utility>
#include <bit>
#include <algorithm>
#include <vector>

namespace
{
bool BcFormat(se::rhi::TextureFormat format)
{
    return format==se::rhi::TextureFormat::Bc1Unorm||format==se::rhi::TextureFormat::Bc1UnormSrgb
        ||format==se::rhi::TextureFormat::Bc5Unorm||format==se::rhi::TextureFormat::Bc4Unorm;
}
UINT BcBlockBytes(se::rhi::TextureFormat format)
{
    return format==se::rhi::TextureFormat::Bc5Unorm?16:8;
}
DXGI_FORMAT StorageFormat(se::rhi::TextureFormat format)
{
    if(format==se::rhi::TextureFormat::Rgba8)return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    if(format==se::rhi::TextureFormat::Rgba16Float)return DXGI_FORMAT_R16G16B16A16_FLOAT;
    if(format==se::rhi::TextureFormat::Bc5Unorm)return DXGI_FORMAT_BC5_TYPELESS;
    if(format==se::rhi::TextureFormat::Bc4Unorm)return DXGI_FORMAT_BC4_TYPELESS;
    return DXGI_FORMAT_BC1_TYPELESS;
}
DXGI_FORMAT SrgbViewFormat(se::rhi::TextureFormat format)
{
    if(format==se::rhi::TextureFormat::Rgba8)return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if(format==se::rhi::TextureFormat::Rgba16Float)return DXGI_FORMAT_R16G16B16A16_FLOAT;
    if(format==se::rhi::TextureFormat::Bc5Unorm)return DXGI_FORMAT_BC5_UNORM; // BC5 没有 sRGB 变体。
    if(format==se::rhi::TextureFormat::Bc4Unorm)return DXGI_FORMAT_BC4_UNORM; // BC4 没有 sRGB 变体。
    return DXGI_FORMAT_BC1_UNORM_SRGB;
}
DXGI_FORMAT LinearViewFormat(se::rhi::TextureFormat format)
{
    if(format==se::rhi::TextureFormat::Rgba8)return DXGI_FORMAT_R8G8B8A8_UNORM;
    if(format==se::rhi::TextureFormat::Rgba16Float)return DXGI_FORMAT_R16G16B16A16_FLOAT;
    if(format==se::rhi::TextureFormat::Bc5Unorm)return DXGI_FORMAT_BC5_UNORM;
    if(format==se::rhi::TextureFormat::Bc4Unorm)return DXGI_FORMAT_BC4_UNORM;
    return DXGI_FORMAT_BC1_UNORM;
}
D3D12_RESOURCE_DESC ResourceDescription(const se::rhi::TextureDescription& description)
{
    D3D12_RESOURCE_DESC image{};image.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    image.Width=description.width;image.Height=description.height;
    image.DepthOrArraySize=static_cast<UINT16>(description.arrayLayers);image.MipLevels=static_cast<UINT16>(description.mipLevels);
    image.Format=StorageFormat(description.format);image.SampleDesc={1,0};return image;
}
}

bool ValidD3D12TexturePixels(D3D12TexturePixels pixels)
{
    const UINT64 bytes=UINT64{pixels.width}*pixels.height*4;
    return pixels.width&&pixels.height&&pixels.width<=16384&&pixels.height<=16384
        &&bytes<=256ull*1024*1024&&pixels.rgba8.size()==bytes;
}
bool InitializeD3D12Texture(D3D12Texture& output,const D3D12Context& context,
    D3D12Presentation& p,D3D12TexturePixels pixels,const Localization& text)
{
    if(!ValidD3D12TexturePixels(pixels)){Log(LogLevel::Error,text.GetText("D3D12.MeshInputsInvalid"));return false;}
    const se::rhi::TextureDescription description{pixels.width,pixels.height};const se::rhi::TextureSubresource data{pixels.rgba8,0};
    return InitializeD3D12Texture(output,context,p,description,std::span(&data,1),text);
}
bool ValidD3D12TextureDescription(const se::rhi::TextureDescription& description)
{
    if(!description.width||!description.height||description.width>16384||description.height>16384||description.samples!=1
        ||!description.mipLevels||description.mipLevels>static_cast<std::uint32_t>(std::bit_width(std::max(description.width,description.height)))
        ||!description.arrayLayers||description.arrayLayers>6
        ||(description.cube&&(description.arrayLayers!=6||description.width!=description.height))
        ||(description.format!=se::rhi::TextureFormat::Rgba8&&description.format!=se::rhi::TextureFormat::Rgba16Float&&!BcFormat(description.format)))return false;
    return true;
}
bool ValidD3D12TextureSubrange(const se::rhi::TextureDescription& description,std::uint32_t firstMip,
    std::span<const se::rhi::TextureSubresource> subresources)
{
    if(!ValidD3D12TextureDescription(description)||description.arrayLayers!=1||description.cube
        ||!subresources.size()||firstMip>=description.mipLevels
        ||subresources.size()>description.mipLevels-firstMip)return false;
    const bool bc=BcFormat(description.format);const UINT bpp=description.format==se::rhi::TextureFormat::Rgba8?4:8;
    const UINT blockBytes=bc?BcBlockBytes(description.format):0;UINT64 total=0;
    for(size_t index=0;index<subresources.size();++index)
    {
        const auto mip=static_cast<unsigned>(firstMip+index);
        UINT64 width=std::max(1u,description.width>>mip),height=std::max(1u,description.height>>mip);
        if(bc){width=(width+3)/4;height=(height+3)/4;}
        const UINT64 row=width*(bc?blockBytes:bpp);
        const auto pitch=subresources[index].rowPitch?subresources[index].rowPitch:row;
        if(pitch<row||subresources[index].bytes.size()<(height-1)*pitch+row||subresources[index].bytes.size()>256ull*1024*1024)return false;
        total+=row*height;
    }
    return total<=256ull*1024*1024;
}
bool ValidD3D12TextureData(const se::rhi::TextureDescription& description,std::span<const se::rhi::TextureSubresource> subresources)
{
    if(!ValidD3D12TextureDescription(description)||subresources.size()!=size_t(description.arrayLayers)*description.mipLevels)return false;
    const bool bc=BcFormat(description.format);const UINT bpp=description.format==se::rhi::TextureFormat::Rgba8?4:8;
    const UINT blockBytes=bc?BcBlockBytes(description.format):0;UINT64 total=0;
    for(size_t index=0;index<subresources.size();++index)
    {
        const auto mip=static_cast<unsigned>(index%description.mipLevels);
        UINT64 width=std::max(1u,description.width>>mip),height=std::max(1u,description.height>>mip);
        if(bc){width=(width+3)/4;height=(height+3)/4;}
        const UINT64 row=width*(bc?blockBytes:bpp);
        const auto pitch=subresources[index].rowPitch?subresources[index].rowPitch:row;
        if(pitch<row||subresources[index].bytes.size()<(height-1)*pitch+row||subresources[index].bytes.size()>256ull*1024*1024)return false;
        total+=row*height;
    }
    return total<=256ull*1024*1024;
}
bool AllocateD3D12Texture(D3D12Texture& output,const D3D12Context& context,
    const se::rhi::TextureDescription& description,const Localization& text)
{
    if(output.resource||output.views||!context.srvDescriptors||!context.memory||!ValidD3D12TextureDescription(description))return false;
    D3D12Texture candidate;const auto image=ResourceDescription(description);
    const auto created=context.memory->Create(image,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,candidate.resource,"Texture");
    if(!CheckGraphicsResult(created,text,created==D3D12MemoryBudgetExceeded?"D3D12.GpuMemoryBudgetExceeded":"D3D12.CreateTextureFailed"))return false;
    candidate.width=description.width;candidate.height=description.height;candidate.mipLevels=description.mipLevels;
    candidate.arrayLayers=description.arrayLayers;candidate.cube=description.cube;candidate.format=description.format;
    if(!AllocateD3D12TextureViews(candidate,context,0,text))return false;
    output=std::move(candidate);return true;
}
bool AllocateD3D12TextureViews(D3D12Texture& output,const D3D12Context& context,
    std::uint32_t mostDetailedMip,const Localization& text)
{
    if(!output.resource||!context.srvDescriptors||!output.mipLevels||mostDetailedMip>=output.mipLevels)return false;
    std::shared_ptr<D3D12DescriptorRange> views;
    if(!CheckGraphicsResult(context.srvDescriptors->Allocate(2,views),text,"D3D12.DescriptorCapacityExceeded"))return false;
    const UINT levels=output.mipLevels-mostDetailedMip;UINT viewIndex=0;
    for(const auto format:{SrgbViewFormat(output.format),LinearViewFormat(output.format)})
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=format;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if(output.cube)
        {
            view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBE;
            view.TextureCube.MostDetailedMip=mostDetailedMip;view.TextureCube.MipLevels=levels;
        }
        else if(output.arrayLayers>1)
        {
            view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.MostDetailedMip=mostDetailedMip;view.Texture2DArray.ArraySize=output.arrayLayers;
            view.Texture2DArray.MipLevels=levels;
        }
        else
        {
            view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MostDetailedMip=mostDetailedMip;view.Texture2D.MipLevels=levels;
        }
        context.device->CreateShaderResourceView(output.resource.Get(),&view,views->Cpu(viewIndex++));
    }
    output.views=std::move(views);output.mostDetailedMip=mostDetailedMip;return true;
}
bool UploadD3D12TextureMips(D3D12Texture& texture,const D3D12Context& context,D3D12Presentation& p,
    std::uint32_t firstMip,std::span<const se::rhi::TextureSubresource> subresources,const Localization& text)
{
    if(!texture.resource||!texture.views||!CanUploadD3D12(context,p))return false;
    se::rhi::TextureDescription description{texture.width,texture.height,texture.mipLevels,1,texture.format};
    description.arrayLayers=texture.arrayLayers;description.cube=texture.cube;
    // 完整上传允许 cube/array；只有按 mip 区间补传（2D）才走单层子区间校验。
    const bool fullUpload=firstMip==0&&subresources.size()==std::size_t(description.arrayLayers)*description.mipLevels;
    if(fullUpload)
    {
        if(!ValidD3D12TextureData(description,subresources))return false;
    }
    else if(!ValidD3D12TextureSubrange(description,firstMip,subresources))return false;
    const auto image=ResourceDescription(description);
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresources.size());
    std::vector<UINT> rows(subresources.size());std::vector<UINT64> rowBytes(subresources.size());UINT64 bytes=0;
    context.device->GetCopyableFootprints(&image,firstMip,static_cast<UINT>(layouts.size()),0,
        layouts.data(),rows.data(),rowBytes.data(),&bytes);
    if(bytes>256ull*1024*1024)return CheckGraphicsResult(E_OUTOFMEMORY,text,"D3D12.UploadCapacityExceeded");
    D3D12UploadOperation operation(p,text);if(!operation)return false;
    auto& frame=p.frames[p.frameCursor];D3D12UploadSlice upload;
    if(!CheckGraphicsResult(frame.uploads.Allocate(bytes,D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT,upload),text,"D3D12.UploadCapacityExceeded"))return false;
    std::memset(upload.cpu,0,static_cast<size_t>(bytes));
    for(size_t index=0;index<layouts.size();++index)
    {
        auto& layout=layouts[index];const auto pitch=subresources[index].rowPitch?subresources[index].rowPitch:rowBytes[index];
        for(UINT row=0;row<rows[index];++row)std::memcpy(upload.cpu+layout.Offset+size_t(row)*layout.Footprint.RowPitch,
            subresources[index].bytes.data()+size_t(row)*pitch,static_cast<size_t>(rowBytes[index]));
        layout.Offset+=upload.offset;p.uploadedBytes+=rowBytes[index]*rows[index];
    }
    frame.uploadDestinations.push_back(texture.resource);
    auto& states=*p.uploadStates;
    if(!CheckGraphicsResult(states.Use(*p.commandList.Get(),*texture.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"TextureUpload"),text,"D3D12.StateTrackingFailed")
        ||!CheckGraphicsResult(states.Use(*p.commandList.Get(),*upload.resource,D3D12_RESOURCE_STATE_COPY_SOURCE,"TextureUpload"),text,"D3D12.StateTrackingFailed"))return false;
    for(UINT index=0;index<layouts.size();++index)
    {
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=upload.resource;
        source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=layouts[index];
        D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=texture.resource.Get();
        destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;destination.SubresourceIndex=firstMip+index;
        p.commandList->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
    }
    if(!CheckGraphicsResult(states.Use(*p.commandList.Get(),*texture.resource.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"TextureUploadComplete"),text,"D3D12.StateTrackingFailed"))return false;
    return operation.Finish();
}
bool InitializeD3D12Texture(D3D12Texture& output,const D3D12Context& context,D3D12Presentation& p,
    const se::rhi::TextureDescription& description,std::span<const se::rhi::TextureSubresource> subresources,const Localization& text)
{
    if(output.resource||output.views||!context.srvDescriptors||!CanUploadD3D12(context,p)||!ValidD3D12TextureData(description,subresources))
    {Log(LogLevel::Error,text.GetText("D3D12.MeshInputsInvalid"));return false;}
    D3D12Texture candidate;
    if(!AllocateD3D12Texture(candidate,context,description,text))return false;
    if(!UploadD3D12TextureMips(candidate,context,p,0,subresources,text))return false;
    output=std::move(candidate);
    return true;
}

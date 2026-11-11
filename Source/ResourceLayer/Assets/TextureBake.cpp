#include "TextureBake.h"
#include "AssetIO.h"
#include "TextureCompression.h"
#include "CoreLayer/Hash.h"
#include <algorithm>
#include <bit>
#include <cstring>

namespace se
{
namespace
{
// .sebc：magic(4) + 版本 + 格式 + 宽 + 高 + mip 数，随后按 mip 顺序存放块数据。
constexpr std::uint32_t TextureBakeFileVersion=1;
constexpr std::uint32_t TextureBakeHeaderBytes=4+5*4;
constexpr std::uint32_t TextureBakeMaximumDimension=16384;
constexpr char TextureBakeMagic[4]={'S','E','B','C'};

void AppendUint32(std::vector<std::uint8_t>& bytes,std::uint32_t value)
{
    for(int shift=0;shift<32;shift+=8)bytes.push_back(static_cast<std::uint8_t>((value>>shift)&0xFF));
}
std::uint32_t ReadUint32(const std::uint8_t* bytes)
{
    std::uint32_t value=0;
    for(int shift=0;shift<32;shift+=8)value|=static_cast<std::uint32_t>(*bytes++)<<shift;
    return value;
}
}

std::uint32_t Bc1MipLevels(std::uint32_t width,std::uint32_t height) noexcept
{
    if(!width||!height)return 0;
    return static_cast<std::uint32_t>(std::bit_width((std::max)(width,height)));
}
std::uint64_t BakedMipBytes(BakedTextureFormat format,std::uint32_t width,std::uint32_t height,
    std::uint32_t mip) noexcept
{
    if(!width||!height)return 0;
    mip=(std::min)(mip,31u);
    const std::uint64_t levelWidth=(std::max)(1u,width>>mip),levelHeight=(std::max)(1u,height>>mip);
    const std::uint64_t blockBytes=format==BakedTextureFormat::Bc5Unorm?16:8;    return ((levelWidth+3)/4)*((levelHeight+3)/4)*blockBytes;
}
std::uint64_t Bc1MipBytes(std::uint32_t width,std::uint32_t height,std::uint32_t mip) noexcept
{
    return BakedMipBytes(BakedTextureFormat::Bc1Unorm,width,height,mip);
}
std::string BakedTextureKey(std::string_view sourceKey,BakedTextureFormat format)
{
    const char* prefix=format==BakedTextureFormat::Bc1UnormSrgb?"bc1s:"
        :format==BakedTextureFormat::Bc5Unorm?"bc5:"
        :format==BakedTextureFormat::Bc4Unorm?"bc4:":"bc1u:";
    std::string key=prefix;
    key+=TextureBakeEncoderVersion;key+=':';key+=sourceKey;return key;
}
bool BakeBc1Texture(const ImageData& image,BakedTextureFormat format,BakedTexture& output,std::string& error)
{
    const auto levels=Bc1MipLevels(image.width,image.height);
    if(!levels||image.rgba8.size()!=std::size_t(image.width)*image.height*4)
    {
        error="Rendering.TextureEncodeFailed";
        return false;
    }
    std::vector<Bc1Image> chain;
    if(!CompressBc1MipChain(image.rgba8,image.width,image.height,levels,chain,error)
        ||chain.size()!=levels)
    {
        if(error.empty())error="Rendering.TextureEncodeFailed";
        return false;
    }
    BakedTexture candidate;candidate.width=image.width;candidate.height=image.height;
    candidate.mipLevels=static_cast<std::uint32_t>(chain.size());candidate.format=format;
    std::uint64_t total=0;
    for(std::uint32_t mip=0;mip<candidate.mipLevels;++mip)
    {
        // 缓存文件按 BakedMipBytes 推导每级长度，链必须与公式一致才能写盘。
        if(chain[mip].blocks.size()!=BakedMipBytes(format,image.width,image.height,mip))
        {
            error="Rendering.TextureEncodeFailed";
            return false;
        }
        total+=chain[mip].blocks.size();
    }
    candidate.bytes.reserve(static_cast<std::size_t>(total));
    for(const auto& mip:chain)
    {
        candidate.mipOffsets.push_back(static_cast<std::uint32_t>(candidate.bytes.size()));
        candidate.bytes.insert(candidate.bytes.end(),mip.blocks.begin(),mip.blocks.end());
    }
    output=std::move(candidate);return true;
}
bool BakeBc5Texture(const ImageData& image,BakedTexture& output,std::string& error)
{
    const auto levels=Bc1MipLevels(image.width,image.height);
    if(!levels||image.rgba8.size()!=std::size_t(image.width)*image.height*4)
    {
        error="Rendering.TextureEncodeFailed";
        return false;
    }
    std::vector<Bc5Image> chain;
    if(!CompressBc5MipChain(image.rgba8,image.width,image.height,levels,chain,error)
        ||chain.size()!=levels)
    {
        if(error.empty())error="Rendering.TextureEncodeFailed";
        return false;
    }
    BakedTexture candidate;candidate.width=image.width;candidate.height=image.height;
    candidate.mipLevels=static_cast<std::uint32_t>(chain.size());candidate.format=BakedTextureFormat::Bc5Unorm;
    std::uint64_t total=0;
    for(std::uint32_t mip=0;mip<candidate.mipLevels;++mip)
    {
        if(chain[mip].blocks.size()!=BakedMipBytes(candidate.format,image.width,image.height,mip))
        {
            error="Rendering.TextureEncodeFailed";
            return false;
        }
        total+=chain[mip].blocks.size();
    }
    candidate.bytes.reserve(static_cast<std::size_t>(total));
    for(const auto& mip:chain)
    {
        candidate.mipOffsets.push_back(static_cast<std::uint32_t>(candidate.bytes.size()));
        candidate.bytes.insert(candidate.bytes.end(),mip.blocks.begin(),mip.blocks.end());
    }
    output=std::move(candidate);return true;
}
bool BakeBc4Texture(const ImageData& image,BakedTexture& output,std::string& error)
{
    const auto levels=Bc1MipLevels(image.width,image.height);
    if(!levels||image.rgba8.size()!=std::size_t(image.width)*image.height*4)
    {
        error="Rendering.TextureEncodeFailed";
        return false;
    }
    std::vector<Bc4Image> chain;
    if(!CompressBc4MipChain(image.rgba8,image.width,image.height,levels,chain,error)
        ||chain.size()!=levels)
    {
        if(error.empty())error="Rendering.TextureEncodeFailed";
        return false;
    }
    BakedTexture candidate;candidate.width=image.width;candidate.height=image.height;
    candidate.mipLevels=static_cast<std::uint32_t>(chain.size());candidate.format=BakedTextureFormat::Bc4Unorm;
    std::uint64_t total=0;
    for(std::uint32_t mip=0;mip<candidate.mipLevels;++mip)
    {
        if(chain[mip].blocks.size()!=BakedMipBytes(candidate.format,image.width,image.height,mip))
        {
            error="Rendering.TextureEncodeFailed";
            return false;
        }
        total+=chain[mip].blocks.size();
    }
    candidate.bytes.reserve(static_cast<std::size_t>(total));
    for(const auto& mip:chain)
    {
        candidate.mipOffsets.push_back(static_cast<std::uint32_t>(candidate.bytes.size()));
        candidate.bytes.insert(candidate.bytes.end(),mip.blocks.begin(),mip.blocks.end());
    }
    output=std::move(candidate);return true;
}
bool WriteBakedTexture(const std::filesystem::path& path,const BakedTexture& texture)
{
    if(!texture.width||!texture.height||!texture.mipLevels
        ||texture.mipOffsets.size()!=texture.mipLevels||texture.bytes.empty())return false;
    std::vector<std::uint8_t> bytes;
    bytes.reserve(TextureBakeHeaderBytes+texture.bytes.size());
    bytes.insert(bytes.end(),TextureBakeMagic,TextureBakeMagic+4);
    for(const auto value:{TextureBakeFileVersion,static_cast<std::uint32_t>(texture.format),
        texture.width,texture.height,texture.mipLevels})AppendUint32(bytes,value);
    bytes.insert(bytes.end(),texture.bytes.begin(),texture.bytes.end());
    return asset_io::WriteAtomic(path,bytes,true);
}
bool ReadBakedTexture(const std::filesystem::path& path,BakedTexture& output)
{
    std::vector<std::uint8_t> bytes;
    if(!asset_io::Read(path,bytes)||bytes.size()<TextureBakeHeaderBytes)return false;
    if(std::memcmp(bytes.data(),TextureBakeMagic,4)!=0)return false;
    const auto version=ReadUint32(bytes.data()+4),format=ReadUint32(bytes.data()+8);
    const auto width=ReadUint32(bytes.data()+12),height=ReadUint32(bytes.data()+16);
    const auto mipLevels=ReadUint32(bytes.data()+20);
    if(version!=TextureBakeFileVersion||format>3||!width||!height
        ||width>TextureBakeMaximumDimension||height>TextureBakeMaximumDimension)return false;
    if(mipLevels!=Bc1MipLevels(width,height))return false;
    const auto bakedFormat=static_cast<BakedTextureFormat>(format);
    std::uint64_t total=0;
    for(std::uint32_t mip=0;mip<mipLevels;++mip)total+=BakedMipBytes(bakedFormat,width,height,mip);
    if(bytes.size()!=TextureBakeHeaderBytes+total)return false;
    BakedTexture candidate;candidate.width=width;candidate.height=height;
    candidate.mipLevels=mipLevels;candidate.format=bakedFormat;
    std::uint32_t offset=0;
    for(std::uint32_t mip=0;mip<mipLevels;++mip)
    {
        candidate.mipOffsets.push_back(offset);
        offset+=static_cast<std::uint32_t>(BakedMipBytes(bakedFormat,width,height,mip));
    }
    candidate.bytes.assign(bytes.begin()+TextureBakeHeaderBytes,bytes.end());
    output=std::move(candidate);return true;
}

const BakedTexture* TextureBakeCache::Bake(const ImageData& image,std::string_view sourceKey,
    BakedTextureFormat format,std::string& error)
{
    if(sourceKey.empty())
    {
        error="Rendering.TextureEncodeFailed";
        return nullptr;
    }
    const auto key=BakedTextureKey(sourceKey,format);
    auto found=entries_.find(key);
    if(found!=entries_.end())return &found->second;
    BakedTexture baked;
    if(!Load(key,baked))
    {
        if(!BakeBc1Texture(image,format,baked,error))return nullptr;
        Store(key,baked); // 磁盘缓存是尽力而为：写失败仍返回内存结果。
    }
    return &entries_.emplace(key,std::move(baked)).first->second;
}
const BakedTexture* TextureBakeCache::BakeBc5(const ImageData& image,std::string_view sourceKey,std::string& error)
{
    if(sourceKey.empty())
    {
        error="Rendering.TextureEncodeFailed";
        return nullptr;
    }
    const auto key=BakedTextureKey(sourceKey,BakedTextureFormat::Bc5Unorm);
    auto found=entries_.find(key);
    if(found!=entries_.end())return &found->second;
    BakedTexture baked;
    if(!Load(key,baked))
    {
        if(!BakeBc5Texture(image,baked,error))return nullptr;
        Store(key,baked);
    }
    return &entries_.emplace(key,std::move(baked)).first->second;
}
bool TextureBakeCache::Load(std::string_view key,BakedTexture& output) const
{
    if(directory_.empty())return false;
    BakedTexture loaded;
    if(!ReadBakedTexture(FilePath(key),loaded))return false;
    ++diskLoads_;output=std::move(loaded);return true;
}
bool TextureBakeCache::Store(std::string_view key,const BakedTexture& texture)
{
    if(directory_.empty())return false;
    std::error_code code;std::filesystem::create_directories(directory_,code);
    if(code)return false;
    if(!WriteBakedTexture(FilePath(key),texture))return false;
    ++diskStores_;return true;
}
const BakedTexture* TextureBakeCache::Find(std::string_view key) const
{
    const auto found=entries_.find(std::string(key));
    return found==entries_.end()?nullptr:&found->second;
}
std::uint64_t TextureBakeCache::Bytes() const noexcept{
    std::uint64_t total=0;
    for(const auto& [_,texture]:entries_)total+=texture.bytes.size();
    return total;
}
std::filesystem::path TextureBakeCache::FilePath(std::string_view key) const
{
    const auto digest=Sha256(key);
    return digest.empty()?std::filesystem::path{}:directory_/(digest+".sebc");
}
}

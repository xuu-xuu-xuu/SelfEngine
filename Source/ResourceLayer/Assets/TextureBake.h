#pragma once
#include "AssetStore.h"
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace se
{
// G04：BC1/BC5 派生纹理烘焙。完整 mip 链一次压块，缓存键含源内容哈希、格式与编码器版本；
// 磁盘 .sebc 只作跨进程缓存，读写失败回退到重新编码，不影响画面。
enum class BakedTextureFormat : std::uint8_t { Bc1Unorm, Bc1UnormSrgb, Bc5Unorm, Bc4Unorm };
inline constexpr const char* TextureBakeEncoderVersion = "tex-1";

struct BakedTexture
{
    std::uint32_t width=0,height=0,mipLevels=0;
    BakedTextureFormat format=BakedTextureFormat::Bc1Unorm;
    std::vector<std::uint32_t> mipOffsets; // 指向 bytes 的起点，顺序 mip0..N。
    std::vector<std::uint8_t> bytes;       // 全部 mip 的 8 字节块，行主序。
};

std::uint32_t Bc1MipLevels(std::uint32_t width,std::uint32_t height) noexcept;
std::uint64_t Bc1MipBytes(std::uint32_t width,std::uint32_t height,std::uint32_t mip) noexcept;
// 通用块尺寸：BC1 每块 8 字节，BC5 每块 16 字节（R/G 各 8 字节）。
std::uint64_t BakedMipBytes(BakedTextureFormat format,std::uint32_t width,std::uint32_t height,
    std::uint32_t mip) noexcept;
// 键 = 格式 + 编码器版本 + 源内容键；格式不同必须得到不同纹理，避免 sRGB/线性误用。
std::string BakedTextureKey(std::string_view sourceKey,BakedTextureFormat format);
// 不做运行时压缩：调用方在资产载入/首帧上传时烘焙一次，失败写稳定错误键且不改 output。
bool BakeBc1Texture(const ImageData& image,BakedTextureFormat format,BakedTexture& output,std::string& error);
bool BakeBc5Texture(const ImageData& image,BakedTexture& output,std::string& error);
bool BakeBc4Texture(const ImageData& image,BakedTexture& output,std::string& error);
// .sebc 单文件读写；头部校验版本/格式/尺寸/mip 数/字节总数。
bool WriteBakedTexture(const std::filesystem::path& path,const BakedTexture& texture);
bool ReadBakedTexture(const std::filesystem::path& path,BakedTexture& output);

// 进程内派生纹理缓存；挂目录后跨进程复用 .sebc。目录为空时只做内存缓存。
class TextureBakeCache
{
public:
    void SetDirectory(std::filesystem::path directory){directory_=std::move(directory);}
    const std::filesystem::path& Directory() const noexcept {return directory_;}
    // 命中内存或磁盘时不重新编码；失败返回空指针并写稳定错误键。
    const BakedTexture* Bake(const ImageData& image,std::string_view sourceKey,
        BakedTextureFormat format,std::string& error);
    const BakedTexture* BakeBc5(const ImageData& image,std::string_view sourceKey,std::string& error);
    bool Load(std::string_view key,BakedTexture& output) const;
    bool Store(std::string_view key,const BakedTexture& texture);
    // 只查内存缓存；用于流送补传时取回块数据（不触发磁盘或重编码）。
    const BakedTexture* Find(std::string_view key) const;
    std::size_t Count() const noexcept {return entries_.size();}
    std::uint64_t Bytes() const noexcept;
    std::uint64_t DiskLoads() const noexcept {return diskLoads_;}
    std::uint64_t DiskStores() const noexcept {return diskStores_;}
    void Clear() noexcept {entries_.clear();diskLoads_=diskStores_=0;}
private:
    std::filesystem::path FilePath(std::string_view key) const;
    std::filesystem::path directory_;
    std::map<std::string,BakedTexture> entries_;
    mutable std::uint64_t diskLoads_=0,diskStores_=0;
};
}

#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace se
{
// G04：BC1（DXT1）基线编码器。4x4 块、RGB565 端点 + 2 位索引，8:1。
// 后续 BC7/BC5 接入时保持同一调用面，只替换编码器。
struct Bc1Image
{
    std::uint32_t width=0,height=0;
    std::vector<std::uint8_t> blocks; // 每块 8 字节，行主序，宽高按 4 对齐补齐。
};

bool CompressBc1(std::span<const std::uint8_t> rgba8,std::uint32_t width,std::uint32_t height,
    Bc1Image& output,std::string& error);
void DecodeBc1(const Bc1Image& image,std::vector<std::uint8_t>& rgba8);
// 生成完整 BC1 mip 链：每级 2x2 盒式降采样后独立压块。
bool CompressBc1MipChain(std::span<const std::uint8_t> rgba8,std::uint32_t width,std::uint32_t height,
    std::uint32_t levels,std::vector<Bc1Image>& output,std::string& error);

// G04.05：BC5（双通道）用于法线贴图：4x4 块 16 字节（R/G 各 8 字节 BC4 块），4:1。
// 法线图只存 XY（存储在 R/G），Z 由 Shader 重建，避免 BC1 的 2 位索引压坏法线。
struct Bc5Image
{
    std::uint32_t width=0,height=0;
    std::vector<std::uint8_t> blocks; // 每块 16 字节，行主序，宽高按 4 对齐补齐。
};
bool CompressBc5(std::span<const std::uint8_t> rgba8,std::uint32_t width,std::uint32_t height,
    Bc5Image& output,std::string& error);
void DecodeBc5(const Bc5Image& image,std::vector<std::uint8_t>& rgba8); // 只写 R/G，B=0、A=255。
bool CompressBc5MipChain(std::span<const std::uint8_t> rgba8,std::uint32_t width,std::uint32_t height,
    std::uint32_t levels,std::vector<Bc5Image>& output,std::string& error);

// G04.08：BC4 单通道（AO/粗糙度等）：4x4 块 8 字节，只压 R 通道，4:1。
struct Bc4Image
{
    std::uint32_t width=0,height=0;
    std::vector<std::uint8_t> blocks; // 每块 8 字节，行主序，宽高按 4 对齐补齐。
};
bool CompressBc4(std::span<const std::uint8_t> rgba8,std::uint32_t width,std::uint32_t height,
    Bc4Image& output,std::string& error);
void DecodeBc4(const Bc4Image& image,std::vector<std::uint8_t>& rgba8); // 写 R，G/B=0、A=255。
bool CompressBc4MipChain(std::span<const std::uint8_t> rgba8,std::uint32_t width,std::uint32_t height,
    std::uint32_t levels,std::vector<Bc4Image>& output,std::string& error);
}

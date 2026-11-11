#pragma once
#include "CoreLayer/Math/Math.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace se
{
// G15：ACES fitted 色调映射与 33³ LUT / lift-gamma-gain 调色。
// 链路约定：HDR（线性）→ tonemap（显示线性 [0,1]）→ 调色（LUT 或 LGG）→ 只做一次 sRGB 编码。
enum class ToneMapper : std::uint8_t { Reinhard=0,AcesFitted=1 };

// G15：.selut 资产的色彩空间标记。数据语义固定为显示线性或已编码 sRGB，
// 加载后统一解码到显示线性；版本与标记都会在读取时校验，未知值直接拒绝。
enum class ColorLutColorSpace : std::uint32_t { Linear=0,Srgb=1 };
inline constexpr std::uint32_t ColorLutAssetVersion=1;
struct ColorLutAsset
{
    std::uint32_t size=0;
    ColorLutColorSpace colorSpace=ColorLutColorSpace::Linear;
    std::vector<Vec3> values; // (r,g,b) 行优先；与 ColorLut::Sample 的晶格顺序一致。
    bool Valid() const noexcept;
};
// 写入前校验 size/值域/非有限值；sRGB 标记时逐通道编码一次。
bool WriteColorLutAsset(const std::filesystem::path& path,const ColorLutAsset& asset,std::string& error);
// 读取时校验 magic/版本/size/通道数/色彩空间标记/值数量与值域；失败不替换 output。
bool ReadColorLutAsset(const std::filesystem::path& path,ColorLutAsset& output,std::string& error);

struct ColorGradingSettings
{
    ToneMapper toneMapper=ToneMapper::AcesFitted;
    bool lutEnabled=false;
    float lift[3]{0,0,0},gamma[3]{1,1,1},gain[3]{1,1,1};
    std::uint32_t lutSize=33;
};

Vec3 TonemapReinhard(Vec3 linear);
// ACES RRT+ODT 的 Stephen Hill 拟合（输入/输出矩阵 + 有理拟合）。
Vec3 TonemapAcesFitted(Vec3 linear);
Vec3 Tonemap(Vec3 linear,ToneMapper mapper);
// 精确分段 sRGB；全链路只在这里做一次编码/解码。
Vec3 SrgbToLinear(Vec3 encoded);
Vec3 LinearToSrgb(Vec3 linear);
// out = pow(max(in*gain + lift, 0), 1/gamma)。
Vec3 ApplyLiftGammaGain(Vec3 linear,const ColorGradingSettings& settings);

class ColorLut
{
public:
    bool Build(std::uint32_t size,const ColorGradingSettings& settings);
    bool Ready() const noexcept {return size_>1&&values_.size()==std::size_t(size_)*size_*size_;}
    std::uint32_t Size() const noexcept {return size_;}
    Vec3 Sample(Vec3 linear) const;
    void Clear() noexcept;
    // G15：热重载与 GPU 上传共用的晶格访问。values 的 (r,g,b) 顺序与 Build/Sample 一致；
    // 拷贝只在资产成功读取时发生，渲染线程不逐帧复制。
    bool Adopt(std::uint32_t size,std::vector<Vec3> values,ColorLutColorSpace sourceSpace=ColorLutColorSpace::Linear);
    const std::vector<Vec3>& Values() const noexcept {return values_;}
    // G15：.selut 资产入口。保存时按 space 标记编码；加载时把 sRGB 标记解码到显示线性，
    // AssetColorSpace() 仍保留文件中的原始标记，便于诊断，不参与采样。
    bool SaveAsset(const std::filesystem::path& path,ColorLutColorSpace space,std::string& error) const;
    bool LoadAsset(const std::filesystem::path& path,std::string& error);
    ColorLutColorSpace AssetColorSpace() const noexcept {return assetColorSpace_;}
private:
    std::uint32_t size_=0;
    ColorLutColorSpace assetColorSpace_=ColorLutColorSpace::Linear;
    std::vector<Vec3> values_;
};

// 显示线性空间的调色：LUT 开启时查表，否则 LGG；随后由 PresentSrgb 统一编码。
Vec3 ApplyGrading(Vec3 displayLinear,const ColorGradingSettings& settings,const ColorLut* lut);
Vec3 GradeColor(Vec3 hdrLinear,const ColorGradingSettings& settings,const ColorLut* lut=nullptr);
Vec3 PresentSrgb(Vec3 hdrLinear,const ColorGradingSettings& settings,const ColorLut* lut=nullptr);
// UI 在 tonemap/调色之后合成；alpha=1 时像素完全等于 UI 颜色，不随曝光变化。
Vec3 ComposeUi(Vec3 hdrLinear,Vec3 uiSrgb,float alpha,float exposure,const ColorGradingSettings& settings,
    const ColorLut* lut=nullptr);

// G15.09：输出 LUT 的 2D strip 烘焙，供 29 号布局的 FinalOutput/ShadowOutput 采样。
// 布局与 FinalOutput.hlsl 一致：x = blue*size + red、y = green。Rgba8 饱和到 [0,1]（兼容路径），
// Rgba16Float 以 half 保留到 65504，避免 8 位量化在暗部产生 sRGB 台阶。非有限值写 0。
enum class ColorLutStripFormat : std::uint32_t { Rgba8=0,Rgba16Float=1 };
struct ColorLutStrip
{
    std::uint32_t width=0,height=0,bytesPerPixel=0;
    ColorLutStripFormat format=ColorLutStripFormat::Rgba8;
    std::vector<std::uint8_t> bytes; // Rgba8 4 B/像素；Rgba16Float 8 B/像素（little-endian half RGBA）
    bool Valid() const noexcept;
};
// 只接受 Ready() 的 LUT；失败不替换 output。
bool BakeColorLutStrip(const ColorLut& lut,ColorLutStripFormat format,ColorLutStrip& output);
}

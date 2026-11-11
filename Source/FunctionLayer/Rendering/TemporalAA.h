#pragma once
#include "CoreLayer/Math/Math.h"
#include <array>
#include <cstdint>
#include <span>

namespace se
{
// G14：TAA 与时域超分的 CPU 参考。抖动、重投影、YCoCg 邻域夹取、历史混合与失效
// 都按与 GPU 计划一致的公式实现；真正的历史纹理/速度缓冲仍由 RHI 与 Shader 承担。
struct TaaJitter
{
    float x=0,y=0; // 像素偏移，范围 [-0.5,0.5]
};
// Halton(2,3) 8 相位；帧号每 8 帧循环，frame 0 的 x 为 0。
TaaJitter HaltonJitter(std::uint32_t frameIndex);
// 把像素抖动加到裁剪矩阵：clip.x += 2*x/width*clip.w，clip.y += 2*y/height*clip.w。
Mat4 ApplyProjectionJitter(Mat4 clipFromWorld,TaaJitter jitter,std::uint32_t width,std::uint32_t height);
struct TaaVelocity
{
    bool valid=false;
    float du=0,dv=0; // previousUV - currentUV（UV 左上原点）
};
TaaVelocity ReprojectVelocity(const Mat4& clipFromWorld,const Mat4& previousClipFromWorld,Vec3 worldPosition);
struct TaaSettings
{
    bool enabled=true;
    float staticBlend=0.1f;   // 新帧权重（静止）
    float dynamicBlend=0.2f;  // 新帧权重（有速度）
    float depthThreshold=0.05f;
    float velocityThreshold=0.01f;
    float sharpen=0.0f;
    bool spatialUpscale=false; // 预留独立开关；本阶段未实现超分
};
struct TaaResolveInput
{
    std::array<Vec3,9> neighborhood{}; // 3×3，行优先，index 4 为中心
    Vec3 history{};
    float currentDepth=0,historyDepth=0;
    TaaVelocity velocity{};
    bool historyValid=false;
    bool transparentOrUi=false; // 透明/UI 不进入历史，直接输出当前帧
};
struct TaaResolveResult
{
    Vec3 color{};
    float historyWeight=0;
    bool disoccluded=false;
};
Vec3 RgbToYCoCg(Vec3 color);
Vec3 YCoCgToRgb(Vec3 color);
void YCoCgNeighborhood(std::span<const Vec3,9> neighborhood,Vec3& minimum,Vec3& maximum);
Vec3 ClampToNeighborhood(Vec3 history,std::span<const Vec3,9> neighborhood);
TaaResolveResult ResolveTaa(const TaaResolveInput& input,const TaaSettings& settings);
// 历史 ping-pong 的 CPU 状态；resize/场景切换/暂停恢复首帧必须失效。
class TaaHistory
{
public:
    void Reset() noexcept;
    void Resize(std::uint32_t width,std::uint32_t height) noexcept;
    void Invalidate() noexcept;
    void Confirm() noexcept;
    bool Usable(std::uint32_t width,std::uint32_t height) const noexcept;
    std::uint32_t Width() const noexcept {return width_;}
    std::uint32_t Height() const noexcept {return height_;}
private:
    std::uint32_t width_=0,height_=0;
    bool valid_=false;
};
}

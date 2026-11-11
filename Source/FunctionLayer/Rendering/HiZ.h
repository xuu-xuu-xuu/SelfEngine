#pragma once
#include "CoreLayer/Math/Bounds.h"
#include <cstdint>
#include <span>
#include <vector>

namespace se
{
// G06：CPU 参考 Hi-Z。深度为 D3D 约定（0=近，1=远），2×2 max 归约，
// 遮挡测试只判“确定被遮挡”，任何跨近平面、屏幕外或数值异常都保留可见。
struct HiZMipLevel
{
    std::uint32_t width=0,height=0;
    std::vector<float> depth;
};
class HiZBuffer
{
public:
    bool Build(std::uint32_t width,std::uint32_t height,std::span<const float> depth);
    bool Ready() const noexcept {return !mips_.empty();}
    std::uint32_t Width() const noexcept {return width_;}
    std::uint32_t Height() const noexcept {return height_;}
    std::uint32_t MipCount() const noexcept {return static_cast<std::uint32_t>(mips_.size());}
    const HiZMipLevel& Mip(std::uint32_t level) const;
    float MaxDepth(std::uint32_t mip,std::uint32_t x,std::uint32_t y) const;
    void Clear() noexcept;
private:
    std::uint32_t width_=0,height_=0;
    std::vector<HiZMipLevel> mips_;
};
struct ScreenRect
{
    float left=0,top=0,right=0,bottom=0;
    bool Valid() const noexcept {return right>left&&bottom>top;}
};
// 世界 AABB 的 8 角点投影到屏幕矩形；任一角点 w<=0 或深度越界返回 false（保守不可剔除）。
bool ProjectScreenRect(const Aabb& worldBox,const Mat4& clipFromWorld,std::uint32_t width,std::uint32_t height,
    ScreenRect& output);
struct HiZTestResult
{
    bool occluded=false;
    std::uint32_t mip=0;
    float objectDepth=1;
    float maxDepth=0;
};
// 用屏幕 AABB 选 mip 并在覆盖的 2×2 深度上取最大；仅当物体最近深度严格大于该最大值时判遮挡。
HiZTestResult TestOcclusion(const HiZBuffer& hzb,const Aabb& worldBox,const Mat4& clipFromWorld,
    std::uint32_t width,std::uint32_t height);

// G06.06：HZB 调试可视化。每级记录尺寸与最小/最大深度，mip0 转成 RGBA8 灰度
// （近=白、远=黑）；用于把 HZB 内容落成 PNG/证据，不进入逐帧渲染路径。
struct HiZPreviewLevel
{
    std::uint32_t width=0,height=0;
    float minimumDepth=1,maximumDepth=0;
};
struct HiZPreview
{
    std::uint32_t width=0,height=0,levels=0;
    std::vector<HiZPreviewLevel> levelInfo;
    std::vector<std::uint8_t> mip0;
    bool Ready() const noexcept {return width&&height&&mip0.size()==std::size_t{width}*height*4;}
};
bool BuildHiZPreview(const HiZBuffer& hzb,HiZPreview& output);

// G06.02：HZB 失效门控（首帧、resize、FOV >10%、相机 cut 时跳过遮挡）。
enum class HiZInvalidation { None, FirstFrame, Resize, FieldOfView, CameraCut };
struct HiZFrameState
{
    Mat4 viewProjection;
    Vec3 cameraPosition{};
    Vec3 cameraForward{0,0,-1};
    float verticalFov=Pi/3;
    std::uint32_t width=0,height=0;
};
class HiZHistory
{
public:
    explicit HiZHistory(float teleportMeters=25.0f,float maximumFovRatio=0.10f,float maximumTurnDegrees=60.0f);
    // 返回本帧是否必须跳过遮挡；内部保存本帧状态供下一帧比较。
    HiZInvalidation Update(const HiZFrameState& state);
    bool Usable() const noexcept {return usable_;}
    HiZInvalidation LastReason() const noexcept {return reason_;}
    void Reset() noexcept;
private:
    bool initialized_=false;
    HiZFrameState previous_{};
    HiZInvalidation reason_=HiZInvalidation::FirstFrame;
    bool usable_=false;
    float teleportMeters_=25.0f,maximumFovRatio_=0.10f,maximumTurnDegrees_=60.0f;
};
}

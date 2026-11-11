#pragma once

#include <Windows.h>
#include <cstdint>
#include <array>
#include <functional>

class Localization;
namespace se { struct FrameData; class Profiler; }

// 当前工程只支持 Windows；借用窗口，不拥有或销毁 HWND。
struct RenderSurface
{
    HWND window = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct RenderFrame
{
    RenderSurface surface;
    double updateDeltaSeconds = 0.0; // 应用限制后的更新间隔。
    double rawDeltaSeconds = 0.0;    // 实际测得的更新间隔。
    double elapsedSeconds = 0.0;    // 实际累计时间。
    std::uint64_t updateIndex = 0;  // 包含不可绘制期间的更新，不等于呈现帧数。
    const se::FrameData* data = nullptr; // 仅在本次 Render 调用期间有效；异步使用须自行复制。
    se::Profiler* profiler=nullptr; // 可选 CPU 采集，生命周期覆盖本次 Render。
};

enum class RenderResult { Success, Occluded, Failed };
struct EditorRenderView
{
    bool hasCamera = false;
    std::array<float,16> viewProjection{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::uint64_t colorTextureToken = 0; // 渲染器自己的纹理标识；由同一渲染器解释。
    std::function<std::uint64_t(float,float)> pick; // 视口归一化坐标 -> 稳定实体 ID。
};

// 应用只调用这三个入口；GPU 对象、命令记录、同步和尺寸重建由渲染实现负责。
class Renderer
{
public:
    virtual ~Renderer() = default;
    virtual bool Initialize(const RenderSurface& surface, const Localization& localization) = 0;
    virtual RenderResult Render(const RenderFrame& frame, const Localization& localization) = 0;
    virtual EditorRenderView QueryEditorView() const { return {}; }
    // Initialize 失败或抛异常也会调用；必须支持部分初始化、重复调用且不抛异常。
    // 正常关闭时窗口仍有效；实现须确认 GPU 不再使用资源后再释放。
    virtual void Shutdown() noexcept = 0;
};

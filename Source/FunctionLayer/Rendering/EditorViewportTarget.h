#pragma once

#include "PlatformLayer/Graphics/Rhi.h"
#include <cstdint>
#include <string>

namespace se
{
// 编辑器/离屏视口拥有的 HDR、LDR 与深度目标。接口刻意不暴露 D3D12 类型，
// 由 Function/Rendering 的实现消费 RHI 句柄，Tool/Editor 只借观察视图。
class EditorViewportTarget
{
public:
    virtual ~EditorViewportTarget()=default;
    // 成功后才替换 Targets()；失败保留上一有效目标并返回稳定错误键。
    virtual bool Resize(std::uint32_t width,std::uint32_t height,std::string& errorKey)=0;
    virtual rhi::SceneTargets Targets() const=0;
    virtual rhi::TextureHandle LdrColor() const=0;
};

// R00 的空实现骨架：只有契约，没有 GPU 资源；不会被现有渲染路径使用。
class NullEditorViewportTarget final:public EditorViewportTarget
{
public:
    bool Resize(std::uint32_t width,std::uint32_t height,std::string& errorKey) override;
    rhi::SceneTargets Targets() const override{return targets_;}
    rhi::TextureHandle LdrColor() const override{return targets_.ldrColor;}
private:
    rhi::SceneTargets targets_{};
};

// 真正的视口目标：HDR、LDR、深度都经 RHI 创建；旧目标由 RHI/Fence lease 保活。
// depth 句柄指向 LDR 目标携带的深度附件，避免重复创建。
class EditorViewportTargetResource final:public EditorViewportTarget
{
public:
    EditorViewportTargetResource(rhi::Device& device,std::uint32_t width,std::uint32_t height);
    bool Resize(std::uint32_t width,std::uint32_t height,std::string& errorKey) override;
    rhi::SceneTargets Targets() const override{return targets_;}
    rhi::TextureHandle LdrColor() const override{return targets_.ldrColor;}
    bool Ready()const noexcept{return static_cast<bool>(targets_.hdrColor)&&static_cast<bool>(targets_.ldrColor);}
private:
    rhi::Device* device_=nullptr;
    rhi::Texture hdr_,ldr_;
    rhi::SceneTargets targets_{};
};
}

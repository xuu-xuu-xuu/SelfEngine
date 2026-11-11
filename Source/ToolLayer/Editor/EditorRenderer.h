#pragma once
#include "FunctionLayer/Rendering/Renderer.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"

namespace se
{
// 产品编辑器组合根：Scene 面板使用 BasicLit + Shadow + 程序化 Sky，
// 场景渲染到 EditorViewportTarget，GUI 由 GuiCompositor 合成。
class EditorRenderer final:public Renderer
{
public:
    EditorRenderer();
    bool Initialize(const RenderSurface& surface,const Localization& localization) override;
    RenderResult Render(const RenderFrame& frame,const Localization& localization) override;
    EditorRenderView QueryEditorView() const override;
    void Shutdown() noexcept override;
    SandboxDrawStatistics DrawStatistics()const{return renderer_.DrawStatistics();}
    rhi::Statistics Statistics()const{return renderer_.Statistics();}
    rhi::AdapterInformation Adapter()const{return renderer_.Adapter();}
    void SetCaptureOutput(std::filesystem::path path,std::uint64_t frame){renderer_.SetCaptureOutput(std::move(path),frame);}
    bool CaptureSucceeded()const noexcept{return renderer_.CaptureSucceeded();}
    void SetProfileOutput(std::filesystem::path directory){renderer_.SetProfileOutput(std::move(directory));}
    bool ProfileExportSucceeded()const noexcept{return renderer_.ProfileExportSucceeded();}
    std::vector<rhi::GpuTiming> GpuTimingsSince(std::uint64_t submission){return renderer_.GpuTimingsSince(submission);}
    rhi::Result WaitIdle(){return renderer_.WaitIdle();}
private:
    SandboxRenderer renderer_;
};
}

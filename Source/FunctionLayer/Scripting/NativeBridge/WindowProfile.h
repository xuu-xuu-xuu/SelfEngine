#pragma once
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "CoreLayer/Profiler.h"
#include <filesystem>
#include <vector>

namespace se
{
// 仅诊断模式启用；借用真实 Player 渲染器，不推进逻辑或输入。
class WindowProfileRenderer final : public Renderer
{
public:
    static constexpr std::size_t FrameLimit=6000;
    WindowProfileRenderer(SandboxRenderer& renderer,Profiler& profiler,std::filesystem::path directory);
    ~WindowProfileRenderer() override {Shutdown();}
    bool Initialize(const RenderSurface& surface,const Localization& localization) override;
    RenderResult Render(const RenderFrame& frame,const Localization& localization) override;
    void Shutdown() noexcept override;
    bool Finish(bool hostSucceeded);
private:
    struct Row
    {
        std::uint64_t frame=0,submission=0,draws=0,uiDraws=0,uiVertices=0;
        double elapsed=0,rawDelta=0,updateDelta=0;
        std::uint32_t width=0,height=0;
        bool visible=false,foreground=false,occluded=false;
        rhi::Statistics statistics;
    };
    SandboxRenderer& renderer_;
    Profiler& profiler_;
    std::filesystem::path directory_;
    std::vector<Row> rows_;
    rhi::AdapterInformation adapter_;
    rhi::Statistics finalStatistics_;
    bool stopped_=false,failed_=false,overflow_=false;
    std::string station_,desktop_;
};
}

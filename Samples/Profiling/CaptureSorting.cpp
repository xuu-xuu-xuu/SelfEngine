#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "Benchmark/BenchmarkScene.h"

// PIX 版本的 launch 参数转发存在已记录的不一致，用两个有界入口固定开关。
int main()
{
    se::BenchmarkSession session;session.captureMode=true;session.config.kind=se::BenchmarkKind::Mixed;
    session.config.visibility=se::BenchmarkVisibility::AllVisible;session.config.radiusMultiplier=5;
    session.config.sortOpaque=SE_CAPTURE_SORTING!=0;session.config.cacheOpaqueBindings=SE_CAPTURE_SORTING!=0;
    se::BenchmarkScene scene(session);auto services=scene.Services();services.maximumFrames=360;
    SandboxRenderer renderer(SandboxScene::Benchmark);return RunSandboxApp("en-US",renderer,false,&services);
}

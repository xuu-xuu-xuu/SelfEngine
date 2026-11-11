#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "StaticModel/StaticModelDemo.h"

// 独立、有界隐藏入口，供外部抓帧工具直接 launch，无需转发命令行参数。
int main()
{
    SandboxRenderer renderer(SandboxScene::Scene);
    se::StaticModelDemo demo("glb");auto services=demo.Services();services.maximumFrames=1000;
    return RunSandboxApp("en-US",renderer,false,&services);
}

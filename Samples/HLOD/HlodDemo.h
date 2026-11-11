#pragma once
#include "FunctionLayer/Application/FrameData.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include <vector>

namespace se
{
// G05.10：HLOD 运行时接入样例。4×4 个 64 m 单元、每单元 4 个材质球，相机固定并可用
// SELFENGINE_HLOD_DISTANCE 选择距离；用于同场景 A/B（实例 vs 单元代理）。
class HlodDemo
{
public:
    explicit HlodDemo(std::string locale="zh-CN"):locale_(std::move(locale)) {}
    ApplicationServices Services();
private:
    World world_;
    std::unique_ptr<AssetStore> assets_;
    EntityHandle camera_,directional_;
    std::vector<EntityHandle> spheres_;
    std::string locale_;
    float distance_=400;
};
}

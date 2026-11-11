#pragma once
#include "FunctionLayer/Application/FrameData.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "CoreLayer/Localization.h"

namespace se
{
class SceneDemo
{
public:
    SceneDemo(std::filesystem::path scene,std::filesystem::path assetRoot,std::string locale="zh-CN",std::filesystem::path ui={})
        :scene_(std::move(scene)),assetRoot_(std::move(assetRoot)),uiPath_(std::move(ui)),locale_(std::move(locale)){}
    ApplicationServices Services();
    bool Load();
    const World& Scene() const noexcept { return world_; }
private:
    std::filesystem::path scene_,assetRoot_,uiPath_;
    std::string locale_;
    Environment environment_;
    World& world_=environment_.GetWorld();
    UiTree ui_;
    std::unique_ptr<AssetStore> assets_;
};
}

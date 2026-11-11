#pragma once
#include "FunctionLayer/Application/FrameData.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "FunctionLayer/GameUI/UiTransitionDemo.h"
namespace se
{
class GameUIDemo
{
public:
    explicit GameUIDemo(std::string locale="zh-CN",bool transitions=false):locale_(std::move(locale)),transitions_(transitions){}
    ApplicationServices Services();
private:
    bool Initialize();
    std::string locale_;UiTree ui_;World world_;UiId title_=0;
    std::unique_ptr<AssetStore> assets_;
    bool transitions_=false;UiTransitionDemo transitionDemo_;
};
}

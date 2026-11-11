#pragma once
#include "UiTree.h"
namespace se
{
struct UiEventRoute {UiId target=0;std::vector<UiId> path;};
// 命中与路由只读，不拥有第二份 hover/capture/focus 或控件生命周期。
class UiEventSystem
{
public:
    static bool Interactive(UiKind kind)noexcept;
    static UiId HitTarget(const UiTree& tree,UiPoint point);
    static UiEventRoute Route(const UiTree& tree,UiPoint point);
private:
    static UiId HitBranch(const UiTree& tree,UiPoint point,UiId root);
};
}

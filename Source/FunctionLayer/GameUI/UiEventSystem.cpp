#include "UiEventSystem.h"
#include "UiControlGeometry.h"
#include <cmath>
namespace se
{
bool UiEventSystem::Interactive(UiKind kind)noexcept
{return kind==UiKind::Button||kind==UiKind::Checkbox||kind==UiKind::Slider||kind==UiKind::TextInput||kind==UiKind::ScrollView;}
UiId UiEventSystem::HitBranch(const UiTree& tree,UiPoint point,UiId root)
{
    const auto& children=root?tree.nodes_.at(root).children:tree.roots_;
    for(auto it=children.rbegin();it!=children.rend();++it)
    {
        const auto& node=tree.nodes_.at(*it);
        if(!tree.Effective(node.id,true)||!node.clipping.Contains(point))continue;
        if(node.kind==UiKind::ScrollView)
        {const auto g=MakeUiScrollGeometry(node.rectangle,node.style.padding*tree.canvasScale_,node.contentHeight*tree.canvasScale_,node.scroll*tree.canvasScale_,tree.canvasScale_);if(g.scrollable&&g.track.Contains(point))return node.id;}
        if(const auto child=HitBranch(tree,point,node.id))return child;
        if(node.rectangle.Contains(point)&&(Interactive(node.kind)||node.blocksPointer))return node.id;
    }
    return 0;
}
UiId UiEventSystem::HitTarget(const UiTree& tree,UiPoint point)
{return std::isfinite(point.x)&&std::isfinite(point.y)?HitBranch(tree,point,0):0;}
UiEventRoute UiEventSystem::Route(const UiTree& tree,UiPoint point)
{
    UiEventRoute result;result.target=HitTarget(tree,point);
    for(auto id=result.target;id;id=tree.nodes_.at(id).parent)result.path.push_back(id);
    return result;
}
}

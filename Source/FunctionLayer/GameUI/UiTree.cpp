#include "UiTree.h"
#include "UiEventSystem.h"
#include "UiControlGeometry.h"
#include "CoreLayer/Utf.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace se
{
namespace
{
UiRect Intersect(UiRect a, UiRect b)
{
    const float x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return { x, y, std::max(0.0f, std::min(a.x + a.width, b.x + b.width) - x),
        std::max(0.0f, std::min(a.y + a.height, b.y + b.height) - y) };
}
bool Interactive(UiKind kind)
{
    return UiEventSystem::Interactive(kind);
}
bool Finite(UiPoint point){return std::isfinite(point.x)&&std::isfinite(point.y);}
bool BindingName(std::string_view name)
{
    if(name.empty()||name.size()>128)return false;
    return std::all_of(name.begin(),name.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c==':'||c=='-';});
}
bool ValidStyle(const UiStyle& s)
{
    for (float v : {s.anchorMin.x, s.anchorMin.y, s.anchorMax.x, s.anchorMax.y,
        s.pivot.x, s.pivot.y, s.offset.x, s.offset.y, s.size.x, s.size.y, s.padding, s.spacing})
        if (!std::isfinite(v) || std::abs(v) > 1000000) return false;
    return s.padding >= 0 && s.spacing >= 0 && s.anchorMin.x >= 0 && s.anchorMin.y >= 0
        && s.anchorMax.x <= 1 && s.anchorMax.y <= 1 && s.anchorMax.x >= s.anchorMin.x
        && s.anchorMax.y >= s.anchorMin.y && static_cast<int>(s.layout) >= 0 && static_cast<int>(s.layout) <= 2;
}
}
bool UiRect::Contains(UiPoint p) const noexcept
{ return p.x >= x && p.y >= y && p.x < x + width && p.y < y + height; }
const UiNode* UiTree::Get(UiId id) const
{ auto it = nodes_.find(id); return it == nodes_.end() ? nullptr : &it->second; }
std::vector<UiId> UiTree::Ids() const
{ std::vector<UiId> result; for (const auto& [id,node] : nodes_) { (void)node; result.push_back(id); } return result; }
UiId UiTree::Create(UiKind kind, UiId parent)
{
    unsigned depth = 0;
    for (auto p = Get(parent); p; p = Get(p->parent)) if (++depth >= 128) return 0;
    if ((parent && !Get(parent)) || nodes_.size() >= 10000 || next_ == UINT64_MAX
        || static_cast<int>(kind) < 0 || static_cast<int>(kind) > 8) return 0;
    const UiId id = next_++;
    UiNode node; node.id = id; node.parent = parent; node.kind = kind;
    nodes_.emplace(id, node);
    (parent ? nodes_.at(parent).children : roots_).push_back(id);
    MarkLayoutDirty(id);
    return id;
}
void UiTree::RemoveBranch(UiId id)
{
    const auto children = nodes_.at(id).children;
    for (auto child : children) RemoveBranch(child);
    if (focus_ == id) { focus_ = 0;CancelComposition();cursor_ = anchor_ = 0;++inputEpoch_; }
    if (capture_ == id) {capture_ = 0;draggingScroll_=false;scrollDragOffset_=sliderDragOffset_=0;}
    if (hover_ == id) hover_ = 0;
    fades_.erase(id);
    moves_.erase(id); colors_.erase(id);
    layoutCache_.erase(id);dirtyNodes_.erase(id);
    std::erase_if(events_, [id](const auto& event) { return event.target == id; });
    nodes_.erase(id);
}
bool UiTree::Remove(UiId id)
{
    const auto* node = Get(id); if (!node) return false;
    const auto parent=node->parent;
    std::erase(parent ? nodes_.at(parent).children : roots_, id);
    RemoveBranch(id);if(parent)MarkLayoutDirty(parent);return true;
}
bool UiTree::Reparent(UiId id, UiId parent)
{
    auto* node = Get(id); if (!node || (parent && !Get(parent))) return false;
    if(DescendantOf(parent,id))return false;
    unsigned depth = 0;
    for (UiId p = parent; p; p = nodes_.at(p).parent) ++depth;
    const auto height = [&](auto&& self, UiId p) -> unsigned {
        unsigned h = 1; for (auto child : nodes_.at(p).children) h = std::max(h, 1 + self(self, child)); return h;
    };
    if (depth + height(height, id) > 128) return false;
    const auto oldParent=node->parent;
    std::erase(oldParent ? nodes_.at(oldParent).children : roots_, id);
    nodes_.at(id).parent = parent;
    (parent ? nodes_.at(parent).children : roots_).push_back(id);
    MarkLayoutDirty(id);if(oldParent)MarkLayoutDirty(oldParent);ReconcileInput();return true;
}
bool UiTree::SetStyle(UiId id, const UiStyle& style)
{ if (!Get(id) || !ValidStyle(style)) return false;moves_.erase(id);if(nodes_.at(id).style==style)return true;nodes_.at(id).style = style;MarkLayoutDirty(id);return true; }
bool UiTree::SetText(UiId id, std::string_view text)
{
    std::u32string decoded;
    if (!Get(id) || text.size() > 65536 || !DecodeUtf8(text, decoded)) return false;
    nodes_.at(id).text = text;
    if (focus_ == id) {cursor_ = anchor_ = decoded.size();CancelComposition();++inputEpoch_;}
    return true; // 当前布局使用显式尺寸；文字变化由字形布局消费。
}
bool UiTree::SetImage(UiId id, std::string assetId)
{ if (!Get(id)) return false; nodes_.at(id).imageAsset = std::move(assetId); return true; }
bool UiTree::SetRange(UiId id, double minimum, double maximum)
{
    if (!Get(id) || !std::isfinite(minimum) || !std::isfinite(maximum) || maximum < minimum || !std::isfinite(maximum-minimum)) return false;
    auto& node = nodes_.at(id); node.minimum = minimum; node.maximum = maximum;
    return SetValue(id, node.value, false);
}
bool UiTree::SetValue(UiId id, double value, bool notify)
{
    if (!Get(id) || !std::isfinite(value)) return false;
    auto& node = nodes_.at(id); value = std::clamp(value, node.minimum, node.maximum);
    if (node.step > 0) value = std::clamp(node.minimum + std::round((value-node.minimum)/node.step)*node.step,node.minimum,node.maximum);
    if (node.kind == UiKind::Checkbox) value = value == 0 ? 0 : 1;
    if (node.value != value) { node.value = value; if (notify) events_.push_back({ UiEventKind::ValueChanged, id, value, {} }); }
    return true;
}
bool UiTree::SetStep(UiId id,double step)
{ if(!Get(id)||!std::isfinite(step)||step<0)return false;nodes_.at(id).step=step;return SetValue(id,nodes_.at(id).value,false); }
bool UiTree::SetColor(UiId id,UiColor color)
{
    if(!Get(id))return false;
    for(float value:{color.r,color.g,color.b,color.a})if(!std::isfinite(value)||value<0||value>1)return false;
    colors_.erase(id);nodes_.at(id).color=color;return true;
}
bool UiTree::SetCanvasReference(float width,float height)
{
    if(!std::isfinite(width)||!std::isfinite(height)||width<0||height<0||(width==0)!=(height==0)
        ||(width>0&&(width<1||height<1||width>1000000||height>1000000)))return false;
    if(referenceWidth_==width&&referenceHeight_==height)return true;
    referenceWidth_=width;referenceHeight_=height;dirty_=true;return true;
}
bool UiTree::SetBindings(UiId id,std::span<const UiEventBinding> bindings)
{
    if(!Get(id)||bindings.size()>6)return false;
    unsigned events=0;
    for(const auto& binding:bindings)
    {
        const auto event=static_cast<int>(binding.event);
        if(event<0||event>5||!binding.entity||!BindingName(binding.script)||!BindingName(binding.action)||(events&(1u<<event)))return false;
        events|=1u<<event;
    }
    std::vector<UiEventBinding> copy(bindings.begin(),bindings.end());nodes_.at(id).bindings=std::move(copy);return true;
}
bool UiTree::Effective(UiId id, bool requireEnabled) const
{
    for (auto* node = Get(id); node; node = Get(node->parent))
        if (!node->visible || (requireEnabled && !node->enabled)) return false;
    return Get(id) != nullptr;
}
bool UiTree::SetVisible(UiId id, bool visible)
{
    if (!Get(id)) return false;if(nodes_.at(id).visible==visible)return true;nodes_.at(id).visible = visible;
    ReconcileInput();
    if(visible)MarkLayoutDirty(id);else if(nodes_.at(id).parent)MarkLayoutDirty(nodes_.at(id).parent);
    return true;
}
bool UiTree::SetEnabled(UiId id, bool enabled)
{
    if (!Get(id)) return false; nodes_.at(id).enabled = enabled;
    ReconcileInput();
    return true;
}
bool UiTree::SetBlocksPointer(UiId id,bool blocks)
{
    if(!Get(id))return false;nodes_.at(id).blocksPointer=blocks;ReconcileInput();return true;
}
void UiTree::ReconcileInput()
{
    const auto valid=[&](UiId id){const auto* node=Get(id);return node&&Effective(id,true)&&(Interactive(node->kind)||node->blocksPointer);};
    if(!valid(focus_))Focus(0);
    if(!valid(capture_)){capture_=0;draggingScroll_=false;scrollDragOffset_=sliderDragOffset_=0;}
    if(!valid(hover_))PointerLeave();
}
bool UiTree::DescendantOf(UiId id,UiId ancestor)const
{
    for(auto* node=Get(id);node;node=Get(node->parent))if(node->id==ancestor)return true;
    return false;
}
void UiTree::MarkLayoutDirty(UiId id)
{
    if(dirty_||!Get(id)||!Effective(id,false))return;
    // 流式父容器会改变兄弟位置；滚动容器还依赖内容范围。绝对父容器不改变子树尺寸。
    while(const auto parent=nodes_.at(id).parent)
    {
        const auto& node=nodes_.at(parent);
        if(layoutCache_.contains(parent)&&node.style.layout==UiLayout::Absolute&&node.kind!=UiKind::ScrollView)break;
        id=parent;
    }
    for(auto* node=Get(id);node;node=Get(node->parent))if(dirtyNodes_.contains(node->id))return;
    // 推迟合并后加入的祖先，避免对一批独立节点逐次扫描整个集合。
    dirtyNodes_.insert(id);
}
void UiTree::RefreshContentHeight(UiId id)
{
    auto& node=nodes_.at(id);const float top=layoutCache_.at(id).rectangle.y+node.style.padding;node.contentHeight=0;
    for(const auto child:node.children)
    {
        const auto found=layoutCache_.find(child);if(!nodes_.at(child).visible||found==layoutCache_.end())continue;
        const auto rectangle=found->second.rectangle;
        node.contentHeight=std::max(node.contentHeight,rectangle.y+rectangle.height-top+node.scroll);
    }
}
void UiTree::ArrangeBranch(UiId id)
{
    const auto parent=nodes_.at(id).parent;
    if(!parent){const UiRect canvas{0,0,width_/canvasScale_,height_/canvasScale_};Arrange(id,canvas,canvas);return;}
    const auto& node=nodes_.at(parent);const auto& cache=layoutCache_.at(parent);const auto& style=node.style;
    UiRect content{cache.rectangle.x+style.padding,cache.rectangle.y+style.padding,
        std::max(0.0f,cache.rectangle.width-2*style.padding),std::max(0.0f,cache.rectangle.height-2*style.padding)};
    if(style.layout!=UiLayout::Absolute)for(const auto sibling:node.children)
    {
        if(sibling==id)break;if(!nodes_.at(sibling).visible)continue;
        const auto rectangle=layoutCache_.at(sibling).rectangle;
        if(style.layout==UiLayout::Horizontal)content.x+=rectangle.width+style.spacing;
        else content.y+=rectangle.height+style.spacing;
    }
    if(node.kind==UiKind::ScrollView)content.y-=node.scroll;
    const auto clip=style.clip||node.kind==UiKind::ScrollView?Intersect(cache.clip,cache.rectangle):cache.clip;
    Arrange(id,content,clip);
}
void UiTree::Arrange(UiId id, UiRect parent, UiRect clip)
{
    auto& node = nodes_.at(id); const auto& s = node.style;
    auto& cache=layoutCache_[id];
    cache.rectangle = {parent.x + parent.width * s.anchorMin.x + s.offset.x - s.pivot.x * s.size.x,
        parent.y + parent.height * s.anchorMin.y + s.offset.y - s.pivot.y * s.size.y,
        std::max(0.0f, parent.width * (s.anchorMax.x - s.anchorMin.x) + s.size.x),
        std::max(0.0f, parent.height * (s.anchorMax.y - s.anchorMin.y) + s.size.y)};
    cache.clip=clip;node.rectangle=cache.rectangle;node.clipping=clip;
    for(auto* rectangle:{&node.rectangle,&node.clipping})
    {rectangle->x*=canvasScale_;rectangle->y*=canvasScale_;rectangle->width*=canvasScale_;rectangle->height*=canvasScale_;}
    ++layoutStatistics_.arrangedNodes;layoutTouched_.push_back(id);
    const auto childClip = s.clip || node.kind == UiKind::ScrollView ? Intersect(clip, cache.rectangle) : clip;
    UiRect content{ cache.rectangle.x + s.padding, cache.rectangle.y + s.padding,
        std::max(0.0f, cache.rectangle.width - 2 * s.padding), std::max(0.0f, cache.rectangle.height - 2 * s.padding) };
    float cursor = 0;
    node.contentHeight = 0;
    for (auto childId : node.children)
    {
        auto& child = nodes_.at(childId); if (!child.visible) continue;
        UiRect base = content;
        if (s.layout == UiLayout::Horizontal) base.x += cursor;
        if (s.layout == UiLayout::Vertical) base.y += cursor;
        if (node.kind == UiKind::ScrollView) base.y -= node.scroll;
        Arrange(childId, base, childClip);
        const auto rectangle=layoutCache_.at(childId).rectangle;
        if (s.layout == UiLayout::Horizontal) cursor += rectangle.width + s.spacing;
        if (s.layout == UiLayout::Vertical) cursor += rectangle.height + s.spacing;
        node.contentHeight = std::max(node.contentHeight,
            rectangle.y + rectangle.height - content.y + node.scroll);
    }
}
void UiTree::Layout(float width, float height)
{
    layoutStatistics_={};layoutTouched_.clear();
    if (!std::isfinite(width) || !std::isfinite(height) || width < 0 || height < 0) return;
    const bool full=dirty_||width!=width_||height!=height_;
    if(!full&&dirtyNodes_.empty())return;
    layoutStatistics_.fullPass=full;
    width_ = width; height_ = height;
    canvasScale_ = referenceWidth_ > 0 && width > 0 && height > 0 ? std::min(width/referenceWidth_,height/referenceHeight_) : 1;
    const auto covered=[&](UiId id,const std::set<UiId>& pending){
        for(auto* node=Get(nodes_.at(id).parent);node;node=Get(node->parent))if(pending.contains(node->id))return true;
        return false;};
    std::vector<UiId> branches;
    if(full)branches=roots_;
    else for(const auto id:dirtyNodes_)if(Get(id)&&!covered(id,dirtyNodes_))branches.push_back(id);
    for(const auto branch:branches)if(Get(branch)&&Effective(branch,false)){++layoutStatistics_.subtreePasses;ArrangeBranch(branch);}
    std::set<UiId> parents;
    for(const auto id:layoutTouched_)if(nodes_.at(id).parent)parents.insert(nodes_.at(id).parent);
    for(const auto parent:parents)if(layoutCache_.contains(parent))RefreshContentHeight(parent);
    std::set<UiId> clamped;
    for(const auto id:layoutTouched_)
    {
        auto& node=nodes_.at(id);
        if (node.kind != UiKind::ScrollView) continue;
        const float maximum = std::max(0.0f,node.contentHeight-layoutCache_.at(id).rectangle.height+2*node.style.padding);
        const float next = std::clamp(node.scroll,0.0f,maximum);
        if(next==node.scroll)continue;node.scroll=next;++layoutStatistics_.scrollClamps;
        clamped.insert(id);
    }
    // 先完成所有滚动范围夹取，再合并嵌套分支重排，每个节点至多再排列一次。
    for(const auto branch:clamped)if(!covered(branch,clamped)){++layoutStatistics_.subtreePasses;ArrangeBranch(branch);}
    dirtyNodes_.clear();dirty_=false;
}
UiId UiTree::Hit(UiPoint point)const{return UiEventSystem::HitTarget(*this,point);}
void UiTree::SetSliderFromPointer(UiId id, UiPoint point)
{
    auto& node = nodes_.at(id);
    const double ratio=UiSliderRatioAt(node.rectangle,canvasScale_,point.x-sliderDragOffset_);
    SetValue(id, node.minimum + (node.maximum - node.minimum) * ratio);
}
bool UiTree::PointerMove(UiPoint point)
{
    if(!Finite(point))return false;
    const UiId hit = Hit(point);
    if (hit != hover_)
    {
        if (hover_) events_.push_back({UiEventKind::PointerLeave, hover_, 0, {}});
        hover_ = hit;
        if (hover_) events_.push_back({UiEventKind::PointerEnter, hover_, 0, {}});
    }
    if (capture_ && nodes_.at(capture_).kind == UiKind::Slider) SetSliderFromPointer(capture_, point);
    if(capture_&&draggingScroll_)
    {
        auto& n=nodes_.at(capture_);
        const auto g=MakeUiScrollGeometry(n.rectangle,n.style.padding*canvasScale_,n.contentHeight*canvasScale_,n.scroll*canvasScale_,canvasScale_);
        const float span=g.track.height-g.thumb.height;
        const float ratio=span>0?std::clamp((point.y-g.track.y-scrollDragOffset_)/span,0.0f,1.0f):0;
        const float next=g.maximum*ratio/canvasScale_;
        if(n.scroll!=next){n.scroll=next;MarkLayoutDirty(capture_);Layout(width_,height_);}
    }
    return capture_ || hit;
}
bool UiTree::PointerDown(UiPoint point)
{
    if(!Finite(point))return false;
    if(capture_)return true;
    PointerMove(point); capture_ = Hit(point);
    draggingScroll_=false;scrollDragOffset_=sliderDragOffset_=0;
    for(UiId id=capture_;id;id=nodes_.at(id).parent)
    {
        const auto& n=nodes_.at(id);
        if(n.kind==UiKind::ScrollView)
        {
            const auto g=MakeUiScrollGeometry(n.rectangle,n.style.padding*canvasScale_,n.contentHeight*canvasScale_,n.scroll*canvasScale_,canvasScale_);
            if(g.scrollable&&g.track.Contains(point)){capture_=id;draggingScroll_=true;scrollDragOffset_=g.thumb.Contains(point)?point.y-g.thumb.y:g.thumb.height/2;PointerMove(point);break;}
        }
    }
    if (!capture_) { Focus(0); return false; }
    Focus(capture_);
    if (nodes_.at(capture_).kind == UiKind::Slider)
    {const auto& n=nodes_.at(capture_);const auto g=MakeUiSliderGeometry(n.rectangle,canvasScale_,n.value,n.minimum,n.maximum);sliderDragOffset_=g.thumb.Contains(point)?point.x-g.thumb.x-g.thumb.width/2:0;SetSliderFromPointer(capture_,point);}
    return true;
}
bool UiTree::PointerUp(UiPoint point)
{
    if(!Finite(point))return false;
    PointerMove(point);
    const UiId target = capture_;capture_ = 0;draggingScroll_=false;scrollDragOffset_=sliderDragOffset_=0;
    if (!target || !Get(target)) return Hit(point) != 0;
    const auto& node = nodes_.at(target);
    if (Effective(target, true) && Hit(point)==target)
    {
        if (node.kind == UiKind::Checkbox) SetValue(target, node.value == 0 ? 1 : 0);
        if (node.kind == UiKind::Button || node.kind == UiKind::Checkbox)
            events_.push_back({ UiEventKind::Click, target, node.value, {} });
    }
    return true;
}
bool UiTree::Wheel(UiPoint point, float delta)
{
    if (!Finite(point)||!std::isfinite(delta)) return false;
    const auto route=UiEventSystem::Route(*this,point);
    for (UiId id:route.path)
    {
        auto& node = nodes_.at(id);
        if (node.kind != UiKind::ScrollView) continue;
        const float maximum = std::max(0.0f, node.contentHeight - node.rectangle.height/canvasScale_ + 2 * node.style.padding);
        const float next = std::clamp(node.scroll - delta * 32, 0.0f, maximum);
        if (next != node.scroll) { node.scroll = next;MarkLayoutDirty(id);Layout(width_, height_); return true; }
    }
    return route.target!=0;
}
bool UiTree::PointerLeave()
{
    const auto previous=hover_;hover_=0;
    if(previous&&Get(previous))events_.push_back({UiEventKind::PointerLeave,previous,0,{}});
    return previous!=0;
}
void UiTree::CancelPointerInput(){capture_=0;draggingScroll_=false;scrollDragOffset_=sliderDragOffset_=0;PointerLeave();}
void UiTree::CancelInput() { CancelPointerInput();Focus(0);CancelComposition(); }
bool UiTree::Focus(UiId id)
{
    if (id && (!Effective(id, true) || (!Interactive(nodes_.at(id).kind)&&!nodes_.at(id).blocksPointer))) return false;
    if (focus_ == id) return true;
    if(focus_) events_.push_back({UiEventKind::FocusChanged,focus_,0,{}});
    focus_ = id;CancelComposition();cursor_ = anchor_ = 0;++inputEpoch_;
    if (id) { std::u32string text; DecodeUtf8(nodes_.at(id).text, text); cursor_ = anchor_ = text.size(); }
    if(id) events_.push_back({ UiEventKind::FocusChanged, id, 1, {} });
    return true;
}
bool UiTree::ReplaceSelection(std::u32string_view inserted)
{
    auto* node = Get(focus_); if (!node || node->kind != UiKind::TextInput) return false;
    std::u32string text; if (!DecodeUtf8(node->text, text)) return false;
    const auto first = std::min(cursor_, anchor_), last = std::max(cursor_, anchor_);
    text.replace(first, last - first, inserted);
    std::string encoded; if (!EncodeUtf8(text, encoded) || encoded.size() > 65536) return false;
    const bool changed=nodes_.at(focus_).text!=encoded;nodes_.at(focus_).text = encoded;
    cursor_ = anchor_ = first + inserted.size();CancelComposition();
    if(changed)events_.push_back({ UiEventKind::TextChanged, focus_, 0, encoded });
    return true;
}
bool UiTree::InsertText(std::string_view utf8)
{
    std::u32string text; if (!DecodeUtf8(utf8, text)) return false;
    std::erase_if(text, [](char32_t c) { return c < 32 || c == 127; });
    if (!utf8.empty() && text.empty()) return false;
    return ReplaceSelection(text);
}
bool UiTree::SetComposition(std::string_view utf8,std::size_t caret)
{
    std::u32string text;
    if (!Get(focus_) || nodes_.at(focus_).kind != UiKind::TextInput || utf8.size()>65536 || !DecodeUtf8(utf8, text)) return false;
    if(std::any_of(text.begin(),text.end(),[](char32_t c){return c<32||c==127;}))return false;
    if(caret==std::string::npos)caret=text.size();if(caret>text.size())return false;
    std::u32string preview;DecodeUtf8(nodes_.at(focus_).text,preview);preview.replace(std::min(cursor_,anchor_),std::max(cursor_,anchor_)-std::min(cursor_,anchor_),text);
    std::string encoded;if(!EncodeUtf8(preview,encoded)||encoded.size()>65536)return false;
    composition_ = utf8;compositionCursor_=caret;return true;
}
bool UiTree::EditKey(unsigned key, bool shift, bool control)
{
    if(key==9)
    {
        std::vector<UiId> targets;
        for(const auto& visual:Visuals())if(Effective(visual.id,true)&&Interactive(visual.kind))targets.push_back(visual.id);
        if(targets.empty())return false;
        const auto found=std::find(targets.begin(),targets.end(),focus_);
        const auto index=found==targets.end()?0:static_cast<std::size_t>(found-targets.begin());
        return Focus(targets[found==targets.end()?0:shift?(index+targets.size()-1)%targets.size():(index+1)%targets.size()]);
    }
    const auto* node = Get(focus_); if (!node) return false;
    if (node->kind == UiKind::Button && (key == 13 || key == 32))
    { events_.push_back({ UiEventKind::Click, focus_, 0, {} }); return true; }
    if (node->kind == UiKind::Checkbox && key == 32)
    {SetValue(focus_,node->value==0?1:0);events_.push_back({UiEventKind::Click,focus_,nodes_.at(focus_).value,{}});return true;}
    if (node->kind == UiKind::Slider && (key==37||key==39||key==36||key==35))
    {
        const double step=node->step>0?node->step:(node->maximum-node->minimum)/100;
        return SetValue(focus_,key==36?node->minimum:key==35?node->maximum:node->value+(key==39?step:-step));
    }
    if (node->kind != UiKind::TextInput) return false;
    std::u32string text; DecodeUtf8(node->text, text);
    if (control && key == 'A') { anchor_ = 0;cursor_ = text.size();CancelComposition();return true; }
    if (key == 8 || key == 46)
    {
        if (cursor_ == anchor_)
        {
            if (key == 8 && cursor_) --anchor_;
            else if (key == 46 && cursor_ < text.size()) ++anchor_;
        }
        return ReplaceSelection(U"");
    }
    if (key == 37) { if(!shift&&cursor_!=anchor_)cursor_=std::min(cursor_,anchor_);else if (cursor_) --cursor_; }
    else if (key == 39) { if(!shift&&cursor_!=anchor_)cursor_=std::max(cursor_,anchor_);else if (cursor_ < text.size()) ++cursor_; }
    else if (key == 36) cursor_ = 0;
    else if (key == 35) cursor_ = text.size();
    else if (key == 27) { CancelComposition();return true; }
    else return false;
    if (!shift) anchor_ = cursor_;
    return true;
}
std::string UiTree::SelectedText() const
{
    const auto* node = Get(focus_); if (!node || node->kind != UiKind::TextInput) return {};
    std::u32string text; DecodeUtf8(node->text, text);
    std::string output; EncodeUtf8(std::u32string_view(text).substr(std::min(cursor_, anchor_),
        std::max(cursor_, anchor_) - std::min(cursor_, anchor_)), output); return output;
}
bool UiTree::SelectText(std::size_t caret, std::size_t anchor)
{
    const auto* n = Get(focus_); if (!n || n->kind != UiKind::TextInput) return false;
    std::u32string text; DecodeUtf8(n->text, text);
    cursor_ = std::min(caret, text.size()); anchor_ = std::min(anchor, text.size());
    CancelComposition();return true;
}
std::vector<UiEvent> UiTree::TakeEvents() { auto result = std::move(events_); events_.clear(); return result; }
void UiTree::Flatten(UiId id,std::vector<UiVisual>& output,std::uint32_t depth) const
{
    const auto& n = nodes_.at(id); if (!n.visible) return;
    output.push_back({ n.id,n.kind,n.rectangle,n.clipping,n.text,n.imageAsset,
        focus_ == id ? composition_ : std::string{}, n.value,n.opacity,hover_ == id,capture_ == id,focus_ == id,Effective(id,true),
        focus_ == id ? cursor_ : 0, focus_ == id ? anchor_ : 0 });
    for (auto p = Get(n.parent); p; p = Get(p->parent)) output.back().opacity *= p->opacity;
    output.back().color=n.color;output.back().minimum=n.minimum;output.back().maximum=n.maximum;
    output.back().scroll=n.scroll*canvasScale_;output.back().contentHeight=n.contentHeight*canvasScale_;output.back().fontSize=18*canvasScale_;
    output.back().compositionCaret=focus_==id?compositionCursor_:std::string::npos;
    output.back().parent=n.parent;output.back().depth=depth;output.back().padding=n.style.padding*canvasScale_;
    for (auto child : n.children) Flatten(child, output,depth+1);
}
std::vector<UiVisual> UiTree::Visuals() const
{ std::vector<UiVisual> output; for (auto root : roots_) Flatten(root, output); return output; }
void UiTree::FadeTo(UiId id, float opacity, double duration)
{
    if (!Get(id) || !std::isfinite(opacity) || !std::isfinite(duration) || duration < 0) return;
    opacity=std::clamp(opacity,0.0f,1.0f);auto& current=nodes_.at(id).opacity;
    if(duration==0||current==opacity){fades_.erase(id);current=opacity;return;}
    const auto old=fades_.find(id);if(old!=fades_.end()&&old->second.to==opacity&&old->second.duration==duration)return;
    fades_[id] = {current,opacity,0,duration};
}
void UiTree::MoveTo(UiId id,UiPoint offset,double duration)
{
    if(!Get(id)||!std::isfinite(offset.x)||!std::isfinite(offset.y)||std::abs(offset.x)>1000000||std::abs(offset.y)>1000000||!std::isfinite(duration)||duration<0)return;
    auto& current=nodes_.at(id).style.offset;
    if(duration==0||current==offset){moves_.erase(id);if(current!=offset){current=offset;MarkLayoutDirty(id);}return;}
    const auto old=moves_.find(id);if(old!=moves_.end()&&old->second.to==offset&&old->second.duration==duration)return;
    moves_[id]={current,offset,0,duration};
}
void UiTree::ColorTo(UiId id,UiColor color,double duration)
{
    if(!Get(id)||!std::isfinite(duration)||duration<0)return;
    for(float v:{color.r,color.g,color.b,color.a})if(!std::isfinite(v)||v<0||v>1)return;
    auto& current=nodes_.at(id).color;
    if(duration==0||current==color){colors_.erase(id);current=color;return;}
    const auto old=colors_.find(id);if(old!=colors_.end()&&old->second.to==color&&old->second.duration==duration)return;
    colors_[id]={current,color,0,duration};
}
bool UiTree::CancelTransitions(UiId id)
{
    if(!Get(id))return false;
    fades_.erase(id);moves_.erase(id);colors_.erase(id);return true;
}
void UiTree::Update(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0) return;
    for (auto it = fades_.begin(); it != fades_.end();)
    {
        auto& f = it->second;const float t=f.Advance(seconds);
        nodes_.at(it->first).opacity = t>=1?f.to:f.from + (f.to - f.from) * t;
        if (t >= 1) it = fades_.erase(it); else ++it;
    }
    for(auto it=moves_.begin();it!=moves_.end();)
    {
        auto& m=it->second;const float t=m.Advance(seconds);
        nodes_.at(it->first).style.offset=t>=1?m.to:UiPoint{m.from.x+(m.to.x-m.from.x)*t,m.from.y+(m.to.y-m.from.y)*t};MarkLayoutDirty(it->first);
        if(t>=1)it=moves_.erase(it);else ++it;
    }
    for(auto it=colors_.begin();it!=colors_.end();)
    {
        auto& m=it->second;const float t=m.Advance(seconds);
        nodes_.at(it->first).color=t>=1?m.to:UiColor{m.from.r+(m.to.r-m.from.r)*t,m.from.g+(m.to.g-m.from.g)*t,m.from.b+(m.to.b-m.from.b)*t,m.from.a+(m.to.a-m.from.a)*t};
        if(t>=1)it=colors_.erase(it);else ++it;
    }
}
std::string UiTree::SaveJson() const
{
    nlohmann::json result{{"version",1},{"nodes",nlohmann::json::array()}};
    result["referenceSize"]={referenceWidth_,referenceHeight_};
    result["nextId"]=next_;
    std::vector<UiId> order;
    const auto visit = [&](auto&& self, UiId id) -> void { order.push_back(id); for (auto c : nodes_.at(id).children) self(self,c); };
    for(auto root : roots_) visit(visit,root);
    for (UiId id : order)
    {
        const auto& n = nodes_.at(id); const auto& s = n.style;
        result["nodes"].push_back({{"id",id},{"parent",n.parent},{"kind",static_cast<int>(n.kind)},
            {"text",n.text},{"image",n.imageAsset},{"value",n.value},{"min",n.minimum},{"max",n.maximum},
            {"step",n.step},{"color",{n.color.r,n.color.g,n.color.b,n.color.a}},
            {"visible",n.visible},{"enabled",n.enabled},{"blocksPointer",n.blocksPointer},{"opacity",n.opacity},{"scroll",n.scroll},
            {"style",{s.anchorMin.x,s.anchorMin.y,s.anchorMax.x,s.anchorMax.y,s.pivot.x,s.pivot.y,
                s.offset.x,s.offset.y,s.size.x,s.size.y,s.padding,s.spacing}},
            {"layout",static_cast<int>(s.layout)},{"clip",s.clip}});
        if(!n.bindings.empty())
        {
            auto& bindings=result["nodes"].back()["bindings"];bindings=nlohmann::json::array();
            for(const auto& binding:n.bindings)bindings.push_back({{"event",static_cast<int>(binding.event)},{"entity",binding.entity},{"script",binding.script},{"action",binding.action}});
        }
    }
    return result.dump(2);
}
bool UiTree::LoadJson(std::string_view text, std::string& error)
{
    try
    {
        if (text.size() > 8 * 1024 * 1024) throw std::runtime_error("UI.TooLarge");
        const auto document = nlohmann::json::parse(text);
        if (document.at("version") != 1 || !document.at("nodes").is_array() || document.at("nodes").size() > 10000)
            throw std::runtime_error("UI.UnsupportedVersion");
        UiTree next;
        if(document.contains("referenceSize"))
        {
            const auto size=document.at("referenceSize").get<std::vector<float>>();
            if(size.size()!=2||!next.SetCanvasReference(size[0],size[1]))throw std::runtime_error("UI.InvalidStyle");
        }
        for (const auto& value : document.at("nodes"))
        {
            if (!value.at("id").is_number_unsigned() || !value.at("parent").is_number_unsigned())
                throw std::runtime_error("UI.InvalidHierarchy");
            const UiId id = value.at("id").get<UiId>(), parent = value.at("parent").get<UiId>();
            const int kind = value.at("kind").get<int>();
            if (!id || id == UINT64_MAX || next.Get(id) || (parent && !next.Get(parent)) || kind < 0 || kind > 8)
                throw std::runtime_error("UI.InvalidHierarchy");
            next.next_ = id;
            if (next.Create(static_cast<UiKind>(kind),parent) != id) throw std::runtime_error("UI.InvalidHierarchy");
            auto& n = next.nodes_.at(id);
            const auto a = value.at("style").get<std::vector<float>>();
            if (a.size() != 12) throw std::runtime_error("UI.InvalidStyle");
            UiStyle s{{a[0],a[1]},{a[2],a[3]},{a[4],a[5]},{a[6],a[7]},{a[8],a[9]},
                static_cast<UiLayout>(value.at("layout").get<int>()),a[10],a[11],value.at("clip").get<bool>()};
            if (!next.SetStyle(id,s) || !next.SetText(id,value.at("text").get<std::string>())
                || !next.SetRange(id,value.at("min"),value.at("max")) || !next.SetValue(id,value.at("value"),false))
                throw std::runtime_error("UI.InvalidStyle");
            n.imageAsset = value.at("image").get<std::string>();
            if(value.contains("step")&&!next.SetStep(id,value.at("step")))throw std::runtime_error("UI.InvalidStyle");
            if(value.contains("color"))
            {
                const auto color=value.at("color").get<std::vector<float>>();
                if(color.size()!=4||!next.SetColor(id,{color[0],color[1],color[2],color[3]}))throw std::runtime_error("UI.InvalidStyle");
            }
            n.visible = value.at("visible"); n.enabled = value.at("enabled"); n.opacity = value.at("opacity"); n.scroll = value.at("scroll");
            n.blocksPointer=value.value("blocksPointer",false);
            if(value.contains("bindings"))
            {
                const auto& entries=value.at("bindings");if(!entries.is_array()||entries.size()>6)throw std::runtime_error("UI.InvalidBinding");
                std::vector<UiEventBinding> bindings;
                for(const auto& entry:entries)
                {
                    if(!entry.at("event").is_number_integer()||!entry.at("entity").is_number_unsigned())throw std::runtime_error("UI.InvalidBinding");
                    const auto event=entry.at("event").get<std::int64_t>();if(event<0||event>5)throw std::runtime_error("UI.InvalidBinding");
                    bindings.push_back({static_cast<UiEventKind>(event),entry.at("entity").get<std::uint64_t>(),entry.at("script").get<std::string>(),entry.at("action").get<std::string>()});
                }
                if(!next.SetBindings(id,bindings))throw std::runtime_error("UI.InvalidBinding");
            }
            if (!std::isfinite(n.opacity) || n.opacity < 0 || n.opacity > 1 || !std::isfinite(n.scroll) || n.scroll < 0)
                throw std::runtime_error("UI.InvalidStyle");
        }
        next.next_ = next.nodes_.empty() ? 1 : next.nodes_.rbegin()->first + 1;
        if(document.contains("nextId"))
        {
            if(!document.at("nextId").is_number_unsigned())throw std::runtime_error("UI.InvalidHierarchy");
            const auto value=document.at("nextId").get<UiId>();
            if(value<next.next_)throw std::runtime_error("UI.InvalidHierarchy");
            next.next_=value;
        }
        next.inputEpoch_=inputEpoch_+1;*this = std::move(next);error.clear();return true;
    }
    catch (const std::exception& e) { error = std::string_view(e.what()).starts_with("UI.") ? e.what() : "UI.InvalidDocument"; return false; }
}
}

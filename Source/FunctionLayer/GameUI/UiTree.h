#pragma once
#include "UiTransition.h"
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace se
{
using UiId = std::uint64_t;
struct UiPoint { float x = 0, y = 0;bool operator==(const UiPoint&)const=default; };
struct UiColor { float r = 1, g = 1, b = 1, a = 1;bool operator==(const UiColor&)const=default; };
struct UiRect
{
    float x = 0, y = 0, width = 0, height = 0;
    bool Contains(UiPoint point) const noexcept;
};
enum class UiKind { Panel, Label, Image, Button, Checkbox, Slider, ProgressBar, ScrollView, TextInput };
enum class UiLayout { Absolute, Horizontal, Vertical };
enum class UiEventKind { Click, ValueChanged, TextChanged, FocusChanged, PointerEnter, PointerLeave };
struct UiEventBinding
{
    UiEventKind event=UiEventKind::Click;
    std::uint64_t entity=0;
    std::string script,action;
    bool operator==(const UiEventBinding&)const=default;
};
struct UiStyle
{
    UiPoint anchorMin{}, anchorMax{}, pivot{}, offset{}, size{ 120, 32 };
    UiLayout layout = UiLayout::Absolute;
    float padding = 0, spacing = 0;
    bool clip = false;
    bool operator==(const UiStyle&)const=default;
};
struct UiNode
{
    UiId id = 0, parent = 0;
    UiKind kind = UiKind::Panel;
    UiStyle style;
    UiRect rectangle{}, clipping{};
    std::string text, imageAsset;
    double value = 0, minimum = 0, maximum = 1;
    double step = 0;
    UiColor color;
    float scroll = 0, contentHeight = 0, opacity = 1;
    bool visible = true, enabled = true, blocksPointer = false;
    std::vector<UiId> children;
    std::vector<UiEventBinding> bindings; // 稳定描述；运行时解析属于脚本层。
};
struct UiEvent { UiEventKind kind; UiId target; double value = 0; std::string text; };
struct UiLayoutStatistics {std::size_t arrangedNodes=0,subtreePasses=0,scrollClamps=0;bool fullPass=false;};
// CPU 显示数据；用户的渲染器负责字形图集、顶点提交、裁剪和混合。
struct UiVisual
{
    UiId id;
    UiKind kind;
    UiRect rectangle, clip;
    std::string text, imageAsset, composition;
    double value;
    float opacity;
    bool hovered, pressed, focused, enabled;
    std::size_t caret = 0, selectionAnchor = 0; // Unicode 码点索引，不是 UTF-8 字节。
    UiColor color;
    double minimum = 0, maximum = 1;
    float scroll = 0, contentHeight = 0, fontSize = 18;
    std::size_t compositionCaret=std::string::npos;
    UiId parent=0;
    std::uint32_t depth=0;
    float padding=0;
};

class UiTree
{
public:
    UiId Create(UiKind kind, UiId parent = 0);
    bool Remove(UiId id);
    bool Reparent(UiId id, UiId parent);
    const UiNode* Get(UiId id) const;
    std::vector<UiId> Ids() const;
    bool SetStyle(UiId id, const UiStyle& style);
    bool SetText(UiId id, std::string_view utf8);
    bool SetImage(UiId id, std::string assetId);
    bool SetRange(UiId id, double minimum, double maximum);
    bool SetValue(UiId id, double value, bool notify = true);
    bool SetStep(UiId id, double step);
    bool SetColor(UiId id, UiColor color);
    bool SetBindings(UiId id,std::span<const UiEventBinding> bindings);
    bool SetCanvasReference(float width, float height); // 0,0 使用物理像素，否则等比缩放逻辑尺寸。
    float CanvasScale() const noexcept { return canvasScale_; }
    UiPoint CanvasReference()const noexcept{return {referenceWidth_,referenceHeight_};}
    const UiLayoutStatistics& LayoutStatistics()const noexcept{return layoutStatistics_;}
    bool SetVisible(UiId id, bool visible);
    bool SetEnabled(UiId id, bool enabled);
    bool SetBlocksPointer(UiId id,bool blocks);
    void Layout(float width, float height);
    bool PointerMove(UiPoint point);
    bool PointerDown(UiPoint point);
    bool PointerUp(UiPoint point);
    bool Wheel(UiPoint point, float delta);
    void CancelInput();
    void CancelPointerInput();
    bool PointerLeave();
    UiId Captured()const noexcept{return capture_;}
    UiId Hovered()const noexcept{return hover_;}
    bool HasPointerInput() const noexcept { return capture_ != 0 || hover_ != 0; }
    bool Focus(UiId id);
    UiId Focused() const noexcept { return focus_; }
    std::uint64_t InputEpoch()const noexcept{return inputEpoch_;}
    bool ConsumesKeyboard() const noexcept { return focus_ != 0; }
    bool InsertText(std::string_view utf8);
    bool SetComposition(std::string_view utf8,std::size_t caret=std::string::npos);
    void CancelComposition() { composition_.clear();compositionCursor_=0; }
    bool EditKey(unsigned virtualKey, bool shift, bool control);
    std::string SelectedText() const;
    bool SelectText(std::size_t caret, std::size_t anchor);
    std::vector<UiEvent> TakeEvents();
    std::vector<UiVisual> Visuals() const;
    void FadeTo(UiId id, float opacity, double duration);
    void MoveTo(UiId id, UiPoint offset, double duration);
    void ColorTo(UiId id, UiColor color, double duration);
    bool CancelTransitions(UiId id); // 停在当前值，不跳到目标。
    std::size_t ActiveTransitions()const noexcept{return fades_.size()+moves_.size()+colors_.size();}
    void Update(double seconds);
    std::string SaveJson() const;
    bool LoadJson(std::string_view json, std::string& error);
    std::size_t Count() const noexcept { return nodes_.size(); }
private:
    friend class UiEventSystem;
    bool Effective(UiId id, bool requireEnabled) const;
    UiId Hit(UiPoint point) const;
    void ReconcileInput();
    void Arrange(UiId id, UiRect parent, UiRect clip);
    void ArrangeBranch(UiId id);
    void MarkLayoutDirty(UiId id);
    void RefreshContentHeight(UiId id);
    bool DescendantOf(UiId id,UiId ancestor)const;
    void Flatten(UiId id, std::vector<UiVisual>& output,std::uint32_t depth=0) const;
    bool ReplaceSelection(std::u32string_view text);
    void SetSliderFromPointer(UiId id, UiPoint point);
    void RemoveBranch(UiId id);
    std::map<UiId, UiNode> nodes_;
    struct LayoutCache {UiRect rectangle,clip;}; // 设计单位；公开 rectangle/clipping 为物理像素。
    std::map<UiId,LayoutCache> layoutCache_;
    std::set<UiId> dirtyNodes_;
    std::vector<UiId> layoutTouched_;
    UiLayoutStatistics layoutStatistics_;
    std::vector<UiId> roots_;
    std::map<UiId, UiTransition<float>> fades_;
    std::map<UiId, UiTransition<UiPoint>> moves_;
    std::map<UiId, UiTransition<UiColor>> colors_;
    std::vector<UiEvent> events_;
    UiId next_ = 1, focus_ = 0, capture_ = 0, hover_ = 0;
    std::size_t cursor_ = 0, anchor_ = 0;
    std::size_t compositionCursor_=0;
    std::uint64_t inputEpoch_=1;
    std::string composition_;
    float width_ = 0, height_ = 0;
    float referenceWidth_ = 0, referenceHeight_ = 0, canvasScale_ = 1;
    bool draggingScroll_ = false;
    float scrollDragOffset_=0,sliderDragOffset_=0;
    bool dirty_ = true;
};
}

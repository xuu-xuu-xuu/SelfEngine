#include "FunctionLayer/UI/Widgets/UiWidgets.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace se::ui
{
namespace
{
bool Usable(const UiDocument& document, UiElementHandle element) noexcept
{
    return element.IsValid() && document.Element(element) && document.Visible(element) &&
        document.Enabled(element);
}
} // namespace

UiButton::UiButton(UiDocument& document, UiElementHandle element)
    : document_(&document), element_(element)
{
}

void UiButton::SetHovered(bool hovered)
{
    hovered_ = hovered && Usable(*document_, element_);
}

void UiButton::PointerDown()
{
    if (Usable(*document_, element_))
    {
        pressed_ = true;
    }
}

void UiButton::PointerUp(bool inside)
{
    if (pressed_)
    {
        clicked_ = inside && Usable(*document_, element_);
        pressed_ = false;
    }
}

bool UiButton::ConsumeClick()
{
    const bool clicked = clicked_;
    clicked_ = false;
    return clicked;
}

bool UiButton::Hovered() const noexcept
{
    return hovered_;
}

bool UiButton::Pressed() const noexcept
{
    return pressed_;
}

UiCheckbox::UiCheckbox(UiDocument& document, UiElementHandle element)
    : document_(&document), element_(element)
{
}

void UiCheckbox::SetChecked(bool checked)
{
    changed_ = checked != checked_;
    checked_ = checked;
}

void UiCheckbox::Toggle()
{
    if (!Usable(*document_, element_))
    {
        return;
    }
    checked_ = !checked_;
    changed_ = true;
}

void UiCheckbox::PointerUp(bool inside)
{
    if (inside)
    {
        Toggle();
    }
}

bool UiCheckbox::Checked() const noexcept
{
    return checked_;
}

bool UiCheckbox::ConsumeChanged()
{
    const bool changed = changed_;
    changed_ = false;
    return changed;
}

UiCombo::UiCombo(UiDocument& document, UiElementHandle element)
    : document_(&document), element_(element)
{
}

void UiCombo::SetItems(std::vector<std::string> items)
{
    items_ = std::move(items);
    selected_ = items_.empty() ? -1 : std::min(selected_, static_cast<int>(items_.size()) - 1);
    if (items_.empty())
    {
        selected_ = -1;
        open_ = false;
    }
}

void UiCombo::Open()
{
    if (Usable(*document_, element_) && !items_.empty())
    {
        open_ = true;
    }
}

void UiCombo::Close()
{
    open_ = false;
}

void UiCombo::Toggle()
{
    if (open_)
    {
        Close();
    }
    else
    {
        Open();
    }
}

void UiCombo::Select(int index)
{
    if (items_.empty())
    {
        return;
    }
    const int clamped = std::clamp(index, 0, static_cast<int>(items_.size()) - 1);
    if (selected_ != clamped)
    {
        selected_ = clamped;
        changed_ = true;
    }
}

bool UiCombo::HandleKey(UiKeyCode key)
{
    if (!open_)
    {
        if (key == UiKeyEnter || key == UiKeySpace || key == UiKeyDown)
        {
            Open();
            return true;
        }
        return false;
    }
    if (key == UiKeyDown)
    {
        Select(selected_ + 1);
        return true;
    }
    if (key == UiKeyUp)
    {
        Select(selected_ - 1);
        return true;
    }
    if (key == UiKeyEnter)
    {
        Select(selected_);
        open_ = false;
        return true;
    }
    if (key == UiKeyEscape)
    {
        Close();
        return true;
    }
    return false;
}

bool UiCombo::IsOpen() const noexcept
{
    return open_;
}

int UiCombo::Selected() const noexcept
{
    return selected_;
}

const std::vector<std::string>& UiCombo::Items() const noexcept
{
    return items_;
}

bool UiCombo::ConsumeChanged()
{
    const bool changed = changed_;
    changed_ = false;
    return changed;
}

void UiScrollView::SetContentHeight(float height)
{
    contentHeight_ = std::max(0.0f, height);
    offset_ = std::clamp(offset_, 0.0f, MaximumOffset());
}

void UiScrollView::SetViewportHeight(float height)
{
    viewportHeight_ = std::max(0.0f, height);
    offset_ = std::clamp(offset_, 0.0f, MaximumOffset());
}

void UiScrollView::SetOffset(float offset)
{
    offset_ = std::clamp(offset, 0.0f, MaximumOffset());
}

void UiScrollView::ScrollBy(float delta)
{
    SetOffset(offset_ + delta);
}

float UiScrollView::Offset() const noexcept
{
    return offset_;
}

float UiScrollView::ContentHeight() const noexcept
{
    return contentHeight_;
}

float UiScrollView::ViewportHeight() const noexcept
{
    return viewportHeight_;
}

float UiScrollView::MaximumOffset() const noexcept
{
    return std::max(0.0f, contentHeight_ - viewportHeight_);
}

float UiScrollView::ThumbHeight() const noexcept
{
    if (contentHeight_ <= 0.0f || viewportHeight_ <= 0.0f)
    {
        return 0.0f;
    }
    return std::min(viewportHeight_, viewportHeight_ * viewportHeight_ / contentHeight_);
}

float UiScrollView::ThumbY() const noexcept
{
    if (contentHeight_ <= 0.0f || viewportHeight_ <= 0.0f)
    {
        return 0.0f;
    }
    const float track = viewportHeight_ - ThumbHeight();
    return MaximumOffset() <= 0.0f ? 0.0f : offset_ / MaximumOffset() * track;
}

UiListView::UiListView(UiDocument& document, UiElementHandle content)
    : document_(&document), content_(content)
{
}

void UiListView::SetItemCount(int count)
{
    const int next = std::max(0, count);
    if (itemCount_ != next)
    {
        itemCount_ = next;
        dirty_ = true;
    }
    const float maximum = std::max(0.0f, itemCount_ * rowHeight_ - viewportHeight_);
    const float clamped = std::clamp(offset_, 0.0f, maximum);
    if (offset_ != clamped)
    {
        offset_ = clamped;
        dirty_ = true;
    }
}

void UiListView::SetRowHeight(float height)
{
    const float next = std::max(1.0f, height);
    if (rowHeight_ != next)
    {
        rowHeight_ = next;
        dirty_ = true;
    }
}

void UiListView::SetViewportHeight(float height)
{
    const float next = std::max(0.0f, height);
    if (viewportHeight_ != next)
    {
        viewportHeight_ = next;
        dirty_ = true;
    }
}

void UiListView::SetScrollOffset(float offset)
{
    const float next = std::clamp(offset, 0.0f, std::max(0.0f, itemCount_ * rowHeight_ - viewportHeight_));
    if (offset_ != next)
    {
        offset_ = next;
        dirty_ = true;
    }
}

void UiListView::SetRowTextProvider(std::function<std::string(int)> provider)
{
    rowText_ = std::move(provider);
    dirty_ = true;
}

UiVisibleRange UiListView::VisibleRange() const noexcept
{
    UiVisibleRange range;
    if (itemCount_ <= 0 || viewportHeight_ <= 0.0f)
    {
        return range;
    }
    const float first = offset_ / rowHeight_ - static_cast<float>(bufferRows_);
    const float last = (offset_ + viewportHeight_) / rowHeight_ + static_cast<float>(bufferRows_);
    range.first = std::max(0, static_cast<int>(std::floor(first)));
    range.last = std::min(itemCount_, static_cast<int>(std::ceil(last)));
    if (range.last < range.first)
    {
        range.last = range.first;
    }
    return range;
}

void UiListView::Sync()
{
    if (!dirty_)
    {
        return;
    }
    dirty_ = false;
    for (const UiElementHandle row : rows_)
    {
        if (document_->Element(row))
        {
            document_->DestroyElement(row);
        }
    }
    rows_.clear();
    if (!content_.IsValid() || !document_->Element(content_))
    {
        return;
    }
    const UiVisibleRange range = VisibleRange();
    for (int index = range.first; index < range.last; ++index)
    {
        const UiElementHandle row = document_->CreateElement(UiElementKind::Label);
        if (!document_->SetParent(row, content_))
        {
            continue;
        }
        UiStyle style;
        style.position = UiPosition::Absolute;
        style.left = UiLength::Px(0.0f);
        style.top = UiLength::Px(static_cast<float>(index) * rowHeight_ - offset_);
        style.width = UiLength::Percent(100.0f);
        style.height = UiLength::Px(rowHeight_);
        // UiStyle 默认 background 是不透明黑；列表行必须显式透明，否则整行画成黑块。
        style.background = rowBackground_ ? rowBackground_(index) : UiColor{0.0f, 0.0f, 0.0f, 0.0f};
        style.color = rowTextColor_;
        document_->SetStyle(row, style);
        document_->SetText(row, rowText_ ? rowText_(index) : "item " + std::to_string(index));
        rows_.push_back(row);
    }
}

float UiListView::Offset() const noexcept
{
    return offset_;
}

const std::vector<UiElementHandle>& UiListView::Rows() const noexcept
{
    return rows_;
}

void UiTreeView::Clear()
{
    nodes_.clear();
    visible_.clear();
}

int UiTreeView::AddNode(int parent, std::string label, bool expanded)
{
    const int index = static_cast<int>(nodes_.size());
    nodes_.push_back({std::move(label), parent, expanded});
    Rebuild();
    return index;
}

void UiTreeView::SetExpanded(int node, bool expanded)
{
    if (node >= 0 && node < static_cast<int>(nodes_.size()))
    {
        nodes_[static_cast<std::size_t>(node)].expanded = expanded;
        Rebuild();
    }
}

const std::vector<int>& UiTreeView::VisibleFlat() const noexcept
{
    return visible_;
}

int UiTreeView::NodeCount() const noexcept
{
    return static_cast<int>(nodes_.size());
}

void UiTreeView::Rebuild()
{
    visible_.clear();
    const auto visit = [this](auto&& self, int node) -> void {
        if (node < 0 || node >= static_cast<int>(nodes_.size()))
        {
            return;
        }
        visible_.push_back(node);
        if (!nodes_[static_cast<std::size_t>(node)].expanded)
        {
            return;
        }
        for (int child = 0; child < static_cast<int>(nodes_.size()); ++child)
        {
            if (nodes_[static_cast<std::size_t>(child)].parent == node)
            {
                self(self, child);
            }
        }
    };
    for (int index = 0; index < static_cast<int>(nodes_.size()); ++index)
    {
        if (nodes_[static_cast<std::size_t>(index)].parent < 0)
        {
            visit(visit, index);
        }
    }
}

void UiTableView::SetDimensions(int rows, int columns)
{
    rows_ = std::max(0, rows);
    columns_ = std::max(0, columns);
    offset_ = std::clamp(offset_, 0.0f, std::max(0.0f, rows_ * rowHeight_ - viewportHeight_));
}

void UiTableView::SetRowHeight(float height)
{
    rowHeight_ = std::max(1.0f, height);
}

void UiTableView::SetViewportHeight(float height)
{
    viewportHeight_ = std::max(0.0f, height);
}

void UiTableView::SetScrollOffset(float offset)
{
    offset_ = std::clamp(offset, 0.0f, std::max(0.0f, rows_ * rowHeight_ - viewportHeight_));
}

UiVisibleRange UiTableView::VisibleRange() const noexcept
{
    UiVisibleRange range;
    if (rows_ <= 0 || viewportHeight_ <= 0.0f)
    {
        return range;
    }
    range.first = std::max(0, static_cast<int>(std::floor(offset_ / rowHeight_)) - bufferRows_);
    range.last = std::min(rows_, static_cast<int>(std::ceil((offset_ + viewportHeight_) / rowHeight_)) + bufferRows_);
    if (range.last < range.first)
    {
        range.last = range.first;
    }
    return range;
}

int UiTableView::Rows() const noexcept
{
    return rows_;
}

int UiTableView::Columns() const noexcept
{
    return columns_;
}

void UiOverlay::Open(Layer layer, const UiRect& rect, int id)
{
    layers_.push_back({layer, rect, id});
}

bool UiOverlay::CloseTop()
{
    if (layers_.empty())
    {
        return false;
    }
    layers_.pop_back();
    return true;
}

void UiOverlay::CloseAll()
{
    layers_.clear();
}

const UiOverlay::Entry* UiOverlay::Top() const noexcept
{
    return layers_.empty() ? nullptr : &layers_.back();
}

bool UiOverlay::BlocksInput() const noexcept
{
    return !layers_.empty() && layers_.back().layer == Layer::Modal;
}

std::size_t UiOverlay::Count() const noexcept
{
    return layers_.size();
}

bool UiOverlay::PointerDown(float x, float y)
{
    if (layers_.empty())
    {
        return false;
    }
    const Entry& top = layers_.back();
    const bool inside = x >= top.rect.x && y >= top.rect.y && x < top.rect.x + top.rect.width &&
        y < top.rect.y + top.rect.height;
    if (inside)
    {
        return true;
    }
    if (top.layer == Layer::Modal)
    {
        return true;
    }
    layers_.pop_back();
    return true;
}

UiRect UiOverlay::Place(const UiRect& rect, float screenWidth, float screenHeight)
{
    UiRect placed = rect;
    if (placed.x + placed.width > screenWidth)
    {
        placed.x = std::max(0.0f, screenWidth - placed.width);
    }
    if (placed.y + placed.height > screenHeight)
    {
        placed.y = std::max(0.0f, screenHeight - placed.height);
    }
    placed.x = std::max(0.0f, placed.x);
    placed.y = std::max(0.0f, placed.y);
    return placed;
}

void UiDragDrop::Begin(std::string type, std::uint64_t id, const UiRect& ghost)
{
    payload_ = {std::move(type), id};
    ghost_ = ghost;
    target_ = {};
    accepted_ = false;
    state_ = State::Dragging;
}

void UiDragDrop::Over(UiElementHandle target, bool accepted)
{
    if (state_ == State::None)
    {
        return;
    }
    target_ = target;
    accepted_ = accepted;
    state_ = target.IsValid() ? State::Over : State::Dragging;
}

bool UiDragDrop::Drop()
{
    if (state_ != State::Over || !accepted_)
    {
        Cancel();
        return false;
    }
    state_ = State::None;
    target_ = {};
    accepted_ = false;
    return true;
}

void UiDragDrop::Cancel()
{
    state_ = State::None;
    target_ = {};
    accepted_ = false;
}

UiDragDrop::State UiDragDrop::Current() const noexcept
{
    return state_;
}

const UiDragDrop::Payload& UiDragDrop::Data() const noexcept
{
    return payload_;
}

UiElementHandle UiDragDrop::Target() const noexcept
{
    return target_;
}

const UiRect& UiDragDrop::Ghost() const noexcept
{
    return ghost_;
}

bool UiDragDrop::Accepted() const noexcept
{
    return accepted_;
}

void UiCanvas::Clear()
{
    commands_.clear();
}

void UiCanvas::AddLine(float x0, float y0, float x1, float y1, float width, const UiColor& color)
{
    Command command;
    command.kind = Kind::Line;
    command.x0 = x0;
    command.y0 = y0;
    command.x1 = x1;
    command.y1 = y1;
    command.width = width;
    command.color = color;
    commands_.push_back(std::move(command));
}

void UiCanvas::AddRect(const UiRect& rect, const UiColor& color)
{
    Command command;
    command.kind = Kind::Rect;
    command.rect = rect;
    command.color = color;
    commands_.push_back(std::move(command));
}

void UiCanvas::AddText(const UiRect& rect, std::string_view text, float fontSize, const UiColor& color)
{
    Command command;
    command.kind = Kind::Text;
    command.rect = rect;
    command.text.assign(text);
    command.fontSize = fontSize;
    command.color = color;
    commands_.push_back(std::move(command));
}

void UiCanvas::AddCircle(float centerX, float centerY, float radius, const UiColor& color)
{
    Command command;
    command.kind = Kind::Circle;
    command.centerX = centerX;
    command.centerY = centerY;
    command.radius = radius;
    command.color = color;
    commands_.push_back(std::move(command));
}

const std::vector<UiCanvas::Command>& UiCanvas::Commands() const noexcept
{
    return commands_;
}

UiSplitter::UiSplitter(bool vertical, float ratio, float minimumFirst, float minimumSecond)
    : vertical_(vertical),
      ratio_(std::clamp(ratio, 0.0f, 1.0f)),
      initialRatio_(ratio_),
      minimumFirst_(std::max(0.0f, minimumFirst)),
      minimumSecond_(std::max(0.0f, minimumSecond))
{
}

void UiSplitter::SetRatio(float ratio)
{
    ratio_ = std::clamp(ratio, 0.0f, 1.0f);
}

void UiSplitter::Drag(float deltaPixels, float totalPixels)
{
    if (totalPixels <= 0.0f)
    {
        return;
    }
    ratio_ += deltaPixels / totalPixels;
    const float minimum = minimumFirst_ / totalPixels;
    const float maximum = 1.0f - minimumSecond_ / totalPixels;
    ratio_ = std::clamp(ratio_, std::min(minimum, maximum), std::max(minimum, maximum));
}

void UiSplitter::Reset()
{
    ratio_ = initialRatio_;
}

float UiSplitter::Ratio() const noexcept
{
    return ratio_;
}

bool UiSplitter::Vertical() const noexcept
{
    return vertical_;
}

void UiTabBar::SetTabs(std::vector<std::string> tabs)
{
    tabs_ = std::move(tabs);
    selected_ = tabs_.empty() ? -1 : 0;
}

void UiTabBar::Select(int index)
{
    if (index >= 0 && index < static_cast<int>(tabs_.size()))
    {
        selected_ = index;
    }
}

bool UiTabBar::Close(int index)
{
    if (index < 0 || index >= static_cast<int>(tabs_.size()))
    {
        return false;
    }
    tabs_.erase(tabs_.begin() + index);
    if (tabs_.empty())
    {
        selected_ = -1;
    }
    else if (selected_ == index)
    {
        selected_ = std::min(index, static_cast<int>(tabs_.size()) - 1);
    }
    else if (selected_ > index)
    {
        --selected_;
    }
    return true;
}

bool UiTabBar::Move(int from, int to)
{
    if (from < 0 || from >= static_cast<int>(tabs_.size()) || to < 0 || to >= static_cast<int>(tabs_.size()))
    {
        return false;
    }
    if (from == to)
    {
        return true;
    }
    std::string tab = std::move(tabs_[from]);
    tabs_.erase(tabs_.begin() + from);
    tabs_.insert(tabs_.begin() + to, std::move(tab));
    if (selected_ == from)
    {
        selected_ = to;
    }
    else if (selected_ > from)
    {
        --selected_;
    }
    if (selected_ >= to)
    {
        ++selected_;
    }
    selected_ = std::clamp(selected_, 0, static_cast<int>(tabs_.size()) - 1);
    return true;
}

int UiTabBar::Selected() const noexcept
{
    return selected_;
}

const std::vector<std::string>& UiTabBar::Tabs() const noexcept
{
    return tabs_;
}

} // namespace se::ui

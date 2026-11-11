#pragma once

#include "FunctionLayer/UI/Core/UiDocument.h"
#include "FunctionLayer/UI/Core/UiInput.h"
#include "FunctionLayer/UI/Core/UiStyle.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace se::ui
{

// 按钮只保存自己的悬停/按下/点击状态；不直接读全局状态。
class UiButton
{
public:
    UiButton(UiDocument& document, UiElementHandle element);

    void SetHovered(bool hovered);
    void PointerDown();
    void PointerUp(bool inside);
    [[nodiscard]] bool ConsumeClick();
    [[nodiscard]] bool Hovered() const noexcept;
    [[nodiscard]] bool Pressed() const noexcept;

private:
    UiDocument* document_;
    UiElementHandle element_;
    bool hovered_ = false;
    bool pressed_ = false;
    bool clicked_ = false;
};

class UiCheckbox
{
public:
    UiCheckbox(UiDocument& document, UiElementHandle element);

    void SetChecked(bool checked);
    void Toggle();
    void PointerUp(bool inside);
    [[nodiscard]] bool Checked() const noexcept;
    [[nodiscard]] bool ConsumeChanged();

private:
    UiDocument* document_;
    UiElementHandle element_;
    bool checked_ = false;
    bool changed_ = false;
};

class UiCombo
{
public:
    UiCombo(UiDocument& document, UiElementHandle element);

    void SetItems(std::vector<std::string> items);
    void Open();
    void Close();
    void Toggle();
    void Select(int index);
    bool HandleKey(UiKeyCode key);
    [[nodiscard]] bool IsOpen() const noexcept;
    [[nodiscard]] int Selected() const noexcept;
    [[nodiscard]] const std::vector<std::string>& Items() const noexcept;
    [[nodiscard]] bool ConsumeChanged();

private:
    UiDocument* document_;
    UiElementHandle element_;
    std::vector<std::string> items_;
    int selected_ = -1;
    bool open_ = false;
    bool changed_ = false;
};

class UiScrollView
{
public:
    void SetContentHeight(float height);
    void SetViewportHeight(float height);
    void SetOffset(float offset);
    void ScrollBy(float delta);
    [[nodiscard]] float Offset() const noexcept;
    [[nodiscard]] float ContentHeight() const noexcept;
    [[nodiscard]] float ViewportHeight() const noexcept;
    [[nodiscard]] float MaximumOffset() const noexcept;
    [[nodiscard]] float ThumbY() const noexcept;
    [[nodiscard]] float ThumbHeight() const noexcept;

private:
    float contentHeight_ = 0.0f;
    float viewportHeight_ = 0.0f;
    float offset_ = 0.0f;
};

struct UiVisibleRange
{
    int first = 0;
    int last = 0;
};

class UiListView
{
public:
    UiListView(UiDocument& document, UiElementHandle content);

    void SetItemCount(int count);
    void SetRowHeight(float height);
    void SetViewportHeight(float height);
    void SetScrollOffset(float offset);
    void SetRowTextProvider(std::function<std::string(int)> provider);
    // 行背景（颜色）由调用方按行提供；默认透明。
    void SetRowBackgroundProvider(std::function<UiColor(int)> provider) { rowBackground_ = std::move(provider); dirty_ = true; }
    // 内容/行文字变化但 itemCount 不变时显式失效；Sync 只在 dirty 时重建行。
    void Invalidate() noexcept { dirty_ = true; }
    void Sync();
    [[nodiscard]] UiVisibleRange VisibleRange() const noexcept;
    [[nodiscard]] float Offset() const noexcept;
    [[nodiscard]] float ViewportHeight() const noexcept { return viewportHeight_; }
    [[nodiscard]] float RowHeight() const noexcept { return rowHeight_; }
    [[nodiscard]] int ItemCount() const noexcept { return itemCount_; }
    [[nodiscard]] const std::vector<UiElementHandle>& Rows() const noexcept;

private:
    UiDocument* document_;
    UiElementHandle content_;
    int itemCount_ = 0;
    float rowHeight_ = 20.0f;
    float viewportHeight_ = 0.0f;
    float offset_ = 0.0f;
    int bufferRows_ = 2;
    bool dirty_ = true;
    std::function<std::string(int)> rowText_;
    std::function<UiColor(int)> rowBackground_;
    std::vector<UiElementHandle> rows_;
};

class UiTreeView
{
public:
    void Clear();
    int AddNode(int parent, std::string label, bool expanded = true);
    void SetExpanded(int node, bool expanded);
    [[nodiscard]] const std::vector<int>& VisibleFlat() const noexcept;
    [[nodiscard]] int NodeCount() const noexcept;

private:
    struct Node
    {
        std::string label;
        int parent = -1;
        bool expanded = true;
    };
    std::vector<Node> nodes_;
    std::vector<int> visible_;
    void Rebuild();
};

class UiTableView
{
public:
    void SetDimensions(int rows, int columns);
    void SetRowHeight(float height);
    void SetViewportHeight(float height);
    void SetScrollOffset(float offset);
    [[nodiscard]] UiVisibleRange VisibleRange() const noexcept;
    [[nodiscard]] int Rows() const noexcept;
    [[nodiscard]] int Columns() const noexcept;

private:
    int rows_ = 0;
    int columns_ = 0;
    float rowHeight_ = 24.0f;
    float viewportHeight_ = 0.0f;
    float offset_ = 0.0f;
    int bufferRows_ = 2;
};

class UiOverlay
{
public:
    enum class Layer : std::uint8_t
    {
        Tooltip = 0,
        Menu,
        Dropdown,
        Modal,
    };

    struct Entry
    {
        Layer layer = Layer::Tooltip;
        UiRect rect{};
        int id = 0;
    };

    void Open(Layer layer, const UiRect& rect, int id);
    bool CloseTop();
    void CloseAll();
    [[nodiscard]] const Entry* Top() const noexcept;
    [[nodiscard]] bool BlocksInput() const noexcept;
    [[nodiscard]] std::size_t Count() const noexcept;
    // 返回是否消费该指针按下；菜单/下拉外部会关闭，模态总是拦截。
    bool PointerDown(float x, float y);
    [[nodiscard]] static UiRect Place(const UiRect& rect, float screenWidth, float screenHeight);

private:
    std::vector<Entry> layers_;
};

class UiDragDrop
{
public:
    struct Payload
    {
        std::string type;
        std::uint64_t id = 0;
    };

    enum class State : std::uint8_t
    {
        None = 0,
        Dragging,
        Over,
    };

    void Begin(std::string type, std::uint64_t id, const UiRect& ghost);
    void Over(UiElementHandle target, bool accepted);
    bool Drop();
    void Cancel();
    [[nodiscard]] State Current() const noexcept;
    [[nodiscard]] const Payload& Data() const noexcept;
    [[nodiscard]] UiElementHandle Target() const noexcept;
    [[nodiscard]] const UiRect& Ghost() const noexcept;
    [[nodiscard]] bool Accepted() const noexcept;

private:
    State state_ = State::None;
    Payload payload_{};
    UiRect ghost_{};
    UiElementHandle target_;
    bool accepted_ = false;
};

class UiCanvas
{
public:
    enum class Kind : std::uint8_t
    {
        Line = 0,
        Rect,
        Circle,
        Text,
    };

    struct Command
    {
        Kind kind = Kind::Line;
        UiRect rect{};
        float x0 = 0.0f;
        float y0 = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;
        float width = 1.0f;
        float centerX = 0.0f;
        float centerY = 0.0f;
        float radius = 0.0f;
        float fontSize = 14.0f;
        std::string text;
        UiColor color{};
    };

    void Clear();
    void AddLine(float x0, float y0, float x1, float y1, float width, const UiColor& color);
    void AddRect(const UiRect& rect, const UiColor& color);
    void AddCircle(float centerX, float centerY, float radius, const UiColor& color);
    void AddText(const UiRect& rect, std::string_view text, float fontSize, const UiColor& color);
    [[nodiscard]] const std::vector<Command>& Commands() const noexcept;

private:
    std::vector<Command> commands_;
};

class UiSplitter
{
public:
    UiSplitter(bool vertical, float ratio, float minimumFirst, float minimumSecond);

    void SetRatio(float ratio);
    void Drag(float deltaPixels, float totalPixels);
    void Reset();
    [[nodiscard]] float Ratio() const noexcept;
    [[nodiscard]] bool Vertical() const noexcept;

private:
    bool vertical_ = true;
    float ratio_ = 0.5f;
    float initialRatio_ = 0.5f;
    float minimumFirst_ = 0.0f;
    float minimumSecond_ = 0.0f;
};

class UiTabBar
{
public:
    void SetTabs(std::vector<std::string> tabs);
    void Select(int index);
    bool Close(int index);
    bool Move(int from, int to);
    [[nodiscard]] int Selected() const noexcept;
    [[nodiscard]] const std::vector<std::string>& Tabs() const noexcept;

private:
    std::vector<std::string> tabs_;
    int selected_ = -1;
};

} // namespace se::ui

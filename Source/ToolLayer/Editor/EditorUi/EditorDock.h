#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace se::ui::editor
{

enum class EditorPanelId : std::uint8_t
{
    None = 0,
    Hierarchy,
    Assets,
    Viewport,
    UiDesigner,
    Inspector,
    Console,
    Material,
    ProjectSettings,
    BuildExport,
    Preferences,
    About,
    Diagnostics,
};

// Horizontal = 子节点左右排列；Vertical = 子节点上下排列。
enum class DockAxis : std::uint8_t
{
    Horizontal = 0,
    Vertical,
};

struct DockNode
{
    enum class Kind : std::uint8_t
    {
        Split = 0,
        Tabs,
    };

    Kind kind = Kind::Tabs;
    DockAxis axis = DockAxis::Horizontal;
    float ratio = 0.5f;
    std::vector<DockNode> children;
    std::vector<EditorPanelId> tabs;
    EditorPanelId active = EditorPanelId::None;

    bool operator==(const DockNode&) const = default;
};

[[nodiscard]] DockNode DefaultEditorDockLayout();
[[nodiscard]] const char* EditorPanelName(EditorPanelId panel) noexcept;
[[nodiscard]] bool EditorPanelFromName(std::string_view name, EditorPanelId& panel) noexcept;

class EditorDock
{
public:
    explicit EditorDock(DockNode root = DefaultEditorDockLayout());

    [[nodiscard]] const DockNode& Root() const noexcept;
    [[nodiscard]] DockNode& Root() noexcept;

    [[nodiscard]] const DockNode* Find(EditorPanelId panel) const noexcept;
    bool SelectTab(EditorPanelId panel);
    bool CloseTab(EditorPanelId panel);
    bool OpenTab(EditorPanelId panel);
    // 优先插到 near 所在组；near 不存在时退回默认 OpenTab。
    bool OpenTabNear(EditorPanelId panel, EditorPanelId near);
    // 把 panel 标签移动到 target 所在组；before 为 true 时插到 target 之前。
    bool MoveTab(EditorPanelId panel, EditorPanelId target, bool before = false);
    // 把 panel 拖出为独立组：在 target 组的 axis 方向插入 new tab 组；before 表示放在左侧/上侧。
    bool SplitTab(EditorPanelId panel, EditorPanelId target, DockAxis axis, bool before);

    bool SetRootRatio(float ratio);
    bool SetSplitRatio(const std::vector<std::size_t>& path, float ratio);
    bool DragSplitter(const std::vector<std::size_t>& path, float deltaPixels, float totalPixels,
                      float minimumFirst, float minimumSecond);

private:
    [[nodiscard]] static DockNode* NodeAt(DockNode& root, const std::vector<std::size_t>& path);
    [[nodiscard]] static DockNode* FindNode(DockNode& root, EditorPanelId panel);
    DockNode root_;
};

} // namespace se::ui::editor

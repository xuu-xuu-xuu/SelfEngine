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

struct FloatingDockWindow
{
    std::uint32_t id=0;
    int x=100,y=100,width=640,height=480; // 屏幕物理像素，保存外框。
    DockNode layout;
    bool operator==(const FloatingDockWindow&) const = default;
};

class EditorDock
{
public:
    explicit EditorDock(DockNode root = DefaultEditorDockLayout());

    [[nodiscard]] const DockNode& Root() const noexcept;
    [[nodiscard]] DockNode& Root() noexcept;
    [[nodiscard]] const std::vector<FloatingDockWindow>& Windows() const noexcept {return windows_;}
    [[nodiscard]] DockNode* Surface(std::uint32_t id) noexcept;
    [[nodiscard]] const DockNode* Surface(std::uint32_t id) const noexcept;
    [[nodiscard]] std::uint32_t SurfaceFor(EditorPanelId panel) const noexcept;
    std::uint32_t FloatTab(EditorPanelId panel,int x,int y,int width=640,int height=480);
    bool SetWindowBounds(std::uint32_t id,int x,int y,int width,int height);
    bool CloseWindow(std::uint32_t id);
    bool RestoreWindows(std::vector<FloatingDockWindow> windows);
    bool operator==(const EditorDock& other) const {return root_==other.root_&&windows_==other.windows_;}

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
    bool SetSplitRatio(std::uint32_t surface,const std::vector<std::size_t>& path,float ratio);
    bool DragSplitter(const std::vector<std::size_t>& path, float deltaPixels, float totalPixels,
                      float minimumFirst, float minimumSecond);

private:
    [[nodiscard]] static DockNode* NodeAt(DockNode& root, const std::vector<std::size_t>& path);
    [[nodiscard]] static DockNode* FindNode(DockNode& root, EditorPanelId panel);
    [[nodiscard]] DockNode* FindMutable(EditorPanelId panel);
    void Normalize();
    DockNode root_;
    std::vector<FloatingDockWindow> windows_;
    std::uint32_t nextWindowId_=1; // 同一会话不复用，避免帧边界前关闭/新建命中旧 HWND。
};

} // namespace se::ui::editor

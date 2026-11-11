#include "ToolLayer/Editor/EditorUi/EditorDock.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

namespace se::ui::editor
{
namespace
{
DockNode MakeTabs(std::initializer_list<EditorPanelId> panels)
{
    DockNode node;
    node.kind = DockNode::Kind::Tabs;
    node.tabs.assign(panels.begin(), panels.end());
    node.active = node.tabs.empty() ? EditorPanelId::None : node.tabs.front();
    return node;
}

// 查找 target 叶子的父节点与索引；target 为根时返回 false。
bool FindParentOfLeaf(DockNode& node, const DockNode* target, DockNode*& parent, std::size_t& index)
{
    for (std::size_t child = 0; child < node.children.size(); ++child)
    {
        if (&node.children[child] == target)
        {
            parent = &node;
            index = child;
            return true;
        }
        if (FindParentOfLeaf(node.children[child], target, parent, index))
        {
            return true;
        }
    }
    return false;
}
} // namespace

DockNode DefaultEditorDockLayout()
{
    DockNode leftTop = MakeTabs({EditorPanelId::Hierarchy});
    DockNode leftBottom = MakeTabs({EditorPanelId::Assets});
    DockNode left;
    left.kind = DockNode::Kind::Split;
    left.axis = DockAxis::Vertical;
    left.ratio = 0.66f;
    left.children = {std::move(leftTop), std::move(leftBottom)};

    DockNode center = MakeTabs({EditorPanelId::Viewport, EditorPanelId::UiDesigner});

    DockNode rightTop = MakeTabs({EditorPanelId::Inspector});
    DockNode rightBottom = MakeTabs({EditorPanelId::Console});
    DockNode right;
    right.kind = DockNode::Kind::Split;
    right.axis = DockAxis::Vertical;
    right.ratio = 0.70f;
    right.children = {std::move(rightTop), std::move(rightBottom)};

    DockNode centerRight;
    centerRight.kind = DockNode::Kind::Split;
    centerRight.axis = DockAxis::Horizontal;
    centerRight.ratio = 0.70f;
    centerRight.children = {std::move(center), std::move(right)};

    DockNode root;
    root.kind = DockNode::Kind::Split;
    root.axis = DockAxis::Horizontal;
    root.ratio = 0.20f;
    root.children = {std::move(left), std::move(centerRight)};
    return root;
}

const char* EditorPanelName(EditorPanelId panel) noexcept
{
    switch (panel)
    {
    case EditorPanelId::None: return "none";
    case EditorPanelId::Hierarchy: return "hierarchy";
    case EditorPanelId::Assets: return "assets";
    case EditorPanelId::Viewport: return "viewport";
    case EditorPanelId::UiDesigner: return "ui-designer";
    case EditorPanelId::Inspector: return "inspector";
    case EditorPanelId::Console: return "console";
    case EditorPanelId::Material: return "material";
    case EditorPanelId::ProjectSettings: return "project-settings";
    case EditorPanelId::BuildExport: return "build-export";
    case EditorPanelId::Preferences: return "preferences";
    case EditorPanelId::About: return "about";
    case EditorPanelId::Diagnostics: return "diagnostics";
    }
    return "none";
}

bool EditorPanelFromName(std::string_view name, EditorPanelId& panel) noexcept
{
    if (name == "hierarchy") panel = EditorPanelId::Hierarchy;
    else if (name == "assets") panel = EditorPanelId::Assets;
    else if (name == "viewport") panel = EditorPanelId::Viewport;
    else if (name == "ui-designer") panel = EditorPanelId::UiDesigner;
    else if (name == "inspector") panel = EditorPanelId::Inspector;
    else if (name == "console") panel = EditorPanelId::Console;
    else if (name == "material") panel = EditorPanelId::Material;
    else if (name == "project-settings") panel = EditorPanelId::ProjectSettings;
    else if (name == "build-export") panel = EditorPanelId::BuildExport;
    else if (name == "preferences") panel = EditorPanelId::Preferences;
    else if (name == "about") panel = EditorPanelId::About;
    else if (name == "diagnostics") panel = EditorPanelId::Diagnostics;
    else return false;
    return true;
}

EditorDock::EditorDock(DockNode root)
    : root_(std::move(root))
{
}

const DockNode& EditorDock::Root() const noexcept
{
    return root_;
}

DockNode& EditorDock::Root() noexcept
{
    return root_;
}

DockNode* EditorDock::NodeAt(DockNode& root, const std::vector<std::size_t>& path)
{
    DockNode* node = &root;
    for (const std::size_t index : path)
    {
        if (index >= node->children.size())
        {
            return nullptr;
        }
        node = &node->children[index];
    }
    return node;
}

DockNode* EditorDock::FindNode(DockNode& root, EditorPanelId panel)
{
    const auto contains = std::find(root.tabs.begin(), root.tabs.end(), panel);
    if (contains != root.tabs.end())
    {
        return &root;
    }
    for (DockNode& child : root.children)
    {
        if (DockNode* found = FindNode(child, panel))
        {
            return found;
        }
    }
    return nullptr;
}

const DockNode* EditorDock::Find(EditorPanelId panel) const noexcept
{
    const auto visit = [panel](const auto& self, const DockNode& node) -> const DockNode* {
        if (std::find(node.tabs.begin(), node.tabs.end(), panel) != node.tabs.end())
        {
            return &node;
        }
        for (const DockNode& child : node.children)
        {
            if (const DockNode* found = self(self, child))
            {
                return found;
            }
        }
        return nullptr;
    };
    return visit(visit, root_);
}

bool EditorDock::SelectTab(EditorPanelId panel)
{
    DockNode* node = FindNode(root_, panel);
    if (!node)
    {
        return false;
    }
    node->active = panel;
    return true;
}

bool EditorDock::CloseTab(EditorPanelId panel)
{
    DockNode* node = FindNode(root_, panel);
    if (!node)
    {
        return false;
    }
    const auto found = std::find(node->tabs.begin(), node->tabs.end(), panel);
    if (found == node->tabs.end())
    {
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(found - node->tabs.begin());
    node->tabs.erase(found);
    if (node->tabs.empty())
    {
        node->active = EditorPanelId::None;
    }
    else if (node->active == panel)
    {
        node->active = node->tabs[std::min(index, node->tabs.size() - 1)];
    }
    return true;
}

bool EditorDock::OpenTab(EditorPanelId panel)
{
    if (panel == EditorPanelId::None || FindNode(root_, panel))
    {
        return false;
    }
    DockNode* fallback = nullptr;
    const auto visit = [panel, &fallback](const auto& self, DockNode& node) -> bool {
        if (node.kind == DockNode::Kind::Tabs)
        {
            if (node.tabs.empty())
            {
                node.tabs.push_back(panel);
                node.active = panel;
                return true;
            }
            if (fallback == nullptr)
            {
                fallback = &node;
            }
            return false;
        }
        for (DockNode& child : node.children)
        {
            if (self(self, child))
            {
                return true;
            }
        }
        return false;
    };
    if (visit(visit, root_))
    {
        return true;
    }
    if (fallback == nullptr)
    {
        return false;
    }
    fallback->tabs.push_back(panel);
    fallback->active = panel;
    return true;
}

bool EditorDock::OpenTabNear(EditorPanelId panel, EditorPanelId near)
{
    if (panel == EditorPanelId::None || FindNode(root_, panel))
    {
        return false;
    }
    DockNode* destination = FindNode(root_, near);
    if (destination == nullptr || destination->kind != DockNode::Kind::Tabs)
    {
        return OpenTab(panel);
    }
    destination->tabs.push_back(panel);
    destination->active = panel;
    return true;
}

bool EditorDock::MoveTab(EditorPanelId panel, EditorPanelId target, bool before)
{
    if (panel == EditorPanelId::None || panel == target)
    {
        return false;
    }
    DockNode* source = FindNode(root_, panel);
    DockNode* destination = FindNode(root_, target);
    if (source == nullptr || destination == nullptr)
    {
        return false;
    }
    const auto sourceIt = std::find(source->tabs.begin(), source->tabs.end(), panel);
    const auto targetIt = std::find(destination->tabs.begin(), destination->tabs.end(), target);
    if (sourceIt == source->tabs.end() || targetIt == destination->tabs.end())
    {
        return false;
    }
    const std::size_t sourceIndex = static_cast<std::size_t>(sourceIt - source->tabs.begin());
    source->tabs.erase(sourceIt);
    if (source->tabs.empty())
    {
        source->active = EditorPanelId::None;
    }
    else if (source->active == panel)
    {
        source->active = source->tabs[std::min(sourceIndex, source->tabs.size() - 1)];
    }
    destination = FindNode(root_, target);
    if (destination == nullptr)
    {
        return false;
    }
    const auto destinationTarget = std::find(destination->tabs.begin(), destination->tabs.end(), target);
    if (destinationTarget == destination->tabs.end())
    {
        return false;
    }
    const std::size_t insertIndex = static_cast<std::size_t>(destinationTarget - destination->tabs.begin());
    if (before)
    {
        destination->tabs.insert(destination->tabs.begin() + static_cast<std::ptrdiff_t>(insertIndex), panel);
    }
    else
    {
        destination->tabs.insert(destination->tabs.begin() + static_cast<std::ptrdiff_t>(insertIndex + 1), panel);
    }
    destination->active = panel;
    return true;
}

bool EditorDock::SplitTab(EditorPanelId panel, EditorPanelId target, DockAxis axis, bool before)
{
    if (panel == EditorPanelId::None || panel == target)
    {
        return false;
    }
    DockNode* source = FindNode(root_, panel);
    DockNode* destination = FindNode(root_, target);
    if (source == nullptr || destination == nullptr)
    {
        return false;
    }
    const auto sourceIt = std::find(source->tabs.begin(), source->tabs.end(), panel);
    if (sourceIt == source->tabs.end())
    {
        return false;
    }
    source->tabs.erase(sourceIt);
    if (source->active == panel)
    {
        source->active = source->tabs.empty() ? EditorPanelId::None : source->tabs.front();
    }

    destination = FindNode(root_, target);
    if (destination == nullptr || destination->kind != DockNode::Kind::Tabs)
    {
        // target 已随源组移除等异常情况，回退到第一个可用叶子。
        destination = FindNode(root_, target);
        if (destination == nullptr)
        {
            return false;
        }
    }

    DockNode detached = MakeTabs({panel});
    if (destination->tabs.empty())
    {
        *destination = std::move(detached);
        return true;
    }

    DockNode* parent = nullptr;
    std::size_t index = 0;
    const bool hasParent = FindParentOfLeaf(root_, destination, parent, index);
    DockNode split;
    split.kind = DockNode::Kind::Split;
    split.axis = axis;
    split.ratio = 0.5f;
    if (before)
    {
        split.children = {std::move(detached), *destination};
    }
    else
    {
        split.children = {*destination, std::move(detached)};
    }
    if (!hasParent)
    {
        root_ = std::move(split);
    }
    else
    {
        parent->children[index] = std::move(split);
    }
    return true;
}

bool EditorDock::SetRootRatio(float ratio)
{
    if (root_.kind != DockNode::Kind::Split || root_.children.size() != 2 || !std::isfinite(ratio))
    {
        return false;
    }
    root_.ratio = std::clamp(ratio, 0.0f, 1.0f);
    return true;
}

bool EditorDock::SetSplitRatio(const std::vector<std::size_t>& path, float ratio)
{
    DockNode* node = NodeAt(root_, path);
    if (!node || node->kind != DockNode::Kind::Split || node->children.size() != 2 || !std::isfinite(ratio))
    {
        return false;
    }
    node->ratio = std::clamp(ratio, 0.0f, 1.0f);
    return true;
}

bool EditorDock::DragSplitter(const std::vector<std::size_t>& path, float deltaPixels, float totalPixels,
                              float minimumFirst, float minimumSecond)
{
    DockNode* node = NodeAt(root_, path);
    if (!node || node->kind != DockNode::Kind::Split || node->children.size() != 2 ||
        !std::isfinite(deltaPixels) || !std::isfinite(totalPixels) || totalPixels <= 0.0f)
    {
        return false;
    }
    const float minimum = minimumFirst / totalPixels;
    const float maximum = 1.0f - minimumSecond / totalPixels;
    float ratio = node->ratio + deltaPixels / totalPixels;
    ratio = std::clamp(ratio, std::min(minimum, maximum), std::max(minimum, maximum));
    node->ratio = std::clamp(ratio, 0.0f, 1.0f);
    return true;
}

} // namespace se::ui::editor

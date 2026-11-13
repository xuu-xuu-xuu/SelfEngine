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

bool CollapseEmptyGroups(DockNode& node)
{
    if(node.kind==DockNode::Kind::Tabs)return node.tabs.empty();
    if(node.children.size()!=2)return false;
    const bool firstEmpty=CollapseEmptyGroups(node.children[0]);
    const bool secondEmpty=CollapseEmptyGroups(node.children[1]);
    if(firstEmpty && secondEmpty)node=MakeTabs({});
    else if(firstEmpty || secondEmpty)
    {
        DockNode remaining=std::move(node.children[firstEmpty?1:0]);
        node=std::move(remaining);
    }
    return firstEmpty && secondEmpty;
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
    Normalize();
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
    if(const auto* found=visit(visit, root_))return found;
    for(const auto& window:windows_)if(const auto* found=visit(visit,window.layout))return found;
    return nullptr;
}

bool EditorDock::SelectTab(EditorPanelId panel)
{
    DockNode* node = FindMutable(panel);
    if (!node)
    {
        return false;
    }
    node->active = panel;
    return true;
}

bool EditorDock::CloseTab(EditorPanelId panel)
{
    DockNode* node = FindMutable(panel);
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
    Normalize();
    return true;
}

bool EditorDock::OpenTab(EditorPanelId panel)
{
    if (panel == EditorPanelId::None || FindMutable(panel))
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
    if (panel == EditorPanelId::None || FindMutable(panel))
    {
        return false;
    }
    DockNode* destination = FindMutable(near);
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
    DockNode* source = FindMutable(panel);
    DockNode* destination = FindMutable(target);
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
    destination = FindMutable(target);
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
    Normalize();
    return true;
}

bool EditorDock::SplitTab(EditorPanelId panel, EditorPanelId target, DockAxis axis, bool before)
{
    if (panel == EditorPanelId::None || panel == target)
    {
        return false;
    }
    DockNode* source = FindMutable(panel);
    DockNode* destination = FindMutable(target);
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

    destination = FindMutable(target);
    if (destination == nullptr || destination->kind != DockNode::Kind::Tabs)
    {
        // target 已随源组移除等异常情况，回退到第一个可用叶子。
        destination = FindMutable(target);
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
    DockNode& targetRoot=*Surface(SurfaceFor(target));
    const bool hasParent = FindParentOfLeaf(targetRoot, destination, parent, index);
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
        targetRoot = std::move(split);
    }
    else
    {
        parent->children[index] = std::move(split);
    }
    Normalize();
    return true;
}

DockNode* EditorDock::FindMutable(EditorPanelId panel)
{
    if(auto* found=FindNode(root_,panel))return found;
    for(auto& window:windows_)if(auto* found=FindNode(window.layout,panel))return found;
    return nullptr;
}
void EditorDock::Normalize()
{
    CollapseEmptyGroups(root_);
    for(auto& window:windows_)CollapseEmptyGroups(window.layout);
    std::erase_if(windows_,[](const auto& w){return w.layout.kind==DockNode::Kind::Tabs&&w.layout.tabs.empty();});
}
DockNode* EditorDock::Surface(std::uint32_t id) noexcept
{
    if(!id)return &root_;
    for(auto& window:windows_)if(window.id==id)return &window.layout;
    return nullptr;
}
const DockNode* EditorDock::Surface(std::uint32_t id)const noexcept
{return const_cast<EditorDock*>(this)->Surface(id);}
std::uint32_t EditorDock::SurfaceFor(EditorPanelId panel)const noexcept
{
    for(const auto& window:windows_)
        if(FindNode(const_cast<DockNode&>(window.layout),panel))return window.id;
    return 0;
}
std::uint32_t EditorDock::FloatTab(EditorPanelId panel,int x,int y,int width,int height)
{
    if(!Find(panel))return 0;
    const std::uint32_t id=nextWindowId_;
    if(!id||id>1000000||windows_.size()>=32)return 0;
    if(!CloseTab(panel))return 0;
    windows_.push_back({id,std::clamp(x,-1000000,1000000),std::clamp(y,-1000000,1000000),std::clamp(width,240,8192),std::clamp(height,160,8192),MakeTabs({panel})});
    ++nextWindowId_;
    return id;
}
bool EditorDock::SetWindowBounds(std::uint32_t id,int x,int y,int width,int height)
{
    for(auto& w:windows_)if(w.id==id)
    {w.x=std::clamp(x,-1000000,1000000);w.y=std::clamp(y,-1000000,1000000);w.width=std::clamp(width,240,8192);w.height=std::clamp(height,160,8192);return true;}
    return false;
}
bool EditorDock::CloseWindow(std::uint32_t id)
{
    const auto size=windows_.size();std::erase_if(windows_,[id](const auto& w){return w.id==id;});
    return size!=windows_.size();
}
bool EditorDock::RestoreWindows(std::vector<FloatingDockWindow> windows)
{
    if(windows.size()>32)return false;
    std::vector<std::uint32_t> ids;
    std::vector<EditorPanelId> panels;
    const auto collect=[&](const auto& self,const DockNode& node)->bool {
        for(const auto p:node.tabs)
        {if(p==EditorPanelId::None||std::find(panels.begin(),panels.end(),p)!=panels.end())return false;panels.push_back(p);}
        for(const auto& child:node.children)if(!self(self,child))return false;
        return true;
    };
    if(!collect(collect,root_))return false;
    for(const auto& w:windows)
    {
        if(!w.id||w.id>1000000||std::find(ids.begin(),ids.end(),w.id)!=ids.end()||!collect(collect,w.layout))return false;
        if(w.width<240||w.height<160||w.width>8192||w.height>8192)return false;
        if(w.x<-1000000||w.x>1000000||w.y<-1000000||w.y>1000000)return false;
        ids.push_back(w.id);
    }
    windows_=std::move(windows);Normalize();nextWindowId_=1;
    for(const auto& window:windows_)nextWindowId_=std::max(nextWindowId_,window.id+1);
    return true;
}
bool EditorDock::SetSplitRatio(std::uint32_t surface,const std::vector<std::size_t>& path,float ratio)
{
    auto* root=Surface(surface);if(!root||!std::isfinite(ratio))return false;
    auto* node=NodeAt(*root,path);if(!node||node->kind!=DockNode::Kind::Split)return false;
    node->ratio=std::clamp(ratio,0.0f,1.0f);return true;
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

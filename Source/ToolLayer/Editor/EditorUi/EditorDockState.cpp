#include "ToolLayer/Editor/EditorUi/EditorDockState.h"

#include <nlohmann/json.hpp>

#include <algorithm>

#include <cmath>
#include <fstream>
#include <string>
#include <utility>

namespace se::ui::editor
{
namespace
{
using Json = nlohmann::ordered_json;
constexpr int kVersion = 1;

Json ToJson(const DockNode& node)
{
    Json result;
    result["type"] = node.kind == DockNode::Kind::Split ? "split" : "tabs";
    if (node.kind == DockNode::Kind::Split)
    {
        result["axis"] = node.axis == DockAxis::Vertical ? "vertical" : "horizontal";
        result["ratio"] = node.ratio;
        Json children = Json::array();
        for (const DockNode& child : node.children)
        {
            children.push_back(ToJson(child));
        }
        result["children"] = std::move(children);
    }
    else
    {
        Json tabs = Json::array();
        for (const EditorPanelId tab : node.tabs)
        {
            tabs.push_back(EditorPanelName(tab));
        }
        result["tabs"] = std::move(tabs);
        result["active"] = EditorPanelName(node.active);
    }
    return result;
}

bool FromJson(const Json& value, DockNode& node, std::string& error, int depth = 0,
              std::size_t* nodeCount = nullptr)
{
    static constexpr int kMaximumDepth = 32;
    static constexpr std::size_t kMaximumNodes = 256;
    std::size_t localCount = 0;
    std::size_t& count = nodeCount ? *nodeCount : localCount;
    if (++count > kMaximumNodes)
    {
        error = "dock tree too large";
        return false;
    }
    if (depth > kMaximumDepth)
    {
        error = "dock tree too deep";
        return false;
    }
    if (!value.is_object())
    {
        error = "dock node must be an object";
        return false;
    }
    const auto type = value.find("type");
    if (type == value.end() || !type->is_string())
    {
        error = "dock node.type missing";
        return false;
    }
    const std::string typeName = type->get<std::string>();
    if (typeName == "split")
    {
        node.kind = DockNode::Kind::Split;
        node.axis = DockAxis::Horizontal;
        const auto axis = value.find("axis");
        if (axis != value.end())
        {
            if (!axis->is_string())
            {
                error = "dock split axis invalid";
                return false;
            }
            const std::string axisName = axis->get<std::string>();
            if (axisName == "vertical") node.axis = DockAxis::Vertical;
            else if (axisName == "horizontal") node.axis = DockAxis::Horizontal;
            else
            {
                error = "dock split axis invalid";
                return false;
            }
        }
        const auto ratio = value.find("ratio");
        if (ratio == value.end() || !ratio->is_number() || !std::isfinite(ratio->get<float>()))
        {
            error = "dock split ratio invalid";
            return false;
        }
        node.ratio = std::clamp(ratio->get<float>(), 0.0f, 1.0f);
        const auto children = value.find("children");
        if (children == value.end() || !children->is_array() || children->size() != 2)
        {
            error = "dock split requires two children";
            return false;
        }
        node.children.resize(2);
        for (std::size_t index = 0; index < 2; ++index)
        {
            if (!FromJson((*children)[index], node.children[index], error, depth + 1, &count))
            {
                return false;
            }
        }
        return true;
    }
    if (typeName == "tabs")
    {
        node.kind = DockNode::Kind::Tabs;
        const auto tabs = value.find("tabs");
        if (tabs == value.end() || !tabs->is_array())
        {
            error = "dock tabs missing";
            return false;
        }
        node.tabs.clear();
        for (const Json& tab : *tabs)
        {
            if (!tab.is_string())
            {
                error = "dock tab must be a string";
                return false;
            }
            EditorPanelId panel = EditorPanelId::None;
            if (!EditorPanelFromName(tab.get<std::string>(), panel))
            {
                error = "unknown dock panel";
                return false;
            }
            node.tabs.push_back(panel);
        }
        node.active = node.tabs.empty() ? EditorPanelId::None : node.tabs.front();
        const auto active = value.find("active");
        if (active != value.end() && active->is_string())
        {
            EditorPanelId panel = EditorPanelId::None;
            if (EditorPanelFromName(active->get<std::string>(), panel) &&
                std::find(node.tabs.begin(), node.tabs.end(), panel) != node.tabs.end())
            {
                node.active = panel;
            }
        }
        return true;
    }
    error = "unknown dock node type";
    return false;
}
} // namespace

bool EditorDockState::Save(const DockNode& layout, const std::filesystem::path& path, std::string& error)
{
    error.clear();
    try
    {
        Json root;
        root["version"] = kVersion;
        root["layout"] = ToJson(layout);
        const std::string text = root.dump(2);
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        const std::filesystem::path temporary = path.string() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                error = "cannot open dock temp file";
                return false;
            }
            output << text;
            if (!output)
            {
                error = "cannot write dock temp file";
                return false;
            }
        }
        if (std::filesystem::exists(path))
        {
            std::filesystem::copy_file(path, path.string() + ".bak",
                                       std::filesystem::copy_options::overwrite_existing);
            std::filesystem::remove(path);
        }
        std::filesystem::rename(temporary, path);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = std::string("dock save failed: ") + exception.what();
        return false;
    }
}

bool EditorDockState::Load(const std::filesystem::path& path, DockNode& layout, std::string& error)
{
    error.clear();
    layout = DefaultEditorDockLayout();
    if (!std::filesystem::exists(path))
    {
        error = "dock layout not found";
        return false;
    }
    std::string text;
    try
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            error = "cannot open dock layout";
            return false;
        }
        text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    catch (const std::exception& exception)
    {
        error = std::string("dock read failed: ") + exception.what();
    }
    DockNode parsed;
    std::string detail;
    if (!Deserialize(text, parsed, detail))
    {
        error = detail;
        try
        {
            std::filesystem::copy_file(path, path.string() + ".bak",
                                       std::filesystem::copy_options::overwrite_existing);
        }
        catch (...)
        {
        }
        layout = DefaultEditorDockLayout();
        return false;
    }
    layout = std::move(parsed);
    return true;
}

std::string EditorDockState::Serialize(const DockNode& layout)
{
    Json root;
    root["version"] = kVersion;
    root["layout"] = ToJson(layout);
    return root.dump(2);
}

bool EditorDockState::Deserialize(std::string_view json, DockNode& layout, std::string& error)
{
    error.clear();
    static constexpr std::size_t kMaximumBytes = 64 * 1024;
    if (json.size() > kMaximumBytes)
    {
        error = "dock JSON too large";
        return false;
    }
    Json root;
    try
    {
        root = Json::parse(json.begin(), json.end());
    }
    catch (const std::exception& exception)
    {
        error = std::string("invalid dock JSON: ") + exception.what();
        return false;
    }
    if (!root.is_object())
    {
        error = "dock root must be an object";
        return false;
    }
    const auto version = root.find("version");
    if (version == root.end() || !version->is_number_integer() || version->get<int>() != kVersion)
    {
        error = "unsupported dock version";
        return false;
    }
    const auto value = root.find("layout");
    if (value == root.end())
    {
        error = "dock layout missing";
        return false;
    }
    DockNode parsed;
    if (!FromJson(*value, parsed, error))
    {
        return false;
    }
    layout = std::move(parsed);
    return true;
}

} // namespace se::ui::editor

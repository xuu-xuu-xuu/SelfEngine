#include "ToolLayer/Editor/EditorUi/EditorPanels.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>

namespace se::ui::editor
{
namespace
{
char Lower(char value) noexcept
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

bool ContainsInsensitive(std::string_view text, std::string_view needle)
{
    if (needle.empty())
    {
        return true;
    }
    if (needle.size() > text.size())
    {
        return false;
    }
    for (std::size_t index = 0; index + needle.size() <= text.size(); ++index)
    {
        bool match = true;
        for (std::size_t offset = 0; offset < needle.size(); ++offset)
        {
            if (Lower(text[index + offset]) != Lower(needle[offset]))
            {
                match = false;
                break;
            }
        }
        if (match)
        {
            return true;
        }
    }
    return false;
}
} // namespace

void EditorHierarchyModel::SetNodes(std::vector<EditorHierarchyNode> nodes)
{
    const std::uint64_t previous = selected_;
    std::unordered_map<std::uint64_t, bool> expanded;
    for (const EditorHierarchyNode& node : nodes_) expanded[node.id] = node.expanded;
    nodes_ = std::move(nodes);
    for (EditorHierarchyNode& node : nodes_)
    {
        const auto found = expanded.find(node.id);
        if (found != expanded.end()) node.expanded = found->second;
    }
    selected_ = std::none_of(nodes_.begin(), nodes_.end(), [previous](const EditorHierarchyNode& node) {
        return node.id == previous;
    }) ? 0 : previous;
}

const std::vector<EditorHierarchyNode>& EditorHierarchyModel::Nodes() const noexcept
{
    return nodes_;
}

bool EditorHierarchyModel::Visible(std::size_t index) const
{
    if (index >= nodes_.size())
    {
        return false;
    }
    std::unordered_map<std::uint64_t, std::uint64_t> parents;
    std::unordered_map<std::uint64_t, bool> expanded;
    for (const EditorHierarchyNode& node : nodes_)
    {
        parents[node.id] = node.parent;
        expanded[node.id] = node.expanded;
    }
    std::uint64_t parent = nodes_[index].parent;
    std::size_t guard = 0;
    while (parent != 0 && guard++ < nodes_.size())
    {
        if (!expanded[parent])
        {
            return false;
        }
        parent = parents[parent];
    }
    return true;
}

std::vector<std::size_t> EditorHierarchyModel::VisibleRows() const
{
    std::vector<std::size_t> rows;
    for (std::size_t index = 0; index < nodes_.size(); ++index)
    {
        if (Visible(index))
        {
            rows.push_back(index);
        }
    }
    return rows;
}

bool EditorHierarchyModel::Toggle(std::size_t index)
{
    if (index >= nodes_.size() || !nodes_[index].hasChildren)
    {
        return false;
    }
    nodes_[index].expanded = !nodes_[index].expanded;
    return true;
}

bool EditorHierarchyModel::Select(std::uint64_t id)
{
    if (id != 0 && std::none_of(nodes_.begin(), nodes_.end(), [id](const EditorHierarchyNode& node) {
            return node.id == id;
        }))
    {
        return false;
    }
    selected_ = id;
    return true;
}

std::uint64_t EditorHierarchyModel::Selected() const noexcept
{
    return selected_;
}

bool EditorHierarchyModel::Rename(std::uint64_t id, std::string label)
{
    for (EditorHierarchyNode& node : nodes_)
    {
        if (node.id == id)
        {
            node.label = std::move(label);
            return true;
        }
    }
    return false;
}

bool EditorHierarchyModel::Reparent(std::uint64_t id, std::uint64_t parent)
{
    if (id == 0 || id == parent)
    {
        return false;
    }
    auto node = std::find_if(nodes_.begin(), nodes_.end(), [id](const EditorHierarchyNode& item) {
        return item.id == id;
    });
    if (node == nodes_.end())
    {
        return false;
    }
    std::uint64_t cursor = parent;
    std::size_t guard = 0;
    while (cursor != 0 && guard++ < nodes_.size())
    {
        if (cursor == id)
        {
            return false;
        }
        const auto found = std::find_if(nodes_.begin(), nodes_.end(), [cursor](const EditorHierarchyNode& item) {
            return item.id == cursor;
        });
        if (found == nodes_.end())
        {
            return false;
        }
        cursor = found->parent;
    }
    node->parent = parent;
    return true;
}

void EditorInspectorModel::SetTarget(std::uint64_t id, std::string label)
{
    target_ = id;
    targetLabel_ = std::move(label);
    properties_.clear();
    changed_ = false;
    lastChangedKey_.clear();
}

std::uint64_t EditorInspectorModel::Target() const noexcept
{
    return target_;
}

const std::string& EditorInspectorModel::TargetLabel() const noexcept
{
    return targetLabel_;
}

void EditorInspectorModel::SetProperties(std::vector<EditorProperty> properties)
{
    properties_ = std::move(properties);
    changed_ = false;
    lastChangedKey_.clear();
}

const std::vector<EditorProperty>& EditorInspectorModel::Properties() const noexcept
{
    return properties_;
}

bool EditorInspectorModel::SetValue(std::string_view key, std::string value)
{
    for (EditorProperty& property : properties_)
    {
        if (property.key == key)
        {
            if (property.readOnly || property.value == value)
            {
                return false;
            }
            property.value = std::move(value);
            changed_ = true;
            lastChangedKey_.assign(key);
            return true;
        }
    }
    return false;
}

bool EditorInspectorModel::ConsumeChanged()
{
    const bool changed = changed_;
    changed_ = false;
    return changed;
}

const std::string& EditorInspectorModel::LastChangedKey() const noexcept
{
    return lastChangedKey_;
}

const std::vector<EditorAssetEntry>& EditorAssetsModel::Entries() const noexcept
{
    return entries_;
}

void EditorAssetsModel::SetEntries(std::vector<EditorAssetEntry> entries)
{
    entries_ = std::move(entries);
    SetFilter(filter_);
}

void EditorAssetsModel::SetFilter(std::string_view filter)
{
    filter_.assign(filter);
    filtered_.clear();
    for (const EditorAssetEntry& entry : entries_)
    {
        if (filter_.empty() || ContainsInsensitive(entry.path, filter_))
        {
            filtered_.push_back(entry);
        }
    }
    if (!selected_.empty() &&
        std::none_of(filtered_.begin(), filtered_.end(), [this](const EditorAssetEntry& entry) {
            return entry.path == selected_;
        }))
    {
        selected_.clear();
    }
}

const std::vector<EditorAssetEntry>& EditorAssetsModel::Filtered() const noexcept
{
    return filtered_;
}

std::vector<EditorAssetEntry> EditorAssetsModel::VisibleRange(std::size_t offset, float viewportHeight,
                                                              float rowHeight, std::size_t bufferRows) const
{
    std::vector<EditorAssetEntry> rows;
    if (rowHeight <= 0.0f || viewportHeight <= 0.0f || filtered_.empty())
    {
        return rows;
    }
    const std::size_t first = offset > 0 ? offset / static_cast<std::size_t>(rowHeight) : 0;
    const std::size_t last = static_cast<std::size_t>(std::ceil((static_cast<float>(offset) + viewportHeight) /
                                                                rowHeight));
    const std::size_t begin = first > bufferRows ? first - bufferRows : 0;
    const std::size_t end = std::min(filtered_.size(), last + bufferRows);
    for (std::size_t index = begin; index < end; ++index)
    {
        rows.push_back(filtered_[index]);
    }
    return rows;
}

bool EditorAssetsModel::Selected(std::string_view path) const noexcept
{
    return selected_ == path;
}

void EditorAssetsModel::Select(std::string path)
{
    selected_ = std::move(path);
}

const std::string& EditorAssetsModel::SelectedPath() const noexcept
{
    return selected_;
}

} // namespace se::ui::editor

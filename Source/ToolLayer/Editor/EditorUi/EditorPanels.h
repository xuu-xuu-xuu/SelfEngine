#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace se::ui::editor
{

struct EditorHierarchyNode
{
    std::uint64_t id = 0;
    std::uint64_t parent = 0;
    std::size_t depth = 0;
    bool hasChildren = false;
    bool expanded = true;
    std::string label;

    bool operator==(const EditorHierarchyNode&) const = default;
};

class EditorHierarchyModel
{
public:
    void SetNodes(std::vector<EditorHierarchyNode> nodes);
    [[nodiscard]] const std::vector<EditorHierarchyNode>& Nodes() const noexcept;
    [[nodiscard]] std::vector<std::size_t> VisibleRows() const;
    bool Toggle(std::size_t index);
    bool Select(std::uint64_t id);
    [[nodiscard]] std::uint64_t Selected() const noexcept;
    bool Rename(std::uint64_t id, std::string label);
    bool Reparent(std::uint64_t id, std::uint64_t parent);

private:
    [[nodiscard]] bool Visible(std::size_t index) const;
    std::vector<EditorHierarchyNode> nodes_;
    std::uint64_t selected_ = 0;
};

enum class EditorPropertyType : std::uint8_t
{
    String = 0,
    Number,
    Boolean,
    Asset,
    Color,
    Combo,
};

struct EditorProperty
{
    std::string key;
    std::string label;
    EditorPropertyType type = EditorPropertyType::String;
    std::string value;
    bool readOnly = false;
    std::vector<std::string> options;

    bool operator==(const EditorProperty&) const = default;
};

class EditorInspectorModel
{
public:
    void SetTarget(std::uint64_t id, std::string label);
    [[nodiscard]] std::uint64_t Target() const noexcept;
    [[nodiscard]] const std::string& TargetLabel() const noexcept;
    void SetProperties(std::vector<EditorProperty> properties);
    [[nodiscard]] const std::vector<EditorProperty>& Properties() const noexcept;
    bool SetValue(std::string_view key, std::string value);
    [[nodiscard]] bool ConsumeChanged();
    [[nodiscard]] const std::string& LastChangedKey() const noexcept;

private:
    std::uint64_t target_ = 0;
    std::string targetLabel_;
    std::vector<EditorProperty> properties_;
    bool changed_ = false;
    std::string lastChangedKey_;
};

struct EditorAssetEntry
{
    std::string path;
    std::string type;

    bool operator==(const EditorAssetEntry&) const = default;
};

class EditorAssetsModel
{
public:
    void SetEntries(std::vector<EditorAssetEntry> entries);
    [[nodiscard]] const std::vector<EditorAssetEntry>& Entries() const noexcept;
    void SetFilter(std::string_view filter);
    [[nodiscard]] const std::vector<EditorAssetEntry>& Filtered() const noexcept;
    [[nodiscard]] std::vector<EditorAssetEntry> VisibleRange(std::size_t offset, float viewportHeight,
                                                            float rowHeight, std::size_t bufferRows) const;
    [[nodiscard]] bool Selected(std::string_view path) const noexcept;
    void Select(std::string path);
    [[nodiscard]] const std::string& SelectedPath() const noexcept;

private:
    std::vector<EditorAssetEntry> entries_;
    std::vector<EditorAssetEntry> filtered_;
    std::string filter_;
    std::string selected_;
};

} // namespace se::ui::editor

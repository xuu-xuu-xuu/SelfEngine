#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace se::ui::editor
{

struct EditorProjectInfo
{
    std::string name;
    std::string root;
};

struct EditorDocumentInfo
{
    std::string path;
    std::string name;
    bool dirty = false;
};

struct EditorSelection
{
    std::uint64_t entity = 0;
    std::string asset;
    std::string panel;
};

struct EditorAssetInfo
{
    std::string path;
    std::string type;
};

struct EditorSettingsInfo
{
    std::string language = "zh-CN";
    float uiScale = 1.0f;
};

struct EditorLayoutInfo
{
    float leftWidth = 0.2f;
    float centerWidth = 0.56f;
    float rightWidth = 0.24f;
};

// 只读查询契约：不暴露 EditorSession*、World*、AssetStore*。
class EditorContext
{
public:
    virtual ~EditorContext() = default;

    [[nodiscard]] virtual EditorProjectInfo Project() const = 0;
    [[nodiscard]] virtual EditorDocumentInfo Document() const = 0;
    [[nodiscard]] virtual std::vector<EditorSelection> Selection() const = 0;
    [[nodiscard]] virtual std::optional<EditorAssetInfo> Asset(std::string_view path) const = 0;
    [[nodiscard]] virtual EditorSettingsInfo Settings() const = 0;
    [[nodiscard]] virtual EditorLayoutInfo Layout() const = 0;
};

} // namespace se::ui::editor

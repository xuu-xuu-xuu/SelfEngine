#pragma once
#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace se
{
// 稳定面板标识：只用于持久化与显隐，显示名可随语言变化。
enum class EditorPanel { Hierarchy, Inspector, Assets, MaterialEditor, Viewport, UiDesigner, ProjectSettings, BuildExport, Console };
inline constexpr std::size_t EditorPanelCount=9;
const char* EditorPanelId(EditorPanel panel);
bool ParseEditorPanelId(std::string_view id,EditorPanel& output);
struct EditorPanelGeometry
{
    float x=0,y=0,width=0,height=0;
};
struct EditorPanelState
{
    bool visible=true;
    std::optional<EditorPanelGeometry> geometry;
};
// schema 1 的几何为客户区比例；旧的仅可见性文件仍可加载。
class EditorLayout
{
public:
    EditorLayout();
    bool Visible(EditorPanel panel) const;
    void SetVisible(EditorPanel panel,bool visible);
    std::size_t VisibleCount() const;
    // 无效输入不修改状态；小于百万分之一客户区的抖动不产生新修订。
    bool CaptureGeometry(EditorPanel panel,float x,float y,float width,float height,float viewportWidth,float viewportHeight);
    std::optional<EditorPanelGeometry> Geometry(EditorPanel panel,float viewportWidth,float viewportHeight,float uiScale=1) const;
    // 显式恢复默认同时允许替换上次 Load 读到的损坏文件；未来 schema 始终保留。
    void ResetDefaults();
    static std::filesystem::path DefaultPath();
    bool Load(const std::filesystem::path& path,std::string& error);
    bool Save(const std::filesystem::path& path,std::string& error) const;
    // 从有效 .bak 恢复时 Load 返回 true，同时这里/error 保留诊断键；不会自动覆盖损坏主文件。
    const std::string& LastError() const noexcept{return lastError_;}
private:
    std::array<EditorPanelState,EditorPanelCount> panels_{};
    std::string lastError_;
    std::filesystem::path protectedPath_;
    std::string protectedText_;
    bool protectedReplaceable_=false;
    mutable bool resetRequested_=false;
};
}

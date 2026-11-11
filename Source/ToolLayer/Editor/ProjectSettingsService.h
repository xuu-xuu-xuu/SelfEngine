#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace se
{
struct ProjectSettings
{
    std::string name;
    std::string contentRoot="Content";
    std::string entryScene="Scenes/Main.scene.json";
    std::vector<std::string> assets; // 只读摘要（id/type），用于设置面板显示。
};
struct BuildSettings
{
    std::string scriptsProject;
    std::string scriptType;
    std::string bindings;
    std::string entryUi;
};
// project.json / build.json 的字段级读写与校验；复用既有格式，不新增第二套设置。
class ProjectSettingsService
{
public:
    static bool ReadProject(const std::filesystem::path& manifest,ProjectSettings& output,std::string& errorKey);
    static bool WriteProject(const std::filesystem::path& manifest,const ProjectSettings& settings,std::string& errorKey);
    static bool ReadBuild(const std::filesystem::path& buildJson,BuildSettings& output,std::string& errorKey);
    static bool WriteBuild(const std::filesystem::path& buildJson,const BuildSettings& settings,std::string& errorKey);
    // 内存文档沿用同一格式；解码/合并仅校验结构和版本，允许尚未通过引用校验的编辑草稿。
    static bool DecodeProject(std::string_view text,ProjectSettings& output,std::string& errorKey);
    static bool DecodeBuild(std::string_view text,BuildSettings& output,std::string& errorKey);
    static bool MergeProject(std::string_view original,const ProjectSettings& settings,std::string& output,std::string& errorKey);
    static bool MergeBuild(std::string_view original,const BuildSettings& settings,std::string& output,std::string& errorKey);
    // 仅最终保存调用：验证入口文件并按规范路径去重追加 scenes，草稿合并不登记场景。
    static bool PrepareProjectSave(const std::filesystem::path& projectRoot,std::string_view original,
        const ProjectSettings& settings,std::string& output,std::string& errorKey);
    static std::vector<std::string> MissingBuildFields(const BuildSettings& settings);
    // 写盘前的字段级校验：入口/内容根/脚本引用必须真实存在且不越出项目根。
    static bool ValidateProject(const std::filesystem::path& projectRoot,const ProjectSettings& settings,
        std::string& errorKey,std::string& detail);
    static bool ValidateBuild(const std::filesystem::path& projectRoot,const BuildSettings& settings,
        std::string& errorKey,std::string& detail);
};
}

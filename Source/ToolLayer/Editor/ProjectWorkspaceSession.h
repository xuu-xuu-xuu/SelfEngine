#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace se
{
struct RecentProject
{
    std::string name,path,lastOpenedUtc;
    bool available=false;
};
struct EditorPreferences
{
    std::string language="zh-CN";
    float uiScale=1.0f;
    std::string externalEditor;
    // G15：编辑器选中的 `.selut` 资产；相对项目根的 UTF-8 路径，空表示不启用 LUT。
    std::string colorLut;
    bool restoreLastProject=true;
    // 兼容旧调用方；磁盘统一保存带名称与打开时间的最近项目对象。
    std::vector<std::string> recentProjects;
};
struct ProjectPreflight
{
    bool ok=false;
    std::filesystem::path manifest,root,contentRoot,entryScene;
    std::string errorKey,detail;
    bool writable=true;
};
enum class ProjectTemplate { BlankScene,Basic3D };
const char* ProjectTemplateId(ProjectTemplate value);
const char* ProjectTemplateNameKey(ProjectTemplate value);

// 独占项目编辑租约；与 Agent 协调锁、T65 构建租约分开管理。
class ProjectLease
{
public:
    ProjectLease()=default;
    ~ProjectLease();
    ProjectLease(ProjectLease&&) noexcept;
    ProjectLease& operator=(ProjectLease&&) noexcept;
    ProjectLease(const ProjectLease&)=delete;
    ProjectLease& operator=(const ProjectLease&)=delete;
    // 失败时 acquired=false；occupant 给出已占用进程的描述（若可读）。
    static ProjectLease Acquire(const std::filesystem::path& userDirectory,const std::filesystem::path& manifest,
        bool& acquired,std::string& occupant);
    bool Held() const noexcept{return handle_!=nullptr;}
    void Release() noexcept;
private:
    void* handle_=nullptr;
};

// 项目工作流：只读预检、模板创建、最近项目与偏好；不拥有 World、AssetStore 或渲染资源。
class ProjectWorkspaceSession
{
public:
    static std::filesystem::path EngineRootFromExecutable();
    static std::filesystem::path UserDataDirectory(const std::filesystem::path& engineRoot);
    static bool LoadPreferences(const std::filesystem::path& directory,EditorPreferences& output,std::string& error);
    static bool SavePreferences(const std::filesystem::path& directory,const EditorPreferences& preferences,std::string& error);
    static ProjectPreflight Preflight(const std::filesystem::path& requested);
    static bool ValidateProjectName(const std::string& name,std::string& detail);
    static bool CreateProject(ProjectTemplate projectTemplate,const std::string& name,const std::filesystem::path& parent,
        const std::filesystem::path& templateRoot,ProjectPreflight& output,std::string& errorKey,std::string& detail);
    bool Load(const std::filesystem::path& userDirectory,std::string& error);
    bool Save(std::string& error) const;
    bool RecordOpened(const ProjectPreflight& project);
    bool RemoveRecent(const std::string& canonicalPath);
    const std::vector<RecentProject>& Recent() const noexcept{return recent_;}
    const std::filesystem::path& Directory() const noexcept{return directory_;}
    const std::string& Language() const noexcept{return language_;}
    void SetLanguage(std::string language){language_=std::move(language);languageChanged_=true;}
    bool RestoreLastProject() const noexcept{return restoreLast_;}
    void SetRestoreLastProject(bool value){restoreLast_=value;restoreChanged_=true;}
    const std::string& LastProject() const noexcept{return lastProject_;}
private:
    std::filesystem::path directory_;
    std::string language_="zh-CN",lastProject_;
    bool restoreLast_=true;
    mutable bool languageChanged_=false,restoreChanged_=false;
    std::vector<RecentProject> recent_;
};
}

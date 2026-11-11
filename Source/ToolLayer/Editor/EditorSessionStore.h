#pragma once
#include "ProjectWorkspaceSession.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace se
{
struct EditorSessionSnapshot
{
    std::string project;
    std::vector<std::string> documents;
    std::string activeDocument;
    std::vector<std::uint64_t> selection;
    float cameraX=0,cameraY=0,cameraZ=0,cameraYaw=0,cameraPitch=0;
    int layoutVersion=1;
    bool operator==(const EditorSessionSnapshot&) const = default;
};
struct RecoveryDocument
{
    std::string documentId,kind,relativePath,sourceDigest,content;
    std::uint64_t revision=0;
};
struct RecoveryGroup
{
    std::string id,createdUtc;
    std::vector<RecoveryDocument> documents;
};
// 偏好/会话的版本化原子存储；损坏文件保留原文件并回退默认，不静默覆盖。
class EditorSessionStore
{
public:
    static bool LoadPreferences(const std::filesystem::path& directory,EditorPreferences& output,std::string& error);
    static bool SavePreferences(const std::filesystem::path& directory,const EditorPreferences& preferences,std::string& error);
    static bool LoadSession(const std::filesystem::path& directory,EditorSessionSnapshot& output,std::string& error);
    static bool SaveSession(const std::filesystem::path& directory,const EditorSessionSnapshot& session,std::string& error);
    static bool ClearSession(const std::filesystem::path& directory,std::string& error);
    static std::filesystem::path ProjectDirectory(const std::filesystem::path& projectRoot);
    // relativePath 为空表示未命名文档；sourceDigest 为空表示没有已保存的基础文件。
    // 完整组仅在最后发布 manifest 后可见；读恢复内容不会写回原文档。
    // protectedGroups 不参与容量/版本淘汰；无法满足上限时拒绝本次写入并保留所有旧组。
    static bool WriteRecoveryGroup(const std::filesystem::path& directory,const std::filesystem::path& projectRoot,
        const std::vector<RecoveryDocument>& documents,std::string& error,
        std::uint64_t maximumTotalBytes=200ull*1024*1024,std::size_t maximumRevisions=5,
        const std::vector<std::string>& protectedGroups={});
    // 按新到旧返回已校验的完整组；跳过坏组并通过 error 返回稳定诊断键。
    static std::vector<RecoveryGroup> ListRecoveryGroups(const std::filesystem::path& directory,
        const std::filesystem::path& projectRoot,std::string& error);
    static bool ReadRecoveryGroup(const std::filesystem::path& directory,const std::filesystem::path& projectRoot,
        const std::string& id,RecoveryGroup& output,std::string& error);
    static bool DiscardRecoveryGroup(const std::filesystem::path& directory,const std::string& id,std::string& error);
    // 启动时、写入本进程 marker 前调用；仅清理无人使用且名称/路径校验通过的本服务候选目录。
    static bool CleanupAbandonedCandidates(const std::filesystem::path& directory,std::string& error);
    // 自动恢复副本：独立目录，最多保留若干完整修订。
    static bool WriteRecovery(const std::filesystem::path& directory,const std::string& documentId,const std::string& content,std::uint32_t revision,
        std::uint64_t maximumTotalBytes,std::string& error);
    static std::size_t RecoveryCount(const std::filesystem::path& directory,const std::string& documentId);
    static std::uint64_t RecoveryBytes(const std::filesystem::path& directory,const std::string& documentId);
    struct CrashMarker {std::uint32_t processId=0;std::uint64_t processCreation=0;std::string startedUtc;};
    static bool WriteCrashMarker(const std::filesystem::path& directory,const CrashMarker& marker,std::string& error);
    static bool ReadCrashMarker(const std::filesystem::path& directory,CrashMarker& marker,std::string& error);
    static bool ClearCrashMarker(const std::filesystem::path& directory,std::string& error);
    static bool IsProcessActive(const CrashMarker& marker);
    static std::filesystem::path EngineRootFromExecutable();
    static std::filesystem::path UserDataDirectory();
    static std::uint64_t CurrentProcessCreation();
    enum class StartupState { Clean, ActiveInstance, UncleanExit };
    static StartupState InspectStartup(const std::filesystem::path& directory,CrashMarker& marker,std::string& error);
    struct RecoveryEntry {std::string documentId;std::uint32_t revision=0;std::uint64_t bytes=0;};
    static std::vector<RecoveryEntry> ListRecovery(const std::filesystem::path& directory);
    class RecoveryDebounce
    {
    public:
        explicit RecoveryDebounce(std::uint64_t intervalSeconds=60):interval_(intervalSeconds) {}
        bool Due(std::uint64_t nowSeconds,std::uint64_t revision) const;
        void Mark(std::uint64_t nowSeconds,std::uint64_t revision);
    private:
        std::uint64_t interval_=60,lastSeconds_=0,lastRevision_=0;
        bool armed_=false;
    };
};
}

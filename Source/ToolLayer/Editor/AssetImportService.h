#pragma once
#include "ResourceLayer/Assets/StaticModel.h"
#include <filesystem>
#include <string>
#include <vector>

namespace se
{
class AssetStore;
struct AssetImportRequest
{
    std::vector<std::filesystem::path> sources;
    std::filesystem::path contentRoot;
    std::filesystem::path targetDirectory; // 相对 contentRoot 的目录，空表示根。
};
struct AssetImportFile
{
    std::filesystem::path source,target;
    bool primary=false; // 用户选择的主输入；共享依赖只保留一份。
    bool copyRequired=true; // 项目内重导入只登记，不移动原文件。
    bool registerAsset=false; // 主输入和可登记图像；缓冲依赖不生成 sidecar。
    std::string sourceDigest; // 计划批准时的 SHA-256，执行时重新验证。
    std::string sidecarDigest; // 仅项目内已有 sidecar；空表示目标不得存在 sidecar。
};
struct AssetImportPlan
{
    bool valid=false;
    std::filesystem::path contentRoot;
    std::vector<AssetImportFile> files;
    std::vector<std::string> conflicts,unsupported;
    std::vector<AssetDiagnostic> diagnostics; // 每个被拒绝的主输入组及其原因。
    std::string errorKey,detail;
};
// 先规划明确的依赖闭包，确认后执行；只清理本次创建的文件和 sidecar。
class AssetImportService
{
public:
    static bool SupportedExtension(const std::filesystem::path& path);
    static AssetImportPlan Plan(const AssetImportRequest& request);
    static bool Execute(const AssetImportPlan& plan,std::string& errorKey,std::string& detail);
    // 登记 plan.target；失败回滚复制/登记并刷新 AssetStore 索引。已有文件和 sidecar 只读保留。
    static bool ExecuteRegistered(const AssetImportPlan& plan,AssetStore& assets,
        std::vector<AssetId>& imported,std::string& errorKey,std::string& detail);
};
}

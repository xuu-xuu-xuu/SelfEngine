#pragma once
#include "ResourceLayer/Assets/StaticModel.h"
#include <filesystem>
#include <string>
#include <vector>

namespace se
{
class AssetStore;
class World;
class UiTree;
enum class EditorAssetDocumentKind { Scene,Material,Ui,Bindings };
struct EditorAssetDocument
{
    EditorAssetDocumentKind kind=EditorAssetDocumentKind::Scene;
    std::filesystem::path path;
    std::string text;
};
struct EditorAssetReference
{
    AssetId asset;
    std::filesystem::path document;
    std::string pointer;
    std::uint64_t entity=0;
    bool inMemory=false;
    EditorAssetDocumentKind kind=EditorAssetDocumentKind::Scene;
};
struct EditorAssetIssue {std::string key,detail;EditorAssetReference reference;};
enum class EditorAssetAction { Move,Delete };
struct EditorAssetActionRequest
{
    EditorAssetAction action=EditorAssetAction::Move;
    std::filesystem::path projectRoot,contentRoot,source,target; // target 为内容根内的完整目标文件路径。
    std::vector<EditorAssetDocument> documents;
};
struct EditorAssetActionFile
{
    std::filesystem::path source,target;
    std::string digest;
    bool rewrite=false; // project.json 的事务更新；其余文件按原字节移动/删除。
    std::string replacement;
};
struct EditorAssetActionPlan
{
    bool valid=false;
    AssetId id;
    EditorAssetActionRequest request;
    std::vector<std::filesystem::path> sources; // 所选资源和将随之移动的明确依赖。
    std::vector<EditorAssetActionFile> files; // 包括 sidecar；删除的 target 为空。
    std::vector<std::string> requirements; // 确认界面说明的稳定文本键。
    std::vector<EditorAssetReference> references;
    std::vector<EditorAssetIssue> diagnostics;
};
class EditorAssetActions
{
public:
    static EditorAssetActionPlan Plan(const EditorAssetActionRequest& request,const AssetStore& assets);
    static bool Execute(const EditorAssetActionPlan& plan,AssetStore& assets,
        const std::vector<EditorAssetDocument>& currentDocuments,std::vector<EditorAssetIssue>& diagnostics);
    // World/UI 只读；材质内存文本优先于其磁盘版本。调用方保证引用的编辑快照在调用期间稳定。
    static bool ValidatePlayAssets(const World& world,const UiTree* ui,AssetStore& assets,
        const std::vector<EditorAssetDocument>& overrides,std::vector<EditorAssetIssue>& diagnostics);
    // 导入已经成功后单独同步目录声明；失败不宣称导入已回滚。
    static bool SyncProjectAssets(const std::filesystem::path& projectRoot,AssetStore& assets,
        std::vector<EditorAssetIssue>& diagnostics);
};
}

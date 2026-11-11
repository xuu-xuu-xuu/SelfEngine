#include "AssetImportService.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace se
{
namespace
{
namespace fs=std::filesystem;
std::string Lower(std::string value)
{
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char character){return static_cast<char>(std::tolower(character));});
    return value;
}
bool IsModel(const fs::path& path)
{
    const auto extension=Lower(path.extension().string());
    return extension==".glb"||extension==".gltf"||extension==".fbx";
}
bool IsImage(const fs::path& path)
{
    const auto extension=Lower(path.extension().string());
    return extension==".png"||extension==".jpg"||extension==".jpeg";
}
fs::path Canonical(const fs::path& path)
{
    std::error_code error;
    const auto result=fs::weakly_canonical(path,error);
    return error?fs::path{}:result;
}
struct PathLess
{
    bool operator()(const fs::path& left,const fs::path& right) const
    {
        const auto a=left.generic_wstring(),b=right.generic_wstring();
        return CompareStringOrdinal(a.c_str(),static_cast<int>(a.size()),b.c_str(),static_cast<int>(b.size()),TRUE)==CSTR_LESS_THAN;
    }
};
bool SamePath(const fs::path& a,const fs::path& b)
{
    return !a.empty()&&!b.empty()&&!PathLess{}(a,b)&&!PathLess{}(b,a);
}
fs::path Metadata(const fs::path& source){auto result=source;result+=".asset.json";return result;}
bool Exists(const fs::path& path)
{
    std::error_code error;
    const bool exists=fs::exists(path,error);
    return exists||static_cast<bool>(error); // 无法确认不存在的目标也不能覆盖。
}
bool Digest(const fs::path& path,std::string& result)
{
    std::vector<std::uint8_t> bytes;
    if(!asset_io::Read(path,bytes))return false;
    result=asset_io::Hash(bytes);
    return !result.empty();
}
bool Fail(std::string& key,std::string& detail,const char* value,const fs::path& path)
{key=value;detail=asset_io::Utf8(path);return false;}
struct Group
{
    fs::path source;
    std::vector<AssetImportFile> files;
    std::string errorKey,detail;
};
void Reject(Group& group,const char* key,const fs::path& path)
{
    if(group.errorKey.empty()){group.errorKey=key;group.detail=asset_io::Utf8(path);}
}
bool AddFile(Group& group,const fs::path& source,const fs::path& target,bool primary,const fs::path& root)
{
    AssetImportFile file;
    file.source=Canonical(source);file.target=Canonical(target);file.primary=primary;
    file.copyRequired=!SamePath(file.source,file.target);file.registerAsset=primary||IsImage(source);
    if(file.source.empty()||file.target.empty()||!asset_io::Within(root,file.target)||!asset_io::Within(root,Metadata(file.target)))
    {Reject(group,"Assets.ReferenceOutsideRoot",target);return false;}
    std::error_code error;
    if(!fs::is_regular_file(file.source,error)||!Digest(file.source,file.sourceDigest))
    {Reject(group,primary?"Assets.ImportInvalidSource":"Assets.ImportDependencyMissing",source);return false;}
    if(file.copyRequired&&(Exists(file.target)||Exists(Metadata(file.target))))
    {Reject(group,"Assets.ImportConflict",file.target);return false;}
    if(!file.copyRequired&&Exists(Metadata(file.target))&&!Digest(Metadata(file.target),file.sidecarDigest))
    {Reject(group,"Assets.ImportConflict",Metadata(file.target));return false;}
    for(auto& previous:group.files)
    {
        if(!SamePath(previous.target,file.target))continue;
        if(!SamePath(previous.source,file.source)||previous.sourceDigest!=file.sourceDigest)
        {Reject(group,"Assets.ImportConflict",file.target);return false;}
        previous.primary|=file.primary;previous.registerAsset|=file.registerAsset;return true;
    }
    group.files.push_back(std::move(file));return true;
}
Group PlanGroup(const fs::path& requested,const fs::path& targetRoot,const fs::path& root)
{
    Group group;group.source=requested;
    const auto source=Canonical(requested);
    if(source.empty()||!AssetImportService::SupportedExtension(source))
    {Reject(group,"Assets.ImportInvalidSource",requested);return group;}
    // 已在项目中的资源原地登记；targetDirectory 只用于外部复制。
    const auto target=asset_io::Within(root,source)?source:targetRoot/source.filename();
    if(!AddFile(group,source,target,true,root))return group;
    if(IsModel(source))
    {
        StaticModel model;std::vector<AssetDiagnostic> diagnostics;
        ImportOptions options;options.allowedRoot=source.parent_path();
        if(!ImportStaticModel(source,options,model,diagnostics))
        {
            Reject(group,"Assets.ImportInvalidSource",source);
            if(!diagnostics.empty())group.detail+=" -> "+diagnostics.back().key+": "+diagnostics.back().detail;
            return group;
        }
        for(const auto& dependency:model.dependencies)
        {
            fs::path resolved;std::string relative;
            if(!asset_io::ResolveRelative(source,source.parent_path(),dependency,resolved,relative))
            {Reject(group,"Assets.ReferenceOutsideRoot",asset_io::FromUtf8(dependency));return group;}
            if(!AddFile(group,resolved,target.parent_path()/asset_io::FromUtf8(relative),false,root))return group;
        }
    }
    for(const auto& file:group.files)
    {
        if(IsImage(file.source))
        {
            ImageData image;std::vector<AssetDiagnostic> diagnostics;
            if(!DecodeImage(file.source,image,diagnostics))
            {Reject(group,"Assets.ImportInvalidSource",file.source);return group;}
        }
        std::string digest;
        if(!Digest(file.source,digest)||digest!=file.sourceDigest)
        {Reject(group,"Assets.ImportSourceChanged",file.source);return group;}
    }
    return group;
}

bool ValidatePlan(const AssetImportPlan& plan,std::string& errorKey,std::string& detail)
{
    if(!plan.valid||plan.files.empty())return Fail(errorKey,detail,"Assets.ImportNothingSupported",{});
    std::error_code error;
    if(!plan.contentRoot.is_absolute()||!SamePath(Canonical(plan.contentRoot),plan.contentRoot)||!fs::is_directory(plan.contentRoot,error))
        return Fail(errorKey,detail,"Assets.ImportContentRoot",plan.contentRoot);
    std::set<fs::path,PathLess> targets;
    for(const auto& file:plan.files)
    {
        if(!file.source.is_absolute()||!file.target.is_absolute()||!SamePath(Canonical(file.source),file.source)
            ||!SamePath(Canonical(file.target),file.target)||!asset_io::Within(plan.contentRoot,file.target)
            ||!asset_io::Within(plan.contentRoot,Metadata(file.target))
            ||file.copyRequired==SamePath(file.source,file.target))
            return Fail(errorKey,detail,"Assets.ReferenceOutsideRoot",file.target);
        if(file.registerAsset!=(file.primary||IsImage(file.source))||(file.primary&&!AssetImportService::SupportedExtension(file.source)))
            return Fail(errorKey,detail,"Assets.ImportInvalidSource",file.source);
        if(!targets.insert(file.target).second||(file.registerAsset&&!targets.insert(Metadata(file.target)).second))
            return Fail(errorKey,detail,"Assets.ImportConflict",file.target);
        std::string digest;
        if(file.sourceDigest.empty()||!Digest(file.source,digest)||digest!=file.sourceDigest)
            return Fail(errorKey,detail,"Assets.ImportSourceChanged",file.source);
        if(file.copyRequired&&Exists(file.target))return Fail(errorKey,detail,"Assets.ImportConflict",file.target);
        if(file.sidecarDigest.empty())
        {
            if(Exists(Metadata(file.target)))return Fail(errorKey,detail,"Assets.ImportConflict",Metadata(file.target));
        }
        else if(file.copyRequired||!Digest(Metadata(file.target),digest)||digest!=file.sidecarDigest)
            return Fail(errorKey,detail,"Assets.ImportSourceChanged",Metadata(file.target));
    }
    return true;
}

struct OwnedFile{fs::path path;std::string digest;};
struct ImportTransaction
{
    const fs::path& root;
    std::vector<OwnedFile> files;
    std::vector<fs::path> directories;
    explicit ImportTransaction(const AssetImportPlan& plan):root(plan.contentRoot){files.reserve(plan.files.size()*2);}
    bool CreateParents(const fs::path& directory)
    {
        std::vector<fs::path> missing;
        for(auto path=directory;!Exists(path);path=path.parent_path())
        {
            if(path.empty()||!asset_io::Within(root,path))return false;
            missing.push_back(path);
        }
        for(auto item=missing.rbegin();item!=missing.rend();++item)
        {
            directories.push_back(*item);
            std::error_code error;
            const bool created=fs::create_directory(*item,error);
            if(!created)directories.pop_back();
            if(error)return false;
        }
        return true;
    }
    bool Add(const fs::path& path,std::span<const std::uint8_t> bytes)
    {
        if(!SamePath(Canonical(path),path)||!asset_io::Within(root,path)||!CreateParents(path.parent_path()))return false;
        // 先记录候选，原子创建成功后才拥有它；从不使用覆盖提交。
        files.push_back({path,asset_io::Hash(bytes)});
        if(files.back().digest.empty()||!asset_io::WriteAtomic(path,bytes,false)){files.pop_back();return false;}
        return true;
    }
    bool Rollback(std::string& detail)
    {
        bool complete=true;
        for(auto item=files.rbegin();item!=files.rend();++item)
        {
            if(!Exists(item->path))continue;
            std::string digest;std::error_code error;
            if(!SamePath(Canonical(item->path),item->path)||!asset_io::Within(root,item->path)||!Digest(item->path,digest)||digest!=item->digest
                ||!fs::remove(item->path,error)||error)
            {complete=false;detail+="; "+asset_io::Utf8(item->path);}
        }
        // 只删除仍为空的自建目录，不递归触碰其他任务或用户文件。
        for(auto item=directories.rbegin();item!=directories.rend();++item)
        {
            std::error_code error;
            if(SamePath(Canonical(*item),*item)&&asset_io::Within(root,*item)&&fs::is_empty(*item,error)&&!error)fs::remove(*item,error);
        }
        return complete;
    }
};
bool CopyFiles(const AssetImportPlan& plan,ImportTransaction& transaction,std::string& errorKey,std::string& detail)
{
    for(const auto& file:plan.files)
    {
        if(!file.copyRequired)continue;
        if(Exists(file.target)||Exists(Metadata(file.target)))return Fail(errorKey,detail,"Assets.ImportConflict",file.target);
        std::vector<std::uint8_t> bytes;
        if(!asset_io::Read(file.source,bytes)||asset_io::Hash(bytes)!=file.sourceDigest)
            return Fail(errorKey,detail,"Assets.ImportSourceChanged",file.source);
        if(!transaction.Add(file.target,bytes))
            return Fail(errorKey,detail,Exists(file.target)?"Assets.ImportConflict":"Assets.ImportWriteFailed",file.target);
    }
    return true;
}
bool VerifyCopied(const AssetImportPlan& plan,std::string& errorKey,std::string& detail)
{
    for(const auto& file:plan.files)
    {
        std::string digest;
        if(!Digest(file.source,digest)||digest!=file.sourceDigest||!Digest(file.target,digest)||digest!=file.sourceDigest)
            return Fail(errorKey,detail,"Assets.ImportSourceChanged",file.source);
        if(file.primary&&IsModel(file.target))
        {
            // 从最终路径再次导入，验证保留的相对目录确实能解析完整依赖。
            StaticModel model;std::vector<AssetDiagnostic> diagnostics;
            ImportOptions options;options.allowedRoot=file.target.parent_path();
            if(!ImportStaticModel(file.target,options,model,diagnostics))
                return Fail(errorKey,detail,"Assets.ImportInvalidSource",file.target);
            for(const auto& dependency:model.dependencies)
            {
                fs::path resolved;std::string relative;
                if(!asset_io::ResolveRelative(file.target,options.allowedRoot,dependency,resolved,relative)
                    ||std::none_of(plan.files.begin(),plan.files.end(),[&](const auto& candidate){return SamePath(candidate.target,resolved);}))
                    return Fail(errorKey,detail,"Assets.ImportDependencyMissing",file.target);
            }
        }
    }
    return true;
}
bool VerifySidecars(const AssetImportPlan& plan,const ImportTransaction& transaction,std::string& errorKey,std::string& detail)
{
    for(const auto& file:plan.files)
    {
        const auto sidecar=Metadata(file.target);
        const auto owned=std::find_if(transaction.files.begin(),transaction.files.end(),[&](const auto& item){return SamePath(item.path,sidecar);});
        const auto& expected=owned!=transaction.files.end()?owned->digest:file.sidecarDigest;
        if(expected.empty())
        {
            if(Exists(sidecar))return Fail(errorKey,detail,"Assets.ImportConflict",sidecar);
        }
        else
        {
            std::string digest;
            if(!Digest(sidecar,digest)||digest!=expected)return Fail(errorKey,detail,"Assets.ImportSourceChanged",sidecar);
        }
    }
    return true;
}
void RegistrationError(const std::vector<AssetDiagnostic>& diagnostics,const fs::path& path,std::string& key,std::string& detail)
{
    Fail(key,detail,"Assets.ImportRegistrationFailed",path);
    if(!diagnostics.empty())detail+=" -> "+diagnostics.back().key+": "+diagnostics.back().detail;
}
}

bool AssetImportService::SupportedExtension(const std::filesystem::path& path)
{
    return IsModel(path)||IsImage(path)||Lower(path.extension().string())==".sebin"||Lower(path.filename().string()).ends_with(".material.json");
}
AssetImportPlan AssetImportService::Plan(const AssetImportRequest& request)
{
    AssetImportPlan plan;
    if(request.sources.empty()){plan.errorKey="Assets.ImportNoFiles";return plan;}
    plan.contentRoot=Canonical(request.contentRoot);
    std::error_code error;
    if(plan.contentRoot.empty()||!fs::is_directory(plan.contentRoot,error))
    {Fail(plan.errorKey,plan.detail,"Assets.ImportContentRoot",request.contentRoot);return plan;}
    const auto targetRoot=Canonical(plan.contentRoot/request.targetDirectory);
    if(request.targetDirectory.is_absolute()||request.targetDirectory.has_root_name()||targetRoot.empty()||!asset_io::Within(plan.contentRoot,targetRoot))
    {Fail(plan.errorKey,plan.detail,"Assets.ReferenceOutsideRoot",request.targetDirectory);return plan;}
    std::set<fs::path,PathLess> inputs;
    std::vector<Group> groups;
    for(const auto& source:request.sources)
    {
        const auto canonical=Canonical(source);
        if(!canonical.empty()&&!inputs.insert(canonical).second)continue;
        try{groups.push_back(PlanGroup(source,targetRoot,plan.contentRoot));}
        catch(const std::exception& failure)
        {Group group;group.source=source;group.errorKey="Assets.ImportInvalidSource";group.detail=failure.what();groups.push_back(std::move(group));}
    }
    // 比较所有仍有效的主输入组；不同来源争用同一目标时同时拒绝双方，避免顺序决定胜者。
    struct Reservation{std::size_t group,file;bool metadata;};
    std::map<fs::path,std::vector<Reservation>,PathLess> reservations;
    for(std::size_t index=0;index<groups.size();++index)
    {
        if(!groups[index].errorKey.empty())continue;
        for(std::size_t file=0;file<groups[index].files.size();++file)
        {
            const auto& candidate=groups[index].files[file];
            reservations[candidate.target].push_back({index,file,false});
            if(candidate.registerAsset)reservations[Metadata(candidate.target)].push_back({index,file,true});
        }
    }
    for(const auto& [path,owners]:reservations)
        for(std::size_t a=0;a<owners.size();++a)
            for(std::size_t b=a+1;b<owners.size();++b)
            {
                const auto& left=owners[a];const auto& right=owners[b];
                const auto& first=groups[left.group].files[left.file];const auto& second=groups[right.group].files[right.file];
                if(left.metadata==right.metadata&&SamePath(first.source,second.source)&&SamePath(first.target,second.target)
                    &&first.sourceDigest==second.sourceDigest)continue;
                Reject(groups[left.group],"Assets.ImportConflict",path);Reject(groups[right.group],"Assets.ImportConflict",path);
            }
    std::map<fs::path,std::size_t,PathLess> targets;
    for(const auto& group:groups)
    {
        if(!group.errorKey.empty())
        {
            const auto description=asset_io::Utf8(group.source.filename())+" -> "+group.detail;
            (group.errorKey=="Assets.ImportConflict"?plan.conflicts:plan.unsupported).push_back(description);
            plan.diagnostics.push_back({group.errorKey,description,false});continue;
        }
        for(const auto& file:group.files)
        {
            const auto [found,inserted]=targets.emplace(file.target,plan.files.size());
            if(inserted)plan.files.push_back(file);
            else{auto& previous=plan.files[found->second];previous.primary|=file.primary;previous.registerAsset|=file.registerAsset;}
        }
    }
    plan.valid=!plan.files.empty();
    if(!plan.valid)plan.errorKey="Assets.ImportNothingSupported";
    return plan;
}
bool AssetImportService::Execute(const AssetImportPlan& plan,std::string& errorKey,std::string& detail)
{
    errorKey.clear();detail.clear();
    if(!ValidatePlan(plan,errorKey,detail))return false;
    ImportTransaction transaction(plan);
    try
    {
        if(CopyFiles(plan,transaction,errorKey,detail)&&VerifyCopied(plan,errorKey,detail)
            &&VerifySidecars(plan,transaction,errorKey,detail))return true;
    }
    catch(const std::exception& failure){errorKey="Assets.ImportWriteFailed";detail=failure.what();}
    if(!transaction.Rollback(detail))errorKey="Assets.ImportRollbackIncomplete";
    return false;
}
bool AssetImportService::ExecuteRegistered(const AssetImportPlan& plan,AssetStore& assets,
    std::vector<AssetId>& imported,std::string& errorKey,std::string& detail)
{
    imported.clear();errorKey.clear();detail.clear();
    if(!ValidatePlan(plan,errorKey,detail))return false;
    if(!SamePath(plan.contentRoot,Canonical(assets.SourceRoot())))return Fail(errorKey,detail,"Assets.ImportContentRoot",assets.SourceRoot());
    const auto previousStore=assets;
    ImportTransaction transaction(plan);
    const auto rollback=[&]()
    {
        if(!transaction.Rollback(detail))errorKey="Assets.ImportRollbackIncomplete";
        std::vector<AssetDiagnostic> diagnostics;
        // Scan 失败不会提交新索引；已有坏 sidecar 时恢复事务前快照，避免残留刚登记的 ID。
        if(!assets.Scan(diagnostics))assets=previousStore;
        imported.clear();return false;
    };
    try
    {
        if(!CopyFiles(plan,transaction,errorKey,detail)||!VerifyCopied(plan,errorKey,detail)
            ||!VerifySidecars(plan,transaction,errorKey,detail))return rollback();
        for(const auto& file:plan.files)
        {
            if(!file.registerAsset)continue;
            const auto sidecar=Metadata(file.target);
            if(!file.sidecarDigest.empty())
            {
                std::string digest;
                if(!Digest(sidecar,digest)||digest!=file.sidecarDigest)
                {Fail(errorKey,detail,"Assets.ImportSourceChanged",sidecar);return rollback();}
                continue; // 既有 ID/选项/字节保持不变，统一交给 Scan 重新登记。
            }
            if(Exists(sidecar)){Fail(errorKey,detail,"Assets.ImportConflict",sidecar);return rollback();}
            const auto id=asset_io::NewId();
            if(id.empty()){Fail(errorKey,detail,"Assets.IdCreationFailed",file.target);return rollback();}
            // 先用 AssetStore 现有 schema 原子占位，RegisterSource 只会重写本事务拥有的 sidecar。
            const nlohmann::json metadata={{"version",1},{"id",id},{"options",{{"scale",1.0f},{"generateTangents",true}}}};
            const auto text=metadata.dump(2);
            if(!transaction.Add(sidecar,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()}))
            {Fail(errorKey,detail,Exists(sidecar)?"Assets.ImportConflict":"Assets.ImportWriteFailed",sidecar);return rollback();}
            AssetId registered;std::vector<AssetDiagnostic> diagnostics;
            if(!assets.RegisterSource(file.target,{},registered,diagnostics)||registered!=id)
            {RegistrationError(diagnostics,file.target,errorKey,detail);return rollback();}
            if(!Digest(sidecar,transaction.files.back().digest))
            {Fail(errorKey,detail,"Assets.ImportRegistrationFailed",sidecar);return rollback();}
        }
        std::vector<AssetDiagnostic> diagnostics;
        if(!assets.Scan(diagnostics)){RegistrationError(diagnostics,plan.contentRoot,errorKey,detail);return rollback();}
        if(!VerifyCopied(plan,errorKey,detail)||!VerifySidecars(plan,transaction,errorKey,detail))return rollback();
        const auto ids=assets.ListIds();
        for(const auto& file:plan.files)
        {
            if(!file.registerAsset)continue;
            std::string digest;
            if(!file.sidecarDigest.empty()&&(!Digest(Metadata(file.target),digest)||digest!=file.sidecarDigest))
            {Fail(errorKey,detail,"Assets.ImportSourceChanged",Metadata(file.target));return rollback();}
            const auto found=std::find_if(ids.begin(),ids.end(),[&](const auto& id){return SamePath(Canonical(assets.Resolve(id)),file.target);});
            if(found==ids.end()){Fail(errorKey,detail,"Assets.ImportRegistrationFailed",file.target);return rollback();}
            imported.push_back(*found);
        }
        return true;
    }
    catch(const std::exception& failure)
    {errorKey="Assets.ImportRegistrationFailed";detail=failure.what();return rollback();}
}
}

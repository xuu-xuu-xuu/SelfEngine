#include "EditorAssetActions.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "FunctionLayer/GameUI/UiTree.h"
#include "ToolLayer/Tools/Project.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <map>
#include <optional>
#include <set>

namespace se
{
namespace
{
namespace fs=std::filesystem;
using Json=nlohmann::json;
std::string Lower(std::string value)
{std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});return value;}
bool Same(const fs::path& a,const fs::path& b)
{
    if(a.empty()||b.empty())return false;
    const auto left=a.generic_wstring(),right=b.generic_wstring();
    return CompareStringOrdinal(left.c_str(),static_cast<int>(left.size()),right.c_str(),static_cast<int>(right.size()),TRUE)==CSTR_EQUAL;
}
fs::path Absolute(const fs::path& path){return fs::absolute(path).lexically_normal();}
fs::path Metadata(fs::path path){path+=".asset.json";return path;}
bool Safe(const fs::path& root,const fs::path& path)
{
    if(!path.is_absolute()||!asset_io::Within(root,path)||!Same(Absolute(path),fs::weakly_canonical(path)))return false;
    fs::path part;
    for(const auto& component:path)
    {
        part/=component;const DWORD flags=GetFileAttributesW(part.c_str());
        if(flags!=INVALID_FILE_ATTRIBUTES){if(flags&FILE_ATTRIBUTE_REPARSE_POINT)return false;}
        else if(GetLastError()!=ERROR_FILE_NOT_FOUND&&GetLastError()!=ERROR_PATH_NOT_FOUND)return false;
    }
    return true;
}
bool Issue(std::vector<EditorAssetIssue>& diagnostics,const char* key,const fs::path& path={},std::string detail={})
{
    diagnostics.push_back({key,detail.empty()?asset_io::Utf8(path):std::move(detail),{{},path,{}}});return false;
}
std::string HashText(std::string_view text)
{return asset_io::Hash({reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});}
bool Read(const fs::path& path,std::string& text)
{
    std::vector<std::uint8_t> bytes;if(!asset_io::Read(path,bytes))return false;
    text.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());return true;
}
Json Parse(std::string_view text)
{
    bool duplicate=false;std::vector<std::set<std::string>> keys;
    auto json=Json::parse(text.begin(),text.end(),[&](int depth,Json::parse_event_t event,Json& value)
    {
        if(depth>64)throw std::runtime_error("document depth");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        else if(event==Json::parse_event_t::key&&!keys.empty()&&!keys.back().insert(value.get<std::string>()).second)duplicate=true;
        else if(event==Json::parse_event_t::object_end)keys.pop_back();return true;
    });
    if(duplicate)throw std::runtime_error("duplicate document key");return json;
}
std::string Type(const fs::path& path)
{
    const auto extension=Lower(path.extension().string());
    if(extension==".gltf"||extension==".glb"||extension==".fbx")return "model";
    if(extension==".png"||extension==".jpg"||extension==".jpeg")return "image";
    if(IsMaterialDocumentPath(path))return "material";
    if(extension==".sebin")return "shader";
    if(extension==".tod")return "tod";
    return {};
}
std::optional<EditorAssetDocumentKind> Kind(const fs::path& path)
{
    const auto name=Lower(asset_io::Utf8(path.filename()));
    if(name.ends_with(".scene.json"))return EditorAssetDocumentKind::Scene;
    if(name.ends_with(".material.json"))return EditorAssetDocumentKind::Material;
    if(name.ends_with(".ui.json"))return EditorAssetDocumentKind::Ui;
    if(name.ends_with(".bindings.json"))return EditorAssetDocumentKind::Bindings;
    return {};
}
std::uint64_t EntityId(const Json& value)
{
    if(value.is_number_unsigned())return value.get<std::uint64_t>();
    if(!value.is_string())return 0;
    const auto text=value.get<std::string>();std::uint64_t id=0;
    const auto result=std::from_chars(text.data(),text.data()+text.size(),id);
    return result.ec==std::errc{}&&result.ptr==text.data()+text.size()?id:0;
}
std::string PointerPart(std::string text)
{
    std::string result;for(char c:text){if(c=='~')result+="~0";else if(c=='/')result+="~1";else result+=c;}return result;
}
bool ReferenceField(EditorAssetDocumentKind kind,const std::string& pointer)
{
    if(kind==EditorAssetDocumentKind::Scene)return pointer.ends_with("/asset")||pointer.find("/materials/")!=std::string::npos||pointer=="/environment/todProfile";
    if(kind==EditorAssetDocumentKind::Material)return pointer.starts_with("/textures/")||pointer=="/shader";
    if(kind==EditorAssetDocumentKind::Ui)return pointer.ends_with("/image");
    return pointer.find("/Fields/")!=std::string::npos&&pointer.ends_with("/Value");
}
void References(const Json& value,EditorAssetDocumentKind kind,const std::set<AssetId>& ids,EditorAssetReference location,std::vector<EditorAssetReference>& output)
{
    if(value.is_string())
    {const auto id=value.get<std::string>();if(ids.contains(id)&&ReferenceField(kind,location.pointer)){location.asset=id;output.push_back(std::move(location));}return;}
    if(value.is_object())
    {
        if(value.contains("id"))location.entity=EntityId(value["id"]);
        else if(value.contains("Entity"))location.entity=EntityId(value["Entity"]);
        for(auto field=value.begin();field!=value.end();++field)
        {auto child=location;child.pointer+="/"+PointerPart(field.key());References(field.value(),kind,ids,std::move(child),output);}
    }
    else if(value.is_array())for(std::size_t i=0;i<value.size();++i)
    {auto child=location;child.pointer+="/"+std::to_string(i);References(value[i],kind,ids,std::move(child),output);}
}
bool DocumentReferences(const EditorAssetDocument& document,bool memory,const std::set<AssetId>& ids,
    std::vector<EditorAssetReference>& output,std::vector<EditorAssetIssue>& diagnostics)
{
    try
    {
        const auto json=Parse(document.text);std::string key;bool valid=false;
        if(document.kind==EditorAssetDocumentKind::Scene)
        {World candidate{World::StagingTag{}};valid=DeserializeScene(document.text,candidate,key);}
        else if(document.kind==EditorAssetDocumentKind::Ui)
        {UiTree candidate;valid=candidate.LoadJson(document.text,key);}
        else if(document.kind==EditorAssetDocumentKind::Material)
        {MaterialDocument candidate;std::vector<AssetDiagnostic> errors;valid=DeserializeMaterialDocument(document.text,candidate,errors);if(!errors.empty())key=errors.back().key;}
        else
        {
            valid=json.is_object()&&json.value("Version",0)==1&&json.contains("Scripts")&&json["Scripts"].is_array();
            if(valid)for(const auto& script:json["Scripts"])
            {
                if(!script.is_object()||!script.contains("Type")||!script["Type"].is_string()||!script.contains("Entity")
                    ||EntityId(script["Entity"])==0||!script.contains("Fields")||!script["Fields"].is_object()
                    ||(script.contains("Enabled")&&!script["Enabled"].is_boolean())){valid=false;break;}
                for(const auto& field:script["Fields"])
                    if(!field.is_object()||!field.contains("Kind")||!field["Kind"].is_string()||!field.contains("Value")){valid=false;break;}
            }
            key="Script.DocumentInvalid";
        }
        if(!valid)return Issue(diagnostics,"Assets.ActionDocumentInvalid",document.path,key);
        References(json,document.kind,ids,{{},document.path,{},0,memory,document.kind},output);return true;
    }
    catch(const std::exception& error){return Issue(diagnostics,"Assets.ActionDocumentInvalid",document.path,error.what());}
}
bool RefreshStore(AssetStore& store,std::vector<EditorAssetIssue>& diagnostics)
{
    std::vector<AssetDiagnostic> errors;if(store.Scan(errors))return true;
    for(const auto& error:errors)diagnostics.push_back({error.key,error.detail,{}});return false;
}
bool Project(const fs::path& root,ProjectDescription& project,Json& json,std::string& text,std::vector<EditorAssetIssue>& diagnostics)
{
    const auto manifest=root/"project.json";std::vector<ProjectDiagnostic> errors;
    if(!Safe(root,manifest)||!ReadProject(manifest,project,errors)||!Read(manifest,text))
    {for(const auto& error:errors)Issue(diagnostics,error.key.c_str(),manifest,error.detail);return Issue(diagnostics,"Assets.ActionDocumentInvalid",manifest);}
    json=Parse(text);return true;
}
bool GatherReferences(const EditorAssetActionPlan& plan,const ProjectDescription& project,const AssetStore& assets,
    std::vector<EditorAssetReference>& references,std::vector<EditorAssetIssue>& diagnostics)
{
    const auto& request=plan.request;std::set<AssetId> affected;
    for(const auto& id:assets.ListIds())for(const auto& source:plan.sources)if(Same(assets.Resolve(id),source))affected.insert(id);
    std::vector<std::pair<fs::path,EditorAssetDocumentKind>> documents;
    const auto add=[&](fs::path path,EditorAssetDocumentKind kind)
    {if(std::none_of(documents.begin(),documents.end(),[&](const auto& item){return Same(item.first,path);}))documents.emplace_back(std::move(path),kind);};
    for(const auto& path:project.scenes)add(path,EditorAssetDocumentKind::Scene);
    for(const auto& path:project.uiDocuments)add(path,EditorAssetDocumentKind::Ui);
    const auto build=request.projectRoot/"build.json";
    if(fs::exists(build))
    {
        std::string text;if(!Safe(request.projectRoot,build)||!Read(build,text))return Issue(diagnostics,"Assets.ActionDocumentInvalid",build);
        const auto json=Parse(text);
        if(!json.is_object()||json.value("format",std::string{})!="SelfEngine.Build"||json.value("version",0)!=1)return Issue(diagnostics,"Assets.ActionDocumentInvalid",build);
        for(const auto& [field,kind]:std::vector<std::pair<std::string,EditorAssetDocumentKind>>{{"bindings",EditorAssetDocumentKind::Bindings},{"entryUi",EditorAssetDocumentKind::Ui}})
        {const auto relative=json.value(field,std::string{});if(!relative.empty())add(Absolute(request.projectRoot/asset_io::FromUtf8(relative)),kind);}
    }
    for(fs::recursive_directory_iterator cursor(request.projectRoot),end;cursor!=end;++cursor)
    {
        const auto path=cursor->path();const auto name=Lower(asset_io::Utf8(path.filename()));
        if(cursor->is_directory()&&(name=="out"||name=="bin"||name=="obj"||name==".git"||name==".cache"))
        {cursor.disable_recursion_pending();continue;}
        if(!Safe(request.projectRoot,path))return Issue(diagnostics,"Assets.ActionUnsafePath",path);
        if(!cursor->is_regular_file())continue;
        if(const auto kind=Kind(path))add(path,*kind);
        if(Type(path)!="model"||!asset_io::Within(request.contentRoot,path))continue;
        StaticModel model;ImportOptions options;options.allowedRoot=request.contentRoot;std::vector<AssetDiagnostic> errors;
        if(!ImportStaticModel(path,options,model,errors))return Issue(diagnostics,"Assets.ActionDocumentInvalid",path,errors.empty()?"Assets.InvalidModel":errors.back().key);
        if(std::any_of(plan.sources.begin(),plan.sources.end(),[&](const auto& source){return Same(source,path);}))continue;
        for(const auto& dependency:model.dependencies)
        {
            fs::path resolved;std::string relative;
            if(!asset_io::ResolveRelative(path,request.contentRoot,dependency,resolved,relative))return Issue(diagnostics,"Assets.ActionUnsafePath",path);
            for(const auto& source:plan.sources)if(Same(resolved,source))
                references.push_back({plan.id,path,"dependencies/"+relative,0,false});
        }
    }
    for(const auto& [path,kind]:documents)
    {
        std::string text;if(!Safe(request.projectRoot,path)||!Read(path,text))return Issue(diagnostics,"Assets.ActionDocumentInvalid",path);
        if(!DocumentReferences({kind,path,std::move(text)},false,affected,references,diagnostics))return false;
    }
    for(const auto& document:request.documents)
    {
        if(!document.path.empty()&&!Safe(request.projectRoot,Absolute(document.path)))return Issue(diagnostics,"Assets.ActionUnsafePath",document.path);
        if(!DocumentReferences(document,true,affected,references,diagnostics))return false;
    }
    return true;
}
bool AddActionFile(EditorAssetActionPlan& plan,const fs::path& source,const fs::path& target)
{
    const auto& root=plan.request.contentRoot;std::string text;
    if(!Safe(root,source)||!fs::is_regular_file(source)||!Read(source,text))return Issue(plan.diagnostics,"Assets.ActionUnsafePath",source);
    if(!target.empty()&&(!Safe(root,target)||fs::exists(target)))return Issue(plan.diagnostics,"Assets.ImportConflict",target);
    for(const auto& file:plan.files)if(Same(file.source,source)||(!target.empty()&&Same(file.target,target)))return Issue(plan.diagnostics,"Assets.ImportConflict",target);
    plan.files.push_back({source,target,HashText(text)});return true;
}
bool SameFiles(const std::vector<EditorAssetActionFile>& a,const std::vector<EditorAssetActionFile>& b)
{
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i)
        if(!Same(a[i].source,b[i].source)||a[i].target!=b[i].target||a[i].digest!=b[i].digest
            ||a[i].rewrite!=b[i].rewrite||a[i].replacement!=b[i].replacement)return false;
    return true;
}
}

EditorAssetActionPlan EditorAssetActions::Plan(const EditorAssetActionRequest& requested,const AssetStore& original)
{
    EditorAssetActionPlan plan;plan.request=requested;
    try
    {
        auto& request=plan.request;request.projectRoot=Absolute(request.projectRoot);
        request.contentRoot=Absolute(request.contentRoot.is_absolute()?request.contentRoot:request.projectRoot/request.contentRoot);
        request.source=Absolute(request.source.is_absolute()?request.source:request.contentRoot/request.source);
        if(request.action==EditorAssetAction::Move)request.target=Absolute(request.target.is_absolute()?request.target:request.contentRoot/request.target);
        else request.target.clear();
        if(!Safe(request.projectRoot,request.projectRoot)||!Safe(request.projectRoot,request.contentRoot)||!Safe(request.contentRoot,request.source)
            ||!Same(request.contentRoot,original.SourceRoot())||!fs::is_regular_file(request.source))
        {Issue(plan.diagnostics,"Assets.ActionUnsafePath",request.source);return plan;}
        if(request.action==EditorAssetAction::Move&&(Same(request.source,request.target)||Type(request.source)!=Type(request.target)
            ||Lower(request.source.extension().string())!=Lower(request.target.extension().string())
            ||!Safe(request.contentRoot,request.target)||fs::exists(request.target)||fs::exists(Metadata(request.target))))
        {Issue(plan.diagnostics,"Assets.ImportConflict",request.target);return plan;}
        auto assets=original;if(!RefreshStore(assets,plan.diagnostics))return plan;
        for(const auto& id:assets.ListIds())if(Same(assets.Resolve(id),request.source)){plan.id=id;break;}
        if(plan.id.empty()){Issue(plan.diagnostics,"Assets.ActionUnregistered",request.source);return plan;}
        ProjectDescription project;Json manifest;std::string manifestText;
        if(!Project(request.projectRoot,project,manifest,manifestText,plan.diagnostics))return plan;
        if(!Same(project.contentRoot,request.contentRoot)){Issue(plan.diagnostics,"Assets.ImportContentRoot",request.contentRoot);return plan;}
        plan.sources.push_back(request.source);std::vector<fs::path> targets{request.target};
        if(request.action==EditorAssetAction::Move&&Type(request.source)=="model"&&!Same(request.source.parent_path(),request.target.parent_path()))
        {
            StaticModel model;ImportOptions options;options.allowedRoot=request.contentRoot;std::vector<AssetDiagnostic> errors;
            if(!ImportStaticModel(request.source,options,model,errors))
            {Issue(plan.diagnostics,"Assets.ActionDocumentInvalid",request.source);return plan;}
            for(const auto& dependency:model.dependencies)
            {
                fs::path source;std::string relative;
                if(!asset_io::ResolveRelative(request.source,request.contentRoot,dependency,source,relative))
                {Issue(plan.diagnostics,"Assets.ActionUnsafePath",request.source);return plan;}
                plan.sources.push_back(source);targets.push_back(Absolute(request.target.parent_path()/asset_io::FromUtf8(relative)));
            }
            if(plan.sources.size()>1)plan.requirements.push_back("Assets.ActionMovesDependencies");
        }
        for(std::size_t i=0;i<plan.sources.size();++i)
        {
            if(!AddActionFile(plan,plan.sources[i],targets[i]))return plan;
            const auto sidecar=Metadata(plan.sources[i]);
            if(fs::exists(sidecar)&&!AddActionFile(plan,sidecar,targets[i].empty()?fs::path{}:Metadata(targets[i])))return plan;
        }
        if(!GatherReferences(plan,project,assets,plan.references,plan.diagnostics))return plan;
        if(!plan.references.empty())
        {for(const auto& reference:plan.references)plan.diagnostics.push_back({"Assets.ActionReferenced",reference.asset,reference});return plan;}
        if(request.action==EditorAssetAction::Delete)
        {
            auto& declarations=manifest["assets"];const auto before=declarations.size();
            std::erase_if(declarations.get_ref<Json::array_t&>(),[&](const auto& declaration){return declaration.at("id")==plan.id;});
            if(declarations.size()!=before)
            {
                const auto path=request.projectRoot/"project.json";
                plan.files.push_back({path,path,HashText(manifestText),true,manifest.dump(2)+"\n"});
                plan.requirements.push_back("Assets.ActionRemovesDeclaration");
            }
        }
        else plan.requirements.push_back("Assets.ActionPreservesIds");
        plan.valid=true;return plan;
    }
    catch(const std::exception& error){Issue(plan.diagnostics,"Assets.ActionDocumentInvalid",requested.source,error.what());return plan;}
}

bool EditorAssetActions::Execute(const EditorAssetActionPlan& plan,AssetStore& assets,
    const std::vector<EditorAssetDocument>& currentDocuments,std::vector<EditorAssetIssue>& diagnostics)
{
    diagnostics.clear();if(!plan.valid)return Issue(diagnostics,"Assets.ActionPlanInvalid",plan.request.source);
    auto request=plan.request;request.documents=currentDocuments;auto fresh=Plan(request,assets);
    if(!fresh.valid){diagnostics=std::move(fresh.diagnostics);return false;}
    if(fresh.id!=plan.id||!SameFiles(plan.files,fresh.files))return Issue(diagnostics,"Assets.ImportSourceChanged",request.source);
    const auto previous=assets;
    struct Change {EditorAssetActionFile file;fs::path held;std::string original;bool staged=false,published=false;};
    std::vector<Change> changes;std::vector<fs::path> directories;fs::path holding;
    const auto parents=[&](const fs::path& path)
    {
        std::vector<fs::path> missing;
        for(auto item=path;!fs::exists(item);item=item.parent_path())
        {if(!Safe(request.projectRoot,item))return false;missing.push_back(item);}
        for(auto item=missing.rbegin();item!=missing.rend();++item)
        {if(fs::create_directory(*item))directories.push_back(*item);}
        return true;
    };
    const auto rollback=[&]()
    {
        bool complete=true;
        for(auto it=changes.rbegin();it!=changes.rend();++it)
        {
            try
            {
            if(!it->staged)continue;
            if(it->published)
            {
                std::string bytes;const auto& expected=it->file.rewrite?it->file.replacement:it->original;
                if(!Safe(request.projectRoot,it->file.target)||!Read(it->file.target,bytes)||bytes!=expected
                    ||!fs::remove(it->file.target)){complete=false;continue;}
            }
            if(fs::exists(it->file.source)){complete=false;continue;}
            if(fs::exists(it->held))
            {if(!MoveFileExW(it->held.c_str(),it->file.source.c_str(),MOVEFILE_WRITE_THROUGH))complete=false;}
            else if(!asset_io::WriteAtomic(it->file.source,{reinterpret_cast<const std::uint8_t*>(it->original.data()),it->original.size()},false))
            {
                // 若源位置被并发占用，将恢复内容保留在自己的事务目录供定位。
                asset_io::WriteAtomic(it->held,{reinterpret_cast<const std::uint8_t*>(it->original.data()),it->original.size()},false);complete=false;
            }
            }
            catch(...){complete=false;}
        }
        std::vector<AssetDiagnostic> errors;if(!assets.Scan(errors))assets=previous;
        for(auto it=directories.rbegin();it!=directories.rend();++it)
        {std::error_code error;if(Safe(request.projectRoot,*it)&&fs::is_empty(*it,error)&&!error)fs::remove(*it,error);}
        if(!complete)Issue(diagnostics,"Assets.ActionRollbackIncomplete",holding);return false;
    };
    try
    {
        std::uint64_t total=0;
        for(const auto& file:fresh.files)
        {
            std::string bytes;if(!Read(file.source,bytes)||HashText(bytes)!=file.digest)return Issue(diagnostics,"Assets.ImportSourceChanged",file.source);
            total+=bytes.size();if(total>512ull*1024*1024)return Issue(diagnostics,"Assets.ActionTooLarge",file.source);
            changes.push_back({file,{},std::move(bytes)});
        }
        const auto token=asset_io::NewId();if(token.empty())return Issue(diagnostics,"Assets.IdCreationFailed");
        holding=request.projectRoot/"out"/"Editor"/"AssetActions"/token;
        if(!Safe(request.projectRoot,holding)||fs::exists(holding)||!parents(holding))
        {Issue(diagnostics,"Assets.ActionWriteFailed",holding);return rollback();}
        for(std::size_t i=0;i<changes.size();++i)
        {
            auto& change=changes[i];change.held=holding/(std::to_string(i)+".held");
            if(!Safe(request.projectRoot,change.file.source)||!MoveFileExW(change.file.source.c_str(),change.held.c_str(),MOVEFILE_WRITE_THROUGH))
            {Issue(diagnostics,"Assets.ActionWriteFailed",change.file.source);return rollback();}
            change.staged=true;
            std::string staged;
            if(!Read(change.held,staged)||HashText(staged)!=change.file.digest)
            {Issue(diagnostics,"Assets.ImportSourceChanged",change.file.source);return rollback();}
        }
        for(auto& change:changes)
        {
            if(change.file.target.empty())continue;
            const auto& bytes=change.file.rewrite?change.file.replacement:change.original;
            if(!Safe(request.projectRoot,change.file.target)||!parents(change.file.target.parent_path())
                ||!asset_io::WriteAtomic(change.file.target,{reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()},false))
            {Issue(diagnostics,"Assets.ActionWriteFailed",change.file.target);return rollback();}
            change.published=true;
        }
        if(!RefreshStore(assets,diagnostics))return rollback();
        if(request.action==EditorAssetAction::Move&&!Same(assets.Resolve(plan.id),request.target))
        {Issue(diagnostics,"Assets.ActionIdentityChanged",request.target);return rollback();}
        if(request.action==EditorAssetAction::Move&&Type(request.target)=="model")
        {
            std::vector<AssetDiagnostic> errors;if(!assets.LoadModel(plan.id,errors))
            {Issue(diagnostics,"Assets.ActionDocumentInvalid",request.target);return rollback();}
        }
        for(auto& change:changes)if(!fs::remove(change.held))
        {Issue(diagnostics,"Assets.ActionWriteFailed",change.held);return rollback();}
        for(auto it=directories.rbegin();it!=directories.rend();++it)
        {std::error_code error;if(Safe(request.projectRoot,*it)&&fs::is_empty(*it,error)&&!error)fs::remove(*it,error);}
        return true;
    }
    catch(const std::exception& error){Issue(diagnostics,"Assets.ActionWriteFailed",holding,error.what());return rollback();}
}

bool EditorAssetActions::SyncProjectAssets(const fs::path& requested,AssetStore& assets,std::vector<EditorAssetIssue>& diagnostics)
{
    diagnostics.clear();
    try
    {
        const auto root=Absolute(requested);ProjectDescription project;Json json;std::string original;
        if(!Project(root,project,json,original,diagnostics))return false;
        if(!Same(project.contentRoot,assets.SourceRoot()))return Issue(diagnostics,"Assets.ImportContentRoot",assets.SourceRoot());
        if(!RefreshStore(assets,diagnostics))return false;
        std::map<AssetId,std::string> declarations;for(const auto& asset:project.assets)declarations.emplace(asset.id,asset.type);
        for(const auto& [id,type]:declarations)
            if(assets.Resolve(id).empty()||!fs::is_regular_file(assets.Resolve(id))||Type(assets.Resolve(id))!=type)
                return Issue(diagnostics,"Assets.ProjectSyncFailed",project.manifest,id);
        bool changed=false;
        for(const auto& id:assets.ListIds())
        {
            const auto path=assets.Resolve(id);const auto type=Type(path);
            if(!Safe(project.contentRoot,path)||!fs::is_regular_file(path))return Issue(diagnostics,"Assets.ProjectSyncFailed",path,id);
            if(type.empty()||declarations.contains(id))continue;
            std::vector<AssetDiagnostic> errors;bool valid=false;
            if(type=="model")
            {
                if(const auto model=assets.LoadModel(id,errors))
                {
                    valid=true;
                    for(const auto& dependency:model->dependencies)
                    {fs::path resolved;std::string relative;if(!asset_io::ResolveRelative(path,project.contentRoot,dependency,resolved,relative)||!fs::is_regular_file(resolved)){valid=false;break;}}
                }
            }
            else if(type=="image")valid=static_cast<bool>(assets.LoadTextureImage(id,errors));
            else if(type=="material")valid=static_cast<bool>(assets.LoadMaterial(id,errors));
            else if(type=="tod")valid=static_cast<bool>(assets.LoadTODProfile(id,errors));
            else{ShaderArtifactSource shader;valid=assets.LoadShaderArtifact(id,shader,errors);}
            if(!valid)return Issue(diagnostics,"Assets.ProjectSyncFailed",path,errors.empty()?id:errors.back().key+": "+errors.back().detail);
            json["assets"].push_back({{"id",id},{"type",type}});changed=true;
        }
        if(!changed)return true;
        std::string current;if(!Read(project.manifest,current)||current!=original)return Issue(diagnostics,"Assets.ImportSourceChanged",project.manifest);
        const auto text=json.dump(2)+"\n";
        if(!asset_io::WriteAtomic(project.manifest,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()}))return Issue(diagnostics,"Assets.ProjectSyncFailed",project.manifest);
        return true;
    }
    catch(const std::exception& error){return Issue(diagnostics,"Assets.ProjectSyncFailed",requested,error.what());}
}

bool EditorAssetActions::ValidatePlayAssets(const World& world,const UiTree* ui,AssetStore& assets,
    const std::vector<EditorAssetDocument>& overrides,std::vector<EditorAssetIssue>& diagnostics)
{
    diagnostics.clear();
    try
    {
    if(!RefreshStore(assets,diagnostics))return false;
    std::map<AssetId,MaterialDocument> materials;
    for(const auto& document:overrides)if(document.kind==EditorAssetDocumentKind::Material)
    {
        MaterialDocument material;std::vector<AssetDiagnostic> errors;
        if(!DeserializeMaterialDocument(document.text,material,errors))
        {Issue(diagnostics,"Assets.ActionDocumentInvalid",document.path);continue;}
        for(const auto& id:assets.ListIds())if(Same(assets.Resolve(id),document.path))materials[id]=material;
    }
    const auto report=[&](const std::vector<AssetDiagnostic>& errors,EditorAssetReference location,const char* fallback)
    {
        if(errors.empty())diagnostics.push_back({fallback,location.asset,std::move(location)});
        else for(const auto& error:errors)diagnostics.push_back({error.key,error.detail,location});
    };
    const auto image=[&](const AssetId& id,EditorAssetReference location)
    {
        if(id.empty())return;location.asset=id;std::vector<AssetDiagnostic> errors;
        if(!assets.LoadTextureImage(id,errors))report(errors,std::move(location),"Assets.UnknownId");
    };
    const auto material=[&](const AssetId& id,EditorAssetReference location)
    {
        if(id.empty())return;location.asset=id;std::vector<AssetDiagnostic> errors;MaterialDocument value;
        if(const auto found=materials.find(id);found!=materials.end())value=found->second;
        else if(const auto loaded=assets.LoadMaterial(id,errors))value=*loaded;
        else{report(errors,location,"Assets.InvalidMaterial");return;}
        auto dependency=location;dependency.document=assets.Resolve(id);
        for(const auto& [name,texture]:std::vector<std::pair<std::string,AssetId>>{{"baseColor",value.baseColorTexture},{"normal",value.normalTexture},{"metallicRoughness",value.metallicRoughnessTexture}})
        {dependency.pointer="/textures/"+name;image(texture,dependency);}
        if(!value.shader.empty())
        {
            dependency.pointer="/shader";dependency.asset=value.shader;ShaderArtifactSource shader;errors.clear();
            if(!assets.LoadShaderArtifact(value.shader,shader,errors))report(errors,dependency,"Assets.ShaderArtifactInvalid");
        }
    };
    for(const auto entity:world.Entities())if(const auto* mesh=world.GetMesh(entity))
    {
        EditorAssetReference location{mesh->meshAsset,{},"/mesh/asset",world.Get(entity)->stableId,true};
        if(!mesh->meshAsset.empty()&&!IsBuiltinMesh(mesh->meshAsset))
        {
            std::vector<AssetDiagnostic> errors;
            if(const auto model=assets.LoadModel(mesh->meshAsset,errors))
            {
                const auto source=assets.Resolve(mesh->meshAsset);location.document=source;
                for(const auto& dependency:model->dependencies)
                {
                    fs::path path;std::string relative;
                    if(!asset_io::ResolveRelative(source,assets.SourceRoot(),dependency,path,relative)||!fs::is_regular_file(path))
                    {auto at=location;at.pointer="dependencies/"+dependency;diagnostics.push_back({"Assets.ImportDependencyMissing",dependency,at});}
                }
                for(std::size_t i=0;i<model->textures.size();++i)
                {
                    const auto& texture=model->textures[i];ImageData decoded;errors.clear();bool valid=false;
                    if(!texture.embeddedBytes.empty())valid=DecodeImage(texture.embeddedBytes,decoded,errors);
                    else if(!texture.relativePath.empty())
                    {fs::path path;std::string relative;valid=asset_io::ResolveRelative(source,assets.SourceRoot(),texture.relativePath,path,relative)&&DecodeImage(path,decoded,errors);}
                    if(!valid){auto at=location;at.pointer="/textures/"+std::to_string(i);report(errors,at,"Assets.ImportDependencyMissing");}
                }
            }
            else report(errors,location,"Assets.InvalidModel");
        }
        for(std::size_t i=0;i<mesh->materials.size();++i)
            material(mesh->materials[i],{{},{},"/mesh/materials/"+std::to_string(i),world.Get(entity)->stableId,true});
    }
    if(!world.EnvironmentSettings().todProfile.empty())
    {
        const auto& id=world.EnvironmentSettings().todProfile;std::vector<AssetDiagnostic> errors;
        if(!assets.LoadTODProfile(id,errors))report(errors,{id,{},"/environment/todProfile",0,true},"TOD.InvalidProfile");
    }
    if(ui)for(const auto node:ui->Ids())image(ui->Get(node)->imageAsset,{{},{},"/nodes/"+std::to_string(node)+"/image",node,true,EditorAssetDocumentKind::Ui});
    return diagnostics.empty();
    }
    catch(const std::exception& error){return Issue(diagnostics,"Assets.PlayValidationFailed",{},error.what());}
}
}

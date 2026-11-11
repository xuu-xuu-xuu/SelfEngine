#include "ToolLayer/Tools/Project.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "ResourceLayer/Assets/MaterialDocument.h"
#include "CoreLayer/Profiler.h"
#include "FunctionLayer/GameUI/UiTree.h"
#include "FunctionLayer/Physics/PhysicsWorld.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "FunctionLayer/Runtime/TODSystem.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <thread>

namespace se
{
namespace
{
using Json=nlohmann::json;
namespace fs=std::filesystem;
bool Fail(std::vector<ProjectDiagnostic>& diagnostics,const char* key,std::string detail={})
{diagnostics.push_back({key,std::move(detail)});return false;}
bool ReadText(const fs::path& path,std::string& text,std::size_t maximum=16*1024*1024)
{
    std::error_code error;const auto size=fs::file_size(path,error);if(error||size>maximum)return false;
    std::ifstream input(path,std::ios::binary);if(!input)return false;text.assign(std::istreambuf_iterator<char>(input),{});return !input.bad();
}
// 目录发布用重命名完成；Windows 上杀毒/索引器可能短暂占用新目录，表现为 Access denied 或共享冲突。
// 只对可重试的 Win32 错误做有限退避，其他错误立即返回，避免掩盖真实失败。
bool RenameDirectoryWithRetry(const fs::path& from,const fs::path& to,std::error_code& error)
{
    for(int attempt=0;attempt<6;++attempt)
    {
        error.clear();fs::rename(from,to,error);
        if(!error)return true;
#if defined(_WIN32)
        const int code=error.value();
        if(code!=5&&code!=32&&code!=33)return false;
#else
        return false;
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(50*(attempt+1)));
    }
    return false;
}
Json Parse(std::string_view text)
{
    bool duplicate=false;std::vector<std::set<std::string>> keys;
    auto result=Json::parse(text.begin(),text.end(),[&](int depth,Json::parse_event_t event,Json& value)
    {
        if(depth>64)throw std::runtime_error("Project.InvalidManifest");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        else if(event==Json::parse_event_t::key&&!keys.empty()){if(!keys.back().insert(value.get<std::string>()).second)duplicate=true;}
        else if(event==Json::parse_event_t::object_end&&!keys.empty())keys.pop_back();return true;
    });
    if(duplicate)throw std::runtime_error("Project.InvalidManifest");return result;
}
bool LocalPath(const fs::path& root,const Json& value,fs::path& result)
{
    if(!value.is_string())return false;const auto text=value.get<std::string>();
    if(text.empty()||text.find('\0')!=std::string::npos||text.find(':')!=std::string::npos)return false;
    const auto relative=asset_io::FromUtf8(text);if(relative.is_absolute()||relative.has_root_path())return false;
    result=fs::weakly_canonical(root/relative);return result!=root&&asset_io::Within(root,result);
}
bool Id(const Json& json,std::uint64_t& value)
{
    if(!json.is_string())return false;const auto text=json.get<std::string>();
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
    return parsed.ec==std::errc{}&&parsed.ptr==text.data()+text.size()&&value!=0;
}
bool Vector(const Json& json,Vec3& value)
{
    if(!json.is_array()||json.size()!=3)return false;
    for(const auto& item:json)if(!item.is_number())return false;
    value={json[0].get<float>(),json[1].get<float>(),json[2].get<float>()};return IsFinite(value);
}
bool WriteJson(const fs::path& path,const Json& data)
{
    const auto text=data.dump(2);return asset_io::WriteAtomic(path,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});
}
bool ValidAssetId(std::string_view id)
{return id.size()==32&&std::all_of(id.begin(),id.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
std::string Relative(const fs::path& root,const fs::path& path)
{return asset_io::Utf8(fs::relative(path,root));}
bool FileDependency(const fs::path& root,const fs::path& path,std::set<fs::path>& files,std::vector<ProjectDiagnostic>& diagnostics)
{
    const auto resolved=fs::weakly_canonical(path);
    if(!asset_io::Within(root,resolved)||!fs::is_regular_file(resolved))return Fail(diagnostics,"Project.MissingDependency",asset_io::Utf8(path));
    files.insert(resolved);return true;
}
bool BuildPhysics(const ProjectDescription& project,World& world,PhysicsWorld& physics,std::map<std::uint64_t,BodyHandle>& bodies,std::vector<ProjectDiagnostic>& diagnostics)
{
    for(const auto& item:project.bodies)
    {
        const auto entity=world.FindByStableId(item.entity);const auto* record=world.Get(entity);const auto* transform=world.GetTransform(entity);
        if(!record||record->parent||!transform||transform->scale.x<=0||transform->scale.y<=0||transform->scale.z<=0)
            return Fail(diagnostics,"Project.InvalidPhysicsEntity",std::to_string(item.entity));
        BodyDesc description;description.shape={transform->position,Hadamard(item.halfExtents,transform->scale),transform->rotation};
        description.mass=item.mass;description.friction=item.friction;description.restitution=item.restitution;description.entityStableId=item.entity;
        const auto body=physics.CreateBody(description);if(!body)return Fail(diagnostics,physics.LastError().c_str(),std::to_string(item.entity));
        bodies.emplace(item.entity,body);
    }
    for(const auto& impulse:project.impulses)
    {
        const auto found=bodies.find(impulse.entity);
        if(found==bodies.end()||physics.GetBody(found->second)->description.mass==0)return Fail(diagnostics,"Project.InvalidPhysicsEntity",std::to_string(impulse.entity));
    }
    return true;
}
}

bool ReadProject(const fs::path& manifest,ProjectDescription& output,std::vector<ProjectDiagnostic>& diagnostics)
{
    diagnostics.clear();
    try
    {
        std::string text;if(!ReadText(manifest,text,1024*1024))return Fail(diagnostics,"Project.ReadFailed",asset_io::Utf8(manifest));
        const auto json=Parse(text);ProjectDescription result;result.manifest=fs::canonical(manifest);result.root=result.manifest.parent_path();
        if(!json.is_object()||json.at("format")!="SelfEngine.Project"||!json.at("version").is_number_integer()||json.at("version")!=1)return Fail(diagnostics,"Project.UnsupportedVersion");
        const std::set<std::string> supported{"format","version","name","contentRoot","entryScene","scenes","ui","assets","replay"};
        for(auto entry=json.begin();entry!=json.end();++entry)if(!supported.contains(entry.key()))return Fail(diagnostics,"Project.UnsupportedField",entry.key());
        result.name=json.at("name").get<std::string>();if(result.name.empty()||result.name.size()>256)return Fail(diagnostics,"Project.InvalidManifest");
        if(!LocalPath(result.root,json.at("contentRoot"),result.contentRoot)||!LocalPath(result.root,json.at("entryScene"),result.entryScene))return Fail(diagnostics,"Project.PathOutsideRoot");
        const auto paths=[&](const Json& array,std::vector<fs::path>& values)
        {
            if(!array.is_array()||array.size()>10000)return false;std::set<fs::path> seen;
            for(const auto& value:array){fs::path path;if(!LocalPath(result.root,value,path)||!seen.insert(path).second)return false;values.push_back(path);}return true;
        };
        if(!paths(json.at("scenes"),result.scenes)||!paths(json.at("ui"),result.uiDocuments)
            ||std::find(result.scenes.begin(),result.scenes.end(),result.entryScene)==result.scenes.end())return Fail(diagnostics,"Project.InvalidManifest");
        if(!json.at("assets").is_array()||json.at("assets").size()>100000)return Fail(diagnostics,"Project.InvalidManifest");
        std::set<std::string> assetIds;
        for(const auto& asset:json.at("assets"))
        {
            ProjectAsset value{asset.at("id").get<std::string>(),asset.at("type").get<std::string>()};
            if(!ValidAssetId(value.id)||!assetIds.insert(value.id).second||(value.type!="model"&&value.type!="image"&&value.type!="material"&&value.type!="tod"&&value.type!="shader"))return Fail(diagnostics,"Project.InvalidManifest");
            result.assets.push_back(std::move(value));
        }
        if(json.contains("replay"))
        {
            const auto& replay=json.at("replay");
            if(!replay.at("steps").is_number_unsigned()||replay.at("steps").get<std::uint64_t>()==0||replay.at("steps").get<std::uint64_t>()>36000)return Fail(diagnostics,"Project.InvalidReplay");
            result.replaySteps=replay.at("steps").get<std::uint32_t>();
            if(!replay.at("bodies").is_array()||replay.at("bodies").size()>256||!replay.at("impulses").is_array()||replay.at("impulses").size()>10000)return Fail(diagnostics,"Project.InvalidReplay");
            std::set<std::uint64_t> ids;
            for(const auto& body:replay.at("bodies"))
            {
                ProjectBody value;
                if(!Id(body.at("entity"),value.entity)||!ids.insert(value.entity).second||!Vector(body.at("halfExtents"),value.halfExtents))return Fail(diagnostics,"Project.InvalidReplay");
                value.mass=body.at("mass").get<float>();value.friction=body.value("friction",0.6f);value.restitution=body.value("restitution",0.0f);
                if(!IsFinite(value.mass)||value.mass<0||!IsFinite(value.friction)||value.friction<0||!IsFinite(value.restitution)||value.restitution<0||value.restitution>1
                    ||value.halfExtents.x<=0.001f||value.halfExtents.y<=0.001f||value.halfExtents.z<=0.001f)return Fail(diagnostics,"Project.InvalidReplay");
                result.bodies.push_back(value);
            }
            for(const auto& impulse:replay.at("impulses"))
            {
                ProjectImpulse value;
                if(!impulse.at("step").is_number_unsigned()||impulse.at("step").get<std::uint64_t>()>=result.replaySteps
                    ||!Id(impulse.at("entity"),value.entity)||!Vector(impulse.at("impulse"),value.impulse)||!Vector(impulse.at("offset"),value.offset))return Fail(diagnostics,"Project.InvalidReplay");
                value.step=impulse.at("step").get<std::uint32_t>();result.impulses.push_back(value);
            }
        }
        output=std::move(result);return true;
    }
    catch(const std::exception& error){return Fail(diagnostics,"Project.InvalidManifest",error.what());}
}

bool ValidateProject(const fs::path& manifest,ProjectValidation& output,std::vector<ProjectDiagnostic>& diagnostics)
{
    ProjectValidation result;if(!ReadProject(manifest,result.project,diagnostics))return false;
    try
    {
        const auto& project=result.project;std::set<fs::path> files;files.insert(project.manifest);
        AssetStore store(project.contentRoot,project.root/".cache"/"assets");std::vector<AssetDiagnostic> assetDiagnostics;
        if(!store.Scan(assetDiagnostics))
        {for(const auto& d:assetDiagnostics)diagnostics.push_back({d.key,d.detail});return false;}
        std::map<std::string,std::string> assetTypes;
        for(const auto& asset:project.assets)assetTypes.emplace(asset.id,asset.type);
        for(const auto& asset:project.assets)
        {
            const auto source=store.Resolve(asset.id);if(source.empty())return Fail(diagnostics,"Project.MissingAsset",asset.id);
            auto sidecar=source;sidecar+=".asset.json";
            if(!FileDependency(project.root,source,files,diagnostics)||!FileDependency(project.root,sidecar,files,diagnostics))return false;
            assetDiagnostics.clear();
            if(asset.type=="model")
            {
                const auto model=store.LoadModel(asset.id,assetDiagnostics);
                if(!model){for(const auto& d:assetDiagnostics)diagnostics.push_back({d.key,d.detail});return false;}
                for(const auto& dependency:model->dependencies)
                {
                    fs::path resolved;std::string relative;
                    if(!asset_io::ResolveRelative(source,project.contentRoot,dependency,resolved,relative))return Fail(diagnostics,"Project.PathOutsideRoot",dependency);
                    if(!FileDependency(project.root,resolved,files,diagnostics))return false;
                }
            }
            else if(asset.type=="image")
            {
                if(!store.LoadTextureImage(asset.id,assetDiagnostics))
                {for(const auto& d:assetDiagnostics)diagnostics.push_back({d.key,d.detail});return false;}
            }
            else if(asset.type=="tod")
            {
                const auto profile=store.LoadTODProfile(asset.id,assetDiagnostics);std::string error;
                if(!profile)
                {for(const auto& d:assetDiagnostics)diagnostics.push_back({d.key,d.detail});return false;}
                if(!TODSystem::ValidateProfile(*profile,1.0/60,error))return Fail(diagnostics,error.c_str(),asset.id);
            }
            else if(asset.type=="shader")
            {
                se::ShaderArtifactSource artifact;
                if(!store.LoadShaderArtifact(asset.id,artifact,assetDiagnostics))
                {for(const auto& d:assetDiagnostics)diagnostics.push_back({d.key,d.detail});return false;}
            }
            else
            {
                const auto material=store.LoadMaterial(asset.id,assetDiagnostics);
                if(!material){for(const auto& d:assetDiagnostics)diagnostics.push_back({d.key,d.detail});return false;}
                for(const auto& texture:MaterialTextureDependencies(*material))
                    if(!assetTypes.contains(texture)||assetTypes.at(texture)!="image")return Fail(diagnostics,"Project.MissingAsset",texture);
                if(!material->shader.empty()&&(!assetTypes.contains(material->shader)||assetTypes.at(material->shader)!="shader"))
                    return Fail(diagnostics,"Project.MissingAsset",material->shader);
            }
        }
        const auto assetReference=[&](const std::string& id,const char* type)
        {return id.empty()||(assetTypes.contains(id)&&assetTypes.at(id)==type);};
        for(const auto& path:project.scenes)
        {
            if(!FileDependency(project.root,path,files,diagnostics))return false;World world;std::string error;
            if(!LoadScene(path,world,error))return Fail(diagnostics,error.c_str(),Relative(project.root,path));
            result.entities+=world.Size();
            if(!assetReference(world.EnvironmentSettings().todProfile,"tod"))return Fail(diagnostics,"Project.MissingAsset",world.EnvironmentSettings().todProfile);
            for(const auto entity:world.Entities())if(const auto* mesh=world.GetMesh(entity))
            {
                if(!IsBuiltinMesh(mesh->meshAsset)&&!assetReference(mesh->meshAsset,"model"))return Fail(diagnostics,"Project.MissingAsset",mesh->meshAsset);
                for(const auto& material:mesh->materials)if(!assetReference(material,"material"))return Fail(diagnostics,"Project.MissingAsset",material);
            }
            if(path==project.entryScene)
            {PhysicsWorld physics;std::map<std::uint64_t,BodyHandle> bodies;if(!BuildPhysics(project,world,physics,bodies,diagnostics))return false;}
        }
        for(const auto& path:project.uiDocuments)
        {
            if(!FileDependency(project.root,path,files,diagnostics))return false;std::string text,error;UiTree ui;
            if(!ReadText(path,text))return Fail(diagnostics,"Project.ReadFailed",Relative(project.root,path));
            (void)Parse(text); // 项目入口统一拒绝重复键和过深 JSON，再交给 UI 领域校验。
            if(!ui.LoadJson(text,error))return Fail(diagnostics,error.c_str(),Relative(project.root,path));
            result.uiNodes+=ui.Count();for(const auto id:ui.Ids())if(!assetReference(ui.Get(id)->imageAsset,"image"))return Fail(diagnostics,"Project.MissingAsset",ui.Get(id)->imageAsset);
        }
        result.dependencies={files.begin(),files.end()};output=std::move(result);return true;
    }
    catch(const std::exception& error){return Fail(diagnostics,"Project.ValidationFailed",error.what());}
}

bool CreateProject(const fs::path& templateDirectory,const fs::path& destination,std::vector<ProjectDiagnostic>& diagnostics)
{
    diagnostics.clear();fs::path staging;
    try
    {
        const auto source=fs::canonical(templateDirectory),target=fs::weakly_canonical(destination);
        if(!fs::is_directory(source)||fs::exists(target)||asset_io::Within(source,target))return Fail(diagnostics,"Project.DestinationExists",asset_io::Utf8(target));
        fs::create_directories(target.parent_path());staging=target.parent_path()/(target.filename().wstring()+L".tmp."+asset_io::FromUtf8(asset_io::NewId()).wstring());
        if(!fs::create_directory(staging))return Fail(diagnostics,"Project.WriteFailed");
        for(auto iterator=fs::recursive_directory_iterator(source);iterator!=fs::recursive_directory_iterator{};++iterator)
        {
            const auto& entry=*iterator;const auto relative=fs::relative(entry.path(),source);
            const auto first=relative.begin()->wstring();
            if(first==L".cache"||first==L"out"||first==L".git"||relative==fs::path("Scripts/bin")||relative==fs::path("Scripts/obj"))
            {if(entry.is_directory())iterator.disable_recursion_pending();continue;}
            if(fs::is_symlink(entry.symlink_status())||!asset_io::Within(source,entry.path()))throw std::runtime_error("Project.PathOutsideRoot");
            const auto output=staging/relative;
            if(entry.is_directory())fs::create_directory(output);else if(entry.is_regular_file())fs::copy_file(entry.path(),output);
        }
        ProjectValidation validated;
        if(!ValidateProject(staging/"project.json",validated,diagnostics))
        {if(asset_io::Within(target.parent_path(),staging))fs::remove_all(staging);return false;}
        std::error_code renameError;
        if(!RenameDirectoryWithRetry(staging,target,renameError))
            throw std::filesystem::filesystem_error("Project.CreateRename",staging,target,renameError);
        return true;
    }
    catch(const std::exception& error)
    {
        if(!staging.empty()&&asset_io::Within(fs::weakly_canonical(destination).parent_path(),staging))
        {std::error_code ignored;fs::remove_all(staging,ignored);}
        return Fail(diagnostics,"Project.CreateFailed",error.what());
    }
}

bool ExportProjectDependencies(const fs::path& manifest,const fs::path& output,std::vector<ProjectDiagnostic>& diagnostics)
{
    ProjectValidation validation;if(!ValidateProject(manifest,validation,diagnostics))return false;
    try
    {
        const auto target=fs::weakly_canonical(output);
        if(std::find(validation.dependencies.begin(),validation.dependencies.end(),target)!=validation.dependencies.end())return Fail(diagnostics,"Project.InvalidOutput");
        Json result{{"format","SelfEngine.Dependencies"},{"version",1},{"project",validation.project.name},{"files",Json::array()}};
        for(const auto& file:validation.dependencies)
        {
            std::vector<std::uint8_t> bytes;if(!asset_io::Read(file,bytes))return Fail(diagnostics,"Project.ReadFailed",asset_io::Utf8(file));
            const auto hash=asset_io::Hash(bytes);if(hash.empty())return Fail(diagnostics,"Project.ReadFailed",asset_io::Utf8(file));
            result["files"].push_back({{"path",Relative(validation.project.root,file)},{"bytes",bytes.size()},{"sha256",hash}});
        }
        if(!WriteJson(output,result))return Fail(diagnostics,"Project.WriteFailed",asset_io::Utf8(output));return true;
    }
    catch(const std::exception& error){return Fail(diagnostics,"Project.WriteFailed",error.what());}
}

bool ReplayProject(const fs::path& manifest,const fs::path& outputDirectory,ReplayReport& report,std::vector<ProjectDiagnostic>& diagnostics,std::uint32_t stepsOverride,bool profile)
{
    ProjectValidation validation;if(!ValidateProject(manifest,validation,diagnostics))return false;
    fs::path staging;
    try
    {
        const auto target=fs::weakly_canonical(outputDirectory);
        if(fs::exists(target))return Fail(diagnostics,"Project.DestinationExists",asset_io::Utf8(target));
        const auto& project=validation.project;const std::uint32_t steps=stepsOverride?stepsOverride:project.replaySteps;
        if(steps>36000)return Fail(diagnostics,"Project.InvalidReplay");
        World world;std::string error;if(!LoadScene(project.entryScene,world,error))return Fail(diagnostics,error.c_str());
        PhysicsWorld physics;std::map<std::uint64_t,BodyHandle> bodies;
        if(!BuildPhysics(project,world,physics,bodies,diagnostics))return false;
        std::vector<UiTree> documents;
        for(const auto& path:project.uiDocuments)
        {std::string text;ReadText(path,text);UiTree ui;if(!ui.LoadJson(text,error))return Fail(diagnostics,error.c_str());documents.push_back(std::move(ui));}
        Profiler profiler(steps);profiler.SetEnabled(profile);
        for(std::uint32_t step=0;step<steps;++step)
        {
            profiler.BeginFrame(step);
            {
                CpuScope timing(profiler,"Physics.Step");
                for(const auto& impulse:project.impulses)if(impulse.step==step)
                {
                    const auto body=bodies.at(impulse.entity);const auto position=physics.GetBody(body)->description.shape.center;
                    if(!physics.ApplyImpulse(body,impulse.impulse,position+impulse.offset))return Fail(diagnostics,physics.LastError().c_str());
                }
                if(!physics.Step())return Fail(diagnostics,physics.LastError().c_str(),std::to_string(step));
            }
            {
                CpuScope timing(profiler,"World.Sync");
                for(const auto& [id,body]:bodies)
                {
                    const auto* state=physics.GetBody(body);if(state->description.mass==0)continue;
                    const auto entity=world.FindByStableId(id);auto transform=*world.GetTransform(entity);
                    transform.position=state->description.shape.center;transform.rotation=state->description.shape.rotation;
                    if(!world.SetTransform(entity,transform))return Fail(diagnostics,world.LastError().c_str());
                }
            }
            {CpuScope timing(profiler,"UI.Layout");for(auto& ui:documents){ui.Update(1.0/60);ui.Layout(1280,720);}}
            profiler.EndFrame();
        }
        std::string scene;if(!SerializeScene(world,scene,error))return Fail(diagnostics,error.c_str());
        ReplayReport completed{steps,asset_io::Hash({reinterpret_cast<const std::uint8_t*>(scene.data()),scene.size()}),world.Size()};
        fs::create_directories(target.parent_path());
        staging=target.parent_path()/(target.filename().wstring()+L".tmp."+asset_io::FromUtf8(asset_io::NewId()).wstring());
        if(!fs::create_directory(staging))return Fail(diagnostics,"Project.WriteFailed");
        if(!SaveScene(staging/"final.scene.json",world,error))throw std::runtime_error(error);
        if(profile&&(!profiler.ExportFramesCsv(staging/"cpu-frames.csv",error)||!profiler.ExportSummaryCsv(staging/"cpu-summary.csv",error)))throw std::runtime_error(error);
        const Json result{{"format","SelfEngine.HeadlessReplay"},{"version",1},{"steps",steps},{"fixedStepSeconds",1.0/60},
            {"stateSha256",completed.stateHash},{"entities",completed.entities},{"cpuProfiling",profile},{"gpuMeasured",false}};
        if(!WriteJson(staging/"replay.json",result))throw std::runtime_error("Project.WriteFailed");
        std::error_code renameError;
        if(!RenameDirectoryWithRetry(staging,target,renameError))
            throw std::filesystem::filesystem_error("Project.ReplayRename",staging,target,renameError);
        report=std::move(completed);return true;
    }
    catch(const std::exception& error)
    {
        if(!staging.empty()&&asset_io::Within(fs::weakly_canonical(outputDirectory).parent_path(),staging))
        {std::error_code ignored;fs::remove_all(staging,ignored);}
        return Fail(diagnostics,"Project.ReplayFailed",error.what());
    }
}
}

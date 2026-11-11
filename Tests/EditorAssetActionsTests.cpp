#include "ToolLayer/Editor/EditorAssetActions.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "FunctionLayer/GameUI/UiTree.h"
#include "ToolLayer/Tools/Project.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
using namespace se;
using Json=nlohmann::json;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Put(const fs::path& path,const std::string& text)
{fs::create_directories(path.parent_path());std::ofstream stream(path,std::ios::binary|std::ios::trunc);stream<<text;Check(bool(stream),"fixture write");}
std::string Read(const fs::path& path)
{std::ifstream stream(path,std::ios::binary);Check(bool(stream),"fixture read");return {std::istreambuf_iterator<char>(stream),{}};}
void Copy(const fs::path& source,const fs::path& target)
{fs::create_directories(target.parent_path());fs::copy_file(source,target,fs::copy_options::overwrite_existing);}
fs::path Metadata(fs::path source){source+=".asset.json";return source;}
const fs::path Samples="Resources/Samples/Models";
struct ProjectFixture
{
    fs::path root,content;AssetStore assets;
    ProjectFixture(const fs::path& output,const char* name):root(output/name),content(root/"Content"),assets(content,root/"out"/"Cache")
    {
        fs::create_directories(content);Copy("Resources/Templates/BlankScene/project.json",root/"project.json");
        Copy("Resources/Templates/BlankScene/Scenes/Main.scene.json",root/"Scenes"/"Main.scene.json");
    }
    AssetId Register(const fs::path& source)
    {AssetId id;std::vector<AssetDiagnostic> diagnostics;Check(assets.RegisterSource(source,{},id,diagnostics),"register fixture");return id;}
    AssetId Image(const char* name="image.png")
    {Copy(Samples/"checker.png",content/name);return Register(content/name);}
    AssetId Model()
    {
        Copy(Samples/"Hierarchy.gltf",content/"old"/"Model.gltf");Copy(Samples/"Hierarchy.bin",content/"old"/"Hierarchy.bin");
        Copy(Samples/"checker.png",content/"old"/"checker.png");Register(content/"old"/"checker.png");return Register(content/"old"/"Model.gltf");
    }
    EditorAssetActionRequest Request(EditorAssetAction action,const fs::path& source,const fs::path& target={})
    {return {action,root,content,source,target,{}};}
};
struct ReadLock
{
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit ReadLock(const fs::path& path)
    {handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);Check(handle!=INVALID_HANDLE_VALUE,"lock fixture");}
    ~ReadLock(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
};
void IdentityAndManifest(const fs::path& output)
{
    ProjectFixture test(output,"identity");const auto id=test.Image();std::vector<EditorAssetIssue> issues;
    Check(EditorAssetActions::SyncProjectAssets(test.root,test.assets,issues),"sync registered asset declaration");
    const auto manifest=Read(test.root/"project.json"),source=Read(test.content/"image.png"),metadata=Read(Metadata(test.content/"image.png"));
    Check(Json::parse(manifest)["assets"].size()==1,"one declaration added");
    const auto plan=EditorAssetActions::Plan(test.Request(EditorAssetAction::Move,"image.png","Images/renamed.png"),test.assets);
    Check(plan.valid&&plan.id==id&&plan.files.size()==2&&plan.sources.size()==1,"rename plan exposes source and sidecar");
    Check(EditorAssetActions::Execute(plan,test.assets,{},issues),"move transaction succeeds");
    const auto moved=test.content/"Images"/"renamed.png";
    Check(!fs::exists(test.content/"image.png")&&Read(moved)==source&&Read(Metadata(moved))==metadata&&test.assets.Resolve(id)==moved,"move preserves bytes and ID");
    Check(Read(test.root/"project.json")==manifest,"identity-only declarations unchanged by move");
    const auto deletion=EditorAssetActions::Plan(test.Request(EditorAssetAction::Delete,moved),test.assets);
    Check(deletion.valid&&deletion.files.size()==3&&deletion.files.back().rewrite,"delete plans project manifest update");
    Check(EditorAssetActions::Execute(deletion,test.assets,{},issues),"unreferenced delete succeeds");
    Check(!fs::exists(moved)&&!fs::exists(Metadata(moved))&&test.assets.Resolve(id).empty(),"delete removes exactly selected source and metadata");
    auto after=Json::parse(Read(test.root/"project.json")),before=Json::parse(manifest);before["assets"]=Json::array();
    Check(after==before,"delete removes declaration and preserves all other project fields");
    ProjectValidation validation;std::vector<ProjectDiagnostic> errors;
    Check(ValidateProject(test.root/"project.json",validation,errors),"project remains export-valid after deletion");
}
void ReferencesAndInvalidDocuments(const fs::path& output)
{
    ProjectFixture test(output,"references");const auto id=test.Image();
    World world;const auto entity=world.Create("Referenced",77);Check(world.SetMesh(entity,{id,{}}),"scene reference");
    std::string scene,error;Check(SerializeScene(world,scene,error),"serialize scene");
    const auto scenePath=test.root/"Scenes"/"Main.scene.json";const auto original=Read(scenePath);Put(scenePath,scene);
    auto request=test.Request(EditorAssetAction::Delete,"image.png");
    auto plan=EditorAssetActions::Plan(request,test.assets);
    Check(!plan.valid&&!plan.references.empty()&&plan.references[0].entity==77&&plan.references[0].document==scenePath,"disk scene reference locates entity and document");
    request.action=EditorAssetAction::Move;request.target="renamed.png";
    Check(!EditorAssetActions::Plan(request,test.assets).valid,"referenced assets cannot move either");Put(scenePath,original);
    request=test.Request(EditorAssetAction::Delete,"image.png");plan=EditorAssetActions::Plan(request,test.assets);Check(plan.valid,"unreferenced plan valid");
    std::vector<EditorAssetIssue> issues;
    Check(!EditorAssetActions::Execute(plan,test.assets,{{EditorAssetDocumentKind::Scene,scenePath,scene}},issues)&&fs::exists(test.content/"image.png"),"new unsaved reference blocks confirmed deletion");
    MaterialDocument material;material.baseColorTexture=id;std::vector<AssetDiagnostic> errors;std::string materialText;
    Check(SerializeMaterialDocument(material,materialText,errors),"serialize material fixture");
    request.documents={{EditorAssetDocumentKind::Material,test.content/"unsaved.material.json",materialText}};
    plan=EditorAssetActions::Plan(request,test.assets);
    Check(!plan.valid&&!plan.references.empty()&&plan.references[0].inMemory&&plan.references[0].pointer=="/textures/baseColor","unsaved material reference locates texture field");
    UiTree ui;const auto node=ui.Create(UiKind::Image);Check(ui.SetImage(node,id),"set UI reference");
    const auto uiPath=test.root/"UI"/"Other.ui.json";Put(uiPath,ui.SaveJson());request.documents.clear();
    Check(!EditorAssetActions::Plan(request,test.assets).valid,"unlisted disk UI document still checked");
    Check(ui.SetImage(node,{})&&ui.SetText(node,id),"ordinary UI text may look like an asset ID");Put(uiPath,ui.SaveJson());
    Check(EditorAssetActions::Plan(request,test.assets).valid,"ordinary text is not an asset reference");fs::remove(uiPath);
    const auto binding=test.root/"Scripts"/"Other.bindings.json";
    Put(binding,Json{{"Version",1},{"Scripts",Json::array({{{"Type","game.test"},{"Entity","77"},{"Fields",{{"Texture",{{"Kind","string"},{"Value",id}}}}}}})}}.dump());
    plan=EditorAssetActions::Plan(request,test.assets);Check(!plan.valid&&!plan.references.empty()&&plan.references[0].entity==77,"binding reference locates entity");fs::remove(binding);
    Put(uiPath,"{broken");plan=EditorAssetActions::Plan(request,test.assets);
    Check(!plan.valid&&!plan.diagnostics.empty()&&plan.diagnostics[0].reference.document==uiPath,"invalid document refuses preflight with exact path");
}
void ModelClosureAndRollback(const fs::path& output)
{
    ProjectFixture test(output,"model-move");const auto modelId=test.Model();
    Copy(test.content/"old"/"Model.gltf",test.content/"old"/"Other.gltf");
    auto request=test.Request(EditorAssetAction::Move,"old/Model.gltf","new/Renamed.gltf");
    auto plan=EditorAssetActions::Plan(request,test.assets);
    Check(!plan.valid&&!plan.references.empty()&&plan.references[0].document==test.content/"old"/"Other.gltf","shared dependency precisely identifies other model");
    fs::remove(test.content/"old"/"Other.gltf");plan=EditorAssetActions::Plan(request,test.assets);
    Check(plan.valid&&plan.sources.size()==3&&plan.files.size()==5,"model move includes unique buffer, image and sidecars");
    std::vector<EditorAssetIssue> issues;Check(EditorAssetActions::Execute(plan,test.assets,{},issues),"move model closure");
    std::vector<AssetDiagnostic> errors;Check(static_cast<bool>(test.assets.LoadModel(modelId,errors)),"moved model dependencies still load");
    Check(fs::exists(test.content/"new"/"Hierarchy.bin")&&fs::exists(test.content/"new"/"checker.png")&&!fs::exists(test.content/"old"/"Hierarchy.bin"),"dependencies preserve relative layout");
    auto deletion=EditorAssetActions::Plan(test.Request(EditorAssetAction::Delete,"new/Renamed.gltf"),test.assets);
    Check(deletion.valid&&EditorAssetActions::Execute(deletion,test.assets,{},issues),"delete unreferenced model only");
    Check(fs::exists(test.content/"new"/"Hierarchy.bin")&&fs::exists(test.content/"new"/"checker.png"),"delete does not silently delete dependency data");

    ProjectFixture locked(output,"rollback");const auto id=locked.Image();
    Check(EditorAssetActions::SyncProjectAssets(locked.root,locked.assets,issues),"sync rollback fixture");
    const auto bytes=Read(locked.content/"image.png"),metadata=Read(Metadata(locked.content/"image.png")),manifest=Read(locked.root/"project.json");
    auto move=EditorAssetActions::Plan(locked.Request(EditorAssetAction::Move,"image.png","new/image.png"),locked.assets);Check(move.valid,"rollback move plan");
    {ReadLock hold(Metadata(locked.content/"image.png"));Check(!EditorAssetActions::Execute(move,locked.assets,{},issues),"sidecar move failure rolls back staged source");}
    Check(Read(locked.content/"image.png")==bytes&&Read(Metadata(locked.content/"image.png"))==metadata&&!fs::exists(locked.content/"new"/"image.png")&&locked.assets.Resolve(id)==locked.content/"image.png","partial move leaves original identity and bytes");
    auto erase=EditorAssetActions::Plan(locked.Request(EditorAssetAction::Delete,"image.png"),locked.assets);Check(erase.valid,"rollback delete plan");
    {ReadLock hold(locked.root/"project.json");Check(!EditorAssetActions::Execute(erase,locked.assets,{},issues),"manifest update failure rolls back source and metadata");}
    Check(Read(locked.root/"project.json")==manifest&&Read(locked.content/"image.png")==bytes&&Read(Metadata(locked.content/"image.png"))==metadata,"delete rollback preserves full transaction");
}
void RacesBoundariesAndSync(const fs::path& output)
{
    ProjectFixture test(output,"boundaries");test.Image();std::vector<EditorAssetIssue> issues;
    auto request=test.Request(EditorAssetAction::Move,"image.png","target.png");auto plan=EditorAssetActions::Plan(request,test.assets);Check(plan.valid,"race plan");
    Put(test.content/"target.png","new user file");Check(!EditorAssetActions::Execute(plan,test.assets,{},issues)&&Read(test.content/"target.png")=="new user file","target race preserves winner");fs::remove(test.content/"target.png");
    Put(test.content/"image.png",Read(test.content/"image.png")+"modified");
    Check(!EditorAssetActions::Execute(plan,test.assets,{},issues)&&issues[0].key=="Assets.ImportSourceChanged","source digest changed after plan");
    Copy(Samples/"checker.png",test.content/"image.png");
    request.target=test.root/"outside.png";Check(!EditorAssetActions::Plan(request,test.assets).valid,"outside target denied");
    Copy(Samples/"checker.png",test.content/"second.png");test.Register(test.content/"second.png");const auto manifest=Read(test.root/"project.json");
    {ReadLock hold(test.root/"project.json");Check(!EditorAssetActions::SyncProjectAssets(test.root,test.assets,issues),"manifest sync reports write failure separately");}
    Check(fs::exists(test.content/"second.png")&&test.assets.ListIds().size()==2&&Read(test.root/"project.json")==manifest,"sync failure preserves completed import and prior manifest");
    Put(Metadata(test.content/"broken.png"),"{}");Check(!EditorAssetActions::SyncProjectAssets(test.root,test.assets,issues),"bad sidecar cannot be silently omitted from sync");fs::remove(Metadata(test.content/"broken.png"));
    const auto linked=test.root/"linked";
    if(CreateSymbolicLinkW(linked.c_str(),test.content.c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY|0x2))
    {request.source=linked/"image.png";request.target="new.png";Check(!EditorAssetActions::Plan(request,test.assets).valid,"symbolic-link input denied");fs::remove(linked);}
    else std::cout<<"Directory symlink case unavailable on this Windows account.\n";
}
void PlayValidation(const fs::path& output)
{
    ProjectFixture test(output,"play");const auto modelId=test.Model(),imageId=test.Image();
    MaterialDocument material;material.baseColorTexture=imageId;AssetId materialId;std::vector<AssetDiagnostic> errors;
    Check(test.assets.SaveMaterialSource(test.content/"surface.material.json",material,materialId,errors),"create play material");
    World world;const auto entity=world.Create("Mesh",123);Check(world.SetMesh(entity,{modelId,{materialId}}),"play mesh");
    UiTree ui;const auto node=ui.Create(UiKind::Image);Check(ui.SetImage(node,imageId),"play image");
    std::vector<EditorAssetIssue> issues;Check(EditorAssetActions::ValidatePlayAssets(world,&ui,test.assets,{},issues),"real mesh material texture and UI validate");
    for(const auto* builtin:{BuiltinCubeAssetId,BuiltinSphereAssetId})
    {
        Check(world.SetMesh(entity,{builtin,{materialId}})&&EditorAssetActions::ValidatePlayAssets(world,&ui,test.assets,{},issues),"built-in mesh needs no external source record");
        Check(world.SetMesh(entity,{builtin,{std::string(32,'e')}})&&!EditorAssetActions::ValidatePlayAssets(world,&ui,test.assets,{},issues),"built-in mesh still validates assigned material");
    }
    Check(world.SetMesh(entity,{modelId,{materialId}}),"restore model fixture");
    material.normalTexture=std::string(32,'e');std::string text;Check(SerializeMaterialDocument(material,text,errors),"serialize unsaved missing texture");
    Check(!EditorAssetActions::ValidatePlayAssets(world,&ui,test.assets,{{EditorAssetDocumentKind::Material,test.content/"surface.material.json",text}},issues),"Play uses unsaved material override");
    Check(std::any_of(issues.begin(),issues.end(),[](const auto& issue){return issue.reference.entity==123&&issue.reference.pointer=="/textures/normal";}),"Play diagnostic locates material and entity");
    Check(ui.SetImage(node,std::string(32,'e')),"set missing UI image");
    Check(!EditorAssetActions::ValidatePlayAssets(world,&ui,test.assets,{},issues)&&std::any_of(issues.begin(),issues.end(),[](const auto& issue){return issue.reference.pointer.starts_with("/nodes/");}),"Play checks UI images");
    Check(ui.SetImage(node,imageId)&&world.SetEnvironmentSettings({std::string(32,'d'),false}),"set missing TOD profile");
    Check(!EditorAssetActions::ValidatePlayAssets(world,&ui,test.assets,{},issues)&&std::any_of(issues.begin(),issues.end(),[](const auto& issue){return issue.reference.pointer=="/environment/todProfile";}),"Play checks TOD profile");
    Check(world.SetEnvironmentSettings({})&&world.SetMesh(entity,{std::string(32,'c'),{materialId}}),"set missing model");
    Check(!EditorAssetActions::ValidatePlayAssets(world,&ui,test.assets,{},issues)&&std::any_of(issues.begin(),issues.end(),[](const auto& issue){return issue.reference.entity==123&&issue.reference.pointer=="/mesh/asset";}),"Play checks model reference");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"usage: EditorAssetActionsTests <output-dir>");const auto id=asset_io::NewId();Check(!id.empty(),"output ID");
        const auto output=fs::absolute(argv[1])/("asset-actions-"+id);fs::create_directories(output);
        IdentityAndManifest(output);ReferencesAndInvalidDocuments(output);ModelClosureAndRollback(output);RacesBoundariesAndSync(output);PlayValidation(output);
        std::cout<<"EditorAssetActionsTests passed (references, identity, dependency moves, rollback, manifest sync, and Play assets).\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"EditorAssetActionsTests failed: "<<error.what()<<'\n';return 1;}
}

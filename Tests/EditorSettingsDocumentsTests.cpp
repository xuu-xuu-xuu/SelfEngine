#include "ToolLayer/Editor/EditorSettingsDocuments.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
using namespace se;
using Json=nlohmann::json;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Put(const fs::path& path,std::string_view text)
{fs::create_directories(path.parent_path());std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(text.data(),static_cast<std::streamsize>(text.size()));Check(bool(file),"fixture write");}
std::string Read(const fs::path& path)
{std::ifstream file(path,std::ios::binary);Check(bool(file),"fixture read");return {std::istreambuf_iterator<char>(file),{}};}
std::string Hash(std::string_view text){return asset_io::Hash({reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});}
std::string Project(const fs::path& root)
{
    fs::create_directories(root/"Content");Put(root/"Scenes"/"Main.scene.json","{}");
    const auto source=std::string("\xEF\xBB\xBF")+Json{{"format","SelfEngine.Project"},{"version",1},{"name","Base"},
        {"contentRoot","Content"},{"entryScene","Scenes/Main.scene.json"},{"scenes",{"Scenes/Main.scene.json"}},
        {"ui",Json::array()},{"assets",Json::array()},{"userExtension",{{"keep",17}}}}.dump(4)+"\n";
    Put(root/"project.json",source);return source;
}
struct ReadLock
{
    HANDLE handle;
    explicit ReadLock(const fs::path& path):handle(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr))
    {Check(handle!=INVALID_HANDLE_VALUE,"lock settings source");}
    ~ReadLock(){CloseHandle(handle);}
};
void ProjectDraft(const fs::path& root)
{
    const auto source=Project(root);std::string error,detail;EditorSettingsDocument document(EditorSettingsKind::Project);
    Check(document.Load(root/"project.json",error)&&document.SourceExists()&&!document.Dirty(),"project loads cleanly");
    Check(document.SourceDigest()==Hash(source),"source digest hashes original BOM and whitespace bytes");
    const auto initialRevision=document.Revision();auto fields=document.Project();
    Check(document.Set(fields,error)&&document.Revision()==initialRevision,"unchanged fields do not increment revision");
    fields.name="Draft";Check(document.Set(fields,error)&&document.Dirty()&&document.Revision()==initialRevision+1,"changed fields mark document dirty once");
    RecoveryDocument snapshot;Check(document.Snapshot(snapshot,error)&&snapshot.documentId=="project-settings"&&snapshot.kind=="project-settings"
        &&snapshot.relativePath=="project.json"&&snapshot.sourceDigest==Hash(source),"recovery identity uses project-relative settings path");
    Check(Json::parse(snapshot.content).at("userExtension").at("keep")==17&&Read(root/"project.json")==source,"snapshot preserves full data without saving source");
    const auto revision=document.Revision();Check(document.Set(fields,error)&&document.Revision()==revision,"repeated identical snapshot content is stable");
    Check(!document.Load(root/"project.json",error)&&error=="Editor.UnsavedChanges"&&document.Project().name=="Draft","implicit reload cannot discard dirty settings");
    fields.name.clear();Check(document.Set(fields,error)&&document.Snapshot(snapshot,error),"incomplete input still has a recovery snapshot");
    Check(!document.Save(error,detail)&&error=="Project.InvalidName"&&document.Dirty()&&Read(root/"project.json")==source,"invalid draft is retained without overwriting source");
    EditorSettingsDocument restored(EditorSettingsKind::Project);Check(restored.Load(root/"project.json",error),"fresh recovery target");
    const auto before=restored.Revision();
    Check(restored.Restore(snapshot,false,error)&&restored.Project().name=="Base"&&!restored.Dirty()&&restored.Revision()==before,"restore validation never mutates memory");
    Check(restored.Restore(snapshot,true,error)&&restored.Project().name.empty()&&restored.Dirty()&&Read(root/"project.json")==source,"restore keeps incomplete draft in memory only");
    fields=restored.Project();fields.name="Saved";Check(restored.Set(fields,error),"correct recovered draft");
    const auto contentRevision=restored.Revision();
    Check(restored.Save(error,detail)&&!restored.Dirty()&&restored.Revision()==contentRevision,"successful save does not count as a content edit");
    Check(restored.SourceDigest()==Hash(Read(root/"project.json"))&&Json::parse(Read(root/"project.json")).at("userExtension").at("keep")==17,"saved baseline matches actual bytes and preserves extensions");
    Check(document.Load(root/"project.json",error,true)&&!document.Dirty()&&document.Project().name=="Saved","explicit discard reload takes the new baseline");
    const auto valid=Read(root/"project.json");Put(root/"project.json","{broken");
    Check(!document.Load(root/"project.json",error,true)&&document.Project().name=="Saved"&&Read(root/"project.json")=="{broken","failed reload retains prior memory and corrupt original");
    Put(root/"project.json",valid);
}
void Conflicts(const fs::path& root)
{
    const auto source=Project(root);EditorSettingsDocument document(EditorSettingsKind::Project);std::string error,detail;
    Check(document.Load(root/"project.json",error),"conflict baseline");auto fields=document.Project();fields.name="Mine";
    Check(document.Set(fields,error),"local edit");RecoveryDocument snapshot;Check(document.Snapshot(snapshot,error),"conflict snapshot");
    auto changed=source;changed.replace(changed.find("Base"),4,"Disk");const auto time=fs::last_write_time(root/"project.json");
    Put(root/"project.json",changed);fs::last_write_time(root/"project.json",time);
    Check(!document.Save(error,detail)&&error=="Editor.ExternalChange"&&document.Dirty()&&Read(root/"project.json")==changed,"same-size same-time external change is detected by raw digest");
    Check(!document.Restore(snapshot,false,error)&&error=="Editor.ExternalChange"&&document.Project().name=="Mine","recovery also refuses changed source bytes");
    Put(root/"project.json",source);const auto revision=document.Revision();
    {ReadLock lock(root/"project.json");Check(!document.Save(error,detail)&&error=="Project.WriteFailed","locked atomic save fails");}
    Check(document.Dirty()&&document.Revision()==revision&&document.SourceDigest()==Hash(source)&&Read(root/"project.json")==source,"failed publication preserves draft and original baseline");
    Check(document.Save(error,detail)&&!document.Dirty(),"failed save can be retried");
}
void NewBuild(const fs::path& root)
{
    Project(root);EditorSettingsDocument document(EditorSettingsKind::Build);std::string error,detail;
    Check(document.Load(root/"build.json",error)&&!document.SourceExists()&&!document.Dirty()&&!fs::exists(root/"build.json"),"missing build is a clean unsaved document");
    BuildSettings fields;fields.scriptsProject="Scripts/Missing.csproj";fields.scriptType="game.spin";
    Check(document.Set(fields,error)&&document.Dirty()&&!document.Save(error,detail)&&error=="Project.InvalidScriptsProject"&&!fs::exists(root/"build.json"),"new build validates references before first publication");
    RecoveryDocument snapshot;Check(document.Snapshot(snapshot,error)&&snapshot.sourceDigest.empty()&&snapshot.relativePath=="build.json","new build recovery preserves absent source marker");
    EditorSettingsDocument restored(EditorSettingsKind::Build);Check(restored.Load(root/"build.json",error)&&restored.Restore(snapshot,false,error)&&!restored.Dirty(),"new build restore can be validated without creating the file");
    Check(restored.Restore(snapshot,true,error)&&restored.Dirty()&&!fs::exists(root/"build.json"),"new build draft restored only in memory");
    Check(restored.Set(BuildSettings{},error)&&!restored.Dirty()&&restored.Save(error,detail)&&restored.SourceExists()&&!restored.Dirty(),"explicit save creates an empty build document");
    const auto created=Read(root/"build.json");
    Check(!document.Save(error,detail)&&Read(root/"build.json")==created,"stale missing-source draft cannot overwrite newly created build");
    Check(document.Set(BuildSettings{},error)&&!document.Save(error,detail)&&error=="Editor.ExternalChange"&&Read(root/"build.json")==created,"new-source existence conflict is explicit");
    auto extended=Json::parse(created);extended["extension"]={{"keep",true}};Put(root/"build.json",extended.dump());
    Check(document.Load(root/"build.json",error,true),"load extended build");Put(root/"UI"/"Main.ui.json","{}");
    fields={};fields.entryUi="UI/Main.ui.json";
    Check(document.Set(fields,error)&&document.Save(error,detail)&&Json::parse(Read(root/"build.json")).at("extension").at("keep")==true,"document build save preserves unknown fields");
}
void EntrySceneRegistration(const fs::path& root)
{
    const auto source=Project(root);Put(root/"Scenes"/"Other.scene.json","{}");
    EditorSettingsDocument document(EditorSettingsKind::Project);std::string error,detail;
    Check(document.Load(root/"project.json",error),"entry scene baseline");auto settings=document.Project();RecoveryDocument snapshot;
    settings.entryScene="Scenes/O";
    Check(document.Set(settings,error)&&document.Snapshot(snapshot,error)
        &&Json::parse(snapshot.content).at("scenes")==Json::array({"Scenes/Main.scene.json"}),"typing a partial entry never registers a draft path");
    settings.entryScene="Scenes/./Other.scene.json";
    Check(document.Set(settings,error)&&document.Snapshot(snapshot,error)
        &&Json::parse(snapshot.content).at("scenes")==Json::array({"Scenes/Main.scene.json"}),"even a valid entry remains unregistered until final save");
    const auto draft=snapshot.content;const auto revision=document.Revision();
    {ReadLock lock(root/"project.json");Check(!document.Save(error,detail),"entry registration waits for successful publication");}
    Check(document.Snapshot(snapshot,error)&&snapshot.content==draft&&document.Revision()==revision&&Read(root/"project.json")==source,"failed save cannot leak derived scene registration into the draft or source");
    Check(document.Save(error,detail)&&!document.Dirty()&&document.Revision()==revision+1&&document.Snapshot(snapshot,error),"successful save records the derived content revision");
    Check(Json::parse(snapshot.content).at("scenes")==Json::array({"Scenes/Main.scene.json","Scenes/Other.scene.json"})
        &&snapshot.content==Read(root/"project.json")&&snapshot.sourceDigest==Hash(snapshot.content),"published scene list, full snapshot and raw baseline agree");
    settings.entryScene="Scenes\\Other.scene.json";Check(document.Set(settings,error)&&document.Save(error,detail),"save equivalent Windows entry spelling");
    Check(Json::parse(Read(root/"project.json")).at("scenes")==Json::array({"Scenes/Main.scene.json","Scenes/Other.scene.json"}),"canonical path match prevents duplicate registration and preserves order");
}
void InvalidRecovery(const fs::path& root)
{
    const auto source=Project(root);EditorSettingsDocument document(EditorSettingsKind::Project);std::string error;
    Check(document.Load(root/"project.json",error),"invalid recovery baseline");RecoveryDocument snapshot;Check(document.Snapshot(snapshot,error),"baseline snapshot");
    const auto revision=document.Revision();auto invalid=snapshot;invalid.relativePath="../project.json";
    Check(!document.Restore(invalid,true,error),"recovery traversal refused");invalid=snapshot;invalid.documentId="build-settings";
    Check(!document.Restore(invalid,true,error),"recovery identity substitution refused");invalid=snapshot;invalid.sourceDigest=std::string(64,'0');
    Check(!document.Restore(invalid,true,error)&&error=="Editor.ExternalChange","recovery source substitution refused");
    auto future=Json::parse(snapshot.content);future["version"]=2;invalid=snapshot;invalid.content=future.dump();
    Check(!document.Restore(invalid,true,error)&&error=="Project.UnsupportedVersion","future recovery schema refused");
    invalid=snapshot;invalid.content="{broken";Check(!document.Restore(invalid,true,error),"malformed recovery refused");
    Check(!document.Dirty()&&document.Revision()==revision&&document.Project().name=="Base"&&Read(root/"project.json")==source,"rejected restores leave memory revision and source unchanged");
    Check(!document.Load(root/"other.json",error)&&error=="Project.PathOutsideRoot","settings loader only accepts its exact source filename");
    Put(root/"project.json",future.dump());Check(!document.Load(root/"project.json",error,true)&&error=="Project.UnsupportedVersion"&&document.Project().name=="Base","future disk schema is preserved without losing existing memory");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"usage: EditorSettingsDocumentsTests <output-dir>");const auto id=asset_io::NewId();Check(!id.empty(),"test output ID");
        const auto output=fs::absolute(argv[1])/("settings-"+id);fs::create_directories(output);
        ProjectDraft(output/"project");Conflicts(output/"conflict");NewBuild(output/"build");EntrySceneRegistration(output/"entry-scene");InvalidRecovery(output/"recovery");
        for(const auto& entry:fs::recursive_directory_iterator(output))Check(entry.path().filename().wstring().find(L".tmp-")==std::wstring::npos,"no unpublished settings candidates left");
        std::cout<<"EditorSettingsDocumentsTests passed (draft/recovery state, exact digests, conflict-safe save, missing build and schema preservation).\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"EditorSettingsDocumentsTests failed: "<<error.what()<<'\n';return 1;}
}

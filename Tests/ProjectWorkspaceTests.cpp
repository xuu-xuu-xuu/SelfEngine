#include "ToolLayer/Editor/ProjectWorkspaceSession.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace fs=std::filesystem;
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Put(const fs::path& path,const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path,std::ios::binary);output<<text;Check(bool(output),"write fixture");
}
std::string Read(const fs::path& path)
{
    std::ifstream input(path,std::ios::binary);Check(bool(input),"read fixture");
    return {std::istreambuf_iterator<char>(input),{}};
}
std::string ToUtf8(const fs::path& path){const auto text=path.u8string();return std::string(reinterpret_cast<const char*>(text.data()),text.size());}
bool HasCandidate(const fs::path& parent)
{
    std::error_code error;
    for(const auto& entry:fs::directory_iterator(parent,error))
        if(entry.path().filename().wstring().find(L".candidate-")!=std::wstring::npos)return true;
    return false;
}
}
int wmain(int argc,wchar_t** argv)
{
    std::string step="start";
    try
    {
        Check(argc==3,"usage: ProjectWorkspaceTests <templates-root> <output-dir>");
        const auto templates=fs::absolute(fs::path(argv[1]));
        auto output=fs::absolute(fs::path(argv[2]));
        {std::error_code cleanup;fs::remove_all(output,cleanup);}
        fs::create_directories(output);
        output/=("run-"+std::to_string(GetTickCount64()));fs::create_directories(output);
        const auto blankTemplate=templates/"BlankScene",basicTemplate=templates/"EmptyGame";
        Check(fs::is_regular_file(blankTemplate/"project.json")&&fs::is_regular_file(basicTemplate/"project.json"),"both templates exist");

        step="preflight";const auto blankCopy=output/"blank-template";
        fs::copy(blankTemplate,blankCopy,fs::copy_options::recursive|fs::copy_options::overwrite_existing);
        const auto blank=ProjectWorkspaceSession::Preflight(blankCopy);
        if(!(blank.ok&&blank.writable&&blank.root==fs::weakly_canonical(blankCopy)))
            throw std::runtime_error("blank template preflight: ok="+std::to_string(blank.ok)+", writable="+std::to_string(blank.writable)
                +", key="+blank.errorKey+", detail="+blank.detail+", root="+blank.root.string()
                +", expected="+fs::weakly_canonical(blankCopy).string());
        Check(blank.entryScene==fs::weakly_canonical(blankCopy/"Scenes/Main.scene.json"),"entry scene resolved");
        Check(!ProjectWorkspaceSession::Preflight(output/"missing").ok,"missing project rejected");

        step="future";auto future=output/"future";fs::copy(blankTemplate,future,fs::copy_options::recursive|fs::copy_options::overwrite_existing);
        auto futureJson=Read(future/"project.json");const std::string versionKey="\"version\": 1";const auto position=futureJson.find(versionKey);
        Check(position!=std::string::npos,"version key present");futureJson.replace(position,versionKey.size(),"\"version\": 2");
        Put(future/"project.json",futureJson);
        Check(ProjectWorkspaceSession::Preflight(future).errorKey=="Project.UnsupportedVersion","future version rejected");

        step="missing-scene";auto missingScene=output/"missing-scene";fs::copy(blankTemplate,missingScene,fs::copy_options::recursive|fs::copy_options::overwrite_existing);
        fs::remove(missingScene/"Scenes/Main.scene.json");
        Check(ProjectWorkspaceSession::Preflight(missingScene).errorKey=="Project.MissingEntryScene","missing entry scene rejected");

        step="create-blank";const auto parent=output/L"父 目录";fs::create_directories(parent);
        ProjectPreflight created;std::string errorKey,detail;
        Check(ProjectWorkspaceSession::CreateProject(ProjectTemplate::BlankScene,"我的 项目",parent,templates,created,errorKey,detail),
            "create blank project in unicode parent");
        Check(created.ok&&fs::is_regular_file(parent/L"我的 项目/project.json"),"created project preflight");
        step="duplicate";Check(!ProjectWorkspaceSession::CreateProject(ProjectTemplate::BlankScene,"我的 项目",parent,templates,created,errorKey,detail)
            &&errorKey=="Project.TargetNotEmpty","duplicate target rejected");
        Check(!ProjectWorkspaceSession::CreateProject(ProjectTemplate::BlankScene,"CON",parent,templates,created,errorKey,detail)
            &&errorKey=="Project.InvalidName","reserved device name rejected");
        step="nonempty";fs::create_directories(output/L"占用");Put(output/L"占用/keep.txt","x");
        Check(!ProjectWorkspaceSession::CreateProject(ProjectTemplate::BlankScene,"占用",output,templates,created,errorKey,detail)
            &&errorKey=="Project.TargetNotEmpty"&&!HasCandidate(output),"non-empty target rejected without candidate");
        Check(fs::is_regular_file(output/L"占用/keep.txt"),"rejected create keeps user content");
        step="basic";Check(ProjectWorkspaceSession::CreateProject(ProjectTemplate::Basic3D,"基础 示例",parent,templates,created,errorKey,detail)
            &&created.ok,"create basic 3D project");
        Check(fs::is_regular_file(parent/L"基础 示例/build.json"),"basic template keeps build settings");

        step="recent";const auto userDirectory=output/"user";
        ProjectWorkspaceSession session;std::string error;
        Check(session.Load(userDirectory,error)&&session.Recent().empty(),"fresh preferences");
        ProjectPreflight first;first.ok=true;first.manifest=output/"a/project.json";first.root=output/"a";
        ProjectPreflight second=first;second.manifest=output/"b/project.json";second.root=output/"b";
        Check(session.RecordOpened(first)&&session.RecordOpened(second)&&session.Recent().size()==2,"two recent projects");
        Check(session.Recent()[0].path==ToUtf8(output/"b/project.json"),"latest project first");
        Check(session.RecordOpened(first)&&session.Recent().size()==2&&session.Recent()[0].path==ToUtf8(output/"a/project.json"),"recent deduplicates by canonical path");
        for(int index=0;index<22;++index)
        {
            ProjectPreflight extra;extra.ok=true;extra.manifest=output/("extra-"+std::to_string(index))/"project.json";extra.root=extra.manifest.parent_path();
            Check(session.RecordOpened(extra),"record extra recent");
        }
        Check(session.Recent().size()==20,"recent list capped at 20");
        Check(session.Save(error)&&fs::is_regular_file(userDirectory/"preferences.json"),"preferences saved");
        ProjectWorkspaceSession reloaded;Check(reloaded.Load(userDirectory,error)&&reloaded.Recent().size()==20,"preferences reloaded");
        Check(reloaded.Language()=="zh-CN"&&reloaded.RestoreLastProject(),"preference defaults survive");
        Check(reloaded.RemoveRecent(reloaded.Recent()[0].path)&&reloaded.Recent().size()==19,"remove recent does not touch project");
        EditorPreferences editorPreferences;
        Check(ProjectWorkspaceSession::LoadPreferences(userDirectory,editorPreferences,error)&&editorPreferences.recentProjects.size()==20,"editor preferences read workspace recent objects");
        editorPreferences.uiScale=1.75f;editorPreferences.externalEditor="my-editor --goto";
        editorPreferences.colorLut="Rendering/Grade.selut";
        editorPreferences.language="en-US";editorPreferences.restoreLastProject=false;
        Check(ProjectWorkspaceSession::SavePreferences(userDirectory,editorPreferences,error),"editor preferences merge with workspace state");
        EditorPreferences colorLutPreferences;
        Check(ProjectWorkspaceSession::LoadPreferences(userDirectory,colorLutPreferences,error)
            &&colorLutPreferences.colorLut=="Rendering/Grade.selut","color LUT path roundtrips through preferences");
        auto stored=nlohmann::json::parse(Read(userDirectory/"preferences.json"));
        Check(stored["recent"][0]["name"].is_string()&&!stored["recent"][0]["last_opened"].get<std::string>().empty()
            &&stored["last_project"]==reloaded.LastProject(),"editor save preserves recent metadata and last project");
        stored["extension_setting"]="keep";Put(userDirectory/"preferences.json",stored.dump());
        Check(reloaded.Save(error),"workspace updates recent list");
        stored=nlohmann::json::parse(Read(userDirectory/"preferences.json"));
        Check(stored["ui_scale"]==1.75f&&stored["external_editor"]=="my-editor --goto"&&stored["extension_setting"]=="keep"
            &&stored["color_lut"]=="Rendering/Grade.selut"
            &&stored["recent"].size()==19&&stored["language"]=="en-US"&&stored["restore_last_project"]==false,"stale workspace recent save keeps newer language, restore setting and other editor preferences");
        reloaded.SetLanguage("zh-CN");reloaded.SetRestoreLastProject(true);
        Check(reloaded.Save(error),"explicit workspace preference edits save");
        stored=nlohmann::json::parse(Read(userDirectory/"preferences.json"));
        Check(stored["language"]=="zh-CN"&&stored["restore_last_project"]==true,"explicit workspace preference edits replace prior values");
        Check(ProjectWorkspaceSession::SavePreferences(userDirectory,editorPreferences,error),"stale preference view can save safely");
        Check(reloaded.Save(error),"workspace may save again after successful preference edit");
        stored=nlohmann::json::parse(Read(userDirectory/"preferences.json"));
        Check(stored["recent"].size()==19&&stored["language"]=="en-US"&&stored["restore_last_project"]==false,"completed workspace preference edits cannot overwrite subsequent settings or resurrect removed project");
        ProjectWorkspaceSession emptyRecent;Check(emptyRecent.Load(userDirectory,error),"load recent removal state");
        while(!emptyRecent.Recent().empty())Check(emptyRecent.RemoveRecent(emptyRecent.Recent()[0].path),"remove all recent entries");
        Check(emptyRecent.Save(error)&&ProjectWorkspaceSession::SavePreferences(userDirectory,editorPreferences,error),"save empty recent list and stale editor settings");
        Check(nlohmann::json::parse(Read(userDirectory/"preferences.json"))["recent"].empty(),"empty recent list stays empty");

        const auto corruptDirectory=output/"corrupt-user";Put(corruptDirectory/"preferences.json","{ broken");
        ProjectWorkspaceSession corrupted;Check(!corrupted.Load(corruptDirectory,error)&&corrupted.Recent().empty(),"corrupt preferences without backup rejected");
        Check(Read(corruptDirectory/"preferences.json")=="{ broken","corrupt preferences preserved");
        const auto validBackup=Read(userDirectory/"preferences.json.bak");Put(userDirectory/"preferences.json","{ interrupted");
        ProjectWorkspaceSession fallback;
        Check(fallback.Load(userDirectory,error)&&error=="Editor.StorageBackupUsed"&&Read(userDirectory/"preferences.json")=="{ interrupted","preferences load valid backup without changing damaged primary");
        Check(fallback.Save(error)&&Read(userDirectory/"preferences.json.bak")==validBackup,"preference repair preserves known-good backup");
        bool archived=false;
        for(const auto& item:fs::directory_iterator(userDirectory))
            if(item.path().filename().string().rfind("preferences.json.corrupt-",0)==0&&Read(item.path())=="{ interrupted")archived=true;
        Check(archived,"preference repair retains exact damaged bytes");
        Put(userDirectory/"preferences.json","{\"schema\":2}");
        ProjectWorkspaceSession futurePreferences;
        Check(!futurePreferences.Load(userDirectory,error)&&error=="Editor.StorageUnsupportedSchema","future preference schema rejects older backup");
        Check(!fallback.Save(error)&&!ProjectWorkspaceSession::SavePreferences(userDirectory,editorPreferences,error)
            &&Read(userDirectory/"preferences.json")=="{\"schema\":2}","neither preference writer overwrites future schema");

        const auto legacyDirectory=output/"legacy-user";
        Put(legacyDirectory/"preferences.json","{\"schema\":1,\"recent\":[\"a/project.json\",\"b/project.json\"],\"external_editor\":\"code\"}");
        ProjectWorkspaceSession legacy;
        Check(legacy.Load(legacyDirectory,error)&&legacy.Recent().size()==2&&legacy.Save(error),"legacy recent strings migrate through shared storage");
        stored=nlohmann::json::parse(Read(legacyDirectory/"preferences.json"));
        Check(stored["recent"][0].is_object()&&stored["recent"][0]["path"]=="a/project.json"&&stored["external_editor"]=="code","legacy migration preserves recent paths and editor setting");

        step="lease";const auto leaseManifest=output/"lease/project.json";fs::create_directories(leaseManifest.parent_path());
        bool acquired=false;std::string occupant;
        auto lease=ProjectLease::Acquire(userDirectory,leaseManifest,acquired,occupant);
        Check(acquired&&lease.Held(),"first project lease acquired");
        bool secondAcquired=false;
        auto secondLease=ProjectLease::Acquire(userDirectory,leaseManifest,secondAcquired,occupant);
        Check(!secondAcquired&&!occupant.empty()&&occupant.find("pid")!=std::string::npos,"second lease reports occupant");
        lease.Release();
        auto thirdLease=ProjectLease::Acquire(userDirectory,leaseManifest,secondAcquired,occupant);
        Check(secondAcquired,"lease reacquired after release");
        (void)secondLease;(void)thirdLease;
        std::cout<<"ProjectWorkspaceTests passed (preflight, templates, unicode parents, recent projects, atomic preferences and project lease).\n";
        return 0;
    }
    catch(const std::exception& failure)
    {
        std::cerr<<"ProjectWorkspaceTests failed at ["<<step<<"]: "<<failure.what()<<'\n';return 1;
    }
}

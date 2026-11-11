#include "ToolLayer/Editor/ProjectSettingsService.h"
#include "ToolLayer/Tools/Project.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <nlohmann/json.hpp>
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
void Put(const fs::path& path,const std::string& text){fs::create_directories(path.parent_path());std::ofstream output(path,std::ios::binary|std::ios::trunc);output<<text;Check(bool(output),"write exact fixture bytes");}
std::string Read(const fs::path& path){std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};}
void Touch(const fs::path& path){Put(path,"{}");}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"usage: ProjectSettingsTests <output-dir>");
        const auto output=fs::absolute(fs::path(argv[1]))/asset_io::NewId();
        fs::create_directories(output);
        fs::create_directories(output/"Content");
        Touch(output/"Scenes/Main.scene.json");
        Touch(output/"Scenes/Other.scene.json");
        const auto manifest=output/"project.json";
        Put(manifest,"{\"format\":\"SelfEngine.Project\",\"version\":1,\"name\":\"Old\",\"contentRoot\":\"Content\",\"entryScene\":\"Scenes/Main.scene.json\",\"scenes\":[\"Scenes/Main.scene.json\"],\"ui\":[],\"assets\":[{\"id\":\"11111111111111111111111111111111\",\"type\":\"model\"}]}");
        ProjectSettings settings;std::string errorKey;
        Check(ProjectSettingsService::ReadProject(manifest,settings,errorKey)&&settings.name=="Old"&&settings.contentRoot=="Content","read project settings");
        Check(settings.assets.size()==1&&settings.assets[0].find("1111")==0,"asset summary read");
        settings.name="New Name";settings.entryScene="Scenes/Other.scene.json";
        Check(ProjectSettingsService::WriteProject(manifest,settings,errorKey),"write project settings");
        Check(Read(manifest).find("\"assets\"")!=std::string::npos,"write preserves unrelated fields");
        Check(ProjectSettingsService::ReadProject(manifest,settings,errorKey)&&settings.name=="New Name"&&settings.entryScene=="Scenes/Other.scene.json","settings round trip");
        Check(nlohmann::json::parse(Read(manifest)).at("scenes")==nlohmann::json::array({"Scenes/Main.scene.json","Scenes/Other.scene.json"}),"saving a new entry appends its scene without removing the prior entry");
        ProjectDescription runtimeProject;std::vector<ProjectDiagnostic> diagnostics;
        Check(se::ReadProject(manifest,runtimeProject,diagnostics),"core project loader accepts the newly selected registered entry");
        settings.entryScene="Scenes/./Other.scene.json";
        Check(ProjectSettingsService::WriteProject(manifest,settings,errorKey)
            &&nlohmann::json::parse(Read(manifest)).at("scenes")==nlohmann::json::array({"Scenes/Main.scene.json","Scenes/Other.scene.json"})
            &&se::ReadProject(manifest,runtimeProject,diagnostics),"equivalent entry spelling does not duplicate a registered scene");
        auto invalid=settings;invalid.name="bad/name";
        Check(!ProjectSettingsService::WriteProject(manifest,invalid,errorKey)&&errorKey=="Project.InvalidName","invalid name rejected");
        Check(Read(manifest).find("New Name")!=std::string::npos,"rejected write keeps file");
        std::string detail;
        Check(ProjectSettingsService::ValidateProject(output,settings,errorKey,detail),"valid project accepted");
        auto missingScene=settings;missingScene.entryScene="Scenes/Missing.scene.json";
        Check(!ProjectSettingsService::ValidateProject(output,missingScene,errorKey,detail)&&errorKey=="Project.InvalidEntryScene","missing entry scene rejected");
        auto outside=settings;outside.contentRoot="../outside";
        Check(!ProjectSettingsService::ValidateProject(output,outside,errorKey,detail)&&errorKey=="Project.InvalidContentRoot","outside content root rejected");
        const auto buildJson=output/"build.json";
        BuildSettings build;
        Check(!ProjectSettingsService::ReadBuild(buildJson,build,errorKey)&&errorKey=="Project.BuildSettingsMissing","missing build settings reported");
        build.scriptsProject="Scripts/Game.csproj";build.scriptType="game.spin";build.bindings="Scripts/Bindings.json";build.entryUi="UI/Main.ui.json";
        Check(ProjectSettingsService::WriteBuild(buildJson,build,errorKey),"write build settings");
        BuildSettings loaded;
        Check(ProjectSettingsService::ReadBuild(buildJson,loaded,errorKey)&&loaded.scriptType=="game.spin"&&loaded.entryUi=="UI/Main.ui.json","build settings round trip");
        Check(!ProjectSettingsService::ValidateBuild(output,loaded,errorKey,detail)&&errorKey=="Project.InvalidScriptsProject","missing scripts project rejected");
        Touch(output/"Scripts/Game.csproj");Touch(output/"Scripts/Bindings.json");Touch(output/"UI/Main.ui.json");
        Check(ProjectSettingsService::ValidateBuild(output,loaded,errorKey,detail),"valid build settings accepted");
        auto missingBindings=loaded;missingBindings.bindings="Scripts/Missing.json";
        Check(!ProjectSettingsService::ValidateBuild(output,missingBindings,errorKey,detail)&&errorKey=="Project.InvalidBindings","missing bindings rejected");
        auto missingUi=loaded;missingUi.entryUi="UI/Missing.ui.json";
        Check(!ProjectSettingsService::ValidateBuild(output,missingUi,errorKey,detail)&&errorKey=="Project.InvalidEntryUi","missing entry UI rejected");
        BuildSettings empty;
        Check(ProjectSettingsService::MissingBuildFields(empty).size()==2,"missing build fields reported");
        auto extended=nlohmann::json::parse(Read(buildJson));extended["userExtension"]={{"keep",17}};Put(buildJson,extended.dump(2));
        loaded.scriptType="game.changed";Check(ProjectSettingsService::WriteBuild(buildJson,loaded,errorKey),"update existing build settings");
        Check(nlohmann::json::parse(Read(buildJson)).at("userExtension").at("keep")==17,"build write preserves extension fields");
        extended["version"]=2;const auto futureBuild=extended.dump(2);Put(buildJson,futureBuild);
        Check(!ProjectSettingsService::ReadBuild(buildJson,loaded,errorKey)&&errorKey=="Project.UnsupportedVersion"
            &&!ProjectSettingsService::WriteBuild(buildJson,build,errorKey)&&errorKey=="Project.UnsupportedVersion"
            &&Read(buildJson)==futureBuild,"future build settings are neither accepted nor overwritten");
        auto future=nlohmann::json::parse(Read(manifest));future["version"]=2;const auto futureProject=future.dump(2);Put(manifest,futureProject);
        Check(!ProjectSettingsService::WriteProject(manifest,settings,errorKey)&&errorKey=="Project.UnsupportedVersion"
            &&Read(manifest)==futureProject,"project write preserves unsupported future schema");
        Put(buildJson,"{\"format\":\"SelfEngine.Build\",\"version\":1,\"version\":1}");const auto duplicate=Read(buildJson);
        Check(!ProjectSettingsService::WriteBuild(buildJson,build,errorKey)&&Read(buildJson)==duplicate,"ambiguous duplicate fields are not rewritten");
        Put(buildJson,"");Check(!ProjectSettingsService::WriteBuild(buildJson,build,errorKey)&&Read(buildJson).empty(),"empty existing build is retained as damaged data");
        std::cout<<"ProjectSettingsTests passed (settings round trip, preservation, asset summary, path validation, build validation).\n";
        return 0;
    }
    catch(const std::exception& failure)
    {
        std::cerr<<"ProjectSettingsTests failed: "<<failure.what()<<'\n';return 1;
    }
}

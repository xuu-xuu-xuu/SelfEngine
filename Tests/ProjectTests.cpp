#include "ToolLayer/Tools/Project.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
using Json=nlohmann::json;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::string Read(const fs::path& path){std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};}
void Write(const fs::path& path,const std::string& text){std::ofstream(path,std::ios::binary)<<text;}
int wmain(int argc,wchar_t** argv)
{
    std::vector<se::ProjectDiagnostic> diagnostics;
    try
    {
        const fs::path source=argc>1?argv[1]:L"Resources/Templates/EmptyGame";
        const fs::path parent=argc>2?argv[2]:L"out/validation/projects";
        const auto root=fs::absolute(parent)/se::asset_io::FromUtf8(se::asset_io::NewId());
        Check(se::CreateProject(source,root,diagnostics),"create valid template");
        const auto manifest=root/"project.json";const auto original=Read(manifest);
        Check(!se::CreateProject(source,root,diagnostics)&&Read(manifest)==original,"existing project never overwritten");
        se::ProjectValidation validation;Check(se::ValidateProject(manifest,validation,diagnostics),"validate assets scenes and UI");
        Check(validation.entities==4&&validation.uiNodes==2&&validation.dependencies.size()==9,"real template contents and closure including material texture");
        Check(se::ExportProjectDependencies(manifest,root/"dependencies.json",diagnostics),"export dependency closure");
        Check(!se::ExportProjectDependencies(manifest,manifest,diagnostics)&&Read(manifest)==original,"dependency output cannot overwrite input");
        const auto dependencyText=Read(root/"dependencies.json");const auto dependencies=Json::parse(dependencyText);
        for(const auto& file:dependencies.at("files"))
        {
            const auto path=se::asset_io::FromUtf8(file.at("path").get<std::string>());
            Check(!path.is_absolute()&&file.at("sha256").get<std::string>().size()==64,"relative path and SHA256");
        }
        se::ReplayReport first,second;
        Check(se::ReplayProject(manifest,root/"run-a",first,diagnostics,180,true),"first fixed replay");
        Check(se::ReplayProject(manifest,root/"run-b",second,diagnostics,180,false),"repeated replay without profiler");
        Check(first.steps==180&&first.stateHash==second.stateHash&&first.stateHash.size()==64,"identical fixed input produces identical world state");
        Check(!se::ReplayProject(manifest,root/"run-a",second,diagnostics,180,false),"replay requires fresh output and preserves prior capture");
        Check(fs::is_regular_file(root/"run-a/cpu-summary.csv")&&!fs::exists(root/"run-b/cpu-summary.csv"),"explicit profiler switch");
        se::World after;std::string error;Check(se::LoadScene(root/"run-a/final.scene.json",after,error),"load replay output");
        const auto box=after.GetTransform(after.FindByStableId(4));Check(box&&box->position.x>0.1f&&box->position.y<3,"replay simulates gravity and command impulse");

        const auto texture=root/"Content/Models/checker.png";fs::rename(texture,root/"saved-checker.png");
        Check(!se::ExportProjectDependencies(manifest,root/"dependencies.json",diagnostics),"missing transitive texture rejected");
        Check(Read(root/"dependencies.json")==dependencyText,"failed dependency export preserves previous file");
        fs::rename(root/"saved-checker.png",texture);
        auto malformed=Json::parse(original);malformed["contentRoot"]="../outside";Write(manifest,malformed.dump());
        Check(!se::ReadProject(manifest,validation.project,diagnostics),"traversal outside project rejected");
        Write(manifest,original);
        const auto scene=root/"Scenes/Main.scene.json";const auto sceneOriginal=Read(scene);
        malformed=Json::parse(sceneOriginal);
        for(auto& component:malformed["entities"][2]["components"])
            if(component.value("type",std::string{})==std::string("3"))
                component["properties"]["materials"]={"unsupported-material"};
        Write(scene,malformed.dump());
        Check(!se::ValidateProject(manifest,validation,diagnostics)&&diagnostics.back().key=="Project.MissingAsset","missing material override rejected");
        Write(scene,sceneOriginal);
        const auto material=root/"Content/Materials/Checker.material.json";const auto materialOriginal=Read(material);
        malformed=Json::parse(materialOriginal);malformed["textures"]["baseColor"]="99999999999999999999999999999999";Write(material,malformed.dump());
        Check(!se::ValidateProject(manifest,validation,diagnostics)&&diagnostics.back().key=="Project.MissingAsset","transitive material texture ID rejected");Write(material,materialOriginal);
        const auto ui=root/"UI/Main.ui.json";const auto uiOriginal=Read(ui);Write(ui,"{broken");
        Check(!se::ValidateProject(manifest,validation,diagnostics),"broken UI rejected");Write(ui,uiOriginal);
        Write(manifest,"{\"format\":\"SelfEngine.Project\",\"version\":1,\"version\":2}");
        Check(!se::ReadProject(manifest,validation.project,diagnostics),"duplicate manifest keys rejected");Write(manifest,original);
        Check(se::ValidateProject(manifest,validation,diagnostics),"project intact after rejected edits");
        std::cout<<"ProjectTests passed: "<<se::asset_io::Utf8(root)<<" state="<<first.stateHash<<'\n';return 0;
    }
    catch(const std::exception& error)
    {std::cerr<<"ProjectTests failed: "<<error.what()<<'\n';for(const auto& d:diagnostics)std::cerr<<d.key<<": "<<d.detail<<'\n';return 1;}
}

#include "ExportService.h"
#include "EditorSessionStore.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iterator>

namespace se
{
namespace
{
using Json=nlohmann::json;
bool ValidName(const std::string& name)
{
    if(name.empty()||name=="."||name==".."||name.size()>128)return false;
    return name.find_first_of("<>:\"/\\|?*")==std::string::npos;
}
std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path,std::ios::binary);
    if(!input)return {};
    return {std::istreambuf_iterator<char>(input),{}};
}
}
std::vector<std::string> ExportService::Arguments(const ExportRequest& request)
{
    std::vector<std::string> arguments={"-Project",request.projectManifest.string(),"-Name",request.name};
    if(!request.evidenceDirectory.empty()){arguments.push_back("-EvidenceDirectory");arguments.push_back(request.evidenceDirectory.string());}
    return arguments;
}
ExportResult ExportService::Run(const ExportRequest& request,const Runner& runner,const Progress& progress)
{
    ExportResult result;
    result.stage="validate";
    std::error_code error;
    if(!std::filesystem::is_regular_file(request.projectManifest,error))
    {
        result.errorKey="Project.ManifestNotFound";result.detail=request.projectManifest.string();return result;
    }
    if(!ValidName(request.name)){result.errorKey="Project.InvalidName";result.detail=request.name;return result;}
    if(!request.evidenceDirectory.empty())
    {
        std::filesystem::create_directories(request.evidenceDirectory,error);
        if(error){result.errorKey="Export.EvidenceWriteFailed";result.detail=request.evidenceDirectory.string();return result;}
    }
    if(!runner)
    {
        result.errorKey="Export.StepFailed";result.detail="runner unavailable";return result;
    }
    const auto arguments=Arguments(request);
    std::string output;
    result.stage="export";
    if(progress)progress("input_validation");
    try
    {
        result.exitCode=runner(arguments,output,[&progress](const std::string& phase){if(progress)progress(phase);});
    }
    catch(const std::exception& failure)
    {
        result.errorKey="Export.StepFailed";result.detail=failure.what();return result;
    }
    result.report=ReadReport(request.evidenceDirectory);
    if(result.report.valid&&!result.report.output.empty())result.outputPackage=result.report.output;
    if(result.exitCode!=0||!result.report.valid||!result.report.success||result.report.phase!="complete"||result.outputPackage.empty()
        ||!std::filesystem::is_regular_file(result.outputPackage/"package-manifest.json",error))
    {
        result.errorKey="Export.StepFailed";
        result.detail=result.report.valid&&!result.report.error.empty()?result.report.error:output;
        if(result.report.valid&&!result.report.failedPhase.empty())result.stage=StageKey(result.report.failedPhase);
        else result.stage="Editor.ExportStage.Failed";
        return result;
    }
    result.success=true;result.stage="complete";return result;
}
ExportReport ExportService::ReadReport(const std::filesystem::path& evidenceDirectory)
{
    ExportReport report;
    if(evidenceDirectory.empty())return report;
    const auto text=ReadText(evidenceDirectory/"export-report.json");
    if(text.empty())return report;
    try
    {
        const auto json=Json::parse(text);
        if(!json.is_object()||json.value("schema",0)!=1||!json.contains("success")||!json["success"].is_boolean())return report;
        report.valid=true;
        report.success=json.value("success",false);
        report.phase=json.value("phase",std::string{});
        report.failedPhase=json.value("failed_phase",std::string{});
        report.error=json.value("error",std::string{});
        const auto stringField=[&](const char* key){return json.contains(key)&&json[key].is_string()?json[key].get<std::string>():std::string{};};
        report.output=stringField("output");report.staging=stringField("staging");report.outputRoot=stringField("output_root");
    }
    catch(const std::exception&)
    {
        report={};
    }
    return report;
}
bool ExportService::CleanupCancelledStage(const ExportRequest& request,const ExportReport& report,std::string& error)
{
    namespace fs=std::filesystem;error.clear();if(report.staging.empty())return true;
    std::error_code ec;
    const auto stage=fs::weakly_canonical(asset_io::FromUtf8(report.staging),ec);
    if(ec){error="Editor.ExportCleanupRejected";return false;}
    if(!fs::exists(stage,ec))return !ec;
    const auto outputRoot=fs::weakly_canonical(asset_io::FromUtf8(report.outputRoot),ec);
    if(ec||report.outputRoot.empty()){error="Editor.ExportCleanupRejected";return false;}
    const auto projectPackages=fs::weakly_canonical(request.projectManifest.parent_path()/"out"/"packages",ec);
    const auto engine=EditorSessionStore::EngineRootFromExecutable();
    const auto enginePackages=engine.empty()?fs::path{}:fs::weakly_canonical(engine/"out"/"packages",ec);
    const auto same=[](const fs::path& a,const fs::path& b){return !a.empty()&&!b.empty()&&asset_io::Within(a,b)&&asset_io::Within(b,a);};
    const auto name=asset_io::Utf8(stage.filename()),prefix="."+request.name+".stage-";
    const auto suffix=name.starts_with(prefix)?name.substr(prefix.size()):std::string{};
    if(!same(stage.parent_path(),outputRoot)||(!same(outputRoot,projectPackages)&&!same(outputRoot,enginePackages))
        ||suffix.size()<32||!std::all_of(suffix.begin(),suffix.end(),[](unsigned char ch){return std::isxdigit(ch)||ch=='-';}))
    {error="Editor.ExportCleanupRejected";return false;}
    for(auto path=stage;!path.empty();path=path.parent_path())
    {
        const auto attributes=GetFileAttributesW(path.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT)){error="Editor.ExportCleanupRejected";return false;}
        if(path==path.parent_path())break;
    }
    for(fs::recursive_directory_iterator it(stage,ec),end;!ec&&it!=end;it.increment(ec))
    {const auto attributes=GetFileAttributesW(it->path().c_str());if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT)){error="Editor.ExportCleanupRejected";return false;}}
    if(ec){error="Editor.ExportCleanupRejected";return false;}
    fs::remove_all(stage,ec);if(ec){error="Editor.ExportCleanupFailed";return false;}return true;
}
const char* ExportService::StageKey(const std::string& phase)
{
    if(phase=="input_validation")return "Editor.ExportStage.Validate";
    if(phase=="content_validation")return "Editor.ExportStage.Content";
    if(phase=="publish_runtime"||phase=="script_name"||phase=="restore_scripts"||phase=="publish_scripts"||phase=="script_sdk")
        return "Editor.ExportStage.Build";
    if(phase=="field_preflight"||phase=="packaged_content"||phase=="headless_smoke"||phase=="window_smoke")
        return "Editor.ExportStage.Verify";
    if(phase=="validated"||phase=="manifest")return "Editor.ExportStage.Package";
    if(phase=="complete")return "Editor.ExportStage.Done";
    if(phase=="failed")return "Editor.ExportStage.Failed";
    return "Editor.ExportStage.Work";
}
}

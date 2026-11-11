#include "ToolLayer/Editor/ExportService.h"
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
    std::ofstream output(path,std::ios::binary|std::ios::trunc);output<<text;Check(bool(output),"write fixture");
}
std::string Read(const fs::path& path)
{
    std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"usage: ExportServiceTests <output-dir>");
        auto output=fs::absolute(fs::path(argv[1]));
        std::error_code cleanup;fs::remove_all(output,cleanup);fs::create_directories(output);
        const auto manifest=output/"project.json";
        Put(manifest,"{\"format\":\"SelfEngine.Project\",\"version\":1}");
        const auto evidence=output/"evidence";
        Put(output/"packages"/"Game"/"package-manifest.json","{}");
        std::vector<std::string> captured;std::vector<std::string> phases;int calls=0;
        const auto runner=[&](const std::vector<std::string>& arguments,std::string&,const ExportService::Progress& progress){
            captured=arguments;++calls;
            if(progress)progress("content_validation");
            Put(evidence/"export-report.json","{\"schema\":1,\"success\":true,\"phase\":\"complete\",\"output\":\""+ (output/"packages"/"Game").generic_string()+"\"}");
            return 0;
        };
        ExportRequest request{manifest,"Game",evidence};
        auto result=ExportService::Run(request,runner,[&](const std::string& phase){phases.push_back(phase);});
        Check(result.success&&calls==1&&captured.size()==6,"export runs once with evidence arguments");
        Check(captured[0]=="-Project"&&captured[2]=="-Name"&&captured[3]=="Game"&&captured[4]=="-EvidenceDirectory","export argument order");
        Check(!phases.empty()&&phases.front()=="input_validation","progress reports input validation");
        Check(result.report.valid&&result.report.success&&result.report.phase=="complete","export report parsed");
        Check(result.outputPackage==output/"packages"/"Game","report output package exposed");
        Check(!ExportService::ReadReport(output/"missing").valid,"missing report is invalid");
        Check(std::string(ExportService::StageKey("window_smoke"))=="Editor.ExportStage.Verify","stage mapping");
        Check(std::string(ExportService::StageKey("unknown-phase"))=="Editor.ExportStage.Work","unknown stage mapping");
        calls=0;
        ExportRequest missing{output/"missing.json","Game",{}};
        result=ExportService::Run(missing,runner);
        Check(!result.success&&result.errorKey=="Project.ManifestNotFound"&&calls==0&&result.stage=="validate","missing manifest rejected before runner");
        ExportRequest badName{manifest,"bad/name",{}};
        result=ExportService::Run(badName,runner);
        Check(!result.success&&result.errorKey=="Project.InvalidName"&&calls==0,"invalid name rejected");
        const auto failing=[&](const std::vector<std::string>&,std::string& capturedOutput,const ExportService::Progress&){
            capturedOutput="step failed";
            Put(evidence/"export-report.json","{\"schema\":1,\"success\":false,\"phase\":\"failed\",\"failed_phase\":\"headless_smoke\",\"error\":\"Export.StepFailed detail\"}");
            return 5;
        };
        result=ExportService::Run(request,failing);
        Check(!result.success&&result.errorKey=="Export.StepFailed"&&result.exitCode==5,"failed export reports exit code");
        Check(result.stage=="Editor.ExportStage.Verify","failed export maps failed phase to stage");
        Check(result.detail=="Export.StepFailed detail","failed export keeps script error detail");
        result=ExportService::Run(request,[&](const auto&,std::string&,const auto&){Put(evidence/"export-report.json","{\"schema\":1,\"success\":false,\"phase\":\"failed\"}");return 0;});
        Check(!result.success,"zero exit cannot override failed report");
        result=ExportService::Run(request,[&](const auto&,std::string&,const auto&){fs::remove(evidence/"export-report.json");return 0;});
        Check(!result.success,"zero exit without report is not success");
        const auto stage=output/"out"/"packages"/".Game.stage-12345678123456781234567812345678";
        Put(stage/"candidate.txt","temporary");ExportReport cancelled;cancelled.staging=stage.generic_string();cancelled.outputRoot=stage.parent_path().generic_string();std::string cleanupError;
        Check(ExportService::CleanupCancelledStage(request,cancelled,cleanupError)&&!fs::exists(stage),"cancelled owned candidate removed");
        cancelled.staging=(output/"packages"/"Game").generic_string();cancelled.outputRoot=(output/"packages").generic_string();
        Check(!ExportService::CleanupCancelledStage(request,cancelled,cleanupError)&&fs::exists(output/"packages"/"Game"),"published package protected from cancellation cleanup");
        std::cout<<"ExportServiceTests passed (validation, evidence arguments, report parsing, stage mapping, runner failure).\n";
        return 0;
    }
    catch(const std::exception& failure)
    {
        std::cerr<<"ExportServiceTests failed: "<<failure.what()<<'\n';return 1;
    }
}

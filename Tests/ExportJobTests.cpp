#include "ToolLayer/Editor/ExportJob.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace fs=std::filesystem;
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Put(const fs::path& path,const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path,std::ios::binary|std::ios::trunc);output<<text;
}
std::string Read(const fs::path& path)
{
    std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};
}
ExportJobSnapshot WaitFinished(ExportJob& job,int timeoutMilliseconds=5000)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMilliseconds);
    while(std::chrono::steady_clock::now()<deadline)
    {
        auto snapshot=job.Snapshot();
        if(snapshot.finished)return snapshot;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("export job timeout");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"usage: ExportJobTests <output-dir>");
        auto output=fs::absolute(fs::path(argv[1]));
        std::error_code cleanup;fs::remove_all(output,cleanup);fs::create_directories(output);
        const auto manifest=output/"project.json";
        Put(manifest,"{\"format\":\"SelfEngine.Project\",\"version\":1}");
        const auto evidence=output/"evidence-success";
        Put(output/"packages"/"Game"/"package-manifest.json","{}");
        const auto successRunner=[&](const std::vector<std::string>&,std::string&,const ExportService::Progress& progress){
            if(progress)progress("content_validation");
            Put(evidence/"export-report.json","{\"schema\":1,\"success\":true,\"phase\":\"complete\",\"output\":\""+ (output/"packages"/"Game").generic_string()+"\"}");
            return 0;
        };
        ExportJob job;std::string error;
        ExportJobRequest request{manifest,"Game",evidence,7};
        Check(job.Start(request,successRunner,error)&&job.Running(),"job starts");
        Check(!job.Start(request,successRunner,error)&&error=="Editor.ExportBusy","second start rejected while running");
        auto snapshot=WaitFinished(job);
        Check(snapshot.report.state==ExportOperationState::Succeeded&&!snapshot.cancelled,"job succeeds");
        Check(snapshot.report.workspaceGeneration==7&&snapshot.report.operationId>0,"job keeps identity and generation");
        Check(snapshot.report.stage=="Editor.ExportStage.Done","job reports done stage");
        Check(snapshot.outputPackage==output/"packages"/"Game","job exposes output package");
        Check(snapshot.reportPath==evidence/"export-report.json","job exposes report path");
        Check(Read(evidence/"editor-export.log").empty(),"log path reserved");
        // 失败作业
        const auto failEvidence=output/"evidence-fail";
        const auto failRunner=[&](const std::vector<std::string>&,std::string&,const ExportService::Progress&){
            Put(failEvidence/"export-report.json","{\"schema\":1,\"success\":false,\"phase\":\"failed\",\"error\":\"boom\"}");
            return 4;
        };
        ExportJobRequest failRequest{manifest,"GameFail",failEvidence,8};
        Check(job.Start(failRequest,failRunner,error),"failed job starts");
        snapshot=WaitFinished(job);
        Check(snapshot.report.state==ExportOperationState::Failed&&snapshot.report.exitCode==4,"job reports failure");
        Check(snapshot.report.diagnosticKey=="Export.StepFailed","job reports diagnostic key");
        Check(snapshot.detail=="boom","job keeps failure detail");
        // 取消作业：runner 检查作业的取消标志，模拟真实子进程轮询。
        const auto cancelEvidence=output/"evidence-cancel";
        std::atomic<bool> cancelObserved{false};
        const auto cancelRunner=[&](const std::vector<std::string>&,std::string&,const ExportService::Progress&){
            for(int index=0;index<200;++index)
            {
                if(job.CancelFlag().load()){cancelObserved.store(true);return 1;}
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return 0;
        };
        ExportJobRequest cancelRequest{manifest,"GameCancel",cancelEvidence,9};
        Check(job.Start(cancelRequest,cancelRunner,error),"cancellable job starts");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        job.Cancel();
        snapshot=WaitFinished(job);
        Check(cancelObserved.load(),"runner observes cancel flag");
        Check(snapshot.report.state==ExportOperationState::Cancelled&&snapshot.cancelled,"job reports cancellation");
        Check(snapshot.report.diagnosticKey=="Editor.ExportCancelled","cancellation diagnostic");
        job.Join();
        // 上次报告持久化
        const auto persistence=output/"persistence";
        Check(SaveExportSnapshot(persistence,snapshot,error),"export report saved");
        ExportJobSnapshot loaded;
        Check(LoadExportSnapshot(persistence,loaded,error)&&loaded.finished,"export report loaded");
        Check(loaded.report.state==ExportOperationState::Cancelled&&loaded.report.operationId==snapshot.report.operationId,"export report round trip");
        Check(LoadExportSnapshot(output/"empty",loaded,error)&&!loaded.finished,"missing report is not fabricated");
        std::cout<<"ExportJobTests passed (success, failure, cancellation, generation and report persistence).\n";
        return 0;
    }
    catch(const std::exception& failure)
    {
        std::cerr<<"ExportJobTests failed: "<<failure.what()<<'\n';return 1;
    }
}

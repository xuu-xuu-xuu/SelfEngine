#include "ExportJob.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <fstream>
#include <iterator>

namespace se
{
namespace
{
using Json=nlohmann::json;
bool WriteAtomic(const std::filesystem::path& path,const std::string& text)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(),error);
    if(error)return false;
    const auto temporary=path.parent_path()/(path.filename().string()+".tmp");
    {
        std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
        if(!output)return false;
        output.write(text.data(),static_cast<std::streamsize>(text.size()));
        if(!output)return false;
    }
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
    {
        std::filesystem::remove(temporary,error);return false;
    }
    return true;
}
}
const char* ExportOperationStateId(ExportOperationState state)
{
    switch(state)
    {
    case ExportOperationState::Queued:return "queued";
    case ExportOperationState::Running:return "running";
    case ExportOperationState::Cancelling:return "cancelling";
    case ExportOperationState::Succeeded:return "succeeded";
    case ExportOperationState::Failed:return "failed";
    case ExportOperationState::Cancelled:return "cancelled";
    }
    return "queued";
}
bool ParseExportOperationState(const std::string& id,ExportOperationState& output)
{
    if(id=="queued")output=ExportOperationState::Queued;
    else if(id=="running")output=ExportOperationState::Running;
    else if(id=="cancelling")output=ExportOperationState::Cancelling;
    else if(id=="succeeded")output=ExportOperationState::Succeeded;
    else if(id=="failed")output=ExportOperationState::Failed;
    else if(id=="cancelled")output=ExportOperationState::Cancelled;
    else return false;
    return true;
}
ExportJob::~ExportJob()
{
    Cancel();
    Join();
}
bool ExportJob::Start(const ExportJobRequest& request,const ExportService::Runner& runner,std::string& errorKey)
{
    {
        std::lock_guard lock(mutex_);
        if(running_){errorKey="Editor.ExportBusy";return false;}
    }
    Join();
    {
        std::lock_guard lock(mutex_);
        if(!operation_.Queue(request.workspaceGeneration,errorKey))return false;
        if(!operation_.Start(errorKey))return false;
        cancel_=false;
        snapshot_={};
        snapshot_.report=operation_.Report();
        snapshot_.reportPath=request.evidenceDirectory/"export-report.json";
        snapshot_.logPath=request.evidenceDirectory/"editor-export.log";
        snapshot_.finished=false;
        running_=true;
    }
    const auto evidence=request.evidenceDirectory;
    const auto manifest=request.projectManifest;
    const auto name=request.name;
    thread_=std::thread([this,evidence,manifest,name,runner]{
        const auto progress=[this](const std::string& phase){
            std::lock_guard lock(mutex_);
            std::string error;
            if(operation_.Report().state==ExportOperationState::Running)operation_.Stage(ExportService::StageKey(phase),error);
            snapshot_.report=operation_.Report();
        };
        const ExportRequest exportRequest{manifest,name,evidence};
        ExportResult result;
        try{result=ExportService::Run(exportRequest,runner,progress);}
        catch(const std::exception& failure)
        {
            result.errorKey="Export.StepFailed";
            result.detail=failure.what();
            result.exitCode=-1;
        }
        std::lock_guard lock(mutex_);
        auto& report=snapshot_.report;
        const auto logPath=(evidence/"editor-export.log").string();
        if(cancel_.load())
        {
            std::string error;
            if(!ExportService::CleanupCancelledStage(exportRequest,result.report,error))snapshot_.detail=error;
            if(report.state==ExportOperationState::Running)operation_.Cancel(error);
            if(operation_.Report().state==ExportOperationState::Cancelling)operation_.Cancelled(error);
            snapshot_.cancelled=true;
            snapshot_.outputPackage.clear();
        }
        else if(result.success)
        {
            std::string error;
            operation_.Stage("Editor.ExportStage.Done",error);
            operation_.Succeed(error);
            snapshot_.outputPackage=result.outputPackage;
        }
        else
        {
            std::string error;
            operation_.Fail(result.errorKey,result.exitCode,logPath,error);
            snapshot_.detail=result.detail;
        }
        report.stage=result.success?"Editor.ExportStage.Done":result.stage.empty()?"Editor.ExportStage.Failed":result.stage;
        snapshot_.report=operation_.Report();
        snapshot_.report.logPath=logPath;
        if(cancel_.load())
        {
            snapshot_.report.diagnosticKey="Editor.ExportCancelled";
            snapshot_.report.stage="Editor.ExportStage.Cancelled";
        }
        snapshot_.finished=true;
        running_=false;
    });
    return true;
}
void ExportJob::Cancel()
{
    cancel_=true;
    std::lock_guard lock(mutex_);
    if(operation_.Report().state==ExportOperationState::Running)
    {
        std::string error;
        operation_.Cancel(error);
        snapshot_.report=operation_.Report();
    }
}
void ExportJob::Join()
{
    if(thread_.joinable())thread_.join();
}
bool ExportJob::Running() const
{
    std::lock_guard lock(mutex_);
    return running_;
}
ExportJobSnapshot ExportJob::Snapshot() const
{
    std::lock_guard lock(mutex_);
    return snapshot_;
}
bool SaveExportSnapshot(const std::filesystem::path& directory,const ExportJobSnapshot& snapshot,std::string& error)
{
    error.clear();
    const Json json{{"schema",1},{"operation_id",snapshot.report.operationId},
        {"workspace_generation",snapshot.report.workspaceGeneration},
        {"state",ExportOperationStateId(snapshot.report.state)},
        {"stage",snapshot.report.stage},{"diagnostic_key",snapshot.report.diagnosticKey},
        {"exit_code",snapshot.report.exitCode},{"log_path",snapshot.report.logPath},
        {"detail",snapshot.detail},
        {"output_package",snapshot.outputPackage.string()},
        {"report_path",snapshot.reportPath.string()},
        {"cancelled",snapshot.cancelled}};
    if(!WriteAtomic(directory/"last-export.json",json.dump(2)+"\n")){error="cannot write export report";return false;}
    return true;
}
bool LoadExportSnapshot(const std::filesystem::path& directory,ExportJobSnapshot& output,std::string& error)
{
    error.clear();output={};
    std::error_code fileError;
    if(!std::filesystem::exists(directory/"last-export.json",fileError))return true;
    try
    {
        std::ifstream input(directory/"last-export.json",std::ios::binary);
        const auto json=Json::parse(std::string{std::istreambuf_iterator<char>(input),{}});
        if(json.at("schema")!=1){error="unsupported export report schema";return false;}
        ExportJobSnapshot loaded;
        loaded.report.operationId=json.value("operation_id",std::uint64_t{0});
        loaded.report.workspaceGeneration=json.value("workspace_generation",std::uint64_t{0});
        if(!ParseExportOperationState(json.value("state",std::string{}),loaded.report.state)){error="invalid export report state";return false;}
        loaded.report.stage=json.value("stage",std::string{});
        loaded.report.diagnosticKey=json.value("diagnostic_key",std::string{});
        loaded.report.exitCode=json.value("exit_code",0);
        loaded.report.logPath=json.value("log_path",std::string{});
        loaded.detail=json.value("detail",std::string{});
        loaded.outputPackage=json.value("output_package",std::string{});
        loaded.reportPath=json.value("report_path",std::string{});
        loaded.cancelled=json.value("cancelled",false);
        loaded.finished=true;
        output=std::move(loaded);
        return true;
    }
    catch(const std::exception& failure){error=failure.what();return false;}
}
}

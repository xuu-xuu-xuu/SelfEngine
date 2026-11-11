#include "ExportOperation.h"

namespace se
{
bool ExportOperationController::FailState(std::string& errorKey,const char* key)
{
    errorKey=key;return false;
}
bool ExportOperationController::Queue(std::uint64_t workspaceGeneration,std::string& errorKey)
{
    errorKey.clear();
    if(report_.operationId!=0&&(report_.state==ExportOperationState::Queued||report_.state==ExportOperationState::Running||report_.state==ExportOperationState::Cancelling))
        return FailState(errorKey,"Editor.ExportBusy");
    report_={};report_.operationId=nextId_++;report_.workspaceGeneration=workspaceGeneration;report_.state=ExportOperationState::Queued;
    return true;
}
bool ExportOperationController::Start(std::string& errorKey)
{
    errorKey.clear();
    if(report_.state!=ExportOperationState::Queued)return FailState(errorKey,"Editor.ExportInvalidState");
    report_.state=ExportOperationState::Running;return true;
}
bool ExportOperationController::Stage(const std::string& stage,std::string& errorKey)
{
    errorKey.clear();
    if(report_.state!=ExportOperationState::Running)return FailState(errorKey,"Editor.ExportInvalidState");
    report_.stage=stage;return true;
}
bool ExportOperationController::Succeed(std::string& errorKey)
{
    errorKey.clear();
    if(report_.state!=ExportOperationState::Running)return FailState(errorKey,"Editor.ExportInvalidState");
    report_.state=ExportOperationState::Succeeded;return true;
}
bool ExportOperationController::Fail(const std::string& diagnosticKey,int exitCode,const std::string& logPath,std::string& errorKey)
{
    errorKey.clear();
    if(report_.state!=ExportOperationState::Running&&report_.state!=ExportOperationState::Cancelling)
        return FailState(errorKey,"Editor.ExportInvalidState");
    report_.state=ExportOperationState::Failed;report_.diagnosticKey=diagnosticKey;report_.exitCode=exitCode;report_.logPath=logPath;
    return true;
}
bool ExportOperationController::Cancel(std::string& errorKey)
{
    errorKey.clear();
    if(report_.state!=ExportOperationState::Running)return FailState(errorKey,"Editor.ExportInvalidState");
    report_.state=ExportOperationState::Cancelling;return true;
}
bool ExportOperationController::Cancelled(std::string& errorKey)
{
    errorKey.clear();
    if(report_.state!=ExportOperationState::Cancelling)return FailState(errorKey,"Editor.ExportInvalidState");
    report_.state=ExportOperationState::Cancelled;return true;
}
bool ExportOperationController::Stale(std::uint64_t workspaceGeneration) const
{
    return report_.workspaceGeneration!=workspaceGeneration;
}
void ExportOperationController::Reset() noexcept
{
    report_={};
}
}

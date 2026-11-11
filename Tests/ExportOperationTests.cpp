#include "ToolLayer/Editor/ExportOperation.h"
#include <iostream>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
}
int wmain()
{
    try
    {
        ExportOperationController operation;std::string error;
        Check(operation.Queue(7,error)&&operation.Report().state==ExportOperationState::Queued,"queue accepted");
        Check(operation.Report().operationId>0&&operation.Report().workspaceGeneration==7,"report identity");
        Check(!operation.Queue(7,error)&&error=="Editor.ExportBusy","second queue rejected while pending");
        Check(operation.Start(error)&&operation.Report().state==ExportOperationState::Running,"start running");
        Check(operation.Stage("validate",error)&&operation.Report().stage=="validate","stage reported");
        Check(!operation.Start(error)&&error=="Editor.ExportInvalidState","double start rejected");
        Check(!operation.Stale(7)&&operation.Stale(8),"workspace generation check");
        Check(operation.Cancel(error)&&operation.Report().state==ExportOperationState::Cancelling,"cancel enters cancelling");
        Check(!operation.Succeed(error)&&error=="Editor.ExportInvalidState","cannot succeed while cancelling");
        Check(operation.Fail("Export.StepFailed",5,"log.txt",error)&&operation.Report().state==ExportOperationState::Failed
            &&operation.Report().exitCode==5&&operation.Report().logPath=="log.txt","failure report");
        Check(operation.Queue(9,error)&&operation.Start(error)&&operation.Succeed(error)&&operation.Report().state==ExportOperationState::Succeeded,"clean success after failure");
        operation.Reset();Check(operation.Report().state==ExportOperationState::Queued,"reset clears report");
        std::cout<<"ExportOperationTests passed (queue/run/stage/cancel/fail and generation guard).\n";
        return 0;
    }
    catch(const std::exception& failure)
    {
        std::cerr<<"ExportOperationTests failed: "<<failure.what()<<'\n';return 1;
    }
}

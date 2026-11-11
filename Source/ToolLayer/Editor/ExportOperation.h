#pragma once
#include <cstdint>
#include <string>

namespace se
{
enum class ExportOperationState { Queued, Running, Cancelling, Succeeded, Failed, Cancelled };
struct ExportOperationReport
{
    std::uint64_t operationId=0,workspaceGeneration=0;
    ExportOperationState state=ExportOperationState::Queued;
    std::string stage,diagnosticKey,logPath;
    int exitCode=0;
};
// 导出/构建操作状态机：Queued -> Running -> (Succeeded|Failed|Cancelling->Cancelled)。
// 工作区代数用于拒绝过期回调；UI 只消费报告，不直接调用打包实现。
class ExportOperationController
{
public:
    bool Queue(std::uint64_t workspaceGeneration,std::string& errorKey);
    bool Start(std::string& errorKey);
    bool Stage(const std::string& stage,std::string& errorKey);
    bool Succeed(std::string& errorKey);
    bool Fail(const std::string& diagnosticKey,int exitCode,const std::string& logPath,std::string& errorKey);
    bool Cancel(std::string& errorKey);
    bool Cancelled(std::string& errorKey);
    bool Stale(std::uint64_t workspaceGeneration) const;
    const ExportOperationReport& Report() const noexcept{return report_;}
    void Reset() noexcept;
private:
    bool FailState(std::string& errorKey,const char* key);
    ExportOperationReport report_;
    std::uint64_t nextId_=1;
};
}

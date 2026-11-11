#pragma once
#include "ExportOperation.h"
#include "ExportService.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace se
{
struct ExportJobRequest
{
    std::filesystem::path projectManifest;
    std::string name;
    std::filesystem::path evidenceDirectory;
    std::uint64_t workspaceGeneration=0;
};
struct ExportJobSnapshot
{
    ExportOperationReport report;
    std::string detail;
    std::filesystem::path outputPackage,reportPath,logPath;
    bool finished=false;
    bool cancelled=false;
};
// 后台导出作业：UI 线程只读快照；工作线程只更新本作业状态，不触碰 World/ImGui。
class ExportJob
{
public:
    ExportJob()=default;
    ~ExportJob();
    ExportJob(const ExportJob&)=delete;
    ExportJob& operator=(const ExportJob&)=delete;
    bool Start(const ExportJobRequest& request,const ExportService::Runner& runner,std::string& errorKey);
    void Cancel();
    void Join();
    bool Running() const;
    ExportJobSnapshot Snapshot() const;
    // 仅供 runner 在子进程等待期间读取；作业对象生命周期内地址稳定。
    const std::atomic<bool>& CancelFlag() const noexcept{return cancel_;}
private:
    mutable std::mutex mutex_;
    std::thread thread_;
    std::atomic<bool> cancel_{false};
    ExportOperationController operation_;
    ExportJobSnapshot snapshot_;
    bool running_=false;
};
// 最近一次导出报告：保存在用户数据目录，关闭/重开编辑器后仍可查看。
bool SaveExportSnapshot(const std::filesystem::path& directory,const ExportJobSnapshot& snapshot,std::string& error);
bool LoadExportSnapshot(const std::filesystem::path& directory,ExportJobSnapshot& output,std::string& error);
const char* ExportOperationStateId(ExportOperationState state);
bool ParseExportOperationState(const std::string& id,ExportOperationState& output);
}

#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace se
{
struct ExportRequest
{
    std::filesystem::path projectManifest;
    std::string name;
    // 编辑器提供的证据目录；脚本把 report/日志写入这里，界面据此显示阶段。
    std::filesystem::path evidenceDirectory;
};
struct ExportReport
{
    bool valid=false;
    bool success=false;
    std::string phase,failedPhase,error,output,staging,outputRoot;
};
struct ExportResult
{
    bool success=false;
    std::string stage,errorKey,detail;
    int exitCode=0;
    ExportReport report;
    std::filesystem::path outputPackage;
};
// 编辑器导出入口：校验输入并调用 T65 的 Export-Game 服务；不复制打包算法。
class ExportService
{
public:
    // progress 报告脚本当前阶段（原始 phase 名）；取消由 runner 内部处理。
    using Progress=std::function<void(const std::string& phase)>;
    using Runner=std::function<int(const std::vector<std::string>& arguments,std::string& output,const Progress& progress)>;
    static std::vector<std::string> Arguments(const ExportRequest& request);
    static ExportResult Run(const ExportRequest& request,const Runner& runner,const Progress& progress={});
    static ExportReport ReadReport(const std::filesystem::path& evidenceDirectory);
    static bool CleanupCancelledStage(const ExportRequest&,const ExportReport&,std::string& error);
    // 把脚本 phase 映射为稳定文本键，未知阶段显示“进行中”。
    static const char* StageKey(const std::string& phase);
};
}

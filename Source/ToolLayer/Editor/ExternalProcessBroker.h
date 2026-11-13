#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace se
{
// 监督者在内部任务 Job 外处理用户显式启动；每次运行使用独立目录和令牌。
class ExternalProcessBroker
{
public:
    explicit ExternalProcessBroker(const std::filesystem::path& directory);
    ~ExternalProcessBroker();
    bool Ready() const;
    std::vector<wchar_t> Environment() const;
    void Pump(bool allowLaunch=true) noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
bool LaunchExternalApplication(const std::wstring& executable,const std::vector<std::wstring>& arguments,
    const std::filesystem::path& workingDirectory,bool showWindow,bool association,std::string& error);
}

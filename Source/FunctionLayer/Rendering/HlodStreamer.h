#pragma once
#include "ResourceLayer/Assets/HlodBake.h"
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace se
{
// G05.13：`.sehlod` 后台读取。工作线程只做文件解析并发布一次结果；
// 呈现线程通过 Poll 取得场景，失败与取消都不会阻塞渲染。
class HlodStreamer
{
public:
    enum class State { Pending, Ready, Failed, Idle };
    ~HlodStreamer();
    HlodStreamer()=default;
    HlodStreamer(const HlodStreamer&)=delete;
    HlodStreamer& operator=(const HlodStreamer&)=delete;
    // 已有任务时先取消并加入旧线程；空路径直接失败。delayMs 仅用于验证“读取未完成时的实例回退”。
    bool Start(const std::filesystem::path& path,std::string& error,std::uint32_t delayMs=0);
    State Poll(HlodScene& scene,std::string& error);
    bool Running() const noexcept;
    // 不等待读取完成也会返回；析构/Start 会 join 工作线程，保证不留下并发文件访问。
    void Cancel();
private:
    void Join();
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::filesystem::path path_;
    HlodScene scene_;
    std::string error_;
    bool running_=false,finished_=false,delivered_=false;
};
}

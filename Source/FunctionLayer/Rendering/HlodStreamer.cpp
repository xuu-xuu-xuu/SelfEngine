#include "HlodStreamer.h"
#include <chrono>
#include <utility>

namespace se
{
HlodStreamer::~HlodStreamer()
{
    Cancel();
}
void HlodStreamer::Join()
{
    if(worker_.joinable())worker_.join();
}
bool HlodStreamer::Start(const std::filesystem::path& path,std::string& error,std::uint32_t delayMs)
{
    Cancel();
    if(path.empty()){error="Hlod.StreamPathEmpty";return false;}
    {
        std::lock_guard lock(mutex_);
        path_=path;scene_=HlodScene{};error_.clear();
        running_=true;finished_=false;delivered_=false;
    }
    worker_=std::thread([this,path,delayMs]
    {
        if(delayMs)std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        HlodScene scene;std::string failure;
        const bool ok=ReadHlodScene(path,scene,failure);
        {
            std::lock_guard lock(mutex_);
            if(ok)scene_=std::move(scene);else error_=failure.empty()?"Assets.ReadFailed":failure;
            finished_=true;
        }
        wake_.notify_all();
    });
    return true;
}
HlodStreamer::State HlodStreamer::Poll(HlodScene& scene,std::string& error)
{
    std::unique_lock lock(mutex_);
    if(!running_)return State::Idle;
    if(!finished_)return State::Pending;
    if(delivered_)return State::Idle;
    delivered_=true;
    const bool failed=!error_.empty();
    if(failed)error=error_;else scene=std::move(scene_);
    lock.unlock();
    Join();
    {
        std::lock_guard done(mutex_);
        running_=false;
    }
    return failed?State::Failed:State::Ready;
}
bool HlodStreamer::Running() const noexcept
{
    std::lock_guard lock(mutex_);
    return running_;
}
void HlodStreamer::Cancel()
{
    {
        std::lock_guard lock(mutex_);
        if(!running_&&!worker_.joinable())return;
    }
    // 读取无法中断；join 后丢弃结果，保证下一次 Start 不与旧任务竞争。
    Join();
    std::lock_guard lock(mutex_);
    running_=false;finished_=false;delivered_=false;
    scene_=HlodScene{};error_.clear();
}
}

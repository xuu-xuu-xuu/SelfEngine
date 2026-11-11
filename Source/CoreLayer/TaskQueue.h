#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace se
{
namespace detail { struct TaskState; }
enum class TaskStatus { Rejected, Queued, Running, Ready, Publishing, Completed, Cancelled, Failed };
struct TaskErrors
{
    std::exception_ptr work, completion, handler;
    bool Any() const noexcept { return work || completion || handler; }
};
class CancellationToken
{
public:
    CancellationToken() = default;
    bool IsCancelled() const noexcept;
    void Cancel() const noexcept;
    bool Accepted() const noexcept;
    TaskStatus Status() const noexcept;
    TaskErrors Errors() const;
private:
    explicit CancellationToken(std::shared_ptr<detail::TaskState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::TaskState> state_;
    friend class TaskQueue;
};

// 后台任务只生成 CPU 结果；完成函数由主线程显式 Pump，不能从工作线程发布世界变更。
class TaskQueue
{
public:
    using Completion = std::function<void()>;
    using Work = std::function<Completion(const CancellationToken&)>;
    using Failure = std::function<void(const TaskErrors&)>;
    explicit TaskQueue(std::size_t workerCount = 2, std::size_t capacity = 1024);
    ~TaskQueue();
    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;
    // 容量包含排队、执行、待发布及正在发布；拒绝立即返回，不阻塞生产者。
    CancellationToken Submit(Work work, Failure failure = {});
    std::size_t Pump(std::size_t limit = 64);
    // 所有者线程可有界休眠至有结果/已空闲/关闭；不在等待期间执行回调。
    bool WaitForReady(std::chrono::milliseconds timeout);
    void Shutdown(); // 仅所有者线程：取消任务，等待协作退出，丢弃尚未发布的回调。
    std::size_t Pending() const;
    std::size_t ReadyCount()const;
    std::size_t Capacity() const noexcept { return capacity_; }
    std::size_t PeakPending() const;
    std::size_t Rejected() const;
    std::size_t Failed() const noexcept { return failed_.load(); }
private:
    void StopWorkers(); // 析构可由静止宿主的终结器执行；只取消/等待，不发布完成回调。
    struct Job { CancellationToken token; Work work; Failure failure; };
    struct Ready { CancellationToken token; Completion completion; Failure failure; };
    void Worker();
    const std::thread::id owner_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable changed_;
    std::deque<Job> jobs_;
    std::deque<Ready> ready_;
    std::vector<CancellationToken> active_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    bool pumping_ = false; // 只由所有者线程访问；先核对线程再读取。
    std::size_t pending_ = 0, peak_ = 0, rejected_ = 0;
    std::atomic_size_t failed_ = 0;
};
}

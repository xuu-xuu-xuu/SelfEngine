#include "TaskQueue.h"
#include "Log.h"
#include <algorithm>
#include <stdexcept>

namespace se
{
namespace detail
{
struct TaskState
{
    std::atomic_bool cancelled = false;
    std::atomic<TaskStatus> status = TaskStatus::Queued;
    mutable std::mutex mutex;
    TaskErrors errors;
};
}
bool CancellationToken::IsCancelled() const noexcept { return !state_ || state_->cancelled.load(); }
void CancellationToken::Cancel() const noexcept
{
    if (!state_) return;
    const auto status = state_->status.load();
    if (status != TaskStatus::Completed && status != TaskStatus::Failed && status != TaskStatus::Rejected)
        state_->cancelled.store(true);
}
bool CancellationToken::Accepted() const noexcept { return state_ && Status() != TaskStatus::Rejected; }
TaskStatus CancellationToken::Status() const noexcept { return state_ ? state_->status.load() : TaskStatus::Rejected; }
TaskErrors CancellationToken::Errors() const
{
    if (!state_) return {};
    std::lock_guard lock(state_->mutex); return state_->errors;
}

TaskQueue::TaskQueue(std::size_t workerCount, std::size_t capacity)
    : owner_(std::this_thread::get_id()), capacity_(capacity)
{
    if (workerCount == 0 || workerCount > 16) throw std::invalid_argument("Tasks.InvalidWorkerCount");
    if (capacity == 0 || capacity > 65536) throw std::invalid_argument("Tasks.InvalidCapacity");
    workers_.reserve(workerCount); active_.reserve(workerCount);
    try { for (std::size_t i = 0; i < workerCount; ++i) workers_.emplace_back([this] { Worker(); }); }
    catch (...) { Shutdown(); throw; }
}
TaskQueue::~TaskQueue() { if(pumping_)std::terminate();StopWorkers(); }

CancellationToken TaskQueue::Submit(Work work, Failure failure)
{
    CancellationToken token(std::make_shared<detail::TaskState>());
    Job job{token, std::move(work), std::move(failure)};
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || !job.work || pending_ == capacity_)
        {
            token.state_->cancelled.store(true); token.state_->status.store(TaskStatus::Rejected);
            ++rejected_; return token;
        }
        jobs_.push_back(std::move(job));
        ++pending_; peak_ = (std::max)(peak_, pending_);
    }
    wake_.notify_one();
    return token;
}

void TaskQueue::Worker()
{
    while (true)
    {
        Job job{ CancellationToken(nullptr), {}, {} };
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            active_.push_back(job.token);
            if (!job.token.IsCancelled()) job.token.state_->status.store(TaskStatus::Running);
        }
        Completion completion;
        if (!job.token.IsCancelled())
        {
            try { completion = job.work(job.token); }
            catch (...)
            {
                std::lock_guard lock(job.token.state_->mutex);
                job.token.state_->errors.work = std::current_exception(); ++failed_;
            }
        }
        Ready ready{job.token, std::move(completion), std::move(job.failure)};
        {
            std::lock_guard lock(mutex_);
            std::erase_if(active_, [&](const auto& token) { return token.state_ == job.token.state_; });
            const bool failed = job.token.Errors().Any();
            if (!stopping_ && !job.token.IsCancelled() && (ready.completion || (failed && ready.failure)))
            {
                try
                {
                    ready_.push_back(std::move(ready));
                    job.token.state_->status.store(TaskStatus::Ready);
                }
                catch (...)
                {
                    std::lock_guard stateLock(job.token.state_->mutex);
                    job.token.state_->errors.completion = std::current_exception(); ++failed_;
                    job.token.state_->status.store(TaskStatus::Failed); --pending_;
                }
            }
            else
            {
                job.token.state_->status.store(failed ? TaskStatus::Failed :
                    (stopping_ || job.token.IsCancelled() ? TaskStatus::Cancelled : TaskStatus::Completed));
                --pending_;
            }
        }
        changed_.notify_all();
    }
}

std::size_t TaskQueue::Pump(std::size_t limit)
{
    if (std::this_thread::get_id() != owner_) throw std::logic_error("Tasks.WrongPublishThread");
    if (pumping_) throw std::logic_error("Tasks.ReentrantPublish");
    pumping_ = true;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{pumping_};
    std::size_t published = 0;
    for (std::size_t i = 0; i < limit; ++i)
    {
        Ready ready{ CancellationToken(nullptr), {}, {} };
        bool cancelled = false;
        {
            std::lock_guard lock(mutex_);
            if (ready_.empty() || stopping_) break;
            ready = std::move(ready_.front());
            ready_.pop_front();
            cancelled = ready.token.IsCancelled();
            if (cancelled)
            {
                ready.token.state_->status.store(ready.token.Errors().Any() ? TaskStatus::Failed : TaskStatus::Cancelled);
                --pending_;
            }
            else ready.token.state_->status.store(TaskStatus::Publishing);
        }
        if (cancelled) continue;
        if (!ready.token.Errors().Any())
        {
            try { ready.completion(); ++published; }
            catch (...)
            {
                std::lock_guard lock(ready.token.state_->mutex);
                ready.token.state_->errors.completion = std::current_exception(); ++failed_;
            }
        }
        const auto errors = ready.token.Errors();
        if (errors.Any() && ready.failure)
        {
            try { ready.failure(errors); }
            catch (...)
            {
                std::lock_guard lock(ready.token.state_->mutex);
                ready.token.state_->errors.handler = std::current_exception(); ++failed_;
            }
        }
        {
            std::lock_guard lock(mutex_);
            ready.token.state_->status.store(ready.token.Errors().Any() ? TaskStatus::Failed : TaskStatus::Completed);
            --pending_;
        }
    }
    return published;
}

bool TaskQueue::WaitForReady(std::chrono::milliseconds timeout)
{
    if (std::this_thread::get_id() != owner_) throw std::logic_error("Tasks.WrongPublishThread");
    if (pumping_) throw std::logic_error("Tasks.WaitDuringPublish");
    if (timeout.count() < 0) throw std::invalid_argument("Tasks.InvalidTimeout");
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, timeout, [this] { return !ready_.empty() || pending_ == 0 || stopping_; });
    return !ready_.empty();
}

void TaskQueue::Shutdown()
{
    if (std::this_thread::get_id() != owner_) throw std::logic_error("Tasks.WrongShutdownThread");
    if (pumping_) throw std::logic_error("Tasks.ShutdownDuringPublish");
    StopWorkers();
}
void TaskQueue::StopWorkers()
{
    LogCheckpoint("Tasks.Shutdown.Begin","workers="+std::to_string(workers_.size()));
    std::deque<Job> abandonedJobs;
    std::deque<Ready> abandonedReady;
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        for (const auto& job : jobs_)
        {
            job.token.Cancel(); job.token.state_->status.store(TaskStatus::Cancelled);
        }
        for (const auto& token : active_) token.Cancel();
        for (const auto& ready : ready_)
        {
            ready.token.Cancel(); ready.token.state_->status.store(ready.token.Errors().Any() ? TaskStatus::Failed : TaskStatus::Cancelled);
        }
        pending_ -= jobs_.size() + ready_.size();
        // 捕获对象的析构可能查询队列或释放其他服务，不能在队列锁内执行。
        abandonedJobs.swap(jobs_);
        abandonedReady.swap(ready_);
    }
    wake_.notify_all();
    changed_.notify_all();
    for (auto& worker : workers_) if (worker.joinable())
    {LogCheckpoint("Tasks.Worker.Join.Begin");worker.join();LogCheckpoint("Tasks.Worker.Join.End");}
    workers_.clear();
    LogCheckpoint("Tasks.Shutdown.End");
}

std::size_t TaskQueue::Pending() const
{
    std::lock_guard lock(mutex_);
    return pending_;
}
std::size_t TaskQueue::ReadyCount()const{std::lock_guard lock(mutex_);return ready_.size();}
std::size_t TaskQueue::PeakPending() const { std::lock_guard lock(mutex_); return peak_; }
std::size_t TaskQueue::Rejected() const { std::lock_guard lock(mutex_); return rejected_; }
}

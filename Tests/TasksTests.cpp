#include "CoreLayer/TaskQueue.h"
#include <array>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace se;
using namespace std::chrono_literals;
namespace
{
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Gate
{
    std::mutex mutex; std::condition_variable changed; bool open = false; std::size_t entered = 0;
    void Block()
    {
        std::unique_lock lock(mutex); ++entered; changed.notify_all();
        Check(changed.wait_for(lock, 5s, [&] { return open; }), "controlled worker gate timed out");
    }
    void Await(std::size_t count)
    {
        std::unique_lock lock(mutex);
        Check(changed.wait_for(lock, 5s, [&] { return entered >= count; }), "worker did not enter gate");
    }
    void Open() { std::lock_guard lock(mutex); open = true; changed.notify_all(); }
};
void Drain(TaskQueue& queue)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (queue.Pending())
    {
        Check(std::chrono::steady_clock::now() < deadline, "bounded completion drain timed out");
        queue.WaitForReady(500ms); queue.Pump(7);
    }
}
std::string Message(const std::exception_ptr& error)
{
    Check(bool(error), "missing original exception");
    try { std::rethrow_exception(error); } catch (const std::exception& value) { return value.what(); }
}
void BoundsAndCancellation()
{
    Gate gate; TaskQueue queue(1, 2); int published = 0;
    auto running = queue.Submit([&](const auto&) { gate.Block(); return [&] { ++published; }; });
    gate.Await(1);
    auto queued = queue.Submit([&](const auto&) { return [&] { published += 100; }; });
    Check(!queue.WaitForReady(1ms), "bounded wait times out while all work is blocked");
    auto rejected = queue.Submit([](const auto&) { return [] {}; });
    Check(queue.Capacity() == 2 && queue.Pending() == 2 && queue.PeakPending() == 2, "shared capacity includes running and queued");
    Check(!rejected.Accepted() && rejected.IsCancelled() && rejected.Status() == TaskStatus::Rejected && queue.Rejected() == 1, "saturation is explicit rejection");
    queued.Cancel(); running.Cancel(); gate.Open(); Drain(queue);
    Check(published == 0 && running.Status() == TaskStatus::Cancelled && queued.Status() == TaskStatus::Cancelled, "running and queued cancellation");
    auto reused = queue.Submit([&](const auto&) { return [&] { ++published; }; });
    Drain(queue); Check(reused.Status() == TaskStatus::Completed && published == 1, "capacity returns after terminal tasks");
    reused.Cancel(); Check(!reused.IsCancelled(), "late cancellation does not undo completed publication");

    TaskQueue readyQueue(1, 1);
    auto ready = readyQueue.Submit([&](const auto&) { return [&] { published += 100; }; });
    Check(readyQueue.WaitForReady(5s) && ready.Status() == TaskStatus::Ready, "ready notification without publication");
    Check(!readyQueue.Submit([](const auto&) { return [] {}; }).Accepted() && readyQueue.Pending() == 1, "ready owns its bounded slot");
    ready.Cancel(); Check(readyQueue.Pump() == 0 && readyQueue.Pending() == 0 && published == 1, "cancel before publication drops callback");
    Check(ready.Status() == TaskStatus::Cancelled && !readyQueue.WaitForReady(0ms), "idle wait returns immediately");
    readyQueue.Shutdown(); Check(!readyQueue.Submit([](const auto&) { return [] {}; }).Accepted(), "closed queue rejects work");
}
void PublicationAndErrors()
{
    TaskQueue queue(2, 8); const auto owner = std::this_thread::get_id(); int handlers = 0;
    auto work = queue.Submit([](const auto&) -> TaskQueue::Completion { throw std::runtime_error("work-original"); }, [&](const TaskErrors& errors) {
        Check(std::this_thread::get_id() == owner && Message(errors.work) == "work-original", "work error arrives on owner"); ++handlers;
    });
    auto completion = queue.Submit([](const auto&) { return [] { throw std::logic_error("completion-original"); }; }, [&](const TaskErrors& errors) {
        Check(std::this_thread::get_id() == owner && Message(errors.completion) == "completion-original", "completion error preserved"); ++handlers;
    });
    auto handler = queue.Submit([](const auto&) -> TaskQueue::Completion { throw std::runtime_error("before-handler"); }, [&](const TaskErrors&) {
        ++handlers; throw std::range_error("handler-original");
    });
    auto unhandled = queue.Submit([](const auto&) -> TaskQueue::Completion { throw std::runtime_error("without-handler"); });
    auto empty = queue.Submit([](const auto&) { return TaskQueue::Completion{}; });
    Drain(queue);
    Check(work.Status() == TaskStatus::Failed && completion.Status() == TaskStatus::Failed && handler.Status() == TaskStatus::Failed, "failed task terminal states");
    Check(handlers == 3 && queue.Failed() == 5, "error handlers are not recursively retried");
    Check(Message(handler.Errors().work) == "before-handler" && Message(handler.Errors().handler) == "handler-original", "both error sources retained");
    Check(Message(unhandled.Errors().work) == "without-handler" && unhandled.Status() == TaskStatus::Failed, "handle retains unhandled error");
    Check(empty.Status() == TaskStatus::Completed && !empty.Errors().Any(), "CPU work without callback ends normally");
    int cancelledHandler = 0;
    auto cancelledError = queue.Submit([](const auto&) -> TaskQueue::Completion { throw std::runtime_error("cancelled-error"); }, [&](const auto&) { ++cancelledHandler; });
    Check(queue.WaitForReady(5s), "failed work ready for owner error callback"); cancelledError.Cancel(); queue.Pump();
    Check(cancelledHandler == 0 && cancelledError.Status() == TaskStatus::Failed && Message(cancelledError.Errors().work) == "cancelled-error", "cancellation preserves original failure");

    TaskQueue one(1, 1); CancellationToken publishing; int checks = 0;
    publishing = one.Submit([&](const auto&) { return [&] {
        Check(publishing.Status() == TaskStatus::Publishing && one.Pending() == 1, "publishing retains capacity");
        Check(!one.Submit([](const auto&) { return [] {}; }).Accepted(), "publication cannot bypass shared bound");
        try { one.Pump(); } catch (const std::logic_error&) { ++checks; }
        try { one.Shutdown(); } catch (const std::logic_error&) { ++checks; }
        try { one.WaitForReady(0ms); } catch (const std::logic_error&) { ++checks; }
    }; });
    Drain(one); Check(checks == 3 && publishing.Status() == TaskStatus::Completed, "publication reentry and blocking misuse rejected");
    std::thread wrong([&] {
        try { one.Pump(); } catch (const std::logic_error&) { ++checks; }
        try { one.Shutdown(); } catch (const std::logic_error&) { ++checks; }
        try { one.WaitForReady(0ms); } catch (const std::logic_error&) { ++checks; }
    });
    wrong.join(); Check(checks == 6 && one.Pending() == 0, "owner-only operations reject other thread");
    bool negative = false; try { one.WaitForReady(-1ms); } catch (const std::invalid_argument&) { negative = true; }
    Check(negative, "negative timeout rejected");
}
void ProducersAndShutdown()
{
    for (unsigned round = 0; round < 12; ++round)
    {
        Gate gate; TaskQueue queue(4, 32); const auto owner = std::this_thread::get_id(); std::size_t published = 0;
        std::array<std::vector<CancellationToken>, 4> handles; std::vector<std::thread> producers;
        for (unsigned producer = 0; producer < 4; ++producer) producers.emplace_back([&, producer] {
            for (unsigned index = 0; index < 128; ++index)
            {
                const unsigned id = producer * 128 + index;
                handles[producer].push_back(queue.Submit([&, id](const CancellationToken& token) -> TaskQueue::Completion {
                    gate.Block(); if (token.IsCancelled()) return {};
                    if ((id + round) % 13 == 0) throw std::runtime_error("stress-work");
                    return [&] { Check(std::this_thread::get_id() == owner, "stress owner publication"); ++published; };
                }));
            }
        });
        for (auto& producer : producers) producer.join();
        gate.Await(4); Check(queue.Pending() == 32 && queue.PeakPending() == 32 && queue.Rejected() == 480, "multiple producers respect common limit");
        unsigned accepted = 0;
        for (auto& producer : handles) for (auto& token : producer) if (token.Accepted()) { if (accepted++ % 3 == 0) token.Cancel(); }
        gate.Open(); Drain(queue); std::size_t completed = 0, failures = 0, cancelled = 0;
        for (const auto& producer : handles) for (const auto& token : producer) if (token.Accepted())
        {
            if (token.Status() == TaskStatus::Completed) ++completed;
            else if (token.Status() == TaskStatus::Cancelled) ++cancelled;
            else { Check(token.Status() == TaskStatus::Failed && token.Errors().Any(), "stress failure retained"); ++failures; }
        }
        Check(accepted == 32 && completed + failures + cancelled == 32 && published == completed && queue.Failed() == failures, "all accepted jobs reach one accounted terminal state");
    }
    for (unsigned round = 0; round < 24; ++round)
    {
        TaskQueue queue(3, 32); std::mutex mutex; std::condition_variable changed; unsigned attempted = 0;
        std::array<std::vector<CancellationToken>, 4> handles; std::vector<std::thread> producers;
        for (unsigned producer = 0; producer < 4; ++producer) producers.emplace_back([&, producer] {
            for (unsigned index = 0; index < 128; ++index)
            {
                auto token = queue.Submit([](const auto&) { return [] { throw std::runtime_error("must not publish after shutdown"); }; });
                if (index % 3 == 0) token.Cancel(); handles[producer].push_back(std::move(token));
                { std::lock_guard lock(mutex); ++attempted; } changed.notify_one();
            }
        });
        { std::unique_lock lock(mutex); Check(changed.wait_for(lock, 5s, [&] { return attempted >= 10; }), "concurrent producer did not progress"); }
        queue.Shutdown(); for (auto& producer : producers) producer.join();
        Check(queue.Pending() == 0 && queue.PeakPending() <= 32 && queue.Failed() == 0 && queue.Pump() == 0, "concurrent shutdown quiesces without publication");
        for (const auto& producer : handles) for (const auto& token : producer)
            Check(token.Status() == TaskStatus::Rejected || token.Status() == TaskStatus::Cancelled, "shutdown leaves no accepted task pending");
    }
}
struct Reenter
{
    TaskQueue& queue; std::atomic<unsigned>& released;
    Reenter(TaskQueue& owner, std::atomic<unsigned>& count) : queue(owner), released(count) {}
    ~Reenter() { (void)queue.Pending(); (void)queue.Submit({}); ++released; }
};
void CaptureDestruction()
{
    std::atomic<unsigned> released = 0; TaskQueue queue(1, 1);
    auto probe = std::make_shared<Reenter>(queue, released);
    queue.Submit([probe](const auto&) { return [probe] {}; }); probe.reset();
    Check(queue.WaitForReady(5s), "ready capture for shutdown"); queue.Shutdown();
    Check(released == 1 && queue.Pending() == 0, "abandoned capture destroys outside queue lock");
    Gate gate; TaskQueue saturated(1, 1);
    saturated.Submit([&](const auto&) { gate.Block(); return TaskQueue::Completion{}; }); gate.Await(1);
    auto rejected = saturated.Submit([capture = std::make_shared<Reenter>(saturated, released)](const auto&) {
        (void)capture; return TaskQueue::Completion{};
    });
    Check(!rejected.Accepted() && released == 2, "rejected capture also destroys outside queue lock"); gate.Open(); Drain(saturated);
    CancellationToken survivor;
    { TaskQueue temporary(1, 1); survivor = temporary.Submit([](const auto&) -> TaskQueue::Completion { throw std::runtime_error("after-queue-destruction"); }); Drain(temporary); }
    Check(Message(survivor.Errors().work) == "after-queue-destruction", "original exception outlives queue in caller handle");
    Check(queue.Submit({}).Status() == TaskStatus::Rejected, "empty work is rejected");
    bool invalid = false; try { TaskQueue bad(1, 0); } catch (const std::invalid_argument&) { invalid = true; }
    Check(invalid, "zero capacity rejected");
    invalid = false; try { TaskQueue bad(1, 65537); } catch (const std::invalid_argument&) { invalid = true; }
    Check(invalid, "excessive capacity rejected");
}
}
int main()
{
    try { BoundsAndCancellation(); PublicationAndErrors(); ProducersAndShutdown(); CaptureDestruction();
        std::cout << "Bounded tasks, owner publication, retained exceptions, cancellation and producer/shutdown stress passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

#include "CoreLayer/FixedStepClock.h"
#include "CoreLayer/TaskQueue.h"
#include "CoreLayer/Utf.h"
#include "CoreLayer/Hash.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>
#include <thread>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)

int main()
{
    CHECK(se::Sha256(std::string_view{})=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(se::Sha256(std::string_view{"abc"})=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::u32string decoded;
    CHECK(se::DecodeUtf8("A\xe4\xb8\xad\xf0\x9f\x98\x80", decoded));
    CHECK(decoded == U"A中😀");
    std::string encoded;
    CHECK(se::EncodeUtf8(decoded, encoded));
    CHECK(encoded == "A\xe4\xb8\xad\xf0\x9f\x98\x80");
    const auto original = decoded;
    CHECK(!se::DecodeUtf8("\xc0\xaf", decoded));
    CHECK(!se::DecodeUtf8("\xed\xa0\x80", decoded));
    CHECK(!se::DecodeUtf8("\xf4\x90\x80\x80", decoded));
    CHECK(decoded == original);
    CHECK(se::DecodeUtf16(se::EncodeUtf16(original), decoded) && decoded == original);
    CHECK(!se::DecodeUtf16(std::u16string(1, 0xd800), decoded));

    se::FixedStepClock clock;
    double simulated = 0;
    for (int i = 0; i < 120; ++i) clock.Advance(1.0 / 120, [&](double dt) { simulated += dt; });
    CHECK(clock.TotalSteps() == 60 && std::abs(simulated - 1) < 1e-9);
    CHECK(clock.Advance(1, [&](double) {}) == 8 && clock.DroppedSeconds() > 0.8);
    CHECK(clock.Alpha() >= 0 && clock.Alpha() < 1);
    CHECK(std::abs(clock.DroppedSeconds()-52.0/60)<1e-9&&clock.Alpha()==0);
    CHECK(clock.Advance(0,[](double){})==0&&clock.Advance(1.0/60,[](double){})==1);
    se::FixedStepClock fractional(.02,2);
    CHECK(fractional.Advance(.105,[](double){})==2&&std::abs(fractional.DroppedSeconds()-.06)<1e-9&&std::abs(fractional.Alpha()-.25)<1e-9);
    CHECK(fractional.Advance(.015,[](double){})==1);
    se::FixedStepClock huge;
    CHECK(huge.Advance((std::numeric_limits<double>::max)(),[](double){})==8);
    CHECK(std::isfinite(huge.Alpha())&&huge.Alpha()>=0&&huge.Alpha()<1&&std::isfinite(huge.DroppedSeconds()));
    bool overflowRejected=false;
    try {huge.Advance((std::numeric_limits<double>::max)(),[](double){});}catch(const std::invalid_argument&){overflowRejected=true;}
    CHECK(overflowRejected&&std::isfinite(huge.Alpha()));

    se::TaskQueue queue(2);
    const auto owner = std::this_thread::get_id();
    std::atomic<int> computed = 0;
    int published = 0;
    bool wrongThread = false;
    for (int i = 0; i < 20; ++i)
        queue.Submit([&](const se::CancellationToken&) {
            ++computed;
            return [&] { if (std::this_thread::get_id() != owner) wrongThread = true; ++published; };
        });
    std::atomic_bool release = false;
    auto cancelled = queue.Submit([&](const se::CancellationToken& token) {
        while (!release && !token.IsCancelled()) std::this_thread::yield();
        return [&] { published += 1000; };
    });
    cancelled.Cancel();
    release = true;
    queue.Submit([](const se::CancellationToken&) -> se::TaskQueue::Completion { throw std::runtime_error("test"); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (queue.Pending() && std::chrono::steady_clock::now() < deadline)
    {
        queue.Pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(queue.Pending() == 0 && computed == 20 && published == 20 && !wrongThread);
    CHECK(queue.Failed() == 1);
    queue.Shutdown();
    CHECK(queue.Submit([](const auto&) { return [] {}; }).IsCancelled());
    se::TaskQueue teardown(1);std::atomic_bool entered=false;
    teardown.Submit([&](const se::CancellationToken& token){entered=true;while(!token.IsCancelled())std::this_thread::yield();return se::TaskQueue::Completion{};});
    while(!entered)std::this_thread::yield();bool released=false;
    auto captured=std::shared_ptr<int>(new int(0),[&](int* pointer){(void)teardown.Pending();released=true;delete pointer;});
    teardown.Submit([captured](const auto&){return se::TaskQueue::Completion{};});captured.reset();
    teardown.Shutdown();CHECK(released);
    std::cout << "Core UTF, fixed clock, task cancellation and main-thread publication passed\n";
    return 0;
}

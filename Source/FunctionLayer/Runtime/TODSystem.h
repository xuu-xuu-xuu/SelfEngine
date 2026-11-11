#pragma once
#include "ResourceLayer/Environment/TODProfile.h"
#include <memory>
#include <span>
#include <thread>

namespace se
{
class EventBus;
inline constexpr std::uint64_t TODTimeChanged=19,TODSunrise=20,TODSunset=21;
struct TODState {double simulationSeconds=0,time=0,rate=1;std::int64_t day=0;bool daylight=false;};
struct EnvironmentSnapshot
{
    std::uint64_t frameIndex=0;double simulationSeconds=0,time=0;std::int64_t day=0;
    Vec3 sunDirection{0,-1,0}; // 右手、Y 向上；从太阳射向世界的单位方向。
    TODParameters parameters;bool daylight=false;
};
struct TODEvent {std::int64_t day=0;double time=0,simulationSeconds=0;};
bool DecodeTODEvent(std::span<const std::uint8_t> bytes,TODEvent& value);
class TODSystem
{
public:
    static constexpr std::size_t EventBudget=64;
    static constexpr std::int64_t MaximumDay=1000000000;
    explicit TODSystem(EventBus& events);
    static bool ValidateProfile(const TODProfile& profile,double step,std::string& error);
    bool ConfigureStep(double step);
    bool SelectProfile(const TODProfile& profile,bool preserveTime=false);
    void Clear();
    void Close()noexcept{closed_=true;}
    void Deactivate();
    void ResetSimulationClock();
    bool SetRate(double rate);
    bool SetTime(std::int64_t day,double time,bool replayBoundaries=false);
    bool Advance(double seconds);
    EnvironmentSnapshot Snapshot(std::uint64_t frameIndex,double interpolation=1)const;
    const TODState& State()const noexcept{return state_;}
    std::shared_ptr<const TODProfile> Profile()const noexcept{return profile_;}
    const std::string& LastError()const noexcept{return error_;}
    std::uint64_t NotificationFailures()const noexcept{return notificationFailures_;}
private:
    struct Boundary {std::uint64_t type;TODEvent value;};
    bool Fail(const char* key);
    bool OwnerThread();
    bool Open(){return OwnerThread()&&(!closed_||Fail("Environment.Closed"));}
    bool Boundaries(const TODState& target,std::vector<Boundary>& output);
    void Emit(std::uint64_t type,const TODEvent& value);
    void RegisterEvents();
    EventBus* events_;
    std::shared_ptr<const TODProfile> profile_;
    TODState state_,previous_;
    double step_=1.0/60.0;
    std::uint64_t notificationFailures_=0;
    std::thread::id thread_=std::this_thread::get_id();
    std::string error_;
    bool closed_=false;
};
}

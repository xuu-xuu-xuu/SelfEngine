#include "TODSystem.h"
#include "EventBus.h"
#include <algorithm>
#include <bit>
#include <numbers>
#include <stdexcept>

namespace se
{
namespace
{
void Write(std::vector<std::uint8_t>& b,std::size_t offset,std::uint64_t v){for(unsigned i=0;i<8;++i)b[offset+i]=static_cast<std::uint8_t>(v>>(8*i));}
std::uint64_t Read(std::span<const std::uint8_t> b,std::size_t offset){std::uint64_t v=0;for(unsigned i=0;i<8;++i)v|=std::uint64_t(b[offset+i])<<(8*i);return v;}
bool RateSafe(const TODProfile& p,double step,double rate){const auto days=step*rate/p.dayLengthSeconds;return std::isfinite(step)&&step>0&&std::isfinite(rate)&&rate>=0&&std::isfinite(days)&&days<=double(TODSystem::EventBudget)/2;}
bool TimeValid(std::int64_t day,double time){return day>=-TODSystem::MaximumDay&&day<=TODSystem::MaximumDay&&std::isfinite(time)&&time>=0&&time<1;}
}
bool DecodeTODEvent(std::span<const std::uint8_t> b,TODEvent& value)
{if(b.size()!=24)return false;TODEvent v{std::bit_cast<std::int64_t>(Read(b,0)),std::bit_cast<double>(Read(b,8)),std::bit_cast<double>(Read(b,16))};if(!TimeValid(v.day,v.time)||!std::isfinite(v.simulationSeconds)||v.simulationSeconds<0)return false;value=v;return true;}
TODSystem::TODSystem(EventBus& events):events_(&events){RegisterEvents();}
void TODSystem::RegisterEvents(){for(const auto& descriptor:std::initializer_list<EventTypeDescriptor>{{TODTimeChanged,"engine.tod_time_changed",1,24},{TODSunrise,"engine.tod_sunrise",1,24},{TODSunset,"engine.tod_sunset",1,24}})if(!events_->RegisterType(descriptor))throw std::logic_error(events_->LastError());}
bool TODSystem::Fail(const char* key){error_=key;return false;}
bool TODSystem::OwnerThread(){return std::this_thread::get_id()==thread_||Fail("TOD.WrongThread");}
bool TODSystem::ValidateProfile(const TODProfile& p,double step,std::string& error)
{if(!ValidateTODProfile(p,error))return false;if(!RateSafe(p,step,p.rate)){error="TOD.EventBudget";return false;}return true;}
bool TODSystem::ConfigureStep(double step)
{
    if(!Open())return false;
    if(!std::isfinite(step)||step<=0)return Fail("TOD.InvalidTime");
    if(profile_&&(!RateSafe(*profile_,step,state_.rate)||!RateSafe(*profile_,step,profile_->rate)))return Fail("TOD.EventBudget");
    step_=step;error_.clear();return true;
}
bool TODSystem::SelectProfile(const TODProfile& p,bool preserve)
{
    if(!Open()||!ValidateProfile(p,step_,error_))return false;
    auto copy=std::make_shared<const TODProfile>(p);auto next=state_;if(!preserve){next.day=0;next.time=p.initialTime;}
    next.rate=p.rate;next.daylight=next.time>=p.sunrise&&next.time<p.sunset;
    profile_=std::move(copy);state_=previous_=next;error_.clear();Emit(TODTimeChanged,{next.day,next.time,next.simulationSeconds});return true;
}
void TODSystem::Clear(){if(!OwnerThread())return;closed_=false;profile_.reset();state_=previous_={};notificationFailures_=0;error_.clear();RegisterEvents();}
void TODSystem::Deactivate(){if(!Open())return;profile_.reset();previous_=state_;error_.clear();}
void TODSystem::ResetSimulationClock(){if(!Open())return;state_.simulationSeconds=0;previous_=state_;}
bool TODSystem::SetRate(double rate)
{
    if(!Open())return false;if(!profile_)return Fail("TOD.NoProfile");
    if(!RateSafe(*profile_,step_,rate))return Fail("TOD.EventBudget");state_.rate=rate;previous_=state_;error_.clear();return true;
}
void TODSystem::Emit(std::uint64_t type,const TODEvent& value)
{
    std::vector<std::uint8_t> bytes(24);Write(bytes,0,std::bit_cast<std::uint64_t>(value.day));Write(bytes,8,std::bit_cast<std::uint64_t>(value.time));Write(bytes,16,std::bit_cast<std::uint64_t>(value.simulationSeconds));
    if(events_->GetPublisher().Post(type,1,bytes,EventPhase::AfterLogic)!=EventPostResult::Accepted)++notificationFailures_;
}
bool TODSystem::Boundaries(const TODState& target,std::vector<Boundary>& output)
{
    const auto difference=target.day-state_.day;const double end=double(difference)+target.time;
    if(end<state_.time)return Fail("TOD.ReplayBackwards");
    // 先计数，再进入最多 EventBudget 次的循环；包括普通步进。
    const double count=std::floor(end-profile_->sunrise)-std::floor(state_.time-profile_->sunrise)
        +std::floor(end-profile_->sunset)-std::floor(state_.time-profile_->sunset);
    if(!std::isfinite(count)||count>EventBudget)return Fail("TOD.EventBudget");
    for(const auto [type,phase]:{std::pair{TODSunrise,profile_->sunrise},std::pair{TODSunset,profile_->sunset}})
    {
        const auto amount=static_cast<std::size_t>(std::floor(end-phase)-std::floor(state_.time-phase));
        auto day=state_.day+(state_.time>=phase?1:0);
        for(std::size_t i=0;i<amount;++i,++day){const double alpha=end==state_.time?0:(double(day-state_.day)+phase-state_.time)/(end-state_.time);output.push_back({type,{day,phase,state_.simulationSeconds+(target.simulationSeconds-state_.simulationSeconds)*alpha}});}
    }
    std::sort(output.begin(),output.end(),[](const auto& a,const auto& b){return a.value.day==b.value.day?a.value.time<b.value.time:a.value.day<b.value.day;});return true;
}
bool TODSystem::SetTime(std::int64_t day,double time,bool replay)
{
    if(!Open())return false;if(!profile_)return Fail("TOD.NoProfile");if(!TimeValid(day,time))return Fail("TOD.InvalidTime");
    auto next=state_;next.day=day;next.time=time;next.daylight=time>=profile_->sunrise&&time<profile_->sunset;
    std::vector<Boundary> events;if(replay&&!Boundaries(next,events))return false;
    previous_=state_=next;error_.clear();for(const auto& e:events)Emit(e.type,e.value);Emit(TODTimeChanged,{day,time,next.simulationSeconds});return true;
}
bool TODSystem::Advance(double seconds)
{
    if(!Open())return false;if(!std::isfinite(seconds)||seconds<0)return Fail("TOD.InvalidTime");if(seconds==0){error_.clear();return true;}
    if(!profile_){if(!std::isfinite(state_.simulationSeconds+seconds))return Fail("TOD.InvalidTime");previous_=state_;state_.simulationSeconds+=seconds;error_.clear();return true;}
    if(!RateSafe(*profile_,seconds,state_.rate))return Fail("TOD.EventBudget");
    const double phase=state_.time+seconds*state_.rate/profile_->dayLengthSeconds;
    const double days=std::floor(phase);auto next=state_;next.day+=static_cast<std::int64_t>(days);next.time=phase-days;next.simulationSeconds+=seconds;
    if(!TimeValid(next.day,next.time)||!std::isfinite(next.simulationSeconds))return Fail("TOD.InvalidTime");
    next.daylight=next.time>=profile_->sunrise&&next.time<profile_->sunset;std::vector<Boundary> events;if(!Boundaries(next,events))return false;
    previous_=state_;state_=next;error_.clear();for(const auto& e:events)Emit(e.type,e.value);return true;
}
EnvironmentSnapshot TODSystem::Snapshot(std::uint64_t frame,double interpolation)const
{
    EnvironmentSnapshot result;result.frameIndex=frame;if(!profile_)return result;
    const auto alpha=std::isfinite(interpolation)?std::clamp(interpolation,0.0,1.0):1.0;
    const double phase=previous_.time+(double(state_.day-previous_.day)+state_.time-previous_.time)*alpha;const double days=std::floor(phase);
    result.day=previous_.day+static_cast<std::int64_t>(days);result.time=phase-days;result.simulationSeconds=previous_.simulationSeconds+(state_.simulationSeconds-previous_.simulationSeconds)*alpha;
    result.daylight=result.time>=profile_->sunrise&&result.time<profile_->sunset;result.parameters=SampleTODProfile(*profile_,result.time);
    // 配置日出/日落之间走正半波，夜间走负半波；方向在两边界连续。
    const auto daylightLength=profile_->sunset-profile_->sunrise;double angle;
    if(result.daylight)angle=(result.time-profile_->sunrise)/daylightLength*std::numbers::pi;
    else{double night=result.time-profile_->sunset;if(night<0)night+=1;angle=std::numbers::pi+night/(1-daylightLength)*std::numbers::pi;}
    const double elevation=std::sin(angle)*profile_->maximumElevationRadians,azimuth=profile_->azimuthRadians;
    result.sunDirection={static_cast<float>(-std::cos(elevation)*std::sin(azimuth)),static_cast<float>(-std::sin(elevation)),static_cast<float>(-std::cos(elevation)*std::cos(azimuth))};return result;
}
}

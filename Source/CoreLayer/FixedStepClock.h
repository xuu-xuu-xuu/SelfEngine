#pragma once
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <stdexcept>

namespace se
{
// 显示帧间隔累积到固定步；超出追赶预算的整步时间明确丢弃并统计。
class FixedStepClock
{
public:
    explicit FixedStepClock(double step = 1.0 / 60.0, std::size_t maxSteps = 8)
        : step_(step), maxSteps_(maxSteps)
    {
        if (!std::isfinite(step) || step <= 0 || maxSteps == 0)
            throw std::invalid_argument("Clock.InvalidSettings");
    }
    std::size_t Advance(double seconds, const std::function<void(double)>& update)
    {
        if (!std::isfinite(seconds) || seconds < 0 || !update
            || seconds > (std::numeric_limits<double>::max)() - accumulator_
            || seconds > (std::numeric_limits<double>::max)() - droppedSeconds_)
            throw std::invalid_argument("Clock.InvalidDelta");
        accumulator_ += seconds;
        std::size_t steps = 0;
        while (steps < maxSteps_ && accumulator_ + step_ * 1e-9 >= step_)
        {
            accumulator_ -= step_;
            if (accumulator_ < 0) accumulator_ = 0;
            ++steps;
            ++totalSteps_;
            update(step_);
        }
        if (accumulator_ + step_ * 1e-9 >= step_)
        {
            // 直接除以小步长可能溢出，即使输入和累计时间本身仍为有限数。
            double remaining = std::fmod(accumulator_, step_);
            // 减法/fmod 可能留下接近完整一步的余数；与上方步进容差一致地丢弃。
            if(step_-remaining<=step_*1e-9)remaining=0;
            const double dropped = accumulator_ - remaining;
            accumulator_ = remaining;
            droppedSeconds_ += dropped;
        }
        return steps;
    }
    void Reset() noexcept { accumulator_ = 0; droppedSeconds_ = 0; totalSteps_ = 0; }
    void ClearRemainder()noexcept{accumulator_=0;}
    double Alpha() const noexcept { return accumulator_ / step_; }
    double Step() const noexcept { return step_; }
    double DroppedSeconds() const noexcept { return droppedSeconds_; }
    std::size_t TotalSteps() const noexcept { return totalSteps_; }
private:
    double step_;
    std::size_t maxSteps_;
    double accumulator_ = 0;
    double droppedSeconds_ = 0;
    std::size_t totalSteps_ = 0;
};
}

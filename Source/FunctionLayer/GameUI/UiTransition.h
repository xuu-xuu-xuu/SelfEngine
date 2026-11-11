#pragma once
#include <cmath>

namespace se
{
// 只保存值与 UI 时间；不持有树、控件指针或完成回调。
template<class Value> struct UiTransition
{
    Value from, to;
    double elapsed = 0, duration = 0;
    float Advance(double seconds) noexcept
    {
        if (std::isfinite(seconds) && seconds >= 0)
            elapsed = seconds >= duration - elapsed ? duration : elapsed + seconds;
        return duration == 0 ? 1 : static_cast<float>(elapsed / duration);
    }
};
}

#pragma once

#include "FunctionLayer/Runtime/Camera.h"
#include "PlatformLayer/InputState.h"

namespace se
{
// 只产生本帧意图，不拥有/修改平台输入，也不推进模拟时钟。
class FreeCameraInputRouter
{
public:
    FreeCameraInput Capture(const InputState& input, bool focused, bool keyboardConsumed, bool pointerConsumed);
    void Reset() noexcept { dragging_ = false; }
private:
    bool dragging_ = false;
    int lastX_ = 0, lastY_ = 0;
};
}

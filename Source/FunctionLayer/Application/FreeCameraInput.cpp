#include "FunctionLayer/Application/FreeCameraInput.h"

namespace se
{
FreeCameraInput FreeCameraInputRouter::Capture(const InputState& input, bool focused,
    bool keyboardConsumed, bool pointerConsumed)
{
    FreeCameraInput result;
    if (!focused) { Reset(); return result; }
    if (!keyboardConsumed)
    {
        result.move = { float(input.keysDown['D']) - float(input.keysDown['A']),
            float(input.keysDown['E']) - float(input.keysDown['Q']),
            float(input.keysDown['W']) - float(input.keysDown['S']) };
        result.fast = input.keysDown[0x10]; // 平台键表沿用 Win32 Shift 编码。
    }
    bool cancelled = false;
    for (const auto& event : input.keyEvents)
        if (event.type == KeyEventType::Reset || (event.type == KeyEventType::Pressed && event.keyCode == 0x1b))
            cancelled = true;
    if (pointerConsumed || cancelled) { Reset(); return result; }
    for (const auto& event : input.pointerEvents)
    {
        if (event.type == PointerEventType::Cancel) Reset();
        else if (event.type == PointerEventType::Down && event.button == 1)
        { dragging_ = true; lastX_ = event.x; lastY_ = event.y; }
        else if (event.type == PointerEventType::Up && event.button == 1) Reset();
        else if (event.type == PointerEventType::Move && dragging_)
        {
            result.lookPixels.x += static_cast<float>(static_cast<double>(event.x) - lastX_);
            result.lookPixels.y += static_cast<float>(static_cast<double>(event.y) - lastY_);
            lastX_ = event.x;
            lastY_ = event.y;
        }
    }
    if (!input.mouseButtons[1]) Reset();
    return result;
}
}

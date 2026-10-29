#pragma once

#include <array>
#include <vector>
#include "InputEvents.h"

// 保存输入状态和待消费事件，不依赖 Windows 类型或窗口生命周期。
struct InputState
{
    std::array<bool, 256> keysDown{};
    bool isLeftDown = false;

    // 消息处理期间追加，主循环读取后清空；状态表本身不按帧清空。
    std::vector<KeyEvent> keyEvents;
    std::vector<TextInputEvent> textEvents;

    // 本轮所有消费者读取后清理事件；保留持续按住状态。
    void ClearEvents()
    {
        keyEvents.clear();
        textEvents.clear();
    }
};

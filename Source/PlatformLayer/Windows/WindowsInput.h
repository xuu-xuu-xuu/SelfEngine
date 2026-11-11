#pragma once

#include "PlatformLayer/InputState.h"
#include <Windows.h>

enum class InputMessageResult
{
    UseDefault, // 未处理，或更新状态后仍需要系统默认行为。
    Handled     // 已处理，窗口过程可以返回 0。
};

// hwnd 仅用于鼠标捕获；input 引用所属窗口的输入状态。
// 返回值表示消息是否还需默认处理，不表示操作成功或失败。
InputMessageResult ProcessWindowsInput(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, InputState& input);

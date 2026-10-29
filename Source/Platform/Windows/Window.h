#pragma once

#include "Platform/InputState.h"
#include <Windows.h>

// 由调用者持有；窗口存活期间不要复制、移动或销毁这个对象。
struct WindowState
{
    HWND hwnd = nullptr;
    InputState input{};
};

// 在主线程启动时调用一次，注册窗口类并创建、关联和显示窗口。
// window 传引用，成功返回 true；失败记录原因并返回 false。
bool CreateEngineWindow(WindowState& window);

// 仍有系统窗口时销毁它；已经销毁则直接成功。
bool DestroyEngineWindow(WindowState& window);

// 排空调用线程的消息；WM_QUIT 时写入退出码并返回 false，否则返回 true。
bool PumpWindowMessages(int& exitCode);

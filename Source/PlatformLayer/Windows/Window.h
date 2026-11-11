#pragma once

#include "PlatformLayer/InputState.h"
#include <Windows.h>
#include <functional>
#include <optional>

// 由调用者持有；窗口存活期间不要复制、移动或销毁这个对象。
struct WindowState
{
    HWND hwnd = nullptr;
    InputState input{};
    UINT width = 0;
    UINT height = 0;
    bool drawable = false; // 非最小化且客户区非零；状态更新不依赖日志。
    bool closeRequested = false;
    bool callbackFailed = false;
    bool postQuitOnDestroy = true;
    std::function<std::optional<LRESULT>(HWND,UINT,WPARAM,LPARAM)> messageHandler;
};

// 在主线程启动时调用一次，注册窗口类并创建、关联窗口；默认显示，检查程序可隐藏。
// window 传引用，成功返回 true；失败记录原因并返回 false。
bool CreateEngineWindow(WindowState& window, bool showWindow = true);
// 按客户区设置尺寸；系统最小窗口或其他约束未满足请求时返回 false，不伪称成功。
bool SetEngineWindowClientSize(WindowState& window,UINT width,UINT height);

// 仍有系统窗口时销毁它；已经销毁则直接成功。
bool DestroyEngineWindow(WindowState& window);

// 排空调用线程的消息；WM_QUIT 时写入退出码并返回 false，否则返回 true。
bool PumpWindowMessages(int& exitCode);

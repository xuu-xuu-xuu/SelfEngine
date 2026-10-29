#include "Window.h"
#include "WindowsInput.h"
#include "Core/Log.h"
#include <iostream>
#include <string>

namespace
{
// 处理窗口消息：hwnd 是目标窗口，message 是消息类型。
// wParam、lParam 是附加数据，具体含义由消息类型决定。
LRESULT CALLBACK WindowProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
{
	// GWLP_USERDATA 保存本窗口关联的 WindowState 地址。
	WindowState* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

	if (message == WM_NCCREATE)
	{
		const CREATESTRUCTW* createInfo = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		state = static_cast<WindowState*>(createInfo->lpCreateParams);
		if (state == nullptr)
		{
			OutputDebugStringW(L"创建窗口时未收到状态对象地址。\n");
			SetLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}

		// 返回旧值 0 也可能表示成功，需要同时检查错误码。
		SetLastError(ERROR_SUCCESS);
		const LONG_PTR previous = SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
		const DWORD error = GetLastError();
		if (previous == 0 && error != ERROR_SUCCESS)
		{
			try
			{
				Log(LogLevel::Error, "关联窗口状态失败，错误码：" + std::to_string(error));
			}
			catch (...)
			{
				OutputDebugStringW(L"记录窗口状态关联失败时发生 C++ 异常。\n");
			}
			SetLastError(error);
			return FALSE;
		}
		state->hwnd = hwnd;

		try
		{
			std::cout << "[Info] WM_NCCREATE 收到的状态地址：" << state << '\n';
		}
		catch (...)
		{
			OutputDebugStringW(L"记录窗口状态地址时发生 C++ 异常。\n");
		}
		return TRUE;
	}

	// 关联建立前或解除后，交给系统默认处理。
	if (state == nullptr)
	{
		return DefWindowProcW(hwnd, message, wParam, lParam);
	}

	switch (message)
	{

		// WM_SIZE：wParam 表示尺寸状态，lParam 携带客户区宽高。
		case WM_SIZE:
		{
			try
			{
				if (wParam == SIZE_MINIMIZED)
				{
					Log(LogLevel::Info, "窗口已最小化");
				}
				else
				{
					// LOWORD 取宽度，HIWORD 取高度。
					const UINT width = LOWORD(lParam);
					const UINT height =HIWORD(lParam);
					Log(LogLevel::Info,"窗口客户区尺寸：" + std::to_string(width) + " x " + std::to_string(height));
				}
			}
			catch (...)
			{
				// 将固定诊断文字输出到调试器。
				OutputDebugStringW(L"记录窗口尺寸时发生 C++ 异常。\n");
			}

			return 0;
		}

		//窗口关闭
		case WM_DESTROY:
			// 发出 WM_QUIT 退出请求，参数 0 是退出码
			PostQuitMessage(0);
			return 0;

		case WM_NCDESTROY:
		{
			// 系统窗口销毁时解除借用，WindowState 仍由调用者持有。
			SetLastError(ERROR_SUCCESS);
			if (SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0) == 0 && GetLastError() != ERROR_SUCCESS)
			{
				OutputDebugStringW(L"解除窗口状态关联失败。\n");
			}
			state->hwnd = nullptr;
			state->input = InputState{};
			return DefWindowProcW(hwnd, message, wParam, lParam);
		}
	}

    // 输入模块只借用输入状态，不访问整个 WindowState。
    if (ProcessWindowsInput(hwnd, message, wParam, lParam, state->input)
        == InputMessageResult::Handled)
    {
        return 0;
    }

	// 对未自行处理的消息执行系统默认行为
	return DefWindowProcW(hwnd, message, wParam, lParam);
}
}

bool CreateEngineWindow(WindowState& window)
{
	// 取得程序模块句柄；nullptr 表示当前 exe 模块。
	const HINSTANCE instance = GetModuleHandleW(nullptr);
	if (instance == nullptr)
	{
		// 取得最近一次 Win32 调用的错误码。
		const DWORD error = GetLastError();
		Log(LogLevel::Error, "获取程序模块句柄失败，错误码：" + std::to_string(error));
		return false;
	}
	const wchar_t windowClassName[] = L"SelfEngineWindowClass";
	WNDCLASSW windowClass{};
	// 登记窗口过程的函数地址，供 Windows 回调。
	windowClass.lpfnWndProc = WindowProc;
	windowClass.hInstance = instance;
	windowClass.lpszClassName = windowClassName;

	// 设置系统箭头光标和窗口背景色。
	windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
	windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);


	// 注册窗口类的创建规则；返回 0 表示失败。
	if (RegisterClassW(&windowClass) == 0)
	{
		const DWORD error = GetLastError();
		Log(LogLevel::Error, "注册窗口类失败，错误码：" + std::to_string(error));
		return false;
	}

	Log(LogLevel::Info, "窗口类注册成功");

	// 根据已注册的窗口类创建实际窗口，成功后返回窗口句柄 HWND。
	window.hwnd = CreateWindowExW(
		0,                              // 暂不使用扩展样式
		windowClassName,                // 使用刚才注册的窗口类
		L"SelfEngine - T03",             // 窗口标题
		WS_OVERLAPPEDWINDOW,             // 带标题栏、边框和窗口按钮
		CW_USEDEFAULT, CW_USEDEFAULT,    // 由系统选择初始位置
		1280, 720,                      // 窗口整体宽度和高度
		nullptr,                        // 没有父窗口
		nullptr,                        // 没有菜单
		instance,                       // 当前程序模块
		&window                         // 传入窗口状态对象的地址
	);

	if (window.hwnd == nullptr)
	{
		const DWORD error = GetLastError();
		Log(LogLevel::Error, "创建窗口失败，错误码：" + std::to_string(error));
		return false;
	}

	// 显示 hwnd 指定的窗口。
	ShowWindow(window.hwnd, SW_SHOW);
	return true;
}

bool DestroyEngineWindow(WindowState& window)
{
	// 即使通过其他路径收到 WM_QUIT，也先销毁仍存在的系统窗口。
	if (window.hwnd != nullptr && !DestroyWindow(window.hwnd))
	{
		const DWORD error = GetLastError();
		Log(LogLevel::Error, "销毁窗口失败，错误码：" + std::to_string(error));
		return false;
	}

	return true;
}

bool PumpWindowMessages(int& exitCode)
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            exitCode = static_cast<int>(message.wParam);
            return false;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}

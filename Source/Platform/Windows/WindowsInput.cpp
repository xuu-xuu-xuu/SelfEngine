#include "WindowsInput.h"
#include "Core/Log.h"
#include <string>
#include <windowsx.h>

InputMessageResult ProcessWindowsInput(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, InputState& input)
{
    switch (message)
    {
		//wParam代表虚拟键码
		case WM_KEYDOWN:
		case WM_KEYUP:
		case WM_SYSKEYDOWN:
		case WM_SYSKEYUP:
		{
			if (wParam >= input.keysDown.size())
			{
				break;
			}

			const bool newState = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
			if (input.keysDown[wParam] == newState)
			{
				break;
			}

			input.keysDown[wParam] = newState;

			const bool cancelMouse = newState && wParam == VK_ESCAPE && GetCapture() == hwnd;

			if (cancelMouse)
			{
				ReleaseCapture();
			}

			try
			{
				input.keyEvents.push_back({
					newState ? KeyEventType::Pressed : KeyEventType::Released,
					static_cast<unsigned int>(wParam) });
				Log(LogLevel::Info,"键码：" + std::to_string(wParam) + "，按下状态：" + std::to_string(input.keysDown[wParam]));
				if (cancelMouse)
				{
					Log(LogLevel::Info,"Esc 取消捕获后的左键状态：" + std::to_string(input.isLeftDown));
				}
			}
			catch (...)
			{
				OutputDebugStringW(L"记录键盘消息时发生 C++ 异常。\n");
			}

			break;
		}

		//左键按下或者松开
		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
		{
			const int x = GET_X_LPARAM(lParam);
			const int y = GET_Y_LPARAM(lParam);

			input.isLeftDown = (message == WM_LBUTTONDOWN);

			if (message == WM_LBUTTONDOWN)
			{
				SetCapture(hwnd); // 当前窗口捕获鼠标
			}
			else if(GetCapture() == hwnd)
			{
				ReleaseCapture(); // 松开时结束捕获
			}

			try
			{
				if (message == WM_LBUTTONDOWN)
				{
					Log(LogLevel::Info,"鼠标左键按下，客户区坐标：(" + std::to_string(x) + ", " + std::to_string(y) + ")");
				}
				else
				{
					Log(LogLevel::Info,"鼠标左键松开，客户区坐标：(" + std::to_string(x) + ", " + std::to_string(y) + ")");
				}

				Log(LogLevel::Info, "当前左键状态：" + std::to_string(input.isLeftDown));
			}
			catch (...)
			{
				OutputDebugStringW(L"记录鼠标消息时发生 C++ 异常。\n");
			}

			return InputMessageResult::Handled;
		}

		//左键按下时记录移动坐标
		case WM_MOUSEMOVE:
		{
			if (!input.isLeftDown)
			{
				return InputMessageResult::Handled;
			}

			const int x = GET_X_LPARAM(lParam);
			const int y = GET_Y_LPARAM(lParam);

			try
			{
				Log(LogLevel::Info,"左键拖动位置：(" + std::to_string(x) + ", " + std::to_string(y) + ")");
			}
			catch (...)
			{
				OutputDebugStringW(L"记录鼠标移动时发生 C++ 异常。\n");
			}
			return InputMessageResult::Handled;
		}

		//窗口 捕获和失去捕获 的 鼠标提示
		case WM_CAPTURECHANGED:
		{
			input.isLeftDown = false;

			try
			{
				Log(LogLevel::Info,"窗口已失去鼠标捕获，左键状态：" + std::to_string(input.isLeftDown));
			}

			catch (...)
			{
				OutputDebugStringW(L"记录鼠标捕获变化时发生 C++ 异常。\n");
			}

			return InputMessageResult::Handled;
		}

		//字符数据 现在只观察w/W
		case WM_CHAR:
		{
			if (wParam != L'w' && wParam != L'W')
			{
				break;
			}

			try
			{
				input.textEvents.push_back({ static_cast<char16_t>(wParam) });
				if (wParam == L'w')
				{
					Log(LogLevel::Info, "文本输入：w");
				}
				else
				{
					Log(LogLevel::Info, "文本输入：W");
				}
			}
			catch (...)
			{
				OutputDebugStringW(L"记录文本输入时发生 C++ 异常。\n");
			}

			return InputMessageResult::Handled;
		}

		//窗口获得和失去键盘焦点共用一个逻辑
		case WM_SETFOCUS:
		case WM_KILLFOCUS:
		{
			// 失焦时清除全部的保存的状态
			if (message == WM_KILLFOCUS)
			{
				input.keysDown.fill(false);
			}


			// 打印当前状态
			try
			{
				if (message == WM_KILLFOCUS)
				{
					input.keyEvents.push_back({ KeyEventType::Reset, 0 });
				}
				if (message == WM_SETFOCUS)
				{
					Log(LogLevel::Info, "窗口获得键盘焦点");
				}
				else
				{
					Log(LogLevel::Info, "窗口失去键盘焦点，键盘状态已清除");
				}
			}
			catch (...)
			{
				//打印错误日志
				OutputDebugStringW(L"记录窗口焦点时发生 C++ 异常。\n");
			}

			return InputMessageResult::Handled;
		}

    }

    // 键盘消息更新状态后仍需默认处理，保留 Alt+F4 等系统行为。
    return InputMessageResult::UseDefault;
}

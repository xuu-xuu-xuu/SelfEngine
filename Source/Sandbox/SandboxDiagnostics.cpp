#include "SandboxDiagnostics.h"
#include "Core/Log.h"
#include "Platform/InputState.h"
#include "Platform/Windows/FrameTimer.h"
#include <string>

SandboxDiagnostics::SandboxDiagnostics(const InputState& initialInput)
    : previousWDown_(initialInput.keysDown['W'])
{
}

void SandboxDiagnostics::ObserveInput(const InputState& input)
{
    for (const KeyEvent& event : input.keyEvents)
    {
        if (event.type == KeyEventType::Reset)
        {
            Log(LogLevel::Info, "帧事件：键盘状态重置");
        }
        else
        {
            Log(LogLevel::Info,
                "帧事件：键码=" + std::to_string(event.keyCode)
                + (event.type == KeyEventType::Pressed ? "，按下" : "，松开"));
        }
    }
    for (const TextInputEvent& event : input.textEvents)
    {
        // 当前仅有 w/W，后续完整文本处理不能直接缩窄 UTF-16 字符。
        Log(LogLevel::Info,
            "帧事件：文本=" + std::string(1, static_cast<char>(event.codeUnit)));
    }

    const bool isWDown = input.keysDown['W'];
    if (isWDown != previousWDown_)
    {
        previousWDown_ = isWDown;
        Log(LogLevel::Info, "主循环读取 W 状态：" + std::to_string(isWDown));
    }
}

void SandboxDiagnostics::ObserveTiming(const FrameTimer& timer, double updateDeltaSeconds)
{
    const double rawDeltaSeconds = timer.deltaSeconds;
    if (rawDeltaSeconds > updateDeltaSeconds)
    {
        Log(LogLevel::Warning,
            "长帧限制：实际 dt=" + std::to_string(rawDeltaSeconds * 1000.0) + " ms"
            + "，更新 dt=" + std::to_string(updateDeltaSeconds * 1000.0) + " ms");
    }
    if (timer.totalSeconds - lastTimingLogSeconds_ >= 1.0)
    {
        Log(LogLevel::Info,
            "帧计时：累计帧数=" + std::to_string(timer.frameCount)
            + "，实际累计时间=" + std::to_string(timer.totalSeconds) + " s"
            + "，实际 dt=" + std::to_string(rawDeltaSeconds * 1000.0) + " ms"
            + "，更新 dt=" + std::to_string(updateDeltaSeconds * 1000.0) + " ms");
        lastTimingLogSeconds_ = timer.totalSeconds;
    }
}

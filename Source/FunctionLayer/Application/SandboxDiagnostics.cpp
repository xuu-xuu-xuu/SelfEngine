#include "SandboxDiagnostics.h"
#include "CoreLayer/Log.h"
#include "CoreLayer/Localization.h"
#include "PlatformLayer/InputState.h"
#include "PlatformLayer/Windows/FrameTimer.h"
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
            Log(LogLevel::Debug, "帧事件：键盘状态重置");
        }
        else
        {
            Log(LogLevel::Debug,
                "帧事件：键码=" + std::to_string(event.keyCode)
                + (event.type == KeyEventType::Pressed ? "，按下" : "，松开"));
        }
    }
    for (const TextInputEvent& event : input.textEvents)
    {
        // 旧教学日志只观察 w/W；完整 Unicode/IME 由 UI 文本管线处理。
        if (event.codeUnit == u'w' || event.codeUnit == u'W')
            Log(LogLevel::Debug, "帧事件：文本=" + std::string(1, static_cast<char>(event.codeUnit)));
    }

    const bool isWDown = input.keysDown['W'];
    if (isWDown != previousWDown_)
    {
        previousWDown_ = isWDown;
        Log(LogLevel::Debug, "主循环读取 W 状态：" + std::to_string(isWDown));
    }
}

void SandboxDiagnostics::ObserveTiming(const FrameTimer& timer,double updateDeltaSeconds,bool fixedStep,const Localization* text)
{
    const double rawDeltaSeconds = timer.deltaSeconds;
    if (!fixedStep&&rawDeltaSeconds>updateDeltaSeconds+1e-12)
    {
        Log(LogLevel::Warning,
            (text?text->GetText("Timer.DeltaClamped"):std::string("Timer.DeltaClamped"))+": "+std::to_string(rawDeltaSeconds*1000.0)+" / "+std::to_string(updateDeltaSeconds*1000.0));
    }
    if (timer.totalSeconds - lastTimingLogSeconds_ >= 1.0)
    {
        Log(LogLevel::Debug,
            (text?text->GetText("Timer.FrameTiming"):std::string("Timer.FrameTiming"))+": "+std::to_string(timer.frameCount)+" / "+std::to_string(timer.totalSeconds)
            +" / "+std::to_string(rawDeltaSeconds*1000.0)+" / "+std::to_string(updateDeltaSeconds*1000.0));
        lastTimingLogSeconds_ = timer.totalSeconds;
    }
}

#include "SandboxApp.h"
#include "SandboxDiagnostics.h"
#include "Core/Log.h"
#include "Core/Localization.h"
#include "Platform/Windows/ExecutablePath.h"
#include "Platform/Windows/FrameTimer.h"
#include "Platform/Windows/Window.h"
#include <iostream>

int RunSandboxApp(const std::string& locale)
{
    Localization localization;
    const auto executableDirectory = GetExecutableDirectory();
    std::string error;
    if (executableDirectory.empty())
    {
        Log(LogLevel::Warning, "Cannot locate executable directory; displaying text keys");
    }
    else
    {
        const auto directory = executableDirectory / "Resources" / "Localization";
        if (!localization.Load(directory / (locale + ".txt"), error))
        {
            Log(LogLevel::Warning, "Language " + locale + ": " + error);
            if (locale != "en-US" && !localization.Load(directory / "en-US.txt", error))
                Log(LogLevel::Warning, "Fallback en-US: " + error);
        }
    }
    WindowState window{};
    std::cout << "[Info] RunSandboxApp 中 window 的地址：" << &window << '\n';
    if (!CreateEngineWindow(window))
    {
        return 1;
    }
    Log(LogLevel::Info, localization.GetText("Window.MessageLoopStarted"));

    FrameTimer timer{};
    if (!InitializeFrameTimer(timer))
    {
        Log(LogLevel::Error, localization.GetText("Timer.InitializeFailed"));
        DestroyEngineWindow(window);
        return 1;
    }

    int exitCode = 0;
    SandboxDiagnostics diagnostics(window.input);
    constexpr double maxUpdateDeltaSeconds = 0.1; // 应用选择的更新上限。
    while (PumpWindowMessages(exitCode))
    {
        if (!UpdateFrameTimer(timer))
        {
            Log(LogLevel::Error, localization.GetText("Timer.UpdateFailed"));
            exitCode = 1;
            break;
        }

        const double updateDeltaSeconds = timer.deltaSeconds > maxUpdateDeltaSeconds
            ? maxUpdateDeltaSeconds : timer.deltaSeconds;

        // 后续游戏更新也在此读取输入和 updateDeltaSeconds。
        diagnostics.ObserveInput(window.input);
        diagnostics.ObserveTiming(timer, updateDeltaSeconds);
        // 清理不依赖诊断组件；所有本轮消费者读完后才执行。
        window.input.ClearEvents();
        Sleep(1);
    }

    if (!DestroyEngineWindow(window))
    {
        return 1;
    }
    Log(LogLevel::Info, localization.GetText("Window.MessageLoopEnded"));
    return exitCode;
}

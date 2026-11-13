#include "SandboxApp.h"
#include "SandboxDiagnostics.h"
#include "CoreLayer/Log.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Profiler.h"
#include "FunctionLayer/Rendering/Renderer.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "PlatformLayer/Windows/ProcessDiagnostics.h"
#include "PlatformLayer/Windows/StartupProgress.h"
#include "PlatformLayer/Windows/FrameTimer.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/WindowsUiInput.h"
#include "FunctionLayer/Application/FrameData.h"
#include <iostream>
#include <exception>
#include <cmath>

namespace
{
struct ProfileFrame
{
    se::Profiler* profiler;bool begun;
    ProfileFrame(se::Profiler* value,std::uint64_t frame):profiler(value),begun(value&&value->BeginFrame(frame)){}
    void End(){if(begun){profiler->EndFrame();begun=false;}}
    ~ProfileFrame(){try{End();}catch(...){}}
};
// 窗口状态始终留在原地址；渲染器借用它的 HWND，退出时先结束渲染，再销毁窗口。
struct SandboxLifetime
{
    WindowState window{};
    Renderer& renderer;
    bool rendererStarted = false;
    bool shutdownFinished = false;
    std::unique_ptr<se::WindowsUiInput> uiInput;
    std::function<void()> windowClosing;

    explicit SandboxLifetime(Renderer& value) : renderer(value) {}
    ~SandboxLifetime() { Shutdown(); }
    bool Shutdown()
    {
        if(shutdownFinished)return true;
        shutdownFinished=true;
        if (rendererStarted)
        {
            rendererStarted = false;
            LogCheckpoint("Renderer.Shutdown.Begin");
            renderer.Shutdown();
            LogCheckpoint("Renderer.Shutdown.End");
        }
        uiInput.reset(); // 在 closing 回调可能释放借用的 UI 树之前结束输入适配器。
        window.messageHandler = {};
        if (windowClosing)
        {
            auto close = std::move(windowClosing);
            try { close(); }
            catch (const std::exception& exception)
            {
                Log(LogLevel::Error, std::string("窗口关闭清理异常: ") + exception.what());
                window.callbackFailed = true;
            }
            catch (...) { Log(LogLevel::Error, "窗口关闭清理异常"); window.callbackFailed = true; }
        }
        // 循环已经结束；此处正常销毁不应遗留 WM_QUIT 使下一次 Play 立即退出。
        window.postQuitOnDestroy = false;
        LogCheckpoint("Window.Destroy.Begin");
        const bool destroyed=DestroyEngineWindow(window);
        LogCheckpoint("Window.Destroy.End",destroyed?"success=1":"success=0");
        return destroyed;
    }
};
}

int RunSandboxApp(const std::string& locale, Renderer& renderer, bool showWindow, se::ApplicationServices* services)
{
    InitializeProcessDiagnostics("engine");
    LogCheckpoint("Application.Start");
    Localization localization;
    const auto executableDirectory = GetExecutableDirectory();
    std::string error;
    if (executableDirectory.empty() && (!services || services->resourceDirectory.empty()))
    {
        Log(LogLevel::Warning, "Cannot locate executable directory; displaying text keys");
    }
    else
    {
        const auto directory = (services && !services->resourceDirectory.empty()
            ? services->resourceDirectory : executableDirectory / "Resources") / "Localization";
        if (!localization.Load(directory / (locale + ".txt"), error))
        {
            Log(LogLevel::Warning, "Language " + locale + ": " + error);
            if (locale != "en-US" && !localization.Load(directory / "en-US.txt", error))
                Log(LogLevel::Warning, "Fallback en-US: " + error);
        }
    }
    if(services&&((!services->initialWidth)!=(!services->initialHeight)||services->initialWidth>16384||services->initialHeight>16384
        ||!std::isfinite(services->fixedDeltaSeconds)||services->fixedDeltaSeconds<0||services->fixedDeltaSeconds>0.1))
    {Log(LogLevel::Error,localization.GetText("App.ServicesInvalid"));return 1;}
    SandboxLifetime lifetime(renderer);
    WindowState& window = lifetime.window;
    if (IsDebugLoggingEnabled())
        std::cout << "[Debug] RunSandboxApp 中 window 的地址：" << &window << '\n';
    LogCheckpoint("Window.Create.Begin");
    bool startupLoading=se::StartupProgressActive();
    if (!CreateEngineWindow(window, showWindow&&!startupLoading))
    {
        return 1;
    }

    LogCheckpoint("Window.Create.End");
    int exitCode = 0;
    bool loopStarted = false;
    try
    {
        if (services) lifetime.windowClosing = services->windowClosing;
        if(services&&services->initialWidth&&!SetEngineWindowClientSize(window,services->initialWidth,services->initialHeight))
        {Log(LogLevel::Error,localization.GetText("App.ServicesInvalid"));return 1;}
        if (services && services->windowOpened && !services->windowOpened(window.hwnd)) return 1;
        if (services && services->ui)
        {
            services->ui->Layout(float(window.width),float(window.height));
            lifetime.uiInput = std::make_unique<se::WindowsUiInput>(*services->ui);
        }
        window.messageHandler = [&](HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) -> std::optional<LRESULT> {
            if (services && services->messageHandler) { auto result = services->messageHandler(hwnd,msg,wp,lp); if (result) return result; }
            return lifetime.uiInput ? lifetime.uiInput->Process(hwnd,msg,wp,lp) : std::nullopt;
        };
        lifetime.rendererStarted = true;
        const RenderSurface surface{ window.hwnd, window.width, window.height };
        LogCheckpoint("Renderer.Initialize.Begin");
        if(!se::ReportStartup(se::StartupStage::Renderer))return 1;
        if (!renderer.Initialize(surface, localization))
        {
            Log(LogLevel::Error, localization.GetText("App.RendererInitFailed"));
            return 1;
        }
        LogCheckpoint("Renderer.Initialize.End");
        if(!se::ReportStartup(se::StartupStage::FirstFrame))return 1;
        FrameTimer timer{};
        if (!InitializeFrameTimer(timer))
        {
            Log(LogLevel::Error, localization.GetText("Timer.InitializeFailed"));
            return 1;
        }

        Log(LogLevel::Info, localization.GetText(showWindow
            ? "Window.MessageLoopStarted" : "Window.HiddenMessageLoopStarted"));
        loopStarted = true;
        if(services&&services->fixedDeltaSeconds>0)Log(LogLevel::Info,localization.GetText("Sample.FixedStep")+": "+std::to_string(services->fixedDeltaSeconds));
        SandboxDiagnostics diagnostics(window.input);
        constexpr double maxUpdateDeltaSeconds = 0.1;
        while (PumpWindowMessages(exitCode) && !window.closeRequested)
        {
            if (!UpdateFrameTimer(timer))
            {
                Log(LogLevel::Error, localization.GetText("Timer.UpdateFailed"));
                exitCode = 1;
                break;
            }
            const double updateDeltaSeconds = services&&services->fixedDeltaSeconds>0?services->fixedDeltaSeconds:
                timer.deltaSeconds > maxUpdateDeltaSeconds ? maxUpdateDeltaSeconds : timer.deltaSeconds;
            auto* profiler=services?services->profiler:nullptr;
            ProfileFrame profileFrame(profiler,timer.frameCount);
            {
            se::CpuScope update(profiler,"CPU.Update");
            if (services)
            {
                if (services->ui) { services->ui->Layout(float(window.width),float(window.height)); if(services->advanceUi) services->ui->Update(updateDeltaSeconds); }
                if (services->input) services->input(window.input,
                    (lifetime.uiInput && lifetime.uiInput->KeyboardConsumed()) || (services->captureKeyboard && services->captureKeyboard()),
                    (lifetime.uiInput && lifetime.uiInput->PointerConsumed()) || (services->capturePointer && services->capturePointer()));
                if (lifetime.uiInput) lifetime.uiInput->EndFrame();
                if(services->runEnvironmentFrame){if(!services->runEnvironmentFrame(updateDeltaSeconds)){exitCode=1;break;}}
                else if (services->update && !services->update(updateDeltaSeconds)) { exitCode = 1; break; }
                if (services->ui) services->ui->Layout(float(window.width),float(window.height));
                if (lifetime.uiInput) {lifetime.uiInput->SynchronizeCapture(window.hwnd);lifetime.uiInput->PositionIme(window.hwnd);}
            }
            }
            diagnostics.ObserveInput(window.input);
            diagnostics.ObserveTiming(timer,updateDeltaSeconds,services&&services->fixedDeltaSeconds>0,&localization);
            RenderResult result = RenderResult::Success;
            if (window.drawable)
            {
                se::FrameData data;
                {se::CpuScope extract(profiler,"CPU.Extract");if (services && services->extract) services->extract(data);}
                const RenderFrame frame{
                    { window.hwnd, window.width, window.height },
                    updateDeltaSeconds, timer.deltaSeconds, timer.totalSeconds, timer.frameCount, services ? &data : nullptr,profiler };
                const bool trace=SampleDiagnosticFrame(timer.frameCount);
                if(trace)LogCheckpoint("Renderer.Frame.Begin",{},timer.frameCount);
                result = renderer.Render(frame, localization);
                if(trace||result==RenderResult::Failed)LogCheckpoint("Renderer.Frame.End","result="+std::to_string(static_cast<int>(result)),timer.frameCount);
            }
            window.input.ClearEvents();
            if (result == RenderResult::Failed)
            {
                Log(LogLevel::Error, localization.GetText("App.RendererFrameFailed"));
                exitCode = 1;
                break;
            }
            if(startupLoading&&window.drawable)
            {
                if(se::StartupCancelled())break;
                // 隐藏窗口的 Present 可以返回 Occluded；此时首帧命令仍已成功提交。
                // 先显示已绘制的主窗口，再通知监督进程收起加载窗口。
                if(showWindow){ShowWindow(window.hwnd,SW_SHOW);SetForegroundWindow(window.hwnd);}
                if(!se::ReportStartup(se::StartupStage::Ready))break;
                startupLoading=false;
            }
            if (services && services->maximumFrames && timer.frameCount >= services->maximumFrames) break;
            profileFrame.End();
            Sleep(!window.drawable || result == RenderResult::Occluded ? 16 : 1);
        }
    }
    catch (const std::exception& exception)
    {
        Log(LogLevel::Error, localization.GetText("App.RendererException") + ": " + exception.what());
        exitCode = 1;
    }
    catch (...)
    {
        Log(LogLevel::Error, localization.GetText("App.RendererException"));
        exitCode = 1;
    }
    if (window.callbackFailed) { Log(LogLevel::Error, "窗口回调失败，退出码置 1"); exitCode = 1; }
    if (!lifetime.Shutdown()) { Log(LogLevel::Error, "窗口销毁失败，退出码置 1"); exitCode = 1; }
    if (window.callbackFailed) exitCode = 1;
    if (loopStarted) Log(LogLevel::Info, localization.GetText("Window.MessageLoopEnded"));
    LogCheckpoint("Application.End","exit="+std::to_string(exitCode));
    return exitCode;
}

#pragma once
#include "PlatformLayer/Windows/StartupProgress.h"
#include <atomic>
#include <filesystem>
#include <memory>

class Localization;
namespace se
{
// CPU 绘制的独立线程窗口：主线程编译/读资产/创建 GPU 时仍能刷新与响应取消。
class EditorLoadingScreen
{
public:
    EditorLoadingScreen(const Localization&,const std::filesystem::path& project,bool hidden);
    ~EditorLoadingScreen();
    void Restart();
    std::atomic<bool>& Cancellation() noexcept {return cancelled_;}
    bool Cancelled() const noexcept {return cancelled_.load();}
    StartupSnapshot Snapshot() const noexcept {return channel_.Read();}
    HWND Window() const noexcept;
private:
    struct Impl;
    void Stop();
    StartupProgressChannel channel_;
    std::atomic<bool> cancelled_=false;
    std::unique_ptr<Impl> impl_;
};
}

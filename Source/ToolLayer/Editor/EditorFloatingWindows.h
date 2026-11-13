#pragma once
#include "EditorUi/EditorShell.h"
#include "EditorUi/EditorInputAdapter.h"
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Rendering/UiPaint.h"
#include "PlatformLayer/Windows/UiDirectWriteTextService.h"
#include "PlatformLayer/InputState.h"
#include <functional>
#include <memory>
#include <optional>

namespace se
{
// 一个项目、一份 UI 文档。每个 HWND 只拥有自己的呈现与输入适配资源。
class EditorFloatingWindows
{
public:
    using MessageHandler=std::function<std::optional<LRESULT>(HWND,UINT,WPARAM,LPARAM)>;
    EditorFloatingWindows(HWND owner,ui::editor::EditorShell& shell,const Localization& text,
        std::filesystem::path resources,MessageHandler handler,bool visible=true);
    ~EditorFloatingWindows();
    bool Synchronize();
    bool Render(const FrameData& frame,ui::UiImageProvider& images,const ui::UiDisplayList& overlay);
    [[nodiscard]] bool Owns(HWND window)const;
    [[nodiscard]] bool Empty()const;
    [[nodiscard]] ui::editor::EditorInputAdapter* Input(HWND window)const;
    // 拖动期间按屏幕位置找目标窗口，转换为目标客户区物理像素。
    HWND RoutePointer(HWND source,UINT message,LPARAM& position);
    void SelectSurface(HWND window);
    void RequestFloat(ui::editor::EditorPanelId panel);
    [[nodiscard]] HWND Window(std::uint32_t id)const;
    [[nodiscard]] const InputState* GameplayInput(float& dpi)const;
    void SetCaptureOutput(std::uint32_t id,const std::filesystem::path& path,std::uint64_t frame);
    void CaptureNext(std::uint32_t id,const std::filesystem::path& path);
private:
    struct Entry;
    static LRESULT CALLBACK WindowProc(HWND,UINT,WPARAM,LPARAM);
    std::optional<LRESULT> Message(Entry&,UINT,WPARAM,LPARAM);
    void Release(Entry&)noexcept;
    HWND owner_;
    ui::editor::EditorShell& shell_;
    const Localization& text_;
    std::filesystem::path resources_;
    MessageHandler handler_;
    bool visible_=true,shuttingDown_=false;
    POINT dropPoint_{};
    std::vector<std::unique_ptr<Entry>> entries_;
};
}

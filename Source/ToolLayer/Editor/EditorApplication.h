#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <functional>
#include "EditorExternalDocuments.h"

class Renderer;
struct InputState;
struct ImGuiContext;
namespace se
{
class World;class UiTree;class EditorSession;class ImGuiEditor;struct FrameData;struct EditorFrameResult;
struct EditorApplicationHooks
{
    std::filesystem::path resourceDirectory;
    std::function<bool(EditorSession&,UiTree&)> initialize;
    std::function<void(ImGuiEditor&)> configure;
    std::function<bool(World&,UiTree&)> playStarted;
    std::function<void(World&,UiTree&)> playStopping;
    std::function<bool(World&,UiTree&,double)> update;
    std::function<bool(World&,UiTree&)> singleStep;
    std::function<bool(EditorSession&,double)> frame;
    std::function<void(const InputState&,bool,bool)> input;
    std::function<void(World&,UiTree&,FrameData&)> extract;
    std::function<void(std::uint64_t,bool)> inspector;
    EditorExternalDocuments externalDocuments;
    // 每帧绘制前的 Tool 内部观察口；自检入口用它驱动真实导出面板状态机，不参与正常编辑流程。
    std::function<void(ImGuiEditor&,EditorSession&)> operationFrame;
    std::function<void(EditorSession&,UiTree&)> closing;
    std::function<void(ImGuiContext*,const EditorFrameResult&)> drawn; // 本帧 UI 观察端口；不得保留借用数据。
    bool startPlaying=false;
    std::string editorUi="toolkit";
};
// 通过公共窗口宿主驱动编辑器；Tool 复制 ImGui 数据，渲染器消费自有 GUI 几何。
int RunEditorApp(Renderer& renderer, const std::string& locale,
    const std::filesystem::path& projectRoot, bool hidden = false, std::uint64_t maximumFrames = 0,
    EditorApplicationHooks* hooks = nullptr);
}

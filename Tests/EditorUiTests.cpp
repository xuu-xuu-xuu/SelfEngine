#include "FunctionLayer/UI/Core/UiInput.h"
#include "ToolLayer/Editor/EditorUi/EditorChrome.h"
#include "ToolLayer/Editor/EditorUi/EditorCommand.h"
#include "ToolLayer/Editor/EditorUi/EditorCommandRouter.h"
#include "ToolLayer/Editor/EditorUi/EditorContext.h"
#include "ToolLayer/Editor/EditorUi/EditorDock.h"
#include "ToolLayer/Editor/EditorUi/EditorDockState.h"
#include "ToolLayer/Editor/EditorUi/EditorInputAdapter.h"

#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace se::ui;
using namespace se::ui::editor;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct FakeContext final : EditorContext
{
    EditorProjectInfo Project() const override { return {"Demo", "D:/Demo"}; }
    EditorDocumentInfo Document() const override { return {"Scenes/Main.scene.json", "Main", true}; }
    std::vector<EditorSelection> Selection() const override { return {{7, "", "hierarchy"}}; }
    std::optional<EditorAssetInfo> Asset(std::string_view path) const override
    {
        if (path == "Textures/a.png") return EditorAssetInfo{"Textures/a.png", "texture"};
        return std::nullopt;
    }
    EditorSettingsInfo Settings() const override { return {"en-US", 1.5f}; }
    EditorLayoutInfo Layout() const override { return {0.2f, 0.56f, 0.24f}; }
};

void ContextAndCommands()
{
    FakeContext context;
    Check(context.Project().name == "Demo", "context project");
    Check(context.Document().dirty, "context document");
    Check(context.Selection().size() == 1 && context.Selection()[0].entity == 7, "context selection");
    Check(context.Asset("Textures/a.png")->type == "texture", "context asset");
    Check(!context.Asset("missing").has_value(), "context missing asset");
    Check(context.Settings().language == "en-US" && context.Layout().rightWidth == 0.24f, "context settings");

    EditorCommandBus bus;
    int saves = 0;
    bus.Register(EditorCommandId::Save, [&saves](const EditorCommand& command) {
        ++saves;
        Check(command.argument.empty() || command.argument == "now", "command argument");
        return true;
    });
    Check(bus.CanExecute(EditorCommandId::Save) && !bus.CanExecute(EditorCommandId::Export), "command availability");
    Check(bus.Execute({EditorCommandId::Save, "now"}) && saves == 1, "command execute");
    Check(!bus.Execute({EditorCommandId::Export, {}}), "unknown command rejected");

    EditorCommandRouter router(bus);
    router.RegisterShortcut('S', true, false, false, EditorCommandId::Save, "now");
    router.RegisterShortcut('Z', true, true, false, EditorCommandId::Redo);
    UiInputEvent key;
    key.type = UiEventType::KeyDown;
    key.key = 'S';
    key.control = true;
    Check(router.RouteKey(key, false) && saves == 2, "shortcut executes");
    Check(!router.RouteKey(key, true), "text field yields shortcut");
    Check(router.HasShortcut('S', true, false, false), "shortcut lookup");
    Check(!router.HasShortcut('S', false, false, false), "shortcut mismatch");
    Check(router.Trigger(EditorCommandId::Save, "now") && saves == 3, "trigger same command");

    EditorChrome chrome(router, bus);
    chrome.AddMenu("File", "Save", EditorCommandId::Save);
    chrome.AddToolbar("Save", EditorCommandId::Save);
    chrome.SetStatus("Ready");
    Check(chrome.ActivateMenu(0) && saves == 4, "menu command");
    Check(chrome.ActivateToolbar(0) && saves == 5, "toolbar command");
    Check(chrome.Status() == "Ready" && chrome.Menus().size() == 1 && chrome.Toolbar().size() == 1, "chrome state");
}

void DockAndPersistence()
{
    EditorDock dock;
    const DockNode& root = dock.Root();
    Check(root.kind == DockNode::Kind::Split && root.axis == DockAxis::Horizontal && root.children.size() == 2,
          "dock default split");
    Check(std::abs(root.ratio - 0.2f) < 1e-5f, "dock left ratio");
    Check(root.children[0].kind == DockNode::Kind::Split && root.children[0].axis == DockAxis::Vertical,
          "dock left split");
    Check(std::abs(root.children[0].ratio - 0.66f) < 1e-5f, "dock left split ratio");
    Check(root.children[0].children[0].tabs.size() == 1 &&
              root.children[0].children[0].active == EditorPanelId::Hierarchy,
          "dock hierarchy tab");
    Check(root.children[0].children[1].tabs.size() == 1 &&
              root.children[0].children[1].active == EditorPanelId::Assets,
          "dock assets tab");
    Check(root.children[1].kind == DockNode::Kind::Split && root.children[1].axis == DockAxis::Horizontal &&
              std::abs(root.children[1].ratio - 0.7f) < 1e-5f,
          "dock center split");
    Check(root.children[1].children[0].tabs.size() == 2, "dock center tabs");
    Check(root.children[1].children[1].kind == DockNode::Kind::Split &&
              root.children[1].children[1].axis == DockAxis::Vertical,
          "dock right split");
    Check(dock.Find(EditorPanelId::Inspector) != nullptr, "dock find inspector");
    Check(dock.SelectTab(EditorPanelId::Assets) && dock.Find(EditorPanelId::Assets)->active == EditorPanelId::Assets,
          "dock select tab");
    Check(dock.CloseTab(EditorPanelId::Assets), "dock close tab");
    Check(dock.Find(EditorPanelId::Assets) == nullptr &&
              root.children[0].children[1].active == EditorPanelId::None,
          "dock close adjusts active");
    Check(dock.OpenTab(EditorPanelId::Assets) && dock.Find(EditorPanelId::Assets)->active == EditorPanelId::Assets,
          "dock reopen tab");
    Check(dock.DragSplitter({1}, 100.0f, 1000.0f, 50.0f, 50.0f), "dock drag splitter");
    Check(std::abs(dock.Root().children[1].ratio - 0.8f) < 1e-5f, "dock splitter ratio");

    // 不写仓库 out/：该目录的 ACL 在部分机器上只允许读；也不依赖用户 Temp 的 ACL。
    const std::filesystem::path path = ".cache/editor-ui-test/dock.json";
    std::filesystem::create_directories(path.parent_path());
    std::string error;
    Check(EditorDockState::Save(dock.Root(), path, error), error.c_str());
    DockNode loaded;
    Check(EditorDockState::Load(path, loaded, error), error.c_str());
    Check(loaded == dock.Root(), "dock roundtrip");

    std::ofstream(path, std::ios::binary | std::ios::trunc) << "{ broken";
    DockNode fallback;
    Check(!EditorDockState::Load(path, fallback, error), "bad dock rejected");
    Check(fallback == DefaultEditorDockLayout(), "bad dock falls back");
    Check(std::filesystem::exists(path.string() + ".bak"), "bad dock backup");
}

void InputAdapter()
{
    EditorInputAdapter adapter(2.0f);
    Check(adapter.DpiScale() == 2.0f, "adapter dpi");
    UiInputEvent event;
    const LPARAM point = MAKELPARAM(200, 100);
    Check(adapter.Translate(WM_MOUSEMOVE, 0, point, event), "adapter move");
    Check(event.type == UiEventType::PointerMove && event.x == 100.0f && event.y == 50.0f, "adapter dpi scale");
    Check(adapter.Translate(WM_LBUTTONDOWN, MK_LBUTTON, point, event) &&
              event.type == UiEventType::PointerDown, "adapter down");
    Check(adapter.Translate(WM_KEYDOWN, 'A', 0, event) && event.type == UiEventType::KeyDown && event.key == 'A',
          "adapter key");
    Check(adapter.Translate(WM_CHAR, U'中', 0, event) && event.type == UiEventType::TextInput &&
              event.codepoint == U'中', "adapter char");
    Check(adapter.Translate(WM_MOUSEWHEEL, MAKEWPARAM(0, 120), point, event) &&
              event.type == UiEventType::PointerWheel && event.wheel == 1.0f, "adapter wheel");
    adapter.SetImeComposition("中文");
    Check(adapter.Translate(WM_IME_COMPOSITION, 0, 0, event) && event.type == UiEventType::ImeComposition &&
              event.text == "中文", "adapter IME composition");
    Check(adapter.Translate(WM_IME_STARTCOMPOSITION, 0, 0, event) &&
              event.type == UiEventType::ImeComposition, "adapter IME start");
    Check(!adapter.Translate(WM_CHAR, U'X', 0, event), "adapter suppresses WM_CHAR during composition");
    Check(adapter.Translate(WM_IME_ENDCOMPOSITION, 0, 0, event) && event.type == UiEventType::ImeCancel,
          "adapter IME end");
    Check(adapter.Translate(WM_CHAR, U'X', 0, event) && event.type == UiEventType::TextInput,
          "adapter WM_CHAR after composition");
    Check(adapter.Translate(WM_IME_CHAR, U'中', 0, event) && event.type == UiEventType::TextInput,
          "adapter WM_IME_CHAR fallback without window");
    adapter.SetImeCandidateAnchor({10.0f, 20.0f, 30.0f, 40.0f});
    Check(adapter.CandidateAnchor().x == 10.0f && adapter.CandidateAnchor().height == 40.0f, "adapter IME anchor");
    Check(!adapter.Translate(WM_CLOSE, 0, 0, event), "adapter ignores unknown message");
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        ContextAndCommands();
        DockAndPersistence();
        InputAdapter();

        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            std::ofstream(proof)
                << "{\"success\":true,\"context\":true,\"commands\":true,\"router\":true,"
                   "\"chrome\":true,\"dock\":true,\"persistence\":true,\"inputAdapter\":true}\n";
        }
        std::cout << "EditorUiTests passed (context, commands, router, chrome, dock, persistence, Win32 adapter).\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}

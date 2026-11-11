#include "FunctionLayer/UI/Core/UiInput.h"
#include "ToolLayer/Editor/EditorSession.h"
#include "ToolLayer/Editor/EditorUi/EditorShell.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace se;
using namespace se::ui;
using namespace se::ui::editor;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void PanelSessionSmoke()
{
    // out/ 在部分机器上只读；测试夹具改用工作区内的 .cache。
    const std::filesystem::path directory = ".cache/editor-panel-session";
    std::filesystem::create_directories(directory);
    const std::filesystem::path scene = directory / "panel.scene.json";

    EditorSession session;
    std::string error;
    Check(session.NewDefaultScene(error), error.c_str());
    const auto entities = session.EditWorld().Entities();
    Check(!entities.empty(), "session entities");
    const EntityRecord* record = session.EditWorld().Get(entities.front());
    Check(record != nullptr, "session record");
    const StableEntityId id = record->stableId;
    const std::string original = record->name;
    Check(session.Select(id), "session select");

    EditorShell shell;
    Check(shell.Initialize(1280.0f, 720.0f), "shell initialize");
    shell.SetSelectionHandler([&session](std::uint64_t selected) { session.Select(selected); });
    shell.SetPropertyHandler([&session](std::uint64_t target, std::string key, std::string value) {
        if (key != "name") return false;
        if (!session.BeginEdit("Rename")) return false;
        if (!session.Rename(target, value)) { session.CancelEdit(); return false; }
        return session.CommitEdit();
    });
    shell.SetHierarchy({{id, 0, 0, false, true, original}});
    shell.SetInspector(id, original, {
        {"name", "Name", EditorPropertyType::String, original, false},
    });
    shell.Frame();

    const UiRect content = shell.Layout().Rect(shell.InspectorContent());
    UiInputEvent click;
    click.type = UiEventType::PointerDown;
    click.x = content.x + 16.0f;
    click.y = content.y + 8.0f;
    Check(shell.Dispatcher().Dispatch(click), "property click");
    UiInputEvent text;
    text.type = UiEventType::TextInput;
    text.codepoint = U'X';
    Check(shell.Dispatcher().Dispatch(text), "property text");
    UiInputEvent enter;
    enter.type = UiEventType::KeyDown;
    enter.key = UiKeyEnter;
    Check(shell.Dispatcher().Dispatch(enter), "property enter");

    const std::string edited = "X" + original;
    const EntityRecord* changed = session.EditWorld().Get(session.EditWorld().FindByStableId(id));
    Check(changed && changed->name == edited, "shell edit applied");
    Check(session.Save(scene), "session save");
    Check(session.Undo(), "session undo");
    changed = session.EditWorld().Get(session.EditWorld().FindByStableId(id));
    Check(changed && changed->name == original, "session undo rename");
    Check(session.Redo(), "session redo");
    Check(session.Save(scene), "session save edited");

    EditorSession reopened;
    Check(reopened.Open(scene, true), "session reopen");
    const EntityRecord* restored = reopened.EditWorld().Get(reopened.EditWorld().FindByStableId(id));
    Check(restored && restored->name == edited, "session save/reopen name");
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        PanelSessionSmoke();
        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            std::ofstream(proof)
                << "{\"success\":true,\"shellEdit\":true,\"undoRedo\":true,\"saveReopen\":true}\n";
        }
        std::cout << "EditorPanelSessionTests passed (shell edit, undo/redo, save/reopen).\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}

#include "ToolLayer/Editor/EditorUi/EditorCommand.h"
#include "ToolLayer/Editor/EditorUi/EditorPanels.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace se::ui::editor;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void Hierarchy()
{
    EditorHierarchyModel model;
    model.SetNodes({
        {1, 0, 0, true, true, "Root"},
        {2, 1, 1, true, true, "Child"},
        {3, 2, 2, false, true, "Grandchild"},
        {4, 0, 0, false, true, "Second"},
    });
    Check(model.VisibleRows().size() == 4, "hierarchy visible");
    Check(model.Toggle(0), "hierarchy collapse");
    const auto collapsed = model.VisibleRows();
    Check(collapsed.size() == 2 && collapsed[0] == 0 && collapsed[1] == 3, "hierarchy collapsed rows");
    model.Toggle(0);
    Check(model.Select(3) && model.Selected() == 3, "hierarchy select");
    Check(!model.Select(99), "hierarchy invalid select");
    Check(model.Rename(2, "Renamed"), "hierarchy rename");
    Check(!model.Reparent(1, 2), "hierarchy cycle rejected");
    Check(model.Reparent(2, 0), "hierarchy reparent");
}

void Inspector()
{
    EditorInspectorModel inspector;
    inspector.SetTarget(7, "Player");
    inspector.SetProperties({
        {"name", "Name", EditorPropertyType::String, "Player", false},
        {"speed", "Speed", EditorPropertyType::Number, "1.0", false},
        {"id", "ID", EditorPropertyType::String, "7", true},
    });
    Check(inspector.Target() == 7 && inspector.TargetLabel() == "Player", "inspector target");
    Check(inspector.SetValue("speed", "2.0") && inspector.ConsumeChanged(), "inspector edit");
    Check(!inspector.ConsumeChanged(), "inspector edit consumed");
    Check(inspector.LastChangedKey() == "speed", "inspector key");
    Check(!inspector.SetValue("id", "8"), "inspector read only");
    Check(!inspector.SetValue("missing", "x"), "inspector unknown");
}

void Assets()
{
    EditorAssetsModel assets;
    std::vector<EditorAssetEntry> entries;
    for (int index = 0; index < 100; ++index)
    {
        entries.push_back({"Textures/tex" + std::to_string(index) + ".png", "texture"});
    }
    assets.SetEntries(std::move(entries));
    Check(assets.Filtered().size() == 100, "assets all");
    assets.SetFilter("tex1");
    Check(assets.Filtered().size() == 11, "assets filter");
    assets.SetFilter("");
    const auto visible = assets.VisibleRange(100, 200.0f, 20.0f, 2);
    Check(visible.size() == 14, "assets visible range with buffer");
    assets.Select("Textures/tex1.png");
    Check(assets.Selected("Textures/tex1.png"), "assets selected");

    EditorCommandBus bus;
    int properties = 0;
    bus.Register(EditorCommandId::SetProperty, [&properties](const EditorCommand&) {
        ++properties;
        return true;
    });
    Check(bus.Execute({EditorCommandId::SetProperty, "speed"}) && properties == 1, "property command");
    Check(std::string(EditorCommandName(EditorCommandId::SetProperty)) == "set-property", "property command name");
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        Hierarchy();
        Inspector();
        Assets();
        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            std::ofstream(proof)
                << "{\"success\":true,\"hierarchy\":true,\"inspector\":true,\"assets\":true,\"commands\":true}\n";
        }
        std::cout << "EditorPanelsTests passed (hierarchy/inspector/assets/commands).\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}

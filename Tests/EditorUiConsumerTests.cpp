#include "ToolLayer/Editor/EditorUi/EditorCommand.h"
#include "ToolLayer/Editor/EditorUi/EditorContext.h"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace se::ui::editor;

struct ConsumerContext final : EditorContext
{
    EditorProjectInfo Project() const override { return {"Consumer", "D:/Consumer"}; }
    EditorDocumentInfo Document() const override { return {"Main.scene.json", "Main", false}; }
    std::vector<EditorSelection> Selection() const override { return {}; }
    std::optional<EditorAssetInfo> Asset(std::string_view) const override { return std::nullopt; }
    EditorSettingsInfo Settings() const override { return {}; }
    EditorLayoutInfo Layout() const override { return {}; }
};
} // namespace

int main()
{
    try
    {
        ConsumerContext context;
        if (context.Project().name != "Consumer" || context.Document().dirty)
        {
            throw std::runtime_error("consumer context mismatch");
        }
        EditorCommandBus bus;
        bool executed = false;
        bus.Register(EditorCommandId::Save, [&executed](const EditorCommand&) {
            executed = true;
            return true;
        });
        if (!bus.Execute({EditorCommandId::Save, {}}) || !executed)
        {
            throw std::runtime_error("consumer command failed");
        }
        std::cout << "EditorUiConsumerTests passed (public EditorContext/EditorCommand only).\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}

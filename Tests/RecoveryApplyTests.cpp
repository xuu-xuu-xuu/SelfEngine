#include "ToolLayer/Editor/RecoveryApply.h"
#include <filesystem>
#include <fstream>
#include <iostream>

#define CHECK(condition, message) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " << message << " (" << #condition << ")\n"; return 1; } } while (false)

int main()
{
    const auto source = std::filesystem::path("Resources/Templates/BlankScene/Scenes/Main.scene.json");
    std::ifstream input(source, std::ios::binary);
    CHECK(static_cast<bool>(input), "blank scene exists");
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK(!content.empty(), "blank scene content");

    const auto directory = std::filesystem::path(".cache/recovery-apply-test");
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);

    se::RecoveryDocument scene;
    scene.documentId = "scene";
    scene.kind = "scene";
    scene.relativePath = "Scenes/Main.scene.json";
    scene.content = content;
    se::RecoveryGroup group;
    group.id = "group-1";
    group.documents.push_back(scene);

    se::EditorSession session;
    std::string message;
    if (!se::ApplyRecoveryScene(group, directory, session, message))
    {
        std::cerr << "apply error: " << message << '\n';
        return 1;
    }
    CHECK(session.Path().filename() == "recovered.scene.json", "recovered path");
    CHECK(!session.Dirty(), "recovered scene is clean");

    se::RecoveryGroup empty;
    empty.id = "empty";
    CHECK(!se::ApplyRecoveryScene(empty, directory, session, message), "missing scene rejected");
    CHECK(message == "Editor.RecoveryNoScene", "missing scene diagnostic");

    std::filesystem::remove_all(directory, error);
    std::cout << "RecoveryApplyTests passed (scene snapshot applied, missing scene rejected).\n";
    return 0;
}

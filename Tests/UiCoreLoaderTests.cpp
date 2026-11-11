#include "FunctionLayer/UI/Core/UiDocumentLoader.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using namespace se::ui;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

const char* kValidJson = R"json({
  "version": 1,
  "styles": {
    "#hud": { "background": "#14161AEE", "padding": [12, 16] },
    ".title": { "font-size": 18 }
  },
  "root": {
    "type": "panel",
    "id": "root",
    "classes": ["screen"],
    "children": [
      { "type": "label", "id": "title", "classes": ["title"], "children": [] },
      { "type": "button", "id": "start", "classes": [] }
    ]
  }
})json";

void ValidTree()
{
    UiDocument document;
    const UiDocumentLoadResult result = LoadUiDocumentJson(kValidJson, document);
    Check(result.success, "valid JSON loads");
    Check(result.error.empty(), "valid JSON has no error");
    Check(document.ElementCount() == 3, "root plus two children");
    Check(document.RootElement()->Kind() == UiElementKind::Panel, "root kind");
    Check(document.RootElement()->Name() == "root", "root id");
    Check(document.HasClass(document.Root(), "screen"), "root class");
    const UiElement* title = document.FindByName("title");
    Check(title != nullptr, "find title");
    Check(title->Kind() == UiElementKind::Label, "title kind");
    Check(title->Parent() == document.Root(), "title parent");
    Check(document.HasClass(title->Handle(), "title"), "title class");
    const UiElement* start = document.FindByName("start");
    Check(start != nullptr && start->Kind() == UiElementKind::Button, "start button");
    Check(document.ChildCount(document.Root()) == 2, "child count");
}

void BadInputPreservesDocument()
{
    UiDocument document;
    Check(LoadUiDocumentJson(kValidJson, document).success, "seed document");
    const UiElementHandle title = document.FindByName("title")->Handle();
    Check(document.SetText(title, "Score"), "seed text");
    const std::size_t count = document.ElementCount();

    const UiDocumentLoadResult badSyntax = LoadUiDocumentJson("{", document);
    Check(!badSyntax.success && !badSyntax.error.empty(), "bad syntax rejected");
    Check(document.ElementCount() == count && document.FindByName("title") != nullptr, "bad syntax preserves tree");
    Check(document.Text(title) == "Score", "bad syntax preserves text");

    const UiDocumentLoadResult badVersion = LoadUiDocumentJson(R"({"version":2,"root":{"type":"panel"}})", document);
    Check(!badVersion.success, "bad version rejected");
    const UiDocumentLoadResult missingRoot = LoadUiDocumentJson(R"({"version":1})", document);
    Check(!missingRoot.success, "missing root rejected");
    const UiDocumentLoadResult unknownType = LoadUiDocumentJson(
        R"({"version":1,"root":{"type":"panel","children":[{"type":"unknown"}]}})", document);
    Check(!unknownType.success && unknownType.error.find("unsupported node type") != std::string::npos, "unknown type rejected");
    const UiDocumentLoadResult duplicateId = LoadUiDocumentJson(
        R"({"version":1,"root":{"type":"panel","children":[{"type":"label","id":"dup"},{"type":"label","id":"dup"}]}})",
        document);
    Check(!duplicateId.success, "duplicate id rejected");
    const UiDocumentLoadResult stylesArray = LoadUiDocumentJson(
        R"({"version":1,"styles":[],"root":{"type":"panel"}})", document);
    Check(!stylesArray.success, "styles array rejected");
    const UiDocumentLoadResult stylesEntry = LoadUiDocumentJson(
        R"({"version":1,"styles":{"x":1},"root":{"type":"panel"}})", document);
    Check(!stylesEntry.success, "non-object style entry rejected");
    const UiDocumentLoadResult childrenObject = LoadUiDocumentJson(
        R"({"version":1,"root":{"type":"panel","children":{}}})", document);
    Check(!childrenObject.success, "children object rejected");
    const UiDocumentLoadResult idNumber = LoadUiDocumentJson(
        R"({"version":1,"root":{"type":"panel","id":7}})", document);
    Check(!idNumber.success, "numeric id rejected");

    Check(document.ElementCount() == count, "all failures preserve element count");
    Check(document.FindByName("title") != nullptr && document.Text(title) == "Score", "all failures preserve content");
}

void DepthLimit()
{
    std::string json = R"({"version":1,"root":{"type":"panel","children":[)";
    for (int i = 0; i < 300; ++i)
    {
        json += R"({"type":"panel","children":[)";
    }
    for (int i = 0; i < 300; ++i)
    {
        json += "]}";
    }
    json += "]}}";

    UiDocument document;
    const UiDocumentLoadResult result = LoadUiDocumentJson(json, document);
    Check(!result.success, "deep tree rejected");
    Check(result.error.find("depth") != std::string::npos, "deep tree reports depth");
    Check(document.ElementCount() == 1, "deep failure preserves empty document");
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        ValidTree();
        BadInputPreservesDocument();
        DepthLimit();

        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            std::ofstream(proof)
                << "{\"success\":true,\"validTree\":true,\"schemaValidation\":true,"
                   "\"preserveOnFailure\":true,\"depthLimit\":true,\"stylesNotCascaded\":true}\n";
        }
        std::cout << "UiCoreLoaderTests passed (JSON v1 tree, schema errors preserve old document, depth limit).\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}

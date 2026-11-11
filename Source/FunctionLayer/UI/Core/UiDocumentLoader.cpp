#include "FunctionLayer/UI/Core/UiDocumentLoader.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <exception>
#include <string>
#include <utility>

namespace se::ui
{
namespace
{
constexpr int kMaxDepth = 256;
constexpr std::size_t kMaxNodes = 100000;

bool ParseKind(std::string_view value, UiElementKind& kind) noexcept
{
    if (value == "panel")
    {
        kind = UiElementKind::Panel;
    }
    else if (value == "label")
    {
        kind = UiElementKind::Label;
    }
    else if (value == "button")
    {
        kind = UiElementKind::Button;
    }
    else if (value == "image")
    {
        kind = UiElementKind::Image;
    }
    else if (value == "checkbox")
    {
        kind = UiElementKind::Checkbox;
    }
    else if (value == "combo")
    {
        kind = UiElementKind::Combo;
    }
    else if (value == "textedit")
    {
        kind = UiElementKind::TextEdit;
    }
    else if (value == "scrollview")
    {
        kind = UiElementKind::ScrollView;
    }
    else if (value == "custom")
    {
        kind = UiElementKind::Custom;
    }
    else
    {
        return false;
    }
    return true;
}

bool ApplyNode(const nlohmann::json& node,
               UiDocument& document,
               UiElementHandle handle,
               int depth,
               std::size_t& nodeCount,
               std::string& error)
{
    if (depth > kMaxDepth)
    {
        error = "UiDocumentLoader: tree depth exceeds 256";
        return false;
    }
    if (!node.is_object())
    {
        error = "UiDocumentLoader: node must be an object";
        return false;
    }
    if (++nodeCount > kMaxNodes)
    {
        error = "UiDocumentLoader: node count exceeds 100000";
        return false;
    }

    const auto type = node.find("type");
    if (type == node.end() || !type->is_string())
    {
        error = "UiDocumentLoader: node.type must be a string";
        return false;
    }
    UiElementKind kind = UiElementKind::Panel;
    if (!ParseKind(type->get_ref<const std::string&>(), kind))
    {
        error = "UiDocumentLoader: unsupported node type '" + type->get<std::string>() + "'";
        return false;
    }
    if (!document.SetKind(handle, kind))
    {
        error = "UiDocumentLoader: cannot set node type";
        return false;
    }

    const auto id = node.find("id");
    if (id != node.end())
    {
        if (!id->is_string())
        {
            error = "UiDocumentLoader: node.id must be a string";
            return false;
        }
        if (!document.SetName(handle, id->get_ref<const std::string&>()))
        {
            error = "UiDocumentLoader: duplicate node id";
            return false;
        }
    }

    const auto classes = node.find("classes");
    if (classes != node.end())
    {
        if (!classes->is_array())
        {
            error = "UiDocumentLoader: node.classes must be an array";
            return false;
        }
        for (const auto& className : *classes)
        {
            if (!className.is_string())
            {
                error = "UiDocumentLoader: node.classes entries must be strings";
                return false;
            }
            if (!document.AddClass(handle, className.get_ref<const std::string&>()))
            {
                error = "UiDocumentLoader: invalid class name";
                return false;
            }
        }
    }

    const auto children = node.find("children");
    if (children != node.end())
    {
        if (!children->is_array())
        {
            error = "UiDocumentLoader: node.children must be an array";
            return false;
        }
        for (const auto& childJson : *children)
        {
            if (!childJson.is_object())
            {
                error = "UiDocumentLoader: child node must be an object";
                return false;
            }
            const UiElementHandle child = document.CreateElement(UiElementKind::Panel);
            if (!document.SetParent(child, handle))
            {
                error = "UiDocumentLoader: cannot attach child";
                return false;
            }
            if (!ApplyNode(childJson, document, child, depth + 1, nodeCount, error))
            {
                return false;
            }
        }
    }
    return true;
}
} // namespace

UiDocumentLoadResult LoadUiDocumentJson(std::string_view json, UiDocument& document)
{
    if (!document.IsValid())
    {
        return {false, "UiDocumentLoader: target document is invalid"};
    }

    try
    {
        const nlohmann::json parsed = nlohmann::json::parse(json.begin(), json.end());
        if (!parsed.is_object())
        {
            return {false, "UiDocumentLoader: root JSON must be an object"};
        }

        const auto version = parsed.find("version");
        if (version == parsed.end() || !version->is_number_integer() || version->get<int>() != 1)
        {
            return {false, "UiDocumentLoader: unsupported or missing version (expected 1)"};
        }

        const auto styles = parsed.find("styles");
        if (styles != parsed.end())
        {
            if (!styles->is_object())
            {
                return {false, "UiDocumentLoader: styles must be an object"};
            }
            for (auto entry = styles->begin(); entry != styles->end(); ++entry)
            {
                if (!entry.value().is_object())
                {
                    return {false, "UiDocumentLoader: each styles entry must be an object"};
                }
            }
        }

        const auto root = parsed.find("root");
        if (root == parsed.end() || !root->is_object())
        {
            return {false, "UiDocumentLoader: root must be an object"};
        }

        UiDocument candidate;
        std::size_t nodeCount = 0;
        std::string error;
        if (!ApplyNode(*root, candidate, candidate.Root(), 0, nodeCount, error))
        {
            return {false, std::move(error)};
        }
        document = std::move(candidate);
        return {true, {}};
    }
    catch (const std::exception& exception)
    {
        return {false, std::string("UiDocumentLoader: invalid JSON: ") + exception.what()};
    }
}

} // namespace se::ui

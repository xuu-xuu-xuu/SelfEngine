#include "FunctionLayer/UI/Core/UiBinding.h"

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

UiElementHandle MakeLabel(UiDocument& document, const char* name)
{
    const UiElementHandle label = document.CreateElement(UiElementKind::Label);
    Check(document.SetParent(label, document.Root()), "attach label");
    Check(document.SetName(label, name), "name label");
    return label;
}

void TextBinding()
{
    UiDocument document;
    const UiElementHandle label = MakeLabel(document, "score");
    UiBindingTable bindings(document);
    std::string score = "1";
    Check(bindings.BindText(label, [&score] { return score; }), "bind text");
    Check(bindings.BindingCount() == 1, "one binding");
    Check(document.Text(label) == "1", "initial text applied");
    document.ClearAllDirty();
    Check(bindings.Update() == 0, "same text is not a change");
    Check(document.Dirty(label) == UiDirtyFlags::None, "same text does not dirty");
    score = "2";
    Check(bindings.Update() == 1, "changed text is applied once");
    Check(document.Text(label) == "2", "changed text value");
    Check(HasFlag(document.Dirty(label), UiDirtyFlags::Layout), "text change dirties layout");
    Check(HasFlag(document.Dirty(label), UiDirtyFlags::Paint), "text change dirties paint");
    Check(bindings.Unbind(label, UiBindingProperty::Text), "unbind text");
    Check(bindings.BindingCount() == 0, "unbind removes binding");
    score = "3";
    Check(bindings.Update() == 0, "unbound text is not applied");
}

void BoolBindings()
{
    UiDocument document;
    const UiElementHandle label = MakeLabel(document, "state");
    UiBindingTable bindings(document);
    bool visible = true;
    bool enabled = true;
    Check(bindings.BindVisible(label, [&visible] { return visible; }), "bind visible");
    Check(bindings.BindEnabled(label, [&enabled] { return enabled; }), "bind enabled");
    Check(bindings.BindingCount() == 2, "two bool bindings");
    Check(document.Visible(label), "initial visible");
    Check(document.Enabled(label), "initial enabled");

    document.ClearAllDirty();
    Check(bindings.Update() == 0, "unchanged bools are not applied");
    visible = false;
    enabled = false;
    Check(bindings.Update() == 2, "both bool changes applied");
    Check(!document.Visible(label), "visible changed");
    Check(!document.Enabled(label), "enabled changed");
    Check(HasFlag(document.Dirty(label), UiDirtyFlags::Layout), "visibility dirties layout");
    Check(HasFlag(document.Dirty(label), UiDirtyFlags::Paint), "enabled dirties paint");

    Check(bindings.Unbind(label, UiBindingProperty::Visible), "unbind visible");
    Check(bindings.Unbind(label, UiBindingProperty::Enabled), "unbind enabled");
    Check(bindings.BindingCount() == 0, "all bindings removed");
}

void MultipleAndDestroyed()
{
    UiDocument document;
    const UiElementHandle label = MakeLabel(document, "multi");
    UiBindingTable bindings(document);
    std::string text = "a";
    bool visible = true;
    bool enabled = true;
    Check(bindings.BindText(label, [&text] { return text; }), "bind text");
    Check(bindings.BindVisible(label, [&visible] { return visible; }), "bind visible");
    Check(bindings.BindEnabled(label, [&enabled] { return enabled; }), "bind enabled");
    Check(bindings.BindingCount() == 3, "three bindings");
    document.ClearAllDirty();
    text = "b";
    visible = false;
    enabled = false;
    Check(bindings.Update() == 3, "three properties updated");
    Check(document.Text(label) == "b" && !document.Visible(label) && !document.Enabled(label), "three values applied");

    const UiElementHandle doomed = MakeLabel(document, "doomed");
    Check(bindings.BindText(doomed, [] { return std::string("x"); }), "bind doomed");
    Check(document.DestroyElement(doomed), "destroy bound element");
    const std::size_t before = bindings.BindingCount();
    Check(bindings.Update() == 0, "destroyed target is skipped");
    Check(bindings.BindingCount() == before, "stale binding remains until unbound");
    Check(bindings.Unbind(doomed, UiBindingProperty::Text), "remove stale binding");
    Check(bindings.BindingCount() == before - 1, "stale binding removed");

    Check(!bindings.BindText(UiElementHandle{}, [] { return std::string(); }), "invalid target rejected");
    Check(!bindings.BindText(label, std::function<std::string()>{}), "empty getter rejected");
    Check(!bindings.BindVisible(label, std::function<bool()>{}), "empty bool getter rejected");
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        TextBinding();
        BoolBindings();
        MultipleAndDestroyed();

        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            std::ofstream(proof)
                << "{\"success\":true,\"text\":true,\"visible\":true,\"enabled\":true,"
                   "\"changeOnly\":true,\"staleSkipped\":true}\n";
        }
        std::cout << "UiCoreBindingTests passed (text/visible/enabled, change-only writes, stale targets skipped).\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}

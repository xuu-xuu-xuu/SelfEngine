#include "FunctionLayer/UI/Core/UiStyle.h"
#include "FunctionLayer/UI/Core/UiDocument.h"
#include "FunctionLayer/UI/Core/UiLayout.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

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

void Near(float actual, float expected, const char* message)
{
    if (std::abs(actual - expected) > 0.01f)
    {
        throw std::runtime_error(std::string(message) + " (actual " + std::to_string(actual) +
                                 ", expected " + std::to_string(expected) + ")");
    }
}

void CheckColor(const UiColor& color, float r, float g, float b, float a, const char* message)
{
    Near(color.r, r, message);
    Near(color.g, g, message);
    Near(color.b, b, message);
    Near(color.a, a, message);
}

UiColor Hex(float r, float g, float b, float a = 1.0f)
{
    return UiColor{r, g, b, a};
}

struct Fixture
{
    UiDocument document;
    UiStyleEngine engine{document};
    UiElementHandle root = document.Root();

    UiElementHandle Add(UiElementKind kind,
                        UiElementHandle parent,
                        std::string_view name,
                        std::initializer_list<std::string_view> classes)
    {
        const UiElementHandle element = document.CreateElement(kind);
        Check(document.SetParent(element, parent), "attach style element");
        if (!name.empty())
        {
            Check(document.SetName(element, name), "name style element");
        }
        for (const std::string_view className : classes)
        {
            Check(document.AddClass(element, className), "class style element");
        }
        return element;
    }
};

const char* kStyles = R"json({
  "panel": { "background": "#101010", "padding": 4 },
  "button": { "border-width": 1 },
  ".primary": { "background": "#202020", "color": "#FFFFFF" },
  "#hero": { "background": "#303030" },
  "panel > .primary": { "gap": 3 },
  "panel .label": { "color": "#AABBCC" },
  "label": { "font-size": 18 },
  "#hero:hover": { "background": "$accent" },
  ".label:focus": { "color": "#ABCDEF" },
  "button:disabled": { "background": "#505050" }
})json";

void SelectorAndSpecificity()
{
    Fixture fixture;
    Check(fixture.document.SetName(fixture.root, "root"), "root id");
    Check(fixture.document.AddClass(fixture.root, "screen"), "root class");
    const UiElementHandle hero = fixture.Add(UiElementKind::Button, fixture.root, "hero", {"primary"});
    const UiElementHandle label = fixture.Add(UiElementKind::Label, hero, "caption", {"label"});
    std::string error;
    Check(fixture.engine.LoadStylesJson(kStyles, error), error.c_str());
    Check(error.empty(), "style load error empty");

    const UiStyle& rootStyle = fixture.engine.Compute(fixture.root);
    CheckColor(rootStyle.background, 16.0f / 255.0f, 16.0f / 255.0f, 16.0f / 255.0f, 1.0f, "type selector");
    Near(rootStyle.padding.left, 4.0f, "padding shorthand");

    const UiStyle& heroStyle = fixture.engine.Compute(hero);
    CheckColor(heroStyle.background, 48.0f / 255.0f, 48.0f / 255.0f, 48.0f / 255.0f, 1.0f, "id over class");
    CheckColor(heroStyle.color, 1.0f, 1.0f, 1.0f, 1.0f, "class color");
    Near(heroStyle.borderWidth, 1.0f, "type border");
    Near(heroStyle.gap, 3.0f, "child combinator");

    const UiStyle& labelStyle = fixture.engine.Compute(label);
    CheckColor(labelStyle.color, 170.0f / 255.0f, 187.0f / 255.0f, 204.0f / 255.0f, 1.0f, "descendant combinator");
    Near(labelStyle.fontSize, 18.0f, "type font size");
}

void PseudoAndTheme()
{
    Fixture fixture;
    const UiElementHandle hero = fixture.Add(UiElementKind::Button, fixture.root, "hero", {"primary"});
    const UiElementHandle label = fixture.Add(UiElementKind::Label, fixture.root, "label", {"label"});
    const UiElementHandle disabled = fixture.Add(UiElementKind::Button, fixture.root, "disabled", {});
    std::string error;
    Check(fixture.engine.LoadStylesJson(kStyles, error), error.c_str());

    CheckColor(fixture.engine.Compute(hero, UiInteractionState::None).background,
               48.0f / 255.0f, 48.0f / 255.0f, 48.0f / 255.0f, 1.0f, "normal id background");
    CheckColor(fixture.engine.Compute(hero, UiInteractionState::Hover).background,
               0.310f, 0.549f, 1.0f, 1.0f, "hover theme accent");
    CheckColor(fixture.engine.Compute(label, UiInteractionState::Focus).color,
               171.0f / 255.0f, 205.0f / 255.0f, 239.0f / 255.0f, 1.0f, "focus pseudo");
    Check(fixture.document.SetEnabled(disabled, false), "disable style element");
    CheckColor(fixture.engine.Compute(disabled).background,
               80.0f / 255.0f, 80.0f / 255.0f, 80.0f / 255.0f, 1.0f, "disabled pseudo");

    UiTheme theme = UiTheme::Dark();
    CheckColor(theme.Color(UiThemeColor::Accent), 0.310f, 0.549f, 1.0f, 1.0f, "dark accent token");
    Near(theme.Metric(UiThemeMetric::SpacingMd), 12.0f, "dark spacing token");
    theme.accent = Hex(1.0f, 0.0f, 0.0f);
    fixture.engine.SetTheme(theme);
    CheckColor(fixture.engine.Compute(hero, UiInteractionState::Hover).background,
               1.0f, 0.0f, 0.0f, 1.0f, "theme switch re-resolves token");
    CheckColor(fixture.engine.Compute(hero, UiInteractionState::None).background,
               48.0f / 255.0f, 48.0f / 255.0f, 48.0f / 255.0f, 1.0f, "theme switch keeps literal");
}

void InlineAndCache()
{
    Fixture fixture;
    const UiElementHandle hero = fixture.Add(UiElementKind::Button, fixture.root, "hero", {"primary"});
    std::string error;
    Check(fixture.engine.LoadStylesJson(
        R"json({
          ".primary": { "background": "#202020", "color": "#FFFFFF" },
          "#hero": { "background": "#303030" },
          ".secondary": { "width": 120 },
          "#hero:hover": { "background": "$accent" }
        })json",
        error), error.c_str());

    const UiStyle& first = fixture.engine.Compute(hero);
    CheckColor(first.background, 48.0f / 255.0f, 48.0f / 255.0f, 48.0f / 255.0f, 1.0f, "initial id background");
    const UiStyleStats beforeHit = fixture.engine.Stats();
    const UiStyle& second = fixture.engine.Compute(hero);
    Check(&first == &second, "same element/state returns cached reference");
    Check(fixture.engine.Stats().cacheHits == beforeHit.cacheHits + 1, "cache hit increments");

    Check(fixture.document.AddClass(hero, "secondary"), "class change");
    const UiStyleStats beforeClass = fixture.engine.Stats();
    const UiStyle& classStyle = fixture.engine.Compute(hero);
    Near(classStyle.width.value, 120.0f, "new class selector applies");
    Check(fixture.engine.Stats().cacheMisses == beforeClass.cacheMisses + 1, "class change invalidates cache");

    UiStyle inlineStyle;
    inlineStyle.background = Hex(0.6f, 0.6f, 0.6f);
    inlineStyle.width = UiLength::Px(77.0f);
    Check(fixture.document.SetStyle(hero, inlineStyle), "inline style");
    const UiStyle& inlineComputed = fixture.engine.Compute(hero);
    CheckColor(inlineComputed.background, 0.6f, 0.6f, 0.6f, 1.0f, "inline beats id/class");
    Near(inlineComputed.width.value, 77.0f, "inline width beats class");

    const UiStyle& hovered = fixture.engine.Compute(hero, UiInteractionState::Hover);
    CheckColor(hovered.background, 0.6f, 0.6f, 0.6f, 1.0f, "inline beats pseudo");
    const UiStyleStats beforeHover = fixture.engine.Stats();
    (void)fixture.engine.Compute(hero, UiInteractionState::Hover);
    Check(fixture.engine.Stats().cacheHits == beforeHover.cacheHits + 1, "pseudo state has separate cache");
}

void DiagnosticsAndTokens()
{
    Fixture fixture;
    const UiElementHandle tokenElement = fixture.Add(UiElementKind::Panel, fixture.root, "token", {"token"});
    std::string error;
    Check(fixture.engine.LoadStylesJson(
        R"json({
          ".token": { "background": "$accent", "padding": "$spacing-md", "radius": "$radius-md" },
          "panel": { "padding": "bad", "unknown-prop": 1 },
          "panel:bogus": { "background": "#000000" },
          "button": { "width": "12parsecs" }
        })json",
        error), error.c_str());
    Check(fixture.engine.Diagnostics().size() >= 4, "unknown/invalid declarations diagnosed");
    CheckColor(fixture.engine.Compute(tokenElement).background,
               0.310f, 0.549f, 1.0f, 1.0f, "color token");
    Near(fixture.engine.Compute(tokenElement).padding.left, 12.0f, "spacing token");
    Near(fixture.engine.Compute(tokenElement).cornerRadius, 6.0f, "radius token");
    Near(fixture.engine.Compute(fixture.root).padding.left, 0.0f, "invalid value ignored");

    UiTheme theme = UiTheme::Dark();
    theme.accent = Hex(0.0f, 1.0f, 0.0f);
    fixture.engine.SetTheme(theme);
    CheckColor(fixture.engine.Compute(tokenElement).background,
               0.0f, 1.0f, 0.0f, 1.0f, "theme token after switch");
}

void ComputedStyleDrivesLayout()
{
    UiDocument document;
    UiStyleEngine styles(document);
    UiLayoutEngine layout(document);
    std::string error;
    Check(styles.LoadStylesJson(R"({".box":{"width":70,"height":30,"padding":5}})", error), error.c_str());
    const UiElementHandle box = document.CreateElement(UiElementKind::Panel);
    Check(document.SetParent(box, document.Root()), "attach styled box");
    Check(document.AddClass(box, "box"), "class styled box");
    layout.SetStyleEngine(&styles);
    Check(layout.Layout(400.0f, 300.0f), "computed style layout");
    Near(layout.Rect(box).width, 70.0f, "stylesheet width drives layout");
    Near(layout.Rect(box).height, 30.0f, "stylesheet height drives layout");
}

void UnrelatedSubtree()
{
    UiDocument document;
    UiLayoutEngine layout(document);
    const UiElementHandle root = document.Root();
    const UiElementHandle first = document.CreateElement(UiElementKind::Panel);
    const UiElementHandle second = document.CreateElement(UiElementKind::Panel);
    Check(document.SetParent(first, root), "attach first");
    Check(document.SetParent(second, root), "attach second");
    UiStyle box;
    box.width = UiLength::Px(50.0f);
    box.height = UiLength::Px(20.0f);
    Check(document.SetStyle(first, box), "first style");
    Check(document.SetStyle(second, box), "second style");
    Check(layout.Layout(400.0f, 300.0f), "layout before style change");
    document.ClearAllDirty();

    UiStyle wider = box;
    wider.width = UiLength::Px(70.0f);
    Check(document.SetStyle(first, wider), "style change");
    Check(document.Dirty(second) == UiDirtyFlags::None, "style change does not dirty sibling");
    Check(layout.Layout(400.0f, 300.0f), "layout after style change");
    Check(layout.Stats().nodesMeasured <= 2, "unrelated sibling is not remeasured");
    Near(layout.Rect(first).width, 70.0f, "changed width applied");
    Near(layout.Rect(second).width, 50.0f, "sibling size unchanged");

    document.ClearAllDirty();
    Check(document.AddClass(first, "highlight"), "class-only style change");
    Check(document.Dirty(second) == UiDirtyFlags::None, "class change does not dirty sibling");
    Check(layout.Layout(400.0f, 300.0f), "layout after class change");
    Check(layout.Stats().nodesMeasured <= 2, "class change does not remeasure sibling");
}

void DirtyBitsAndBadInput()
{
    Fixture fixture;
    const UiElementHandle target = fixture.Add(UiElementKind::Panel, fixture.root, "target", {});
    fixture.document.ClearAllDirty();
    const std::uint64_t styleRevision = fixture.document.StyleRevision();
    const std::uint64_t layoutRevision = fixture.document.LayoutRevision();
    Check(fixture.document.AddClass(target, "changed"), "add class");
    const UiDirtyFlags classDirty = fixture.document.Dirty(target);
    Check(HasFlag(classDirty, UiDirtyFlags::Style) && HasFlag(classDirty, UiDirtyFlags::Layout) &&
              HasFlag(classDirty, UiDirtyFlags::Paint), "class marks style/layout/paint");
    Check(fixture.document.StyleRevision() > styleRevision, "class bumps style revision");
    Check(fixture.document.LayoutRevision() > layoutRevision, "class bumps layout revision");

    fixture.document.ClearAllDirty();
    Check(fixture.document.SetEnabled(target, false), "disable");
    const UiDirtyFlags enabledDirty = fixture.document.Dirty(target);
    Check(HasFlag(enabledDirty, UiDirtyFlags::Paint), "enabled marks paint");
    Check(!HasFlag(enabledDirty, UiDirtyFlags::Layout), "enabled does not mark layout");

    fixture.document.ClearAllDirty();
    Check(fixture.document.SetName(target, "renamed"), "rename");
    Check(HasFlag(fixture.document.Dirty(target), UiDirtyFlags::Style), "rename marks style");

    UiStyle inlineStyle;
    inlineStyle.background = Hex(0.2f, 0.3f, 0.4f);
    Check(fixture.document.SetStyle(target, inlineStyle), "inline background");
    Check(HasFlag(fixture.document.Dirty(target), UiDirtyFlags::Style) &&
              HasFlag(fixture.document.Dirty(target), UiDirtyFlags::Layout) &&
              HasFlag(fixture.document.Dirty(target), UiDirtyFlags::Paint), "inline style marks all");

    std::string error;
    Check(!fixture.engine.LoadStylesJson("{", error) && !error.empty(), "invalid JSON rejected");
    Check(!fixture.engine.LoadStylesJson("[]", error), "styles array rejected");
    Check(fixture.engine.LoadStylesJson("{}", error), "empty styles accepted");
    Check(fixture.engine.Diagnostics().empty(), "empty styles no diagnostics");
    CheckColor(fixture.engine.Compute(target).background, 0.2f, 0.3f, 0.4f, 1.0f, "inline survives empty styles");
    const UiStyle& invalid = fixture.engine.Compute(UiElementHandle{});
    CheckColor(invalid.color, 1.0f, 1.0f, 1.0f, 1.0f, "invalid element returns default style");
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        SelectorAndSpecificity();
        PseudoAndTheme();
        InlineAndCache();
        DiagnosticsAndTokens();
        ComputedStyleDrivesLayout();
        UnrelatedSubtree();
        DirtyBitsAndBadInput();

        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            std::ofstream(proof)
                << "{\"success\":true,\"selectors\":true,\"specificity\":true,\"pseudo\":true,"
                   "\"inline\":true,\"cache\":true,\"theme\":true,\"diagnostics\":true,"
                   "\"dirtyBits\":true}\n";
        }
        std::cout << "UiCoreStyleTests passed (selectors, specificity, pseudo, inline, cache, theme, diagnostics).\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}

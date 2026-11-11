#include "FunctionLayer/UI/Core/UiDocument.h"
#include "FunctionLayer/UI/Core/UiInput.h"
#include "FunctionLayer/UI/Core/UiStyle.h"
#include "FunctionLayer/UI/Widgets/UiTextEdit.h"
#include "FunctionLayer/UI/Widgets/UiWidgets.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

struct FakePlatform final : UiPlatformServices
{
    std::string clipboard;
    float dpi = 1.0f;

    bool ReadClipboard(std::string& utf8) override
    {
        utf8 = clipboard;
        return true;
    }

    bool WriteClipboard(std::string_view utf8) override
    {
        clipboard.assign(utf8);
        return true;
    }

    [[nodiscard]] float DpiScale() const override
    {
        return dpi;
    }

    void SetImeComposition(std::string_view) override {}
    void SetImeCandidateAnchor(const UiImeCandidateAnchor&) override {}
    void CancelIme() override {}
};

struct Fixture
{
    UiDocument document;
    UiStubTextService text{8.0f, 16.0f};
    UiElementHandle root = document.Root();

    UiElementHandle Add(UiElementKind kind)
    {
        const UiElementHandle element = document.CreateElement(kind);
        Check(document.SetParent(element, root), "attach widget");
        return element;
    }
};

void ButtonsAndChecks()
{
    Fixture fixture;
    const UiElementHandle handle = fixture.Add(UiElementKind::Button);
    UiButton button(fixture.document, handle);
    button.SetHovered(true);
    Check(button.Hovered(), "button hover");
    button.PointerDown();
    Check(button.Pressed(), "button pressed");
    button.PointerUp(false);
    Check(!button.ConsumeClick(), "release outside is not a click");
    button.PointerDown();
    button.PointerUp(true);
    Check(button.ConsumeClick(), "release inside clicks");
    Check(!button.ConsumeClick(), "click consumed once");
    Check(fixture.document.SetEnabled(handle, false), "disable button");
    button.PointerDown();
    Check(!button.Pressed(), "disabled button does not press");
    button.SetHovered(true);
    Check(!button.Hovered(), "disabled button does not hover");

    const UiElementHandle checkHandle = fixture.Add(UiElementKind::Checkbox);
    UiCheckbox checkbox(fixture.document, checkHandle);
    checkbox.Toggle();
    Check(checkbox.Checked() && checkbox.ConsumeChanged(), "checkbox toggles");
    checkbox.PointerUp(false);
    Check(checkbox.Checked() && !checkbox.ConsumeChanged(), "outside release does not toggle");
    checkbox.PointerUp(true);
    Check(!checkbox.Checked(), "inside release toggles");
}

void ComboAndScroll()
{
    Fixture fixture;
    const UiElementHandle handle = fixture.Add(UiElementKind::Combo);
    UiCombo combo(fixture.document, handle);
    combo.SetItems({"A", "B", "C"});
    Check(combo.Selected() == -1, "combo starts unselected");
    combo.Open();
    Check(combo.IsOpen(), "combo opens");
    Check(combo.HandleKey(UiKeyDown), "combo down handled");
    Check(combo.Selected() == 0 && combo.ConsumeChanged(), "combo first item");
    Check(combo.HandleKey(UiKeyDown) && combo.Selected() == 1, "combo next item");
    Check(combo.HandleKey(UiKeyEnter) && !combo.IsOpen(), "combo enter closes");
    combo.Open();
    Check(combo.HandleKey(UiKeyEscape) && !combo.IsOpen(), "combo escape closes");
    combo.Select(99);
    Check(combo.Selected() == 2, "combo clamps selection");

    UiScrollView scroll;
    scroll.SetContentHeight(1000.0f);
    scroll.SetViewportHeight(100.0f);
    Check(scroll.MaximumOffset() == 900.0f, "scroll maximum");
    scroll.SetOffset(500.0f);
    Check(scroll.Offset() == 500.0f, "scroll offset");
    scroll.ScrollBy(1000.0f);
    Check(scroll.Offset() == 900.0f, "scroll clamps");
    Check(scroll.ThumbHeight() > 0.0f && scroll.ThumbY() > 0.0f, "scroll thumb");
}

void ListVirtualization()
{
    Fixture fixture;
    const UiElementHandle content = fixture.Add(UiElementKind::Panel);
    UiListView list(fixture.document, content);
    list.SetItemCount(1000);
    list.SetRowHeight(20.0f);
    list.SetViewportHeight(100.0f);
    UiVisibleRange range = list.VisibleRange();
    Check(range.first == 0 && range.last == 7, "list visible range with buffer");
    list.Sync();
    Check(list.Rows().size() == 7, "list creates visible rows");
    list.SetScrollOffset(500.0f);
    range = list.VisibleRange();
    Check(range.first == 23 && range.last == 32, "list scrolled range");
    list.Sync();
    Check(list.Rows().size() == 9, "list reuses only visible rows");
    list.SetItemCount(3);
    list.SetScrollOffset(0.0f);
    list.Sync();
    Check(list.Rows().size() == 3, "list short dataset");
}

void TreesAndTables()
{
    UiTreeView tree;
    const int root = tree.AddNode(-1, "root");
    const int child = tree.AddNode(root, "child");
    tree.AddNode(child, "grandchild");
    Check(tree.VisibleFlat().size() == 3, "tree visible all");
    tree.SetExpanded(root, false);
    Check(tree.VisibleFlat().size() == 1 && tree.VisibleFlat()[0] == root, "tree collapsed");
    tree.SetExpanded(root, true);
    Check(tree.VisibleFlat().size() == 3, "tree expanded");

    UiTableView table;
    table.SetDimensions(1000, 4);
    table.SetRowHeight(20.0f);
    table.SetViewportHeight(100.0f);
    table.SetScrollOffset(500.0f);
    const UiVisibleRange range = table.VisibleRange();
    Check(range.first == 23 && range.last == 32, "table visible range");
    Check(table.Rows() == 1000 && table.Columns() == 4, "table dimensions");
}

void OverlayAndDrag()
{
    UiOverlay overlay;
    overlay.Open(UiOverlay::Layer::Tooltip, {10.0f, 10.0f, 50.0f, 20.0f}, 1);
    overlay.Open(UiOverlay::Layer::Menu, {20.0f, 40.0f, 100.0f, 80.0f}, 2);
    Check(overlay.Count() == 2 && overlay.Top()->id == 2, "overlay top");
    Check(!overlay.BlocksInput(), "menu does not modal block");
    Check(overlay.PointerDown(10.0f, 10.0f), "outside menu click consumed");
    Check(overlay.Count() == 1, "outside click closes menu");
    overlay.Open(UiOverlay::Layer::Modal, {50.0f, 50.0f, 100.0f, 100.0f}, 3);
    Check(overlay.BlocksInput(), "modal blocks");
    Check(overlay.PointerDown(60.0f, 60.0f), "inside modal consumed");
    Check(overlay.Count() == 2, "inside modal stays");
    const UiRect placed = UiOverlay::Place({380.0f, 280.0f, 50.0f, 40.0f}, 400.0f, 300.0f);
    Check(placed.x == 350.0f && placed.y == 260.0f, "overlay placement flips");

    UiDragDrop drag;
    drag.Begin("asset", 42, {0.0f, 0.0f, 32.0f, 32.0f});
    Check(drag.Current() == UiDragDrop::State::Dragging, "drag starts");
    drag.Over(UiElementHandle{1, 1}, true);
    Check(drag.Current() == UiDragDrop::State::Over && drag.Accepted(), "drag over");
    Check(drag.Drop(), "drag drop accepted");
    Check(drag.Current() == UiDragDrop::State::None && drag.Data().id == 42, "drag payload");
    drag.Begin("entity", 7, {});
    drag.Cancel();
    Check(drag.Current() == UiDragDrop::State::None, "drag cancel");
}

void CanvasSplitterTabs()
{
    UiCanvas canvas;
    canvas.AddLine(0.0f, 0.0f, 10.0f, 10.0f, 2.0f, UiColor{1.0f, 1.0f, 1.0f, 1.0f});
    canvas.AddRect({0.0f, 0.0f, 10.0f, 20.0f}, UiColor{1.0f, 0.0f, 0.0f, 1.0f});
    canvas.AddCircle(20.0f, 20.0f, 5.0f, UiColor{0.0f, 1.0f, 0.0f, 1.0f});
    canvas.AddText({0.0f, 0.0f, 80.0f, 16.0f}, "gizmo", 12.0f, UiColor{1.0f, 1.0f, 1.0f, 1.0f});
    Check(canvas.Commands().size() == 4, "canvas commands");

    UiSplitter splitter(true, 0.5f, 100.0f, 100.0f);
    splitter.Drag(200.0f, 1000.0f);
    Check(std::abs(splitter.Ratio() - 0.7f) < 1e-5f, "splitter drag");
    splitter.Drag(500.0f, 1000.0f);
    Check(std::abs(splitter.Ratio() - 0.9f) < 1e-5f, "splitter max");
    splitter.Reset();
    Check(std::abs(splitter.Ratio() - 0.5f) < 1e-5f, "splitter reset");

    UiTabBar tabs;
    tabs.SetTabs({"a", "b", "c"});
    tabs.Select(2);
    Check(tabs.Selected() == 2, "tab select");
    Check(tabs.Close(0), "tab close");
    Check(tabs.Selected() == 1 && tabs.Tabs().size() == 2, "tab selected adjusts");
    Check(tabs.Move(0, 1), "tab move");
    Check(tabs.Tabs().size() == 2, "tab count stable");
}

void TextEditing()
{
    Fixture fixture;
    const UiElementHandle handle = fixture.Add(UiElementKind::TextEdit);
    FakePlatform platform;
    UiTextEdit edit(fixture.document, handle, &fixture.text, &platform);
    edit.SetText("A中B");
    Check(edit.Text() == "A中B" && fixture.document.Text(handle) == "A中B", "text sync");
    edit.Select(1, 3);
    Check(edit.SelectedText() == "中B", "unicode selection");
    Check(edit.Copy() && platform.clipboard == "中B", "copy selected");
    Check(edit.Cut() && edit.Text() == "A", "cut selection");
    Check(edit.Paste() && edit.Text() == "A中B", "paste clipboard");
    Check(edit.CanUndo(), "undo available");
    Check(edit.Undo() && edit.Text() == "A", "undo");
    Check(edit.Redo() && edit.Text() == "A中B", "redo");

    edit.SetCaret(3);
    Check(edit.HandleKey(UiKeyEnter, false, false) && edit.Text() == "A中B\n", "enter inserts newline");
    Check(edit.HandleKey(UiKeyBackspace, false, false) && edit.Text() == "A中B", "backspace");
    edit.SetCaret(0);
    Check(edit.HandleKey(UiKeyDelete, false, false) && edit.Text() == "中B", "delete");
    Check(edit.HandleKey('A', false, true) && edit.SelectedText() == "中B", "ctrl+a");
    Check(edit.HandleKey('C', false, true) && platform.clipboard == "中B", "ctrl+c");

    edit.SetText("A");
    edit.SetCaret(1);
    Check(edit.SetComposition("中") && edit.DisplayText() == "A中" && edit.Composing(), "IME preview");
    Check(edit.CommitComposition() && edit.Text() == "A中" && !edit.Composing(), "IME commit");
    Check(edit.SetComposition("文") && edit.DisplayText() == "A中文", "IME second preview");
    edit.CancelComposition();
    Check(edit.Text() == "A中", "IME cancel");

    edit.SetText(std::string(500, 'a'));
    edit.SetViewport(80.0f, 16.0f);
    edit.SetCaret(500);
    edit.EnsureCaretVisible();
    Check(edit.ScrollOffset() > 0.0f, "caret scroll");
    edit.ScrollBy(-1000.0f);
    Check(edit.ScrollOffset() == 0.0f, "scroll clamp");
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        ButtonsAndChecks();
        ComboAndScroll();
        ListVirtualization();
        TreesAndTables();
        OverlayAndDrag();
        CanvasSplitterTabs();
        TextEditing();

        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            std::ofstream(proof)
                << "{\"success\":true,\"button\":true,\"checkbox\":true,\"combo\":true,"
                   "\"scroll\":true,\"listVirtual\":true,\"tree\":true,\"table\":true,"
                   "\"overlay\":true,\"dragDrop\":true,\"canvas\":true,\"splitter\":true,"
                   "\"tabs\":true,\"textEdit\":true}\n";
        }
        std::cout << "UiCoreWidgetsTests passed (button/check/combo/scroll/list/tree/table/overlay/drag/canvas/text).\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}

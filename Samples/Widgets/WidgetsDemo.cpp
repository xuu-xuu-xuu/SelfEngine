#include "FunctionLayer/Rendering/UiPaint.h"
#include "FunctionLayer/UI/Core/UiDocument.h"
#include "FunctionLayer/UI/Core/UiInput.h"
#include "FunctionLayer/UI/Core/UiLayout.h"
#include "FunctionLayer/UI/Core/UiStyle.h"
#include "FunctionLayer/UI/Widgets/UiTextEdit.h"
#include "FunctionLayer/UI/Widgets/UiWidgets.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace se;
using namespace se::ui;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

UiElementHandle Add(UiDocument& document, UiElementHandle parent, UiElementKind kind,
                    const UiStyle& style, std::string_view name = {})
{
    const UiElementHandle element = document.CreateElement(kind);
    Check(document.SetParent(element, parent), "demo attach");
    if (!name.empty())
    {
        Check(document.SetName(element, name), "demo name");
    }
    Check(document.SetStyle(element, style), "demo style");
    return element;
}
} // namespace

int main()
{
    try
    {
        UiDocument document;
        UiStubTextService text(8.0f, 16.0f);
        UiLayoutEngine layout(document, &text);
        UiDispatcher dispatcher(document, layout);
        const UiElementHandle root = document.Root();
        UiStyle rootStyle;
        rootStyle.background = UiColor{0.078f, 0.086f, 0.102f, 1.0f};
        rootStyle.flexDirection = UiFlexDirection::Column;
        rootStyle.padding = UiEdges{24.0f, 24.0f, 24.0f, 24.0f};
        rootStyle.gap = 12.0f;
        document.SetStyle(root, rootStyle);

        UiStyle panelStyle;
        panelStyle.width = UiLength::Percent(100.0f);
        panelStyle.height = UiLength::Px(260.0f);
        panelStyle.background = UiColor{0.118f, 0.133f, 0.165f, 1.0f};
        panelStyle.borderWidth = 1.0f;
        panelStyle.borderColor = UiColor{0.165f, 0.184f, 0.227f, 1.0f};
        panelStyle.padding = UiEdges{16.0f, 16.0f, 16.0f, 16.0f};
        panelStyle.flexDirection = UiFlexDirection::Column;
        const UiElementHandle panel = Add(document, root, UiElementKind::Panel, panelStyle, "panel");

        UiStyle buttonStyle;
        buttonStyle.width = UiLength::Px(120.0f);
        buttonStyle.height = UiLength::Px(32.0f);
        buttonStyle.background = UiColor{0.310f, 0.549f, 1.0f, 1.0f};
        const UiElementHandle button = Add(document, panel, UiElementKind::Button, buttonStyle, "start");
        document.SetText(button, "Start");

        UiStyle checkStyle = buttonStyle;
        checkStyle.background = UiColor{0.2f, 0.22f, 0.26f, 1.0f};
        const UiElementHandle check = Add(document, panel, UiElementKind::Checkbox, checkStyle, "check");

        UiStyle comboStyle = buttonStyle;
        comboStyle.width = UiLength::Px(200.0f);
        const UiElementHandle comboHandle = Add(document, panel, UiElementKind::Combo, comboStyle, "combo");

        UiStyle editStyle = buttonStyle;
        editStyle.width = UiLength::Px(320.0f);
        editStyle.height = UiLength::Px(64.0f);
        editStyle.background = UiColor{0.05f, 0.06f, 0.08f, 1.0f};
        const UiElementHandle editHandle = Add(document, panel, UiElementKind::TextEdit, editStyle, "edit");

        UiStyle listStyle;
        listStyle.width = UiLength::Px(420.0f);
        listStyle.height = UiLength::Px(300.0f);
        listStyle.background = UiColor{0.09f, 0.1f, 0.13f, 1.0f};
        listStyle.overflow = UiOverflow::Hidden;
        const UiElementHandle listHandle = Add(document, root, UiElementKind::Panel, listStyle, "list");

        Check(layout.Layout(1280.0f, 720.0f), "demo layout");

        UiButton buttonModel(document, button);
        buttonModel.SetHovered(true);
        buttonModel.PointerDown();
        buttonModel.PointerUp(true);
        Check(buttonModel.ConsumeClick(), "demo button click");

        UiCheckbox checkbox(document, check);
        checkbox.Toggle();
        Check(checkbox.ConsumeChanged(), "demo checkbox");

        UiCombo combo(document, comboHandle);
        combo.SetItems({"One", "Two", "Three"});
        combo.Open();
        combo.HandleKey(UiKeyDown);
        combo.HandleKey(UiKeyEnter);
        Check(combo.Selected() == 0, "demo combo");

        UiTextEdit edit(document, editHandle, &text);
        edit.SetText("Hello UI Toolkit");
        edit.SetCaret(5);
        edit.HandleKey(UiKeyEnd, false, false);
        edit.SetComposition("中文");
        edit.CommitComposition();
        Check(!edit.Text().empty(), "demo text");

        UiListView list(document, listHandle);
        list.SetItemCount(1000);
        list.SetRowHeight(24.0f);
        list.SetViewportHeight(300.0f);
        list.SetScrollOffset(480.0f);
        list.Sync();
        Check(list.Rows().size() <= 20, "demo virtual rows");
        Check(layout.Layout(1280.0f, 720.0f), "demo relayout with rows");

        UiScrollView scroll;
        scroll.SetContentHeight(1000.0f);
        scroll.SetViewportHeight(300.0f);
        scroll.ScrollBy(480.0f);

        UiOverlay overlay;
        overlay.Open(UiOverlay::Layer::Menu, {100.0f, 100.0f, 200.0f, 120.0f}, 1);
        overlay.Open(UiOverlay::Layer::Modal, {200.0f, 150.0f, 400.0f, 240.0f}, 2);
        Check(overlay.BlocksInput(), "demo modal");

        UiDragDrop drag;
        drag.Begin("asset", 7, {10.0f, 10.0f, 32.0f, 32.0f});
        drag.Over(button, true);
        Check(drag.Drop(), "demo drag drop");

        UiCanvas canvas;
        canvas.AddLine(0.0f, 0.0f, 100.0f, 100.0f, 1.0f, UiColor{1.0f, 1.0f, 1.0f, 1.0f});
        canvas.AddRect({10.0f, 10.0f, 80.0f, 40.0f}, UiColor{0.3f, 0.5f, 1.0f, 1.0f});
        canvas.AddCircle(50.0f, 50.0f, 20.0f, UiColor{1.0f, 0.3f, 0.3f, 1.0f});
        canvas.AddText({0.0f, 0.0f, 120.0f, 16.0f}, "Gizmo", 12.0f, UiColor{1.0f, 1.0f, 1.0f, 1.0f});

        UiSplitter splitter(true, 0.5f, 100.0f, 100.0f);
        splitter.Drag(100.0f, 1000.0f);
        UiTabBar tabs;
        tabs.SetTabs({"Hierarchy", "Console"});
        tabs.Select(1);

        UiHitTest hit(document, layout);
        Check(hit.Hit(30.0f, 30.0f).target.IsValid(), "demo hit");

        UiPaintStats paintStats;
        UiDisplayList displayList;
        UiPainter painter(document, layout);
        Check(painter.Build(root, displayList, paintStats), "demo paint");
        GuiGeometry geometry;
        UiGeometryStats geometryStats;
        std::vector<std::string> errors;
        Check(UiGeometryBuilder{}.Build(displayList, &text, nullptr, {}, geometry, geometryStats, errors),
              "demo geometry");

        std::cout << "WidgetsDemo OK: nodes=" << paintStats.nodes
                  << " display=" << paintStats.commands
                  << " quads=" << geometryStats.quads
                  << " batches=" << geometryStats.drawCalls
                  << " listRows=" << list.Rows().size()
                  << " tabs=" << tabs.Tabs().size()
                  << " canvas=" << canvas.Commands().size() << '\n';
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}

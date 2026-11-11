#include "FunctionLayer/UI/Core/UiInput.h"
#include "FunctionLayer/UI/Core/UiDocument.h"
#include "FunctionLayer/UI/Core/UiLayout.h"
#include "FunctionLayer/UI/Core/UiStyle.h"

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

UiStyle Absolute(float x, float y, float width, float height)
{
    UiStyle style;
    style.position = UiPosition::Absolute;
    style.left = UiLength::Px(x);
    style.top = UiLength::Px(y);
    style.width = UiLength::Px(width);
    style.height = UiLength::Px(height);
    return style;
}

struct Fixture
{
    UiDocument document;
    UiStubTextService text{8.0f, 16.0f};
    UiLayoutEngine layout{document, &text};
    UiDispatcher dispatcher{document, layout};
    UiElementHandle root = document.Root();

    UiElementHandle Add(UiElementKind kind, UiElementHandle parent, const UiStyle& style)
    {
        const UiElementHandle element = document.CreateElement(kind);
        Check(document.SetParent(element, parent), "attach input element");
        Check(document.SetStyle(element, style), "style input element");
        return element;
    }
};

void HitTesting()
{
    Fixture fixture;
    const UiElementHandle bottom = fixture.Add(UiElementKind::Button, fixture.root,
                                                Absolute(10.0f, 10.0f, 100.0f, 50.0f));
    const UiElementHandle top = fixture.Add(UiElementKind::Panel, fixture.root,
                                             Absolute(10.0f, 10.0f, 100.0f, 50.0f));
    Check(fixture.layout.Layout(400.0f, 300.0f), "hit layout");
    UiHitTest hit(fixture.document, fixture.layout);
    Check(hit.Hit(50.0f, 30.0f).target == top, "last child wins overlap");
    Check(hit.Hit(200.0f, 200.0f).target == fixture.root, "root fallback");

    UiStyle transparent = fixture.document.Style(top);
    transparent.pointerEvents = UiPointerEvents::None;
    fixture.document.SetStyle(top, transparent);
    Check(hit.Hit(50.0f, 30.0f).target == bottom, "pointer-events none passes through");

    Check(fixture.document.SetEnabled(bottom, false), "disable bottom");
    Check(hit.Hit(50.0f, 30.0f).target != bottom, "disabled target skipped");
    Check(fixture.document.SetEnabled(bottom, true), "enable bottom");

    const UiElementHandle clipping = fixture.Add(UiElementKind::Panel, fixture.root,
                                                  Absolute(150.0f, 100.0f, 50.0f, 50.0f));
    UiStyle clippingStyle = fixture.document.Style(clipping);
    clippingStyle.overflow = UiOverflow::Hidden;
    fixture.document.SetStyle(clipping, clippingStyle);
    const UiElementHandle outside = fixture.Add(UiElementKind::Button, clipping,
                                                 Absolute(60.0f, 0.0f, 50.0f, 50.0f));
    Check(fixture.layout.Layout(400.0f, 300.0f), "clip layout");
    const UiHitResult clipped = hit.Hit(180.0f, 120.0f);
    Check(clipped.target != outside, "child outside overflow clip is not hit");
    Check(clipped.path.empty() || clipped.path.back() != outside, "path excludes clipped child");
}

void EventOrder()
{
    Fixture fixture;
    const UiElementHandle panel = fixture.Add(UiElementKind::Panel, fixture.root,
                                               Absolute(20.0f, 20.0f, 200.0f, 100.0f));
    const UiElementHandle button = fixture.Add(UiElementKind::Button, panel,
                                                Absolute(10.0f, 10.0f, 80.0f, 30.0f));
    Check(fixture.layout.Layout(400.0f, 300.0f), "event order layout");

    std::vector<std::string> order;
    fixture.dispatcher.AddHandler(fixture.root, UiEventType::PointerDown,
                                  [&order](UiInputEvent&) { order.emplace_back("root-capture"); },
                                  UiEventPhase::Capture);
    fixture.dispatcher.AddHandler(panel, UiEventType::PointerDown,
                                  [&order](UiInputEvent&) { order.emplace_back("panel-capture"); },
                                  UiEventPhase::Capture);
    fixture.dispatcher.AddHandler(button, UiEventType::PointerDown,
                                  [&order](UiInputEvent& event) {
                                      order.emplace_back("target");
                                      Check(event.phase == UiEventPhase::Target, "target phase");
                                  });
    fixture.dispatcher.AddHandler(panel, UiEventType::PointerDown,
                                  [&order](UiInputEvent&) { order.emplace_back("panel-bubble"); },
                                  UiEventPhase::Bubble);
    fixture.dispatcher.AddHandler(fixture.root, UiEventType::PointerDown,
                                  [&order](UiInputEvent&) { order.emplace_back("root-bubble"); },
                                  UiEventPhase::Bubble);

    UiInputEvent event;
    event.type = UiEventType::PointerDown;
    event.x = 50.0f;
    event.y = 45.0f;
    Check(fixture.dispatcher.Dispatch(event), "event dispatched");
    Check(order.size() == 5, "five phase handlers");
    Check(order[0] == "root-capture" && order[1] == "panel-capture" && order[2] == "target" &&
              order[3] == "panel-bubble" && order[4] == "root-bubble", "capture/target/bubble order");

    order.clear();
    fixture.dispatcher.AddHandler(panel, UiEventType::PointerDown,
                                  [](UiInputEvent& event) { event.StopPropagation(); },
                                  UiEventPhase::Bubble);
    Check(fixture.dispatcher.Dispatch(event), "stopped event dispatched");
    Check(order.size() == 4 && order[3] == "panel-bubble", "stop propagation prevents root bubble");
}

void FocusAndKeyboard()
{
    Fixture fixture;
    const UiElementHandle textEdit = fixture.Add(UiElementKind::TextEdit, fixture.root,
                                                  Absolute(10.0f, 10.0f, 200.0f, 40.0f));
    const UiElementHandle button = fixture.Add(UiElementKind::Button, fixture.root,
                                                Absolute(10.0f, 60.0f, 100.0f, 30.0f));
    Check(fixture.layout.Layout(400.0f, 300.0f), "focus layout");

    int gained = 0;
    int lost = 0;
    fixture.dispatcher.AddHandler(textEdit, UiEventType::FocusGained,
                                  [&gained](UiInputEvent&) { ++gained; });
    fixture.dispatcher.AddHandler(textEdit, UiEventType::FocusLost,
                                  [&lost](UiInputEvent&) { ++lost; });

    UiInputEvent down;
    down.type = UiEventType::PointerDown;
    down.x = 20.0f;
    down.y = 20.0f;
    Check(fixture.dispatcher.Dispatch(down), "focus click");
    Check(fixture.dispatcher.Focus() == textEdit, "click focuses text edit");
    Check(gained == 1, "focus gained");

    UiInputEvent key;
    key.type = UiEventType::KeyDown;
    key.key = UiKeyRight;
    int keyCount = 0;
    int textCount = 0;
    fixture.dispatcher.AddHandler(textEdit, UiEventType::KeyDown,
                                  [&keyCount](UiInputEvent&) { ++keyCount; });
    fixture.dispatcher.AddHandler(textEdit, UiEventType::TextInput,
                                  [&textCount](UiInputEvent& event) {
                                      ++textCount;
                                      Check(event.codepoint == U'中', "text codepoint");
                                  });
    Check(fixture.dispatcher.Dispatch(key), "key dispatch");
    Check(keyCount == 1, "focused key handler");
    UiInputEvent text;
    text.type = UiEventType::TextInput;
    text.codepoint = U'中';
    Check(fixture.dispatcher.Dispatch(text), "text dispatch");
    Check(textCount == 1, "focused text handler");

    UiInputEvent tab;
    tab.type = UiEventType::KeyDown;
    tab.key = UiKeyTab;
    Check(fixture.dispatcher.Dispatch(tab), "tab handled");
    Check(fixture.dispatcher.Focus() == button, "tab next focus");
    tab.shift = true;
    Check(fixture.dispatcher.Dispatch(tab), "shift tab handled");
    Check(fixture.dispatcher.Focus() == textEdit, "shift tab previous focus");

    Check(fixture.document.SetVisible(textEdit, false), "hide focused");
    fixture.dispatcher.ValidateFocus();
    Check(!fixture.dispatcher.Focus().IsValid(), "hidden focus cleared");
    Check(lost >= 1, "focus lost on hide");
    UiInputEvent textWhenUnfocused;
    textWhenUnfocused.type = UiEventType::TextInput;
    textWhenUnfocused.codepoint = U'A';
    Check(!fixture.dispatcher.Dispatch(textWhenUnfocused), "unfocused text rejected");
}

void PointerCaptureAndIme()
{
    Fixture fixture;
    const UiElementHandle textEdit = fixture.Add(UiElementKind::TextEdit, fixture.root,
                                                  Absolute(10.0f, 10.0f, 100.0f, 40.0f));
    const UiElementHandle button = fixture.Add(UiElementKind::Button, fixture.root,
                                                Absolute(10.0f, 70.0f, 100.0f, 40.0f));
    Check(fixture.layout.Layout(400.0f, 300.0f), "capture layout");

    int moves = 0;
    int lost = 0;
    fixture.dispatcher.AddHandler(button, UiEventType::PointerMove,
                                  [&moves](UiInputEvent&) { ++moves; });
    fixture.dispatcher.AddHandler(button, UiEventType::PointerCaptureLost,
                                  [&lost](UiInputEvent&) { ++lost; });
    UiInputEvent down;
    down.type = UiEventType::PointerDown;
    down.x = 20.0f;
    down.y = 80.0f;
    Check(fixture.dispatcher.Dispatch(down), "capture down");
    Check(fixture.dispatcher.PointerCapture() == button, "pointer captured");
    UiInputEvent move;
    move.type = UiEventType::PointerMove;
    move.x = 350.0f;
    move.y = 250.0f;
    Check(fixture.dispatcher.Dispatch(move), "captured move");
    Check(moves == 1, "move routed to captured button");
    UiInputEvent up = move;
    up.type = UiEventType::PointerUp;
    Check(fixture.dispatcher.Dispatch(up), "capture up");
    Check(!fixture.dispatcher.PointerCapture().IsValid(), "capture released");
    Check(lost == 1, "pointer capture lost event");

    fixture.dispatcher.SetFocus(textEdit);
    std::vector<UiEventType> ime;
    fixture.dispatcher.AddHandler(textEdit, UiEventType::ImeComposition,
                                  [&ime](UiInputEvent& event) {
                                      ime.push_back(event.type);
                                      Check(event.text == "中", "IME composition text");
                                  });
    fixture.dispatcher.AddHandler(textEdit, UiEventType::ImeCommit,
                                  [&ime](UiInputEvent&) { ime.push_back(UiEventType::ImeCommit); });
    fixture.dispatcher.AddHandler(textEdit, UiEventType::ImeCancel,
                                  [&ime](UiInputEvent&) { ime.push_back(UiEventType::ImeCancel); });
    UiInputEvent composition;
    composition.type = UiEventType::ImeComposition;
    composition.text = "中";
    Check(fixture.dispatcher.Dispatch(composition), "IME composition");
    UiInputEvent commit;
    commit.type = UiEventType::ImeCommit;
    commit.text = "中文";
    Check(fixture.dispatcher.Dispatch(commit), "IME commit");
    Check(ime.size() == 2 && ime[0] == UiEventType::ImeComposition && ime[1] == UiEventType::ImeCommit,
          "IME order");
    fixture.dispatcher.ClearFocus();
    Check(ime.size() == 3 && ime[2] == UiEventType::ImeCancel, "focus loss cancels IME");
}

void OverlayBlocks()
{
    Fixture fixture;
    const UiElementHandle viewport = fixture.Add(UiElementKind::Button, fixture.root,
                                                  Absolute(10.0f, 10.0f, 200.0f, 200.0f));
    const UiElementHandle overlay = fixture.Add(UiElementKind::Panel, fixture.root,
                                                 Absolute(0.0f, 0.0f, 400.0f, 300.0f));
    Check(fixture.layout.Layout(400.0f, 300.0f), "overlay layout");
    UiHitTest hit(fixture.document, fixture.layout);
    Check(hit.Hit(50.0f, 50.0f).target == overlay, "editor overlay blocks viewport");
    Check(hit.Hit(50.0f, 50.0f).target != viewport, "overlay does not pass through");
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        HitTesting();
        EventOrder();
        FocusAndKeyboard();
        PointerCaptureAndIme();
        OverlayBlocks();

        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            std::ofstream(proof)
                << "{\"success\":true,\"hit\":true,\"overlap\":true,\"clip\":true,"
                   "\"phaseOrder\":true,\"stopPropagation\":true,\"focus\":true,\"tab\":true,"
                   "\"pointerCapture\":true,\"ime\":true,\"overlay\":true}\n";
        }
        std::cout << "UiCoreInputTests passed (hit, clip, phases, focus, tab, capture, IME, overlay).\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}

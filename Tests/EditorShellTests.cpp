#include "CoreLayer/Localization.h"
#include "FunctionLayer/UI/Core/UiFrameData.h"
#include "ToolLayer/Editor/EditorUi/EditorShell.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
using namespace se::ui;
using namespace se::ui::editor;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void ShellComposition()
{
    EditorShell shell;
    Check(shell.Initialize(1280.0f, 720.0f), "shell initialize");
    Check(shell.Root().IsValid() && shell.ConsolePanel().IsValid(), "shell handles");
    Check(shell.Dock().Root().kind == DockNode::Kind::Split && shell.Dock().Root().children.size() == 2,
          "shell dock default");
    Check(shell.Chrome().Status() == "Ready", "shell chrome status");

    shell.SetHierarchy({
        {1, 0, 0, true, true, "Root"},
        {2, 1, 1, false, true, "Child"},
    });
    Check(!shell.HierarchyList().Rows().empty(), "shell hierarchy rows");
    shell.Frame();
    const UiRect hierarchyRect = shell.Layout().Rect(shell.HierarchyContent());
    UiInputEvent collapse;
    collapse.type = UiEventType::PointerDown;
    collapse.x = hierarchyRect.x + 4.0f;
    collapse.y = hierarchyRect.y + 8.0f;
    Check(shell.Dispatcher().Dispatch(collapse), "shell hierarchy collapse");
    Check(shell.HierarchyList().Rows().size() == 1, "shell hierarchy collapsed rows");
    shell.SetHierarchy({
        {1, 0, 0, true, true, "Root"},
        {2, 1, 1, false, true, "Child"},
    });
    Check(shell.HierarchyList().Rows().size() == 1, "shell hierarchy expansion persisted");

    UiInputEvent expand = collapse;
    Check(shell.Dispatcher().Dispatch(expand), "shell hierarchy expand");
    Check(shell.HierarchyList().Rows().size() == 2, "shell hierarchy expanded rows");
    std::uint64_t draggedId = 0;
    std::uint64_t parentId = 0;
    shell.SetReparentHandler([&](std::uint64_t id, std::uint64_t parent) {
        draggedId = id;
        parentId = parent;
        return true;
    });
    UiInputEvent dragDown = collapse;
    dragDown.x = hierarchyRect.x + 30.0f;
    dragDown.y = hierarchyRect.y + 28.0f;
    Check(shell.Dispatcher().Dispatch(dragDown), "shell hierarchy drag down");
    UiInputEvent dragMove = dragDown;
    dragMove.type = UiEventType::PointerMove;
    dragMove.y = hierarchyRect.y + 8.0f;
    Check(shell.Dispatcher().Dispatch(dragMove), "shell hierarchy drag move");
    UiInputEvent dragUp = dragMove;
    dragUp.type = UiEventType::PointerUp;
    Check(shell.Dispatcher().Dispatch(dragUp), "shell hierarchy drag up");
    Check(draggedId == 2 && parentId == 1, "shell hierarchy reparent callback");
    shell.SetAssets({
        {"Textures/a.png", "texture"},
        {"Scenes/Main.scene.json", "scene"},
    });
    shell.Frame();
    Check(!shell.AssetsList().Rows().empty(), "shell asset rows");
    std::string selectedAsset;
    shell.SetAssetSelectionHandler([&selectedAsset](std::string path) { selectedAsset = std::move(path); });
    const UiRect assetsRect = shell.Layout().Rect(shell.AssetsContent());
    UiInputEvent assetClick;
    assetClick.type = UiEventType::PointerDown;
    assetClick.x = assetsRect.x + 20.0f;
    assetClick.y = assetsRect.y + 8.0f;
    Check(shell.Dispatcher().Dispatch(assetClick), "shell asset click");
    Check(selectedAsset == "Textures/a.png", "shell asset selection");
    shell.SetInspector(2, "Child", {
        {"name", "Name", EditorPropertyType::String, "Child", false},
    });
    Check(!shell.InspectorList().Rows().empty(), "shell inspector rows");

    std::string committedKey;
    std::string committedValue;
    std::uint64_t committedId = 0;
    shell.SetPropertyHandler([&](std::uint64_t id, std::string key, std::string value) {
        committedId = id;
        committedKey = std::move(key);
        committedValue = std::move(value);
        return true;
    });
    const UiRect inspectorRect = shell.Layout().Rect(shell.InspectorContent());
    UiInputEvent propertyClick;
    propertyClick.type = UiEventType::PointerDown;
    propertyClick.x = inspectorRect.x + 20.0f;
    propertyClick.y = inspectorRect.y + 10.0f;
    Check(shell.Dispatcher().Dispatch(propertyClick), "shell property click");
    UiInputEvent propertyText;
    propertyText.type = UiEventType::TextInput;
    propertyText.codepoint = U'X';
    Check(shell.Dispatcher().Dispatch(propertyText), "shell property text");
    UiInputEvent propertyEnter;
    propertyEnter.type = UiEventType::KeyDown;
    propertyEnter.key = UiKeyEnter;
    Check(shell.Dispatcher().Dispatch(propertyEnter), "shell property enter");
    Check(committedId == 2 && committedKey == "name" && committedValue == "XChild",
          "shell property commit");

    int executed = 0;
    shell.Commands().Register(EditorCommandId::Save, [&executed](const EditorCommand&) {
        ++executed;
        return true;
    });
    shell.Router().RegisterShortcut('S', true, false, false, EditorCommandId::Save);
    UiInputEvent key;
    key.type = UiEventType::KeyDown;
    key.key = 'S';
    key.control = true;
    Check(shell.Router().RouteKey(key, false) && executed == 1, "shell shortcut");
    int opened = 0, savedAs = 0;
    shell.Commands().Register(EditorCommandId::Open, [&opened](const EditorCommand&) { ++opened; return true; });
    shell.Commands().Register(EditorCommandId::SaveAs, [&savedAs](const EditorCommand&) { ++savedAs; return true; });
    UiInputEvent openKey;
    openKey.type = UiEventType::KeyDown;
    openKey.key = 'O';
    openKey.control = true;
    Check(shell.Router().RouteKey(openKey, false) && opened == 1, "ctrl+o open");
    UiInputEvent saveAsKey;
    saveAsKey.type = UiEventType::KeyDown;
    saveAsKey.key = 'S';
    saveAsKey.control = true;
    saveAsKey.shift = true;
    Check(shell.Router().RouteKey(saveAsKey, false) && savedAs == 1, "ctrl+shift+s save as");

    for (int index = 0; index < 1000; ++index)
    {
        shell.AppendConsoleLine("line " + std::to_string(index));
    }
    shell.Frame();
    Check(shell.ConsoleLines().size() == 1003, "shell console lines");
    Check(!shell.ConsoleList().Rows().empty(), "shell console visible rows");
    Check(shell.ConsoleList().Rows().size() < 50, "shell console virtualized");

    shell.Resize(800.0f, 600.0f);
    shell.Frame();
    Check(shell.Width() == 800.0f && shell.Height() == 600.0f, "shell resize");
    const UiRect consoleRect = shell.Layout().Rect(shell.ConsolePanel());
    Check(consoleRect.width > 0.0f && consoleRect.height > 0.0f, "shell console layout");
    Check(consoleRect.x + consoleRect.width <= 800.0f + 1.0f, "shell console bounds");
    shell.SetDpiScale(1.5f);
    shell.Resize(1280.0f, 760.0f);
    shell.Frame();
    Check(std::abs(shell.Width() - 1280.0f / 1.5f) < 1.0f &&
              std::abs(shell.Height() - 760.0f / 1.5f) < 1.0f,
          "shell dpi 1.5 logical size");
    const UiRect scaledHierarchy = shell.Layout().Rect(shell.HierarchyContent());
    Check(scaledHierarchy.width > 0.0f && scaledHierarchy.height > 0.0f, "shell dpi 1.5 layout");
    shell.SetDpiScale(1.0f);
    shell.Resize(800.0f, 600.0f);
    shell.Frame();
    UiInputEvent wheel;
    wheel.type = UiEventType::PointerWheel;
    wheel.x = consoleRect.x + 20.0f;
    wheel.y = consoleRect.y + 40.0f;
    wheel.wheel = 1.0f;
    Check(shell.Dispatcher().Dispatch(wheel), "shell console wheel dispatch");
    Check(shell.ConsoleList().Offset() > 0.0f, "shell console wheel scroll");

    const float leftBefore = shell.Dock().Root().ratio;
    UiInputEvent splitDown;
    splitDown.type = UiEventType::PointerDown;
    splitDown.x = shell.Width() * leftBefore;
    splitDown.y = 200.0f;
    Check(shell.Dispatcher().Dispatch(splitDown), "shell splitter down");
    UiInputEvent splitMove = splitDown;
    splitMove.type = UiEventType::PointerMove;
    splitMove.x += 60.0f;
    Check(shell.Dispatcher().Dispatch(splitMove), "shell splitter move");
    UiInputEvent splitUp = splitMove;
    splitUp.type = UiEventType::PointerUp;
    Check(shell.Dispatcher().Dispatch(splitUp), "shell splitter up");
    Check(shell.Dock().Root().ratio > leftBefore, "shell splitter moves dock ratio");

    const UiElementHandle consoleTab = shell.TabButton(EditorPanelId::Console);
    Check(consoleTab.IsValid(), "shell console tab");
    const UiRect tabRect = shell.Layout().Rect(consoleTab);
    UiInputEvent tabClick;
    tabClick.type = UiEventType::PointerDown;
    tabClick.x = tabRect.x + tabRect.width * 0.5f;
    tabClick.y = tabRect.y + tabRect.height * 0.5f;
    Check(shell.Dispatcher().Dispatch(tabClick), "shell tab dispatch");
    Check(shell.ActivePanel() == EditorPanelId::Console, "shell tab activates console");

    shell.SetDpiScale(2.0f);
    shell.Resize(1600.0f, 1200.0f);
    Check(shell.DpiScale() == 2.0f && shell.Width() == 800.0f && shell.Height() == 600.0f,
          "shell dpi logical size");
    shell.SetDpiScale(1.0f);
    shell.Resize(800.0f, 600.0f);

    shell.SetGizmo(400.0f, 300.0f, true);
    shell.Frame();
    int gizmoAxis = -1;
    float gizmoDx = 0.0f;
    shell.SetGizmoDragHandler([&](int axis, float dx, float) {
        gizmoAxis = axis;
        gizmoDx += dx;
    });
    UiInputEvent gizmoDown;
    gizmoDown.type = UiEventType::PointerDown;
    gizmoDown.x = 418.0f;
    gizmoDown.y = 300.0f;
    Check(shell.Dispatcher().Dispatch(gizmoDown), "shell gizmo down");
    UiInputEvent gizmoMove = gizmoDown;
    gizmoMove.type = UiEventType::PointerMove;
    gizmoMove.x += 12.0f;
    Check(shell.Dispatcher().Dispatch(gizmoMove), "shell gizmo move");
    UiInputEvent gizmoUp = gizmoMove;
    gizmoUp.type = UiEventType::PointerUp;
    Check(shell.Dispatcher().Dispatch(gizmoUp), "shell gizmo up");
    Check(gizmoAxis == 0 && gizmoDx > 0.0f, "shell gizmo axis drag");

    UiFrameData frame;
    frame.role = UiLayerRole::Editor;
    Check(frame.geometry.vertices.empty() && frame.role == UiLayerRole::Editor, "frame data contract");
}

void ChromeDockAndPersistence()
{
    EditorShell shell;
    Check(shell.Initialize(1280.0f, 760.0f), "chrome shell initialize");
    Check(shell.MenuBar().IsValid() && shell.Toolbar().IsValid() && shell.StatusBar().IsValid(),
          "chrome bars exist");
    shell.Frame();
    Check(shell.Layout().Rect(shell.MenuBar()).height == 24.0f, "menu bar height");
    {
        Localization zh;
        std::string error;
        Check(zh.Load("Resources/Localization/zh-CN.txt", error), "localization load");
        EditorShell localized;
        localized.SetLocalization(&zh);
        Check(localized.Initialize(1280.0f, 760.0f), "localized shell initialize");
        const UiElementHandle fileMenu = localized.Document().FirstChild(localized.MenuBar());
        Check(fileMenu.IsValid() && localized.Document().Text(fileMenu) == "文件", "localized menu text");
        Check(localized.Chrome().Status() == "就绪", "localized status text");
    }
    Check(shell.Layout().Rect(shell.Toolbar()).height == 32.0f, "toolbar height");
    Check(std::abs(shell.Layout().Rect(shell.StatusBar()).y - (760.0f - 24.0f)) < 1.0f,
          "status bar bottom");

    const UiRect viewport = shell.Layout().Rect(shell.ViewportPanel());
    Check(std::abs(viewport.x - 256.0f) < 1.0f, "dock left 20 percent");
    Check(std::abs(viewport.width - 716.8f) < 1.5f, "dock center 56 percent");
    Check(std::abs(viewport.y - 80.0f) < 1.0f, "dock top chrome offset");
    Check(std::abs(viewport.height - 656.0f) < 1.5f, "dock bottom status offset");

    const auto click = [&shell](UiElementHandle element) {
        const UiRect rect = shell.Layout().Rect(element);
        UiInputEvent down;
        down.type = UiEventType::PointerDown;
        down.x = rect.x + rect.width * 0.5f;
        down.y = rect.y + rect.height * 0.5f;
        Check(shell.Dispatcher().Dispatch(down), "click down");
        UiInputEvent up = down;
        up.type = UiEventType::PointerUp;
        Check(shell.Dispatcher().Dispatch(up), "click up");
    };

    const UiElementHandle assetsTab = shell.TabButton(EditorPanelId::Assets);
    Check(assetsTab.IsValid(), "assets tab button");
    click(assetsTab);
    shell.Frame();
    Check(shell.ActivePanel() == EditorPanelId::Assets, "tab click activates assets");
    Check(shell.Document().Visible(shell.Panel(EditorPanelId::Assets)), "assets panel visible");

    const UiElementHandle designerTab = shell.TabButton(EditorPanelId::UiDesigner);
    Check(designerTab.IsValid(), "ui designer tab button");
    click(designerTab);
    shell.Frame();
    Check(shell.ActivePanel() == EditorPanelId::UiDesigner, "tab click activates ui designer");
    Check(shell.Document().Visible(shell.Panel(EditorPanelId::UiDesigner)), "ui designer visible");
    Check(!shell.Document().Visible(shell.Panel(EditorPanelId::Viewport)), "viewport hidden by tab");
    click(shell.TabButton(EditorPanelId::Viewport));
    shell.Frame();
    Check(shell.Document().Visible(shell.Panel(EditorPanelId::Viewport)), "viewport restored by tab");

    shell.TogglePanel(EditorPanelId::Console);
    shell.Frame();
    Check(shell.Dock().Find(EditorPanelId::Console) == nullptr, "panel close removes tab");
    shell.TogglePanel(EditorPanelId::Console);
    shell.Frame();
    Check(shell.Dock().Find(EditorPanelId::Console) != nullptr, "panel reopen restores tab");

    shell.SetStatus("TestStatus");
    shell.SetDirty(true);
    shell.SetSelectionLabel("Entity 7");
    const std::string_view status = shell.Document().Text(shell.StatusBar());
    Check(status.find("TestStatus") != std::string_view::npos, "status text");
    Check(status.find("unsaved") != std::string_view::npos, "status dirty marker");
    Check(status.find("Entity 7") != std::string_view::npos, "status selection");

    int saves = 0;
    shell.Commands().Register(EditorCommandId::Save, [&saves](const EditorCommand&) {
        ++saves;
        return true;
    });
    const UiElementHandle fileMenu = shell.Document().FirstChild(shell.MenuBar());
    Check(fileMenu.IsValid(), "file menu button");
    click(fileMenu);
    Check(shell.MenuOpen(), "menu opens");
    shell.Frame();
    const UiElementHandle dropdown = shell.MenuDropdown();
    Check(dropdown.IsValid(), "menu dropdown exists");
    const UiElementHandle openItem = shell.Document().FirstChild(dropdown);
    const UiElementHandle newItem = shell.Document().NextSibling(openItem);
    const UiElementHandle saveItem = shell.Document().NextSibling(newItem);
    Check(saveItem.IsValid(), "menu save item");
    click(saveItem);
    Check(saves == 1, "menu triggers command");
    Check(!shell.MenuOpen(), "menu closes after command");

    const std::filesystem::path layoutPath = ".cache/editor-ui-test/shell.json";
    std::error_code error;
    std::filesystem::remove(layoutPath, error);
    std::filesystem::remove(layoutPath.string() + ".bak", error);
    shell.SetLayoutPath(layoutPath);
    Check(shell.SaveLayout(), "save layout");

    const float leftBefore = shell.Dock().Root().ratio;
    const float splitX = shell.Width() * leftBefore;
    UiInputEvent splitDown;
    splitDown.type = UiEventType::PointerDown;
    splitDown.x = splitX;
    splitDown.y = 200.0f;
    Check(shell.Dispatcher().Dispatch(splitDown), "splitter down");
    UiInputEvent splitMove = splitDown;
    splitMove.type = UiEventType::PointerMove;
    splitMove.x += 32.0f;
    Check(shell.Dispatcher().Dispatch(splitMove), "splitter move");
    UiInputEvent splitUp = splitMove;
    splitUp.type = UiEventType::PointerUp;
    Check(shell.Dispatcher().Dispatch(splitUp), "splitter up");
    Check(shell.Dock().Root().ratio > leftBefore, "splitter persisted value changed");
    Check(shell.SaveLayout(), "save layout after splitter");

    EditorShell restored;
    Check(restored.Initialize(1280.0f, 760.0f), "restored shell initialize");
    restored.SetLayoutPath(layoutPath);
    restored.Frame();
    Check(std::abs(restored.Dock().Root().ratio - shell.Dock().Root().ratio) < 1e-4f,
          "dock ratio restored from disk");

    std::ofstream(layoutPath, std::ios::binary | std::ios::trunc) << "{ broken";
    EditorShell fallback;
    Check(fallback.Initialize(1280.0f, 760.0f), "fallback shell initialize");
    fallback.SetLayoutPath(layoutPath);
    fallback.Frame();
    Check(fallback.Dock().Root() == DefaultEditorDockLayout(), "bad layout falls back");
    Check(std::filesystem::exists(layoutPath.string() + ".bak"), "bad layout backed up");

    // Tab 拖动合并：把 Assets 拖到 Hierarchy 组，两个标签进入同一组。
    const UiElementHandle hierarchyTab = shell.TabButton(EditorPanelId::Hierarchy);
    const UiElementHandle draggedTab = shell.TabButton(EditorPanelId::Assets);
    Check(hierarchyTab.IsValid() && draggedTab.IsValid(), "tab drag handles");
    const UiRect hierarchyRect = shell.Layout().Rect(hierarchyTab);
    const UiRect draggedRect = shell.Layout().Rect(draggedTab);
    UiInputEvent dragDown;
    dragDown.type = UiEventType::PointerDown;
    dragDown.x = draggedRect.x + draggedRect.width * 0.5f;
    dragDown.y = draggedRect.y + draggedRect.height * 0.5f;
    Check(shell.Dispatcher().Dispatch(dragDown), "tab drag down");
    UiInputEvent dragMove = dragDown;
    dragMove.type = UiEventType::PointerMove;
    dragMove.x = hierarchyRect.x + hierarchyRect.width * 0.5f;
    dragMove.y = hierarchyRect.y + hierarchyRect.height * 0.5f;
    Check(shell.Dispatcher().Dispatch(dragMove), "tab drag move");
    UiInputEvent dragUp = dragMove;
    dragUp.type = UiEventType::PointerUp;
    Check(shell.Dispatcher().Dispatch(dragUp), "tab drag up");
    shell.Frame();
    const DockNode* merged = shell.Dock().Find(EditorPanelId::Assets);
    const DockNode* mergedWith = shell.Dock().Find(EditorPanelId::Hierarchy);
    Check(merged != nullptr && merged == mergedWith, "tab drag merges groups");
    Check(std::find(merged->tabs.begin(), merged->tabs.end(), EditorPanelId::Assets) != merged->tabs.end(),
          "merged group contains assets");
}

void CloseProtection()
{
    EditorShell clean;
    Check(clean.Initialize(800.0f, 600.0f), "clean close initialize");
    Check(!clean.RequestClose(), "clean close allowed");
    Check(clean.CloseApproved(), "clean close approved");

    EditorShell shell;
    Check(shell.Initialize(800.0f, 600.0f), "dirty close initialize");
    int saves = 0;
    shell.Commands().Register(EditorCommandId::Save, [&saves](const EditorCommand&) {
        ++saves;
        return true;
    });
    shell.SetDirty(true);
    Check(shell.RequestClose(), "dirty close blocked");
    Check(shell.CloseModalOpen(), "close modal visible");
    shell.Frame();
    Check(shell.CloseModalDialog().IsValid(), "close modal dialog");
    const auto click = [&shell](UiElementHandle element) {
        const UiRect rect = shell.Layout().Rect(element);
        UiInputEvent down;
        down.type = UiEventType::PointerDown;
        down.x = rect.x + rect.width * 0.5f;
        down.y = rect.y + rect.height * 0.5f;
        Check(shell.Dispatcher().Dispatch(down), "modal click down");
        UiInputEvent up = down;
        up.type = UiEventType::PointerUp;
        Check(shell.Dispatcher().Dispatch(up), "modal click up");
    };

    const UiElementHandle title = shell.Document().FirstChild(shell.CloseModalDialog());
    const UiElementHandle message = shell.Document().NextSibling(title);
    const UiElementHandle saveButton = shell.Document().NextSibling(message);
    const UiElementHandle discardButton = shell.Document().NextSibling(saveButton);
    const UiElementHandle cancelButton = shell.Document().NextSibling(discardButton);
    Check(saveButton.IsValid() && discardButton.IsValid() && cancelButton.IsValid(), "modal buttons");
    click(cancelButton);
    Check(!shell.CloseModalOpen() && !shell.CloseApproved(), "cancel keeps editor open");
    Check(shell.RequestClose(), "close requested again");
    shell.Frame();
    Check(shell.CloseModalOpen(), "modal reopened after cancel");
    const UiElementHandle newTitle = shell.Document().FirstChild(shell.CloseModalDialog());
    const UiElementHandle newMessage = shell.Document().NextSibling(newTitle);
    const UiElementHandle newSave = shell.Document().NextSibling(newMessage);
    const UiElementHandle newDiscard = shell.Document().NextSibling(newSave);
    click(newDiscard);
    Check(shell.CloseApproved(), "discard approves close");
    Check(shell.ConsumeCloseRequest(), "discard requests application close");
    Check(saves == 0, "discard does not save");

    // 保存路径。
    EditorShell saveShell;
    Check(saveShell.Initialize(800.0f, 600.0f), "save close initialize");
    int saved = 0;
    saveShell.Commands().Register(EditorCommandId::Save, [&saved](const EditorCommand&) {
        ++saved;
        return true;
    });
    saveShell.SetDirty(true);
    Check(saveShell.RequestClose(), "save close blocked");
    saveShell.Frame();
    const UiElementHandle saveTitle = saveShell.Document().FirstChild(saveShell.CloseModalDialog());
    const UiElementHandle saveMessage = saveShell.Document().NextSibling(saveTitle);
    const UiElementHandle saveAction = saveShell.Document().NextSibling(saveMessage);
    const UiRect saveRect = saveShell.Layout().Rect(saveAction);
    UiInputEvent saveDown;
    saveDown.type = UiEventType::PointerDown;
    saveDown.x = saveRect.x + saveRect.width * 0.5f;
    saveDown.y = saveRect.y + saveRect.height * 0.5f;
    Check(saveShell.Dispatcher().Dispatch(saveDown), "save modal click");
    Check(saved == 1 && saveShell.CloseApproved(), "save approves close");
    Check(saveShell.ConsumeCloseRequest(), "save requests application close");
}

void FormPanels()
{
    EditorShell shell;
    Check(shell.Initialize(1000.0f, 700.0f), "form shell initialize");
    shell.TogglePanel(EditorPanelId::Material);
    shell.Frame();
    shell.SetFormProperties(EditorPanelId::Material, {
        {"name", "Name", EditorPropertyType::String, "NewMaterial", false},
        {"roughness", "Roughness", EditorPropertyType::Number, "0.5", false},
    });
    std::string committedKey;
    std::string committedValue;
    int saves = 0;
    int actions = 0;
    shell.SetFormHandlers(EditorPanelId::Material,
        [&](std::string key, std::string value) {
            committedKey = std::move(key);
            committedValue = std::move(value);
            return true;
        },
        [&]() {
            ++saves;
            return true;
        });
    shell.SetFormAction(EditorPanelId::Material, "Preview", [&]() {
        ++actions;
        return true;
    });
    shell.Frame();
    const UiElementHandle row = shell.FormRow(EditorPanelId::Material, 0);
    Check(row.IsValid(), "form row exists");
    const UiRect rowRect = shell.Layout().Rect(row);
    UiInputEvent rowDown;
    rowDown.type = UiEventType::PointerDown;
    rowDown.x = rowRect.x + 20.0f;
    rowDown.y = rowRect.y + 10.0f;
    Check(shell.Dispatcher().Dispatch(rowDown), "form row click");
    UiInputEvent text;
    text.type = UiEventType::TextInput;
    text.codepoint = U'X';
    Check(shell.Dispatcher().Dispatch(text), "form row text");
    UiInputEvent enter;
    enter.type = UiEventType::KeyDown;
    enter.key = UiKeyEnter;
    Check(shell.Dispatcher().Dispatch(enter), "form row commit");
    Check(committedKey == "name" && !committedValue.empty(), "form commit callback");

    const UiElementHandle saveButton = shell.FormSaveButton(EditorPanelId::Material);
    const UiRect saveRect = shell.Layout().Rect(saveButton);
    UiInputEvent saveDown;
    saveDown.type = UiEventType::PointerDown;
    saveDown.x = saveRect.x + 10.0f;
    saveDown.y = saveRect.y + 10.0f;
    Check(shell.Dispatcher().Dispatch(saveDown), "form save click");
    Check(saves == 1, "form save callback");
    const UiElementHandle actionButton = shell.FormActionButton(EditorPanelId::Material);
    const UiRect actionRect = shell.Layout().Rect(actionButton);
    UiInputEvent actionDown;
    actionDown.type = UiEventType::PointerDown;
    actionDown.x = actionRect.x + 10.0f;
    actionDown.y = actionRect.y + 10.0f;
    Check(shell.Dispatcher().Dispatch(actionDown), "form action click");
    Check(actions == 1, "form action callback");
    shell.SetFormStatus(EditorPanelId::Material, "FormStatus");
    Check(shell.Document().Visible(shell.FormRow(EditorPanelId::Material, 0)), "form row visible");

    std::vector<EditorProperty> many;
    for (int index = 0; index < 20; ++index)
    {
        many.push_back({"field" + std::to_string(index), "Field", EditorPropertyType::String,
                        "value" + std::to_string(index), false});
    }
    shell.SetFormProperties(EditorPanelId::Material, std::move(many));
    shell.Frame();
    const UiRect formList = shell.Layout().Rect(shell.FormListContent(EditorPanelId::Material));
    const float rowBefore = shell.Layout().Rect(shell.FormRow(EditorPanelId::Material, 0)).y;
    UiInputEvent wheel;
    wheel.type = UiEventType::PointerWheel;
    wheel.x = formList.x + 10.0f;
    wheel.y = formList.y + 10.0f;
    wheel.wheel = 1.0f;
    Check(shell.Dispatcher().Dispatch(wheel), "form wheel dispatch");
    shell.Frame();
    const float rowAfter = shell.Layout().Rect(shell.FormRow(EditorPanelId::Material, 0)).y;
    Check(rowAfter < rowBefore, "form wheel scrolls rows");
    const UiElementHandle thumb = shell.ScrollThumb(EditorPanelId::Material);
    Check(thumb.IsValid() && shell.Document().Visible(thumb), "form scrollbar thumb visible");
    Check(shell.Document().Style(thumb).height.value < shell.Layout().Rect(shell.FormListContent(EditorPanelId::Material)).height,
          "form scrollbar thumb shorter than viewport");
    const UiRect thumbRect = shell.Layout().Rect(thumb);
    UiInputEvent dragDown;
    dragDown.type = UiEventType::PointerDown;
    dragDown.x = thumbRect.x + 2.0f;
    dragDown.y = thumbRect.y + 2.0f;
    Check(shell.Dispatcher().Dispatch(dragDown), "scrollbar drag down");
    UiInputEvent dragMove;
    dragMove.type = UiEventType::PointerMove;
    dragMove.x = dragDown.x;
    dragMove.y = dragDown.y - 40.0f;
    Check(shell.Dispatcher().Dispatch(dragMove), "scrollbar drag move");
    UiInputEvent dragUp;
    dragUp.type = UiEventType::PointerUp;
    Check(shell.Dispatcher().Dispatch(dragUp), "scrollbar drag up");

    shell.SetFormProperties(EditorPanelId::Material,
        {{"baseColor", "Base Color", EditorPropertyType::Color, "1, 0, 0, 1", false}});
    shell.Frame();
    const UiStyle colorRow = shell.Document().Style(shell.FormRow(EditorPanelId::Material, 0));
    Check(colorRow.background.r > 0.9f && colorRow.background.g < 0.1f && colorRow.background.a > 0.0f,
          "color row swatch background");
}

void AuxiliaryPanels()
{
    EditorShell shell;
    Check(shell.Initialize(1000.0f, 700.0f), "aux initialize");
    std::string chosenLanguage;
    shell.SetFormProperties(EditorPanelId::Preferences,
        {{"language", "Language", EditorPropertyType::Combo, "zh-CN", false, {"zh-CN", "en-US"}},
         {"uiScale", "UI scale", EditorPropertyType::Number, "1.000000", false}});
    shell.SetFormHandlers(EditorPanelId::Preferences,
        [&chosenLanguage](std::string key, std::string value) {
            if (key == "language") chosenLanguage = value;
            return true;
        },
        []() { return true; });
    shell.TogglePanel(EditorPanelId::Preferences);
    shell.Frame();
    Check(shell.Document().Visible(shell.PanelContent(EditorPanelId::Preferences)), "preferences visible");
    Check(shell.FormRow(EditorPanelId::Preferences, 0).IsValid(), "preferences form row");
    const UiRect languageRow = shell.Layout().Rect(shell.FormRow(EditorPanelId::Preferences, 0));
    UiInputEvent comboClick;
    comboClick.type = UiEventType::PointerDown;
    comboClick.x = languageRow.x + 20.0f;
    comboClick.y = languageRow.y + 4.0f;
    Check(shell.Dispatcher().Dispatch(comboClick), "combo click dispatch");
    shell.Frame();
    Check(shell.Document().Visible(shell.FormComboPopup(EditorPanelId::Preferences)), "combo popup visible");
    const UiElementHandle option = shell.FormComboOption(EditorPanelId::Preferences, 1);
    Check(option.IsValid(), "combo option exists");
    const UiRect optionRect = shell.Layout().Rect(option);
    UiInputEvent optionClick;
    optionClick.type = UiEventType::PointerDown;
    optionClick.x = optionRect.x + 10.0f;
    optionClick.y = optionRect.y + 4.0f;
    Check(shell.Dispatcher().Dispatch(optionClick), "combo option dispatch");
    Check(chosenLanguage == "en-US", "combo option commits");
    Check(!shell.Document().Visible(shell.FormComboPopup(EditorPanelId::Preferences)), "combo popup closes");
    shell.SetPanelText(EditorPanelId::About, "AboutTest");
    shell.TogglePanel(EditorPanelId::About);
    shell.Frame();
    Check(shell.Document().Visible(shell.PanelContent(EditorPanelId::About)), "about visible");
    Check(shell.Document().Text(shell.PanelContent(EditorPanelId::About)) == "AboutTest", "about text");
    shell.SetPanelText(EditorPanelId::Diagnostics, "DiagTest");
    shell.TogglePanel(EditorPanelId::Diagnostics);
    shell.Frame();
    Check(shell.Document().Text(shell.PanelContent(EditorPanelId::Diagnostics)) == "DiagTest",
          "diagnostics text");

    bool renamed = false;
    shell.SetPropertyHandler([&renamed](std::uint64_t, std::string key, std::string) {
        renamed = key == "name";
        return true;
    });
    shell.SetInspector(1, "Before", {{"name", "Name", EditorPropertyType::String, "Before", false}});
    shell.Frame();
    Check(shell.BeginRename(), "rename begins");
    UiInputEvent enter;
    enter.type = UiEventType::KeyDown;
    enter.key = UiKeyEnter;
    Check(shell.Dispatcher().Dispatch(enter), "rename enter dispatch");
    Check(renamed, "rename commits through property handler");

    bool confirmed = false;
    shell.ShowConfirm("Recovery", "Recover unsaved document?", "Recover", "Discard", "Later",
        [&confirmed]() { confirmed = true; }, nullptr);
    shell.Frame();
    Check(shell.ConfirmOpen(), "confirm modal open");
    const UiRect confirmDialog = shell.Layout().Rect(shell.ConfirmDialog());
    UiInputEvent confirmClick;
    confirmClick.type = UiEventType::PointerDown;
    confirmClick.x = confirmDialog.x + 30.0f;
    confirmClick.y = confirmDialog.y + 150.0f;
    Check(shell.Dispatcher().Dispatch(confirmClick), "confirm primary dispatch");
    Check(confirmed && !shell.ConfirmOpen(), "confirm primary invokes and closes");
}

void ListRefresh()
{
    EditorShell shell;
    Check(shell.Initialize(1000.0f, 700.0f), "list refresh initialize");
    shell.SetHierarchy({{1, 0, 0, false, true, "Before"}});
    shell.Frame();
    Check(!shell.HierarchyList().Rows().empty(), "list refresh row");
    Check(shell.Document().Text(shell.HierarchyList().Rows().front()).find("Before") != std::string::npos,
          "list refresh initial text");
    shell.SetHierarchy({{1, 0, 0, false, true, "After"}});
    shell.Frame();
    Check(!shell.HierarchyList().Rows().empty(), "list refresh row after");
    Check(shell.Document().Text(shell.HierarchyList().Rows().front()).find("After") != std::string::npos,
          "list refresh same-count text changes");
    shell.SetAssets({{"a.png", "texture"}, {"b.png", "texture"}});
    shell.Frame();
    Check(shell.Document().Text(shell.AssetsList().Rows().front()).find("a.png") != std::string::npos,
          "list refresh assets initial");
    shell.SetAssets({{"c.png", "texture"}, {"b.png", "texture"}});
    shell.Frame();
    Check(shell.Document().Text(shell.AssetsList().Rows().front()).find("c.png") != std::string::npos,
          "list refresh assets same-count text changes");
    shell.SetAssetsPreviewText("Material preview: Test");
    shell.Frame();
    Check(shell.Document().Text(shell.AssetsPreview()) == "Material preview: Test", "assets preview text");
    shell.SetAssetPreviewImage("asset:test", 64.0f, 64.0f);
    shell.Frame();
    Check(shell.Document().Visible(shell.AssetsPreviewImage()), "assets image preview visible");
    Check(shell.Document().Style(shell.AssetsPreviewImage()).image == "asset:test", "assets image id");
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        ShellComposition();
        ChromeDockAndPersistence();
        CloseProtection();
        FormPanels();
        AuxiliaryPanels();
        ListRefresh();
        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            std::ofstream(proof)
                << "{\"success\":true,\"shell\":true,\"consoleVirtual\":true,\"dock\":true,"
                   "\"shortcut\":true,\"resize\":true,\"frameData\":true}\n";
        }
        std::cout << "EditorShellTests passed (composition root, console virtualization, dock, shortcut, resize).\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}

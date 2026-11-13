#include "ToolLayer/Editor/EditorUi/EditorShell.h"

#include "ToolLayer/Editor/EditorUi/EditorDockState.h"
#include "CoreLayer/Localization.h"
#include "ToolLayer/Editor/FlarePalette.h"

#include <Windows.h>
#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace se::ui::editor
{
namespace
{
constexpr float kTabHeight = 24.0f;
constexpr float kMinimumGroupSize = 80.0f;

// UiPainter 接收线性颜色，打包 GUI 顶点时才转成 sRGB。
UiColor UiInk(flare::Color color, float alpha=1.0f)
{
    const auto linear=[](std::uint8_t channel) {
        const float v=channel/255.0f;
        return v<=0.04045f ? v/12.92f : std::pow((v+0.055f)/1.055f,2.4f);
    };
    return {linear(color.r),linear(color.g),linear(color.b),alpha};
}
const auto kBackground = UiInk(flare::Background);
const auto kSurface = UiInk(flare::Surface);
const auto kSurfaceRaised = UiInk(flare::Raised);
const auto kSurfaceActive = UiInk(flare::Selected);
const auto kContent = kBackground;
const auto kBorder = UiInk(flare::Border);
const auto kText = UiInk(flare::Text);
const auto kMuted = UiInk(flare::Muted);
const auto kAccent = UiInk(flare::Silver);

std::array<UiRect,2> SplitDockRects(const DockNode& node,const UiRect& rect)
{
    const float total=node.axis==DockAxis::Horizontal?rect.width:rect.height;
    const float minimum=total>0.0f?std::min(0.45f,kMinimumGroupSize/total):0.0f;
    const float ratio=std::clamp(node.ratio,minimum,1.0f-minimum);
    std::array<UiRect,2> children{rect,rect};
    if(node.axis==DockAxis::Horizontal)
    {
        children[0].width=rect.width*ratio;
        children[1].x+=children[0].width;
        children[1].width-=children[0].width;
    }
    else
    {
        children[0].height=rect.height*ratio;
        children[1].y+=children[0].height;
        children[1].height-=children[0].height;
    }
    return children;
}

UiRect DockPanelRect(const DockNode& node,EditorPanelId panel,const UiRect& rect)
{
    if(node.kind==DockNode::Kind::Tabs)
        return std::find(node.tabs.begin(),node.tabs.end(),panel)!=node.tabs.end()?rect:UiRect{};
    if(node.children.size()!=2)return {};
    const auto children=SplitDockRects(node,rect);
    for(size_t index=0;index<2;++index)
    {
        const auto found=DockPanelRect(node.children[index],panel,children[index]);
        if(found.width>0 && found.height>0)return found;
    }
    return {};
}

UiStyle AbsoluteStyle(float x, float y, float width, float height)
{
    UiStyle style;
    style.position = UiPosition::Absolute;
    style.left = UiLength::Px(x);
    style.top = UiLength::Px(y);
    style.width = UiLength::Px(width);
    style.height = UiLength::Px(height);
    return style;
}

UiStyle PanelStyle(const UiRect& rect)
{
    UiStyle style = AbsoluteStyle(rect.x, rect.y, rect.width, rect.height);
    style.background = kSurface;
    style.borderWidth = 1.0f;
    style.borderColor = kBorder;
    style.flexDirection = UiFlexDirection::Column;
    return style;
}

UiStyle ButtonStyle(float x, float y, float width, float height, const UiColor& background)
{
    UiStyle style = AbsoluteStyle(x, y, width, height);
    style.background = background;
    style.color = kText;
    style.fontSize = 13.0f;
    return style;
}

DockNode* NodeAt(DockNode& root, const std::vector<std::size_t>& path)
{
    DockNode* node = &root;
    for (const std::size_t index : path)
    {
        if (index >= node->children.size())
        {
            return nullptr;
        }
        node = &node->children[index];
    }
    return node;
}

std::size_t Utf8Length(std::string_view text)
{
    std::size_t length = 0;
    for (const char ch : text)
    {
        if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80)
        {
            ++length;
        }
    }
    return length;
}

float FallbackTextWidth(const std::string& label, float fontSize)
{
    return static_cast<float>(Utf8Length(label)) * fontSize * 0.62f;
}

UiColor RowBackgroundColor(const EditorProperty& property)
{
    if (property.type != EditorPropertyType::Color)
    {
        return UiColor{0.0f, 0.0f, 0.0f, 0.0f};
    }
    float channels[4]{0.0f, 0.0f, 0.0f, 1.0f};
    const char* cursor = property.value.c_str();
    int count = 0;
    while (count < 4)
    {
        char* end = nullptr;
        const float value = std::strtof(cursor, &end);
        if (end == cursor)
        {
            break;
        }
        channels[count++] = std::clamp(value, 0.0f, 1.0f);
        cursor = end;
        while (*cursor == ' ' || *cursor == ',' || *cursor == '\t')
        {
            ++cursor;
        }
    }
    if (count < 3)
    {
        return UiColor{0.0f, 0.0f, 0.0f, 0.0f};
    }
    // 半透明叠加，保留白色行文字可读性；颜色本身仍可见。
    return UiColor{channels[0], channels[1], channels[2], 0.38f};
}

// 列表行是固定 20px 单行；按估算像素宽截断，中文/ASCII 使用不同宽度。
std::string RowText(std::string text, float maximumWidth = 230.0f)
{
    float width = 0.0f;
    std::size_t bytes = 0;
    while (bytes < text.size())
    {
        const unsigned char lead = static_cast<unsigned char>(text[bytes]);
        std::size_t length = 1;
        if ((lead & 0xE0) == 0xC0) length = 2;
        else if ((lead & 0xF0) == 0xE0) length = 3;
        else if ((lead & 0xF8) == 0xF0) length = 4;
        const float advance = lead < 0x80 ? 7.5f : 13.0f;
        if (width + advance > maximumWidth - 18.0f)
        {
            break;
        }
        width += advance;
        bytes += length;
    }
    if (bytes >= text.size())
    {
        return text;
    }
    return text.substr(0, bytes) + "...";
}

} // namespace

void EditorShell::SetLocalization(const Localization* text) noexcept
{
    localization_ = text;
}

std::string EditorShell::Text(std::string_view key, std::string_view fallback) const
{
    if (localization_ != nullptr)
    {
        std::string value = localization_->GetText(key);
        if (!value.empty() && value != key)
        {
            return value;
        }
    }
    return std::string(fallback);
}

float EditorShell::MeasuredWidth(const std::string& text, float fontSize) const
{
    if (text_ != nullptr && !text.empty())
    {
        const UiTextMeasure measure = text_->Measure(text, fontSize, 100000.0f, false);
        if (std::isfinite(measure.width) && measure.width > 0.0f)
        {
            return measure.width;
        }
    }
    return FallbackTextWidth(text, fontSize);
}

float EditorShell::TabWidth(const std::string& label) const
{
    // 文本宽 + 左内边距 + close 区（18）+ 余量，close 不覆盖最后一个字形。
    return std::clamp(MeasuredWidth(label, 13.0f) + 42.0f, 64.0f, 240.0f);
}

float EditorShell::MenuWidth(const std::string& label) const
{
    return std::max(56.0f, MeasuredWidth(label, 13.0f) + 32.0f);
}

float EditorShell::ToolbarWidth(const std::string& label) const
{
    return std::max(52.0f, MeasuredWidth(label, 13.0f) + 28.0f);
}

std::string EditorShell::PanelLabel(EditorPanelId panel) const
{
    switch (panel)
    {
    case EditorPanelId::Hierarchy: return Text("EditorUi.Hierarchy", "Hierarchy");
    case EditorPanelId::Assets: return Text("EditorUi.Assets", "Assets");
    case EditorPanelId::Viewport: return Text("EditorUi.Viewport", "Viewport");
    case EditorPanelId::UiDesigner: return Text("EditorUi.UiDesigner", "UI Designer");
    case EditorPanelId::Inspector: return Text("EditorUi.Inspector", "Inspector");
    case EditorPanelId::Console: return Text("EditorUi.Console", "Console");
    case EditorPanelId::Material: return Text("EditorUi.MaterialEditor", "Material");
    case EditorPanelId::ProjectSettings: return Text("EditorUi.ProjectSettings", "Project Settings");
    case EditorPanelId::BuildExport: return Text("EditorUi.BuildExport", "Build / Export");
    case EditorPanelId::Preferences: return Text("EditorUi.Preferences", "Preferences");
    case EditorPanelId::About: return Text("EditorUi.About", "About");
    case EditorPanelId::Diagnostics: return Text("EditorUi.Diagnostics", "Diagnostics");
    default: return "Panel";
    }
}

EditorShell::EditorShell(UiTextService* textService)
    : stub_(),
      text_(textService ? textService : &stub_),
      layout_(document_, text_),
      style_(document_),
      dispatcher_(document_, layout_, &style_),
      router_(commands_),
      chrome_(router_, commands_),
      dock_(),
      root_(document_.CreateElement(UiElementKind::Panel))
{
    document_.SetParent(root_,document_.Root());
    dispatcher_.SetInputRoot(root_);
    layout_.SetStyleEngine(&style_);
    chrome_.SetStatus("Ready");
}

EditorShell::~EditorShell() = default;

EditorPanelId EditorShell::ActivePanel() const noexcept
{
    return activePanel_;
}

UiElementHandle EditorShell::Root() const noexcept
{
    return root_;
}

UiElementHandle EditorShell::MenuBar() const noexcept
{
    return menuBar_;
}

UiElementHandle EditorShell::Toolbar() const noexcept
{
    return toolbar_;
}

UiElementHandle EditorShell::StatusBar() const noexcept
{
    return statusBar_;
}

UiElementHandle EditorShell::MenuDropdown() const noexcept
{
    return menuDropdown_;
}

bool EditorShell::MenuOpen() const noexcept
{
    return !openMenu_.empty();
}

UiElementHandle EditorShell::Panel(EditorPanelId panel) const noexcept
{
    return Slot(panel).panel;
}

UiElementHandle EditorShell::TabBar(EditorPanelId panel) const noexcept
{
    for (const GroupView& group : groups_)
    {
        if (std::find(group.tabs.begin(), group.tabs.end(), panel) != group.tabs.end())
        {
            return group.tabBar;
        }
    }
    return {};
}

UiElementHandle EditorShell::TabButton(EditorPanelId panel) const noexcept
{
    for (const GroupView& group : groups_)
    {
        for (std::size_t index = 0; index < group.tabs.size() && index < group.tabButtons.size(); ++index)
        {
            if (group.tabs[index] == panel)
            {
                return group.tabButtons[index];
            }
        }
    }
    return {};
}

const std::vector<std::string>& EditorShell::ConsoleLines() const noexcept
{
    return lines_;
}

UiElementHandle EditorShell::ConsolePanel() const noexcept
{
    return Slot(EditorPanelId::Console).panel;
}

UiElementHandle EditorShell::HierarchyContent() const noexcept
{
    return Slot(EditorPanelId::Hierarchy).content;
}

UiElementHandle EditorShell::AssetsContent() const noexcept
{
    return Slot(EditorPanelId::Assets).content;
}

UiElementHandle EditorShell::ViewportPanel() const noexcept
{
    return Slot(EditorPanelId::Viewport).panel;
}

UiElementHandle EditorShell::InspectorContent() const noexcept
{
    return Slot(EditorPanelId::Inspector).content;
}

UiElementHandle EditorShell::InspectorEdit() const noexcept
{
    return inspectorEditElement_;
}

float EditorShell::DpiScale() const noexcept
{
    return dpiScale_;
}

float EditorShell::Width() const noexcept
{
    return width_;
}

float EditorShell::Height() const noexcept
{
    return height_;
}

const std::filesystem::path& EditorShell::LayoutPath() const noexcept
{
    return layoutPath_;
}

EditorShell::PanelSlot& EditorShell::Slot(EditorPanelId panel)
{
    const std::size_t index = static_cast<std::size_t>(panel);
    return slots_[std::min(index, kPanelCount - 1)];
}

const EditorShell::PanelSlot& EditorShell::Slot(EditorPanelId panel) const
{
    const std::size_t index = static_cast<std::size_t>(panel);
    return slots_[std::min(index, kPanelCount - 1)];
}

bool EditorShell::Initialize(float width, float height)
{
    if (initialized_)
    {
        return true;
    }
    width_ = std::max(320.0f, width);
    height_ = std::max(240.0f, height);
    if (!root_.IsValid())
    {
        return false;
    }
    UiStyle rootStyle;
    rootStyle.width = UiLength::Percent(100.0f);
    rootStyle.height = UiLength::Percent(100.0f);
    rootStyle.background = kBackground;
    rootStyle.flexDirection = UiFlexDirection::Column;
    document_.SetStyle(root_, rootStyle);
    BuildPanels();
    BuildChromeMenus();
    BuildChrome();
    chrome_.SetStatus(Text("EditorUi.Ready", "Ready"));
    router_.RegisterShortcut('S', true, false, false, EditorCommandId::Save);
    router_.RegisterShortcut('S', true, true, true, EditorCommandId::SaveAll);
    router_.RegisterShortcut('Z', true, false, false, EditorCommandId::Undo);
    router_.RegisterShortcut('Z', true, true, false, EditorCommandId::Redo);
    router_.RegisterShortcut('Y', true, false, false, EditorCommandId::Redo);
    router_.RegisterShortcut(VK_F5, false, false, false, EditorCommandId::Play);
    router_.RegisterShortcut(VK_F6, false, false, false, EditorCommandId::Pause);
    router_.RegisterShortcut(VK_F7, false, false, false, EditorCommandId::Step);
    router_.RegisterShortcut(VK_F8, false, false, false, EditorCommandId::Stop);
    router_.RegisterShortcut(VK_DELETE, false, false, false, EditorCommandId::Delete);
    router_.RegisterShortcut('D', true, false, false, EditorCommandId::Duplicate);
    router_.RegisterShortcut(VK_F2, false, false, false, EditorCommandId::Rename);
    router_.RegisterShortcut('I', true, false, false, EditorCommandId::Import);
    router_.RegisterShortcut('E', true, false, false, EditorCommandId::Export);
    router_.RegisterShortcut('B', true, false, false, EditorCommandId::Build);
    router_.RegisterShortcut('O', true, false, false, EditorCommandId::Open);
    router_.RegisterShortcut('N', true, false, false, EditorCommandId::NewScene);
    router_.RegisterShortcut('S', true, true, false, EditorCommandId::SaveAs);
    router_.RegisterShortcut('B', true, true, false, EditorCommandId::TogglePanel, "build-export");
    router_.RegisterShortcut(VK_OEM_COMMA, true, false, false, EditorCommandId::OpenSettings);
    router_.RegisterShortcut('P', true, true, false, EditorCommandId::TogglePanel, "preferences");
    router_.RegisterShortcut(VK_F11, false, false, false, EditorCommandId::FocusViewport);
    router_.RegisterShortcut(VK_F1, false, false, false, EditorCommandId::TogglePanel, "about");
    initialized_ = true;
    if (!layoutPath_.empty())
    {
        LoadLayout();
    }
    Frame();
    return true;
}

void EditorShell::BuildPanels()
{
    for (std::size_t index = 1; index < kPanelCount; ++index)
    {
        PanelSlot& slot = slots_[index];
        slot.id = static_cast<EditorPanelId>(index);
        slot.panel = document_.CreateElement(UiElementKind::Panel);
        if (!slot.panel.IsValid() || !document_.SetParent(slot.panel, root_))
        {
            continue;
        }
        document_.SetStyle(slot.panel, PanelStyle({0.0f, 0.0f, 100.0f, 100.0f}));
        document_.SetVisible(slot.panel, false);
        if (slot.id == EditorPanelId::Viewport)
        {
            continue;
        }
        slot.content = document_.CreateElement(UiElementKind::Panel);
        if (!document_.SetParent(slot.content, slot.panel))
        {
            continue;
        }
        UiStyle contentStyle = AbsoluteStyle(0.0f, 0.0f, 100.0f, 100.0f);
        contentStyle.width = UiLength::Percent(100.0f);
        contentStyle.height = UiLength::Percent(100.0f);
        contentStyle.overflow = UiOverflow::Hidden;
        contentStyle.background = kContent;
        document_.SetStyle(slot.content, contentStyle);
        BuildPanelContent(slot);
    }
}

void EditorShell::BuildPanelContent(PanelSlot& slot)
{
    const auto makeList = [this, &slot](std::unique_ptr<UiListView>& list) {
        list = std::make_unique<UiListView>(document_, slot.content);
        list->SetRowTextColor(kText);
        list->SetRowHeight(20.0f);
    };

    switch (slot.id)
    {
    case EditorPanelId::Hierarchy:
        makeList(hierarchyList_);
        if (hierarchyList_)
        {
            hierarchyList_->SetRowBackgroundProvider([this](int index){
                const auto rows=hierarchyModel_.VisibleRows();
                return index>=0&&static_cast<std::size_t>(index)<rows.size()&&
                    hierarchyModel_.Nodes()[rows[index]].id==inspectorModel_.Target()
                    ?kSurfaceActive:UiColor{0,0,0,0};
            });
            hierarchyList_->SetRowTextProvider([this](int index) {
                const auto rows = hierarchyModel_.VisibleRows();
                if (index < 0 || static_cast<std::size_t>(index) >= rows.size())
                {
                    return std::string{};
                }
                const EditorHierarchyNode& node =
                    hierarchyModel_.Nodes()[rows[static_cast<std::size_t>(index)]];
                return RowText(std::string(node.depth * 2, ' ') + node.label);
            });
        }
        dispatcher_.AddHandler(slot.content, UiEventType::PointerDown, [this](UiInputEvent& event) {
            const UiRect rect = layout_.Rect(Slot(EditorPanelId::Hierarchy).content);
            const float offset = hierarchyList_ ? hierarchyList_->Offset() : 0.0f;
            const int row = static_cast<int>((event.y - rect.y + offset) / 20.0f);
            const auto rows = hierarchyModel_.VisibleRows();
            if (row >= 0 && static_cast<std::size_t>(row) < rows.size())
            {
                const std::size_t nodeIndex = rows[static_cast<std::size_t>(row)];
                dragHierarchyId_ = hierarchyModel_.Nodes()[nodeIndex].id;
                hierarchyDragStartX_ = event.x;
                hierarchyDragStartY_ = event.y;
                draggingHierarchy_ = false;
                if (event.x - rect.x <= 18.0f && hierarchyModel_.Nodes()[nodeIndex].hasChildren)
                {
                    hierarchyModel_.Toggle(nodeIndex);
                    if (hierarchyList_)
                    {
                        hierarchyList_->Invalidate();
                        hierarchyList_->SetItemCount(static_cast<int>(hierarchyModel_.VisibleRows().size()));
                    }
                    Frame();
                    event.handled = true;
                    return;
                }
                const std::uint64_t id = hierarchyModel_.Nodes()[nodeIndex].id;
                hierarchyModel_.Select(id);
                if (selectionHandler_)
                {
                    selectionHandler_(id);
                }
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerMove, [this](UiInputEvent& event) {
            if (dragHierarchyId_ == 0)
            {
                return;
            }
            if (std::abs(event.x - hierarchyDragStartX_) + std::abs(event.y - hierarchyDragStartY_) > 4.0f)
            {
                draggingHierarchy_ = true;
                if (dragGhost_.IsValid())
                {
                    document_.SetVisible(dragGhost_, true);
                    UiStyle ghostStyle = document_.Style(dragGhost_);
                    ghostStyle.left = UiLength::Px(event.x + 8.0f);
                    ghostStyle.top = UiLength::Px(event.y);
                    document_.SetStyle(dragGhost_, ghostStyle);
                    for (const EditorHierarchyNode& node : hierarchyModel_.Nodes())
                    {
                        if (node.id == dragHierarchyId_)
                        {
                            document_.SetText(dragGhost_, node.label);
                        }
                    }
                }
            }
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerWheel, [this](UiInputEvent& event) {
            if (hierarchyList_)
            {
                hierarchyList_->SetScrollOffset(hierarchyList_->Offset() + event.wheel * 48.0f);
                hierarchyList_->Sync();
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerUp, [this](UiInputEvent& event) {
            if (dragHierarchyId_ == 0 || !draggingHierarchy_)
            {
                dragHierarchyId_ = 0;
                draggingHierarchy_ = false;
                return;
            }
            const UiRect rect = layout_.Rect(Slot(EditorPanelId::Hierarchy).content);
            const float offset = hierarchyList_ ? hierarchyList_->Offset() : 0.0f;
            const int row = static_cast<int>((event.y - rect.y + offset) / 20.0f);
            const auto rows = hierarchyModel_.VisibleRows();
            if (row >= 0 && static_cast<std::size_t>(row) < rows.size())
            {
                const std::uint64_t parent = hierarchyModel_.Nodes()[rows[static_cast<std::size_t>(row)]].id;
                if (parent != dragHierarchyId_ && reparentHandler_)
                {
                    reparentHandler_(dragHierarchyId_, parent);
                }
            }
            if (dragGhost_.IsValid())
            {
                document_.SetVisible(dragGhost_, false);
            }
            dragHierarchyId_ = 0;
            draggingHierarchy_ = false;
        }, UiEventPhase::Bubble);
        break;
    case EditorPanelId::Assets: {
        makeList(assetsList_);
        if (assetsList_)
        {
            assetsList_->SetRowTextProvider([this](int index) {
                const auto& entries = assetsModel_.Filtered();
                if (index < 0 || static_cast<std::size_t>(index) >= entries.size())
                {
                    return std::string{};
                }
                const EditorAssetEntry& entry = entries[static_cast<std::size_t>(index)];
                return RowText("[" + entry.type + "] " + entry.path);
            });
        }
        assetsFilterElement_ = document_.CreateElement(UiElementKind::TextEdit);
        if (document_.SetParent(assetsFilterElement_, slot.panel))
        {
            document_.SetStyle(assetsFilterElement_, ButtonStyle(0.0f, 0.0f, 100.0f, 24.0f, kContent));
            assetsFilter_ = std::make_unique<UiTextEdit>(document_, assetsFilterElement_, text_);
            dispatcher_.AddHandler(assetsFilterElement_, UiEventType::PointerDown, [this](UiInputEvent&) {
                dispatcher_.SetFocus(assetsFilterElement_);
            });
            const auto refreshFilter = [this]() {
                if (!assetsFilter_)
                {
                    return;
                }
                assetsModel_.SetFilter(assetsFilter_->Text());
                if (assetsList_)
                {
                    assetsList_->Invalidate();
                    assetsList_->SetItemCount(static_cast<int>(assetsModel_.Filtered().size()));
                }
            };
            dispatcher_.AddHandler(assetsFilterElement_, UiEventType::KeyDown, [this, refreshFilter](UiInputEvent& event) {
                if (event.key == UiKeyEnter || event.key == UiKeyEscape)
                {
                    dispatcher_.ClearFocus();
                    event.handled = true;
                    return;
                }
                if (assetsFilter_)
                {
                    assetsFilter_->HandleKey(event.key, event.shift, event.control);
                }
                refreshFilter();
                event.handled = true;
            });
            dispatcher_.AddHandler(assetsFilterElement_, UiEventType::TextInput,
                                   [this, refreshFilter](UiInputEvent& event) {
                if (!assetsFilter_ || event.codepoint == 0)
                {
                    return;
                }
                char32_t codepoint = event.codepoint;
                std::string utf8;
                if (codepoint < 0x80) utf8.push_back(static_cast<char>(codepoint));
                else if (codepoint < 0x800)
                {
                    utf8.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                    utf8.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                }
                else
                {
                    utf8.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                    utf8.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                    utf8.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                }
                assetsFilter_->InsertText(utf8);
                refreshFilter();
                event.handled = true;
            });
        }
        assetsImportElement_ = document_.CreateElement(UiElementKind::Button);
        if (document_.SetParent(assetsImportElement_, slot.panel))
        {
            document_.SetStyle(assetsImportElement_, ButtonStyle(0.0f, 0.0f, 64.0f, 24.0f, kSurfaceActive));
            document_.SetText(assetsImportElement_, Text("EditorUi.Import", "Import"));
            dispatcher_.AddHandler(assetsImportElement_, UiEventType::PointerDown, [this](UiInputEvent& event) {
                if (assetImportHandler_)
                {
                    assetImportHandler_();
                }
                event.handled = true;
            });
        }
        assetsPreview_ = document_.CreateElement(UiElementKind::Label);
        if (document_.SetParent(assetsPreview_, slot.panel))
        {
            document_.SetStyle(assetsPreview_, ButtonStyle(0.0f, 0.0f, 100.0f, 24.0f, kSurfaceRaised));
            document_.SetText(assetsPreview_, Text("EditorUi.Preview", "Preview") + ": " + Text("EditorUi.None", "none"));
        }
        assetsPreviewImageElement_ = document_.CreateElement(UiElementKind::Image);
        if (document_.SetParent(assetsPreviewImageElement_, slot.panel))
        {
            UiStyle style;
            style.position = UiPosition::Absolute;
            style.left = UiLength::Px(4.0f);
            style.top = UiLength::Px(4.0f);
            style.width = UiLength::Px(120.0f);
            style.height = UiLength::Px(64.0f);
            document_.SetStyle(assetsPreviewImageElement_, style);
            document_.SetVisible(assetsPreviewImageElement_, false);
        }
        dispatcher_.AddHandler(slot.content, UiEventType::PointerWheel, [this](UiInputEvent& event) {
            if (assetsList_)
            {
                assetsList_->SetScrollOffset(assetsList_->Offset() + event.wheel * 48.0f);
                assetsList_->Sync();
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerDown, [this](UiInputEvent& event) {
            const UiRect rect = layout_.Rect(Slot(EditorPanelId::Assets).content);
            const float offset = assetsList_ ? assetsList_->Offset() : 0.0f;
            const int row = static_cast<int>((event.y - rect.y + offset) / 20.0f);
            const auto& entries = assetsModel_.Filtered();
            if (row >= 0 && static_cast<std::size_t>(row) < entries.size())
            {
                assetsModel_.Select(entries[static_cast<std::size_t>(row)].path);
                if (assetsPreview_.IsValid())
                {
                    document_.SetText(assetsPreview_, assetsPreviewText_.empty()
                        ? Text("EditorUi.Preview", "Preview") + ": " + assetsModel_.SelectedPath()
                        : assetsPreviewText_);
                }
                if (assetSelectionHandler_)
                {
                    assetSelectionHandler_(assetsModel_.SelectedPath());
                }
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        break;
    }
    case EditorPanelId::Inspector: {
        makeList(inspectorList_);
        if (inspectorList_)
        {
            inspectorList_->SetRowTextProvider([this](int index) {
                const auto& properties = inspectorModel_.Properties();
                if (index < 0 || static_cast<std::size_t>(index) >= properties.size())
                {
                    return std::string{};
                }
                const EditorProperty& property = properties[static_cast<std::size_t>(index)];
                const std::string suffix = property.type == EditorPropertyType::Combo ? " [v]" : "";
                return RowText(property.label + ": " + property.value + suffix, 280.0f);
            });
        }
        dispatcher_.AddHandler(slot.content, UiEventType::PointerWheel, [this](UiInputEvent& event) {
            if (inspectorList_)
            {
                inspectorList_->SetScrollOffset(inspectorList_->Offset() + event.wheel * 48.0f);
                inspectorList_->Sync();
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerDown, [this](UiInputEvent& event) {
            const UiRect rect = layout_.Rect(Slot(EditorPanelId::Inspector).content);
            const float offset = inspectorList_ ? inspectorList_->Offset() : 0.0f;
            const int row = static_cast<int>((event.y - rect.y + offset) / 20.0f);
            const auto& properties = inspectorModel_.Properties();
            if (row >= 0 && static_cast<std::size_t>(row) < properties.size() &&
                !properties[static_cast<std::size_t>(row)].readOnly)
            {
                editingKey_ = properties[static_cast<std::size_t>(row)].key;
                if (inspectorEdit_)
                {
                    inspectorEdit_->SetText(properties[static_cast<std::size_t>(row)].value);
                    dispatcher_.SetFocus(inspectorEditElement_);
                }
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        inspectorEditElement_ = document_.CreateElement(UiElementKind::TextEdit);
        if (document_.SetParent(inspectorEditElement_, slot.panel))
        {
            document_.SetStyle(inspectorEditElement_, ButtonStyle(0.0f, 0.0f, 100.0f, 24.0f, kContent));
            inspectorEdit_ = std::make_unique<UiTextEdit>(document_, inspectorEditElement_, text_);
            dispatcher_.AddHandler(inspectorEditElement_, UiEventType::PointerDown, [this](UiInputEvent&) {
                dispatcher_.SetFocus(inspectorEditElement_);
            });
            dispatcher_.AddHandler(inspectorEditElement_, UiEventType::KeyDown, [this](UiInputEvent& event) {
                if (event.key == UiKeyEnter)
                {
                    if (propertyHandler_ && !editingKey_.empty() && inspectorEdit_)
                    {
                        propertyHandler_(inspectorModel_.Target(), editingKey_, std::string(inspectorEdit_->Text()));
                    }
                    dispatcher_.ClearFocus();
                    event.handled = true;
                    return;
                }
                if (event.key == UiKeyEscape)
                {
                    dispatcher_.ClearFocus();
                    event.handled = true;
                    return;
                }
                if (inspectorEdit_)
                {
                    inspectorEdit_->HandleKey(event.key, event.shift, event.control);
                }
                event.handled = true;
            });
            dispatcher_.AddHandler(inspectorEditElement_, UiEventType::TextInput, [this](UiInputEvent& event) {
                if (!inspectorEdit_ || event.codepoint == 0)
                {
                    return;
                }
                char32_t codepoint = event.codepoint;
                std::string utf8;
                if (codepoint < 0x80) utf8.push_back(static_cast<char>(codepoint));
                else if (codepoint < 0x800)
                {
                    utf8.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                    utf8.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                }
                else
                {
                    utf8.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                    utf8.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                    utf8.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                }
                inspectorEdit_->InsertText(utf8);
                event.handled = true;
            });
            dispatcher_.AddHandler(inspectorEditElement_, UiEventType::ImeComposition, [this](UiInputEvent& event) {
                if (inspectorEdit_) inspectorEdit_->SetComposition(event.text);
            });
            dispatcher_.AddHandler(inspectorEditElement_, UiEventType::ImeCommit, [this](UiInputEvent& event) {
                if (inspectorEdit_)
                {
                    inspectorEdit_->SetComposition(event.text);
                    inspectorEdit_->CommitComposition();
                }
            });
        }
        break;
    }
    case EditorPanelId::Console: {
        makeList(console_);
        if (console_)
        {
            console_->SetRowTextProvider([this](int index) {
                return index >= 0 && static_cast<std::size_t>(index) < lines_.size()
                    ? RowText(lines_[static_cast<std::size_t>(index)], 280.0f) : std::string{};
            });
            console_->SetItemCount(0);
        }
        dispatcher_.AddHandler(slot.content, UiEventType::PointerWheel, [this](UiInputEvent& event) {
            if (console_)
            {
                console_->SetScrollOffset(console_->Offset() + event.wheel * 48.0f);
                console_->Sync();
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        break;
    }
    case EditorPanelId::UiDesigner:
        BuildDesignerPanel(slot);
        break;
    case EditorPanelId::Material:
    case EditorPanelId::ProjectSettings:
    case EditorPanelId::BuildExport:
    case EditorPanelId::Preferences:
        BuildFormPanel(slot);
        break;
    case EditorPanelId::About:
        document_.SetText(slot.content, Text("EditorUi.AboutPlaceholder", "Flare Engine Editor"));
        break;
    case EditorPanelId::Diagnostics:
        document_.SetText(slot.content, Text("EditorUi.DiagnosticsPlaceholder", "Diagnostics pending"));
        break;
    default:
        break;
    }
    BuildScrollThumb(slot.id);
}

void EditorShell::BuildScrollThumb(EditorPanelId panel)
{
    const bool hasList = panel == EditorPanelId::Hierarchy || panel == EditorPanelId::Assets ||
                         panel == EditorPanelId::Inspector || panel == EditorPanelId::Console ||
                         panel == EditorPanelId::Material || panel == EditorPanelId::ProjectSettings ||
                         panel == EditorPanelId::BuildExport || panel == EditorPanelId::Preferences;
    if (!hasList)
    {
        return;
    }
    UiElementHandle thumb = document_.CreateElement(UiElementKind::Panel);
    if (!document_.SetParent(thumb, Slot(panel).content))
    {
        return;
    }
    UiStyle style;
    style.position = UiPosition::Absolute;
    style.left = UiLength::Px(0.0f);
    style.top = UiLength::Px(0.0f);
    style.width = UiLength::Px(4.0f);
    style.height = UiLength::Px(20.0f);
    style.background = UiInk(flare::Muted, 0.85f);
    style.borderColor = kBorder;
    style.borderWidth = 1.0f;
    document_.SetStyle(thumb, style);
    document_.SetVisible(thumb, false);
    scrollThumbs_[static_cast<std::size_t>(panel)] = thumb;

    dispatcher_.AddHandler(thumb, UiEventType::PointerDown, [this, panel](UiInputEvent& event) {
        UiListView* list = ListFor(panel);
        if (!list)
        {
            return;
        }
        scrollDragging_ = true;
        scrollDragPanel_ = panel;
        scrollDragStartY_ = event.y;
        scrollDragStartOffset_ = list->Offset();
        dispatcher_.SetPointerCapture(scrollThumbs_[static_cast<std::size_t>(panel)]);
        event.handled = true;
    });
    dispatcher_.AddHandler(thumb, UiEventType::PointerMove, [this](UiInputEvent& event) {
        if (!scrollDragging_)
        {
            return;
        }
        UiListView* list = ListFor(scrollDragPanel_);
        if (!list)
        {
            return;
        }
        const float viewport = list->ViewportHeight();
        const float content = static_cast<float>(list->ItemCount()) * list->RowHeight();
        const float thumbHeight = std::max(20.0f, viewport * viewport / std::max(content, 1.0f));
        const float track = std::max(1.0f, viewport - thumbHeight);
        const float maximumOffset = std::max(0.0f, content - viewport);
        const float offset = scrollDragStartOffset_ + (event.y - scrollDragStartY_) / track * maximumOffset;
        list->SetScrollOffset(offset);
        list->Sync();
        event.handled = true;
    });
    dispatcher_.AddHandler(thumb, UiEventType::PointerUp, [this](UiInputEvent& event) {
        scrollDragging_ = false;
        scrollDragPanel_ = EditorPanelId::None;
        dispatcher_.ReleasePointerCapture();
        event.handled = true;
    });
}

UiListView* EditorShell::ListFor(EditorPanelId panel) const
{
    switch (panel)
    {
    case EditorPanelId::Hierarchy: return hierarchyList_.get();
    case EditorPanelId::Assets: return assetsList_.get();
    case EditorPanelId::Inspector: return inspectorList_.get();
    case EditorPanelId::Console: return console_.get();
    case EditorPanelId::Material: return forms_[0].list.get();
    case EditorPanelId::ProjectSettings: return forms_[1].list.get();
    case EditorPanelId::BuildExport: return forms_[2].list.get();
    case EditorPanelId::Preferences: return forms_[3].list.get();
    default: return nullptr;
    }
}

void EditorShell::UpdateScrollThumbs()
{
    for (const EditorPanelId panel : {EditorPanelId::Hierarchy, EditorPanelId::Assets, EditorPanelId::Inspector,
                                      EditorPanelId::Console, EditorPanelId::Material,
                                      EditorPanelId::ProjectSettings, EditorPanelId::BuildExport,
                                      EditorPanelId::Preferences})
    {
        UiElementHandle thumb = scrollThumbs_[static_cast<std::size_t>(panel)];
        UiListView* list = ListFor(panel);
        if (!thumb.IsValid() || !list)
        {
            continue;
        }
        const float viewport = list->ViewportHeight();
        const float content = static_cast<float>(list->ItemCount()) * list->RowHeight();
        if (viewport <= 1.0f || content <= viewport + 1.0f)
        {
            if (document_.Visible(thumb))
            {
                document_.SetVisible(thumb, false);
            }
            continue;
        }
        const float maximumOffset = content - viewport;
        const float thumbHeight = std::max(20.0f, viewport * viewport / content);
        const float progress = std::clamp(list->Offset() / maximumOffset, 0.0f, 1.0f);
        const UiRect contentRect = layout_.Rect(Slot(panel).content);
        const float left = std::max(0.0f, contentRect.width - 6.0f);
        const float top = progress * std::max(0.0f, viewport - thumbHeight);
        UiStyle style = document_.Style(thumb);
        // 只在几何变化时 SetStyle，避免空闲帧把 layout 标脏（UiPerf 要求 stable layout=0）。
        if (style.left.value != left || style.top.value != top || style.height.value != thumbHeight ||
            style.left.unit != UiUnit::Px || style.top.unit != UiUnit::Px || style.height.unit != UiUnit::Px)
        {
            style.left = UiLength::Px(left);
            style.top = UiLength::Px(top);
            style.height = UiLength::Px(thumbHeight);
            document_.SetStyle(thumb, style);
        }
        if (!document_.Visible(thumb))
        {
            document_.SetVisible(thumb, true);
        }
    }
}

EditorShell::FormPanel& EditorShell::Form(EditorPanelId panel)
{
    switch (panel)
    {
    case EditorPanelId::Material: return forms_[0];
    case EditorPanelId::ProjectSettings: return forms_[1];
    case EditorPanelId::BuildExport: return forms_[2];
    case EditorPanelId::Preferences: return forms_[3];
    default: return forms_[0];
    }
}

const EditorShell::FormPanel& EditorShell::Form(EditorPanelId panel) const
{
    switch (panel)
    {
    case EditorPanelId::Material: return forms_[0];
    case EditorPanelId::ProjectSettings: return forms_[1];
    case EditorPanelId::BuildExport: return forms_[2];
    case EditorPanelId::Preferences: return forms_[3];
    default: return forms_[0];
    }
}

void EditorShell::BuildFormPanel(PanelSlot& slot)
{
    FormPanel& form = Form(slot.id);
    form.listContent = document_.CreateElement(UiElementKind::Panel);
    if (!document_.SetParent(form.listContent, slot.content))
    {
        return;
    }
    UiStyle listStyle = AbsoluteStyle(0.0f, 32.0f, 100.0f, 100.0f);
    listStyle.width = UiLength::Percent(100.0f);
    listStyle.background = kContent;
    listStyle.overflow = UiOverflow::Hidden;
    document_.SetStyle(form.listContent, listStyle);
    form.list = std::make_unique<UiListView>(document_, form.listContent);
    form.list->SetRowTextColor(kText);
    form.list->SetRowHeight(20.0f);
    form.list->SetRowTextProvider([&form](int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= form.properties.size())
        {
            return std::string{};
        }
        const EditorProperty& property = form.properties[static_cast<std::size_t>(index)];
        const std::string suffix = property.type == EditorPropertyType::Combo ? " [v]" : "";
        return RowText(property.label + ": " + property.value + suffix, 280.0f);
    });
    dispatcher_.AddHandler(form.listContent, UiEventType::PointerWheel, [this, &form](UiInputEvent& event) {
        if (form.list)
        {
            form.list->SetScrollOffset(form.list->Offset() + event.wheel * 48.0f);
            form.list->Sync();
        }
        event.handled = true;
    }, UiEventPhase::Bubble);
    const EditorPanelId panelId = slot.id;
    dispatcher_.AddHandler(form.listContent, UiEventType::PointerDown, [this, &form, panelId](UiInputEvent& event) {
        const UiRect rect = layout_.Rect(form.listContent);
        const float offset = form.list ? form.list->Offset() : 0.0f;
        const int row = static_cast<int>((event.y - rect.y + offset) / 20.0f);
        if (row < 0 || static_cast<std::size_t>(row) >= form.properties.size() ||
            form.properties[static_cast<std::size_t>(row)].readOnly)
        {
            event.handled = true;
            return;
        }
        EditorProperty& property = form.properties[static_cast<std::size_t>(row)];
        if (property.type == EditorPropertyType::Combo && !property.options.empty())
        {
            ShowFormCombo(panelId, form, row);
            event.handled = true;
            return;
        }
        if (property.type == EditorPropertyType::Boolean)
        {
            const std::string next = property.value == "true" ? "false" : "true";
            if (form.commit && form.commit(property.key, next))
            {
                property.value = next;
                form.status = Text("EditorUi.Applied", "Applied");
            }
            else
            {
                form.status = Text("EditorUi.InvalidValue", "Invalid value");
            }
            RefreshForm(form);
            event.handled = true;
            return;
        }
        form.editingKey = form.properties[static_cast<std::size_t>(row)].key;
        if (form.edit && form.editElement.IsValid())
        {
            form.edit->SetText(form.properties[static_cast<std::size_t>(row)].value);
            document_.SetVisible(form.editElement, true);
            dispatcher_.SetFocus(form.editElement);
        }
        event.handled = true;
    }, UiEventPhase::Bubble);

    form.saveButton = document_.CreateElement(UiElementKind::Button);
    if (document_.SetParent(form.saveButton, slot.content))
    {
        document_.SetStyle(form.saveButton, ButtonStyle(0.0f, 0.0f, 80.0f, 24.0f, kSurfaceActive));
        document_.SetText(form.saveButton, Text("EditorUi.Save", "Save"));
        dispatcher_.AddHandler(form.saveButton, UiEventType::PointerDown, [this, &form](UiInputEvent& event) {
            if (form.save && form.save())
            {
                form.status = Text("EditorUi.Saved", "Saved");
            }
            else
            {
                form.status = form.status.empty() ? Text("EditorUi.SaveFailed", "Save failed") : form.status;
            }
            RefreshForm(form);
            event.handled = true;
        });
    }
    form.actionButton = document_.CreateElement(UiElementKind::Button);
    if (document_.SetParent(form.actionButton, slot.content))
    {
        document_.SetStyle(form.actionButton, ButtonStyle(84.0f, 0.0f, 80.0f, 24.0f, kSurfaceRaised));
        document_.SetText(form.actionButton, "Action");
        document_.SetVisible(form.actionButton, false);
        dispatcher_.AddHandler(form.actionButton, UiEventType::PointerDown, [this, &form](UiInputEvent& event) {
            if (form.action && form.action())
            {
                form.status = Text("EditorUi.Running", "Running");
            }
            RefreshForm(form);
            event.handled = true;
        });
    }
    form.statusLabel = document_.CreateElement(UiElementKind::Label);
    if (document_.SetParent(form.statusLabel, slot.content))
    {
        UiStyle statusStyle = ButtonStyle(168.0f, 0.0f, 100.0f, 24.0f, kContent);
        statusStyle.color = kMuted;
        statusStyle.fontSize = 12.0f;
        document_.SetStyle(form.statusLabel, statusStyle);
    }
    form.comboPopup = document_.CreateElement(UiElementKind::Panel);
    if (document_.SetParent(form.comboPopup, slot.panel))
    {
        UiStyle popup;
        popup.position = UiPosition::Absolute;
        popup.left = UiLength::Px(0.0f);
        popup.top = UiLength::Px(0.0f);
        popup.width = UiLength::Px(180.0f);
        popup.height = UiLength::Px(22.0f);
        popup.background = kSurfaceRaised;
        popup.borderWidth = 1.0f;
        popup.borderColor = kAccent;
        document_.SetStyle(form.comboPopup, popup);
        document_.SetVisible(form.comboPopup, false);
        dispatcher_.AddHandler(form.comboPopup, UiEventType::PointerDown, [](UiInputEvent& event) {
            event.handled = true;
            event.propagationStopped = true;
        });
    }
    form.editElement = document_.CreateElement(UiElementKind::TextEdit);
    if (document_.SetParent(form.editElement, slot.content))
    {
        document_.SetStyle(form.editElement, ButtonStyle(0.0f, 0.0f, 100.0f, 24.0f, kContent));
        document_.SetVisible(form.editElement, false);
        form.edit = std::make_unique<UiTextEdit>(document_, form.editElement, text_);
        dispatcher_.AddHandler(form.editElement, UiEventType::PointerDown, [this, &form](UiInputEvent&) {
            dispatcher_.SetFocus(form.editElement);
        });
        dispatcher_.AddHandler(form.editElement, UiEventType::KeyDown, [this, &form](UiInputEvent& event) {
            if (event.key == UiKeyEnter)
            {
                bool committed = false;
                if (form.commit && form.edit && !form.editingKey.empty())
                {
                    committed = form.commit(form.editingKey, std::string(form.edit->Text()));
                }
                if (committed)
                {
                    for (EditorProperty& property : form.properties)
                    {
                        if (property.key == form.editingKey)
                        {
                            property.value = form.edit ? std::string(form.edit->Text()) : std::string{};
                        }
                    }
                    form.status = Text("EditorUi.Applied", "Applied");
                }
                else
                {
                    form.status = Text("EditorUi.InvalidValue", "Invalid value");
                }
                form.editingKey.clear();
                document_.SetVisible(form.editElement, false);
                dispatcher_.ClearFocus();
                RefreshForm(form);
                event.handled = true;
                return;
            }
            if (event.key == UiKeyEscape)
            {
                form.editingKey.clear();
                document_.SetVisible(form.editElement, false);
                dispatcher_.ClearFocus();
                event.handled = true;
                return;
            }
            if (form.edit)
            {
                form.edit->HandleKey(event.key, event.shift, event.control);
            }
            event.handled = true;
        });
        dispatcher_.AddHandler(form.editElement, UiEventType::TextInput, [this, &form](UiInputEvent& event) {
            if (!form.edit || event.codepoint == 0)
            {
                return;
            }
            char32_t codepoint = event.codepoint;
            std::string utf8;
            if (codepoint < 0x80) utf8.push_back(static_cast<char>(codepoint));
            else if (codepoint < 0x800)
            {
                utf8.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                utf8.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
            else
            {
                utf8.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                utf8.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                utf8.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
            form.edit->InsertText(utf8);
            event.handled = true;
        });
        dispatcher_.AddHandler(form.editElement, UiEventType::ImeComposition, [this, &form](UiInputEvent& event) {
            if (form.edit) form.edit->SetComposition(event.text);
        });
        dispatcher_.AddHandler(form.editElement, UiEventType::ImeCommit, [this, &form](UiInputEvent& event) {
            if (form.edit)
            {
                form.edit->SetComposition(event.text);
                form.edit->CommitComposition();
            }
        });
    }
    RefreshForm(form);
}

UiElementHandle EditorShell::FormComboOption(EditorPanelId panel, std::size_t index) const noexcept
{
    const FormPanel& form = Form(panel);
    return index < form.comboOptions.size() ? form.comboOptions[index] : UiElementHandle{};
}

void EditorShell::HideFormCombo(FormPanel& form)
{
    if (form.comboPopup.IsValid())
    {
        document_.SetVisible(form.comboPopup, false);
        for (const UiElementHandle option : form.comboOptions)
        {
            if (option.IsValid())
            {
                document_.SetVisible(option, false);
                pendingDestroy_.push_back(option);
            }
        }
    }
    form.comboOptions.clear();
    form.comboRow = -1;
}

void EditorShell::ShowFormCombo(EditorPanelId panel, FormPanel& form, int row)
{
    if (!form.comboPopup.IsValid() || row < 0 || static_cast<std::size_t>(row) >= form.properties.size())
    {
        return;
    }
    HideFormCombo(form);
    const EditorProperty& property = form.properties[static_cast<std::size_t>(row)];
    if (property.type != EditorPropertyType::Combo || property.options.empty())
    {
        return;
    }
    const UiRect panelRect = layout_.Rect(Slot(panel).panel);
    const UiRect listRect = layout_.Rect(form.listContent);
    const float rowTop = listRect.y + static_cast<float>(row) * form.list->RowHeight() - form.list->Offset();
    const float popupWidth = std::max(80.0f, std::min(200.0f, panelRect.width - 8.0f));
    UiStyle popup = document_.Style(form.comboPopup);
    popup.left = UiLength::Px(4.0f);
    popup.top = UiLength::Px(std::max(0.0f, rowTop + form.list->RowHeight() - panelRect.y));
    popup.width = UiLength::Px(popupWidth);
    popup.height = UiLength::Px(static_cast<float>(property.options.size()) * 22.0f);
    document_.SetStyle(form.comboPopup, popup);
    document_.SetVisible(form.comboPopup, true);
    form.comboRow = row;
    for (std::size_t index = 0; index < property.options.size(); ++index)
    {
        const UiElementHandle option = document_.CreateElement(UiElementKind::Button);
        if (!document_.SetParent(option, form.comboPopup))
        {
            continue;
        }
        document_.SetStyle(option, ButtonStyle(0.0f, static_cast<float>(index) * 22.0f, popupWidth, 22.0f,
            property.options[index] == property.value ? kSurfaceActive : kSurfaceRaised));
        document_.SetText(option, property.options[index]);
        form.comboOptions.push_back(option);
        const std::string value = property.options[index];
        dispatcher_.AddHandler(option, UiEventType::PointerDown, [this, &form, panel, row, value](UiInputEvent& event) {
            EditorProperty& target = form.properties[static_cast<std::size_t>(row)];
            if (form.commit && form.commit(target.key, value))
            {
                target.value = value;
                form.status = Text("EditorUi.Applied", "Applied");
            }
            else
            {
                form.status = Text("EditorUi.InvalidValue", "Invalid value");
            }
            HideFormCombo(form);
            RefreshForm(form);
            event.handled = true;
        });
    }
}

void EditorShell::RefreshForm(FormPanel& form)
{
    HideFormCombo(form);
    if (form.list)
    {
        form.list->SetRowBackgroundProvider([&form](int index) {
            if (index < 0 || static_cast<std::size_t>(index) >= form.properties.size())
            {
                return UiColor{0.0f, 0.0f, 0.0f, 0.0f};
            }
            return RowBackgroundColor(form.properties[static_cast<std::size_t>(index)]);
        });
        form.list->Invalidate();
        form.list->SetItemCount(static_cast<int>(form.properties.size()));
        form.list->Sync();
    }
    if (form.statusLabel.IsValid())
    {
        document_.SetText(form.statusLabel, form.status);
    }
    if (form.actionButton.IsValid())
    {
        document_.SetVisible(form.actionButton, !form.actionLabel.empty());
        document_.SetText(form.actionButton, form.actionLabel.empty() ? "Action" : form.actionLabel);
    }
    if (form.editElement.IsValid())
    {
        document_.SetVisible(form.editElement, !form.editingKey.empty());
    }
}

void EditorShell::UpdateFormRects(EditorPanelId panel)
{
    FormPanel& form = Form(panel);
    if (!form.listContent.IsValid())
    {
        return;
    }
    float width = 0.0f;
    float height = 0.0f;
    for (const GroupView& group : groups_)
    {
        if (std::find(group.tabs.begin(), group.tabs.end(), panel) != group.tabs.end())
        {
            width = group.rect.width;
            height = std::max(0.0f, group.rect.height - kTabHeight);
            break;
        }
    }
    UiStyle listStyle = document_.Style(form.listContent);
    listStyle.left = UiLength::Px(0.0f);
    listStyle.top = UiLength::Px(32.0f);
    listStyle.width = UiLength::Percent(100.0f);
    listStyle.height = UiLength::Px(std::max(0.0f, height - 56.0f));
    document_.SetStyle(form.listContent, listStyle);
    if (form.list)
    {
        form.list->SetViewportHeight(std::max(0.0f, height - 56.0f));
        form.list->Sync();
    }
    if (form.editElement.IsValid())
    {
        UiStyle style = document_.Style(form.editElement);
        style.top = UiLength::Px(std::max(0.0f, height - 24.0f));
        style.width = UiLength::Percent(100.0f);
        document_.SetStyle(form.editElement, style);
    }
    if (form.statusLabel.IsValid())
    {
        UiStyle style = document_.Style(form.statusLabel);
        style.width = UiLength::Px(std::max(0.0f, width - 168.0f));
        document_.SetStyle(form.statusLabel, style);
    }
}

void EditorShell::SetFormProperties(EditorPanelId panel, std::vector<EditorProperty> properties)
{
    FormPanel& form = Form(panel);
    bool changed = properties.size() != form.properties.size();
    if (!changed)
    {
        for (std::size_t index = 0; index < properties.size(); ++index)
        {
            if (properties[index].key != form.properties[index].key ||
                properties[index].value != form.properties[index].value ||
                properties[index].label != form.properties[index].label)
            {
                changed = true;
                break;
            }
        }
    }
    if (!changed)
    {
        return;
    }
    form.properties = std::move(properties);
    RefreshForm(form);
}

void EditorShell::SetFormHandlers(EditorPanelId panel, std::function<bool(std::string, std::string)> commit,
                                  std::function<bool()> save)
{
    FormPanel& form = Form(panel);
    form.commit = std::move(commit);
    form.save = std::move(save);
}

void EditorShell::SetFormAction(EditorPanelId panel, std::string label, std::function<bool()> action)
{
    FormPanel& form = Form(panel);
    form.actionLabel = std::move(label);
    form.action = std::move(action);
    RefreshForm(form);
}

void EditorShell::SetFormActionLabel(EditorPanelId panel, std::string label)
{
    FormPanel& form = Form(panel);
    if (form.actionLabel == label)
    {
        return;
    }
    form.actionLabel = std::move(label);
    RefreshForm(form);
}

void EditorShell::SetFormStatus(EditorPanelId panel, std::string status)
{
    FormPanel& form = Form(panel);
    if (form.status == status)
    {
        return;
    }
    form.status = std::move(status);
    RefreshForm(form);
}

UiElementHandle EditorShell::FormRow(EditorPanelId panel, std::size_t index) const noexcept
{
    const FormPanel& form = Form(panel);
    if (!form.list || index >= form.list->Rows().size())
    {
        return {};
    }
    return form.list->Rows()[index];
}

UiElementHandle EditorShell::FormSaveButton(EditorPanelId panel) const noexcept
{
    return Form(panel).saveButton;
}

UiElementHandle EditorShell::FormActionButton(EditorPanelId panel) const noexcept
{
    return Form(panel).actionButton;
}

UiElementHandle EditorShell::FormListContent(EditorPanelId panel) const noexcept
{
    return Form(panel).listContent;
}

UiElementHandle EditorShell::PanelContent(EditorPanelId panel) const noexcept
{
    return Slot(panel).content;
}

UiElementHandle EditorShell::ScrollThumb(EditorPanelId panel) const noexcept
{
    return scrollThumbs_[static_cast<std::size_t>(panel)];
}

void EditorShell::BuildDesignerPanel(PanelSlot& slot)
{
    designerCanvas_ = document_.CreateElement(UiElementKind::Panel);
    if (!document_.SetParent(designerCanvas_, slot.content))
    {
        return;
    }
    UiStyle canvasStyle = AbsoluteStyle(0.0f, 0.0f, 100.0f, 100.0f);
    canvasStyle.width = UiLength::Percent(100.0f);
    canvasStyle.height = UiLength::Percent(100.0f);
    canvasStyle.background = UiColor{0.035f, 0.04f, 0.05f, 1.0f};
    canvasStyle.overflow = UiOverflow::Hidden;
    document_.SetStyle(designerCanvas_, canvasStyle);
    designerPlaceholder_ = document_.CreateElement(UiElementKind::Label);
    if (document_.SetParent(designerPlaceholder_, designerCanvas_))
    {
        document_.SetStyle(designerPlaceholder_, ButtonStyle(16.0f, 16.0f, 360.0f, 24.0f,
                                                             UiColor{0.035f, 0.04f, 0.05f, 0.0f}));
        document_.SetText(designerPlaceholder_, Text("EditorUi.NoUiDocument", "No UI document loaded"));
    }
    dispatcher_.AddHandler(designerCanvas_, UiEventType::PointerDown, [this](UiInputEvent& event) {
        const UiRect canvas = layout_.Rect(designerCanvas_);
        const float x = event.x - canvas.x;
        const float y = event.y - canvas.y;
        bool hit = false;
        for (auto node = designerNodes_.rbegin(); node != designerNodes_.rend(); ++node)
        {
            if (node->visible && x >= node->rect.x && x < node->rect.x + node->rect.width &&
                y >= node->rect.y && y < node->rect.y + node->rect.height)
            {
                hit = true;
                break;
            }
        }
        designerDragging_ = hit && designerDown_ != nullptr;
        if (designerDragging_ && designerDown_)
        {
            designerDown_(event.x, event.y);
        }
        event.handled = true;
    }, UiEventPhase::Bubble);
    dispatcher_.AddHandler(designerCanvas_, UiEventType::PointerMove, [this](UiInputEvent& event) {
        if (designerDragging_ && designerMove_)
        {
            designerMove_(event.x, event.y);
            event.handled = true;
        }
    }, UiEventPhase::Bubble);
    dispatcher_.AddHandler(designerCanvas_, UiEventType::PointerUp, [this](UiInputEvent& event) {
        if (designerDragging_ && designerUp_)
        {
            designerUp_(event.x, event.y);
        }
        designerDragging_ = false;
        event.handled = true;
    }, UiEventPhase::Bubble);
}

void EditorShell::SetDesignerNodes(std::vector<EditorDesignerNode> nodes)
{
    if (nodes == designerNodes_)
    {
        return;
    }
    designerNodes_ = std::move(nodes);
    RefreshDesignerNodes();
}

void EditorShell::SetDesignerHandlers(std::function<void(float, float)> down,
                                      std::function<void(float, float)> move,
                                      std::function<void(float, float)> up)
{
    designerDown_ = std::move(down);
    designerMove_ = std::move(move);
    designerUp_ = std::move(up);
}

void EditorShell::RefreshDesignerNodes()
{
    for (const UiElementHandle element : designerNodeElements_)
    {
        if (element.IsValid())
        {
            document_.DestroyElement(element);
        }
    }
    designerNodeElements_.clear();
    if (designerPlaceholder_.IsValid())
    {
        document_.SetVisible(designerPlaceholder_, designerNodes_.empty());
    }
    for (const EditorDesignerNode& node : designerNodes_)
    {
        const UiElementHandle element = document_.CreateElement(UiElementKind::Panel);
        if (!document_.SetParent(element, designerCanvas_))
        {
            continue;
        }
        UiStyle style = AbsoluteStyle(node.rect.x, node.rect.y, node.rect.width, node.rect.height);
        style.background = node.color.a > 0.0f ? node.color : kSurfaceActive;
        style.borderWidth = node.selected ? 2.0f : 1.0f;
        style.borderColor = node.selected ? kAccent : kBorder;
        style.color = kText;
        style.fontSize = 13.0f;
        style.overflow = UiOverflow::Hidden;
        document_.SetStyle(element, style);
        document_.SetText(element, RowText(node.text, 140.0f));
        document_.SetVisible(element, node.visible);
        designerNodeElements_.push_back(element);
    }
}

UiElementHandle EditorShell::UiDesignerContent() const noexcept
{
    return Slot(EditorPanelId::UiDesigner).content;
}

UiElementHandle EditorShell::DesignerNodeElement(std::size_t index) const noexcept
{
    return index < designerNodeElements_.size() ? designerNodeElements_[index] : UiElementHandle{};
}

void EditorShell::BuildChromeMenus()
{
    const std::string file = Text("EditorUi.File", "File");
    const std::string edit = Text("EditorUi.Edit", "Edit");
    const std::string play = Text("EditorUi.Play", "Play");
    const std::string project = Text("EditorUi.Project", "Project");
    const std::string panels = Text("EditorUi.Panels", "Panels");
    chrome_.AddMenu(file, Text("EditorUi.Open", "Open"), EditorCommandId::Open);
    chrome_.AddMenu(file, Text("EditorUi.NewScene", "New Scene"), EditorCommandId::NewScene);
    chrome_.AddMenu(file, Text("EditorUi.Save", "Save"), EditorCommandId::Save);
    chrome_.AddMenu(file, Text("EditorUi.SaveSceneAs", "Save As"), EditorCommandId::SaveAs);
    chrome_.AddMenu(file, Text("EditorUi.SaveAll", "Save All"), EditorCommandId::SaveAll);
    chrome_.AddMenu(edit, Text("EditorUi.Undo", "Undo"), EditorCommandId::Undo);
    chrome_.AddMenu(edit, Text("EditorUi.Redo", "Redo"), EditorCommandId::Redo);
    chrome_.AddMenu(edit, Text("EditorUi.Preferences", "Preferences"), EditorCommandId::TogglePanel, "preferences");
    chrome_.AddMenu(play, Text("EditorUi.Play", "Play / Stop"), EditorCommandId::Play);
    chrome_.AddMenu(play, Text("EditorUi.Pause", "Pause / Resume"), EditorCommandId::Pause);
    chrome_.AddMenu(play, Text("EditorUi.StepPlay", "Step"), EditorCommandId::Step);
    chrome_.AddMenu(play, Text("EditorUi.Stop", "Stop"), EditorCommandId::Stop);
    chrome_.AddMenu(project, Text("EditorUi.ProjectSettings", "Project Settings"), EditorCommandId::OpenSettings);
    chrome_.AddMenu(project, Text("EditorUi.BuildExport", "Build / Export Panel"), EditorCommandId::TogglePanel,
                    "build-export");
    chrome_.AddMenu(project, Text("EditorUi.Export", "Export"), EditorCommandId::Export);
    chrome_.AddMenu(project, Text("EditorUi.BuildRestart", "Build / Restart"), EditorCommandId::Build);
    const EditorPanelId panelIds[] = {EditorPanelId::Hierarchy, EditorPanelId::Assets, EditorPanelId::Viewport,
                                      EditorPanelId::UiDesigner, EditorPanelId::Inspector, EditorPanelId::Console,
                                      EditorPanelId::Material, EditorPanelId::ProjectSettings,
                                      EditorPanelId::BuildExport};
    for (const EditorPanelId panel : panelIds)
    {
        chrome_.AddMenu(panels, PanelLabel(panel), EditorCommandId::TogglePanel, EditorPanelName(panel));
    }
    chrome_.AddMenu(panels, Text("EditorUi.FocusViewport", "Focus Viewport"), EditorCommandId::FocusViewport);
    const std::string help = Text("EditorUi.Help", "Help");
    chrome_.AddMenu(help, Text("EditorUi.About", "About"), EditorCommandId::TogglePanel, "about");
    chrome_.AddMenu(help, Text("EditorUi.Diagnostics", "Diagnostics"), EditorCommandId::TogglePanel, "diagnostics");

    chrome_.AddToolbar(Text("EditorUi.Save", "Save"), EditorCommandId::Save);
    chrome_.AddToolbar(Text("EditorUi.Undo", "Undo"), EditorCommandId::Undo);
    chrome_.AddToolbar(Text("EditorUi.Redo", "Redo"), EditorCommandId::Redo);
    chrome_.AddToolbar(Text("EditorUi.Play", "Play"), EditorCommandId::Play);
    chrome_.AddToolbar(Text("EditorUi.Pause", "Pause"), EditorCommandId::Pause);
    chrome_.AddToolbar(Text("EditorUi.StepPlay", "Step"), EditorCommandId::Step);
    chrome_.AddToolbar(Text("EditorUi.Stop", "Stop"), EditorCommandId::Stop);
    chrome_.AddToolbar(Text("EditorUi.Export", "Export"), EditorCommandId::Export);
    chrome_.AddToolbar(Text("EditorUi.BuildRestart", "Build"), EditorCommandId::Build);
}

void EditorShell::BuildChrome()
{
    menuBar_ = document_.CreateElement(UiElementKind::Panel);
    if (document_.SetParent(menuBar_, root_))
    {
        UiStyle style = AbsoluteStyle(0.0f, 0.0f, width_, menuHeight_);
        style.width = UiLength::Percent(100.0f);
        style.background = kSurfaceRaised;
        style.borderColor = kBorder;
        style.borderWidth = 1.0f;
        document_.SetStyle(menuBar_, style);
    }
    toolbar_ = document_.CreateElement(UiElementKind::Panel);
    if (document_.SetParent(toolbar_, root_))
    {
        UiStyle style = AbsoluteStyle(0.0f, menuHeight_, width_, toolbarHeight_);
        style.width = UiLength::Percent(100.0f);
        style.background = kSurface;
        style.borderColor = kBorder;
        style.borderWidth = 1.0f;
        document_.SetStyle(toolbar_, style);
    }
    statusBar_ = document_.CreateElement(UiElementKind::Panel);
    if (document_.SetParent(statusBar_, root_))
    {
        UiStyle style = AbsoluteStyle(0.0f, height_ - statusHeight_, width_, statusHeight_);
        style.width = UiLength::Percent(100.0f);
        style.background = kSurfaceRaised;
        style.color = kMuted;
        style.fontSize = 12.0f;
        style.borderColor = kBorder;
        style.borderWidth = 1.0f;
        document_.SetStyle(statusBar_, style);
    }

    const auto& menus = chrome_.Menus();
    menuNames_.clear();
    menuFirstItem_.clear();
    for (std::size_t index = 0; index < menus.size(); ++index)
    {
        if (menuNames_.empty() || menuNames_.back() != menus[index].menu)
        {
            menuNames_.push_back(menus[index].menu);
            menuFirstItem_.push_back(index);
        }
    }
    float menuX = 6.0f;
    for (std::size_t index = 0; index < menuNames_.size(); ++index)
    {
        const std::string& name = menuNames_[index];
        const float width = MenuWidth(name);
        const UiElementHandle button = document_.CreateElement(UiElementKind::Button);
        if (!document_.SetParent(button, menuBar_))
        {
            continue;
        }
        document_.SetStyle(button, ButtonStyle(menuX, 0.0f, width, menuHeight_, kSurfaceRaised));
        document_.SetText(button, name);
        dispatcher_.AddHandler(button, UiEventType::PointerDown, [this, name](UiInputEvent& event) {
            if (openMenu_ == name)
            {
                CloseMenu();
            }
            else
            {
                OpenMenu(name);
            }
            event.handled = true;
        });
        menuButtons_.push_back(button);
        menuX += width + 2.0f;
    }

    float toolbarX = 6.0f;
    for (std::size_t index = 0; index < chrome_.Toolbar().size(); ++index)
    {
        const float width = ToolbarWidth(chrome_.Toolbar()[index].label);
        const UiElementHandle button = document_.CreateElement(UiElementKind::Button);
        if (!document_.SetParent(button, toolbar_))
        {
            continue;
        }
        document_.SetStyle(button, ButtonStyle(toolbarX, 4.0f, width, toolbarHeight_ - 8.0f, kSurfaceRaised));
        document_.SetText(button, chrome_.Toolbar()[index].label);
        dispatcher_.AddHandler(button, UiEventType::PointerDown, [this, index](UiInputEvent& event) {
            if (!chrome_.ActivateToolbar(index))
            {
                SetStatus(Text("EditorUi.CommandUnavailable", "Command unavailable") + ": " +
                          chrome_.Toolbar()[index].label);
            }
            event.handled = true;
        });
        toolbarButtons_.push_back(button);
        toolbarX += width + 4.0f;
    }

    viewportTools_=document_.CreateElement(UiElementKind::Panel);
    document_.SetParent(viewportTools_,root_);
    document_.SetStyle(viewportTools_,AbsoluteStyle(0,0,1,27));
    const char* actions[]={"Q","W","E","R","X","Z","Sky","Grid","Gizmos","FrameAll"};
    for(const char* action:actions)
    {
        const auto button=document_.CreateElement(UiElementKind::Button);
        document_.SetParent(button,viewportTools_);
        dispatcher_.AddHandler(button,UiEventType::PointerDown,[this,action](UiInputEvent& e){
            if(e.button==0&&viewportAction_)viewportAction_(action);
            e.handled=true;e.StopPropagation();
        });
        viewportToolButtons_.push_back(button);
    }

    gizmo_ = document_.CreateElement(UiElementKind::Panel);
    if (document_.SetParent(gizmo_, root_))
    {
        UiStyle gizmoStyle = AbsoluteStyle(0.0f, 0.0f, 12.0f, 12.0f);
        gizmoStyle.borderWidth = 2.0f;
        gizmoStyle.borderColor = kAccent;
        document_.SetStyle(gizmo_, gizmoStyle);
        document_.SetVisible(gizmo_, false);
    }
    const UiColor axisColors[] = {{0.95f, 0.25f, 0.25f, 1.0f}, {0.25f, 0.85f, 0.35f, 1.0f},
                                  {0.30f, 0.55f, 1.0f, 1.0f}};
    for (int axis = 0; axis < 3; ++axis)
    {
        const UiElementHandle handle = document_.CreateElement(UiElementKind::Panel);
        if (!document_.SetParent(handle, root_))
        {
            continue;
        }
        UiStyle axisStyle = AbsoluteStyle(0.0f, 0.0f, 8.0f, 8.0f);
        axisStyle.background = axisColors[axis];
        document_.SetStyle(handle, axisStyle);
        document_.SetVisible(handle, false);
        dispatcher_.AddHandler(handle, UiEventType::PointerDown, [this, axis](UiInputEvent& event) {
            gizmoDragAxis_ = axis + 1;
            gizmoLastX_ = event.x;
            gizmoLastY_ = event.y;
        });
        dispatcher_.AddHandler(handle, UiEventType::PointerMove, [this, axis](UiInputEvent& event) {
            if (gizmoDragAxis_ != axis + 1)
            {
                return;
            }
            if (gizmoDragHandler_)
            {
                gizmoDragHandler_(axis, event.x - gizmoLastX_, event.y - gizmoLastY_);
            }
            gizmoLastX_ = event.x;
            gizmoLastY_ = event.y;
        });
        dispatcher_.AddHandler(handle, UiEventType::PointerUp, [this, axis](UiInputEvent&) {
            if (gizmoDragAxis_ == axis + 1)
            {
                gizmoDragAxis_ = 0;
            }
        });
        gizmoAxes_.push_back(handle);
    }

    dragGhost_ = document_.CreateElement(UiElementKind::Label);
    if (document_.SetParent(dragGhost_, root_))
    {
        UiStyle ghostStyle = AbsoluteStyle(0.0f, 0.0f, 220.0f, 24.0f);
        ghostStyle.background = UiInk(flare::Selected, 0.95f);
        ghostStyle.borderWidth = 1.0f;
        ghostStyle.borderColor = kAccent;
        ghostStyle.color = kText;
        ghostStyle.fontSize = 13.0f;
        ghostStyle.pointerEvents=UiPointerEvents::None;
        document_.SetStyle(dragGhost_,ghostStyle);
        document_.SetVisible(dragGhost_, false);
    }
    dropHighlight_ = document_.CreateElement(UiElementKind::Panel);
    if (document_.SetParent(dropHighlight_, root_))
    {
        UiStyle highlight = AbsoluteStyle(0.0f, 0.0f, 0.0f, 0.0f);
        highlight.background = UiInk(flare::Silver, 0.20f);
        highlight.borderWidth = 1.0f;
        highlight.borderColor = kAccent;
        highlight.pointerEvents=UiPointerEvents::None;
        document_.SetStyle(dropHighlight_,highlight);
        document_.SetVisible(dropHighlight_, false);
    }
    tabInsertMarker_=document_.CreateElement(UiElementKind::Panel);
    document_.SetParent(tabInsertMarker_,root_);
    UiStyle marker=AbsoluteStyle(0,0,2,kTabHeight);
    marker.background=kAccent;
    marker.pointerEvents=UiPointerEvents::None;
    document_.SetStyle(tabInsertMarker_,marker);
    document_.SetVisible(tabInsertMarker_,false);

    dispatcher_.AddHandler(document_.Root(), UiEventType::PointerDown, [this](UiInputEvent& event) {
        if (closeModalOpen_ || confirmOpen_)
        {
            event.handled = true;
            return;
        }
        BeginSplitter(event.x, event.y);
        if (splitterActive_ != 0)
        {
            event.handled = true;
            return;
        }
        if (!openMenu_.empty())
        {
            bool insideDropdown = false;
            if (menuDropdown_.IsValid())
            {
                const UiRect dropdown = layout_.Rect(menuDropdown_);
                insideDropdown = dropdown.x <= event.x && event.x < dropdown.x + dropdown.width &&
                    dropdown.y <= event.y && event.y < dropdown.y + dropdown.height;
            }
            if (!insideDropdown && event.y >= menuHeight_ + toolbarHeight_)
            {
                CloseMenu();
            }
        }
    }, UiEventPhase::Capture);
    dispatcher_.AddHandler(document_.Root(), UiEventType::PointerMove, [this](UiInputEvent& event) {
        if (closeModalOpen_)
        {
            event.handled = true;
            return;
        }
        if (splitterActive_ != 0)
        {
            DragSplitter(event.x, event.y);
            event.handled = true;
        }
    }, UiEventPhase::Capture);
    dispatcher_.AddHandler(document_.Root(), UiEventType::PointerUp, [this](UiInputEvent& event) {
        if (closeModalOpen_)
        {
            event.handled = true;
            return;
        }
        if (splitterActive_ != 0)
        {
            EndSplitter();
            event.handled = true;
        }
    }, UiEventPhase::Capture);
    dispatcher_.AddHandler(document_.Root(), UiEventType::KeyDown, [this](UiInputEvent& event) {
        if(event.key==UiKeyEscape && dragTabPanel_!=EditorPanelId::None)
        {
            CancelTabDrag();
            event.handled=true;
            event.StopPropagation();
            return;
        }
        if (confirmOpen_)
        {
            if (event.key == UiKeyEscape)
            {
                HideConfirm();
                event.handled = true;
            }
            return;
        }
        if (!closeModalOpen_)
        {
            return;
        }
        if (event.key == UiKeyEscape)
        {
            HideCloseModal();
            event.handled = true;
        }
        else if (event.key == VK_F9)
        {
            // 无鼠标回退：F9 = 保存，F10 = 丢弃。按钮点击仍是主路径。
            if (commands_.Execute(EditorCommand{EditorCommandId::Save, {}}))
            {
                closeApproved_ = true;
                closeRequested_ = true;
                HideCloseModal();
            }
            event.handled = true;
        }
        else if (event.key == VK_F10)
        {
            closeApproved_ = true;
            closeRequested_ = true;
            HideCloseModal();
            event.handled = true;
        }
    }, UiEventPhase::Capture);

    lines_.push_back("Editor toolkit shell ready");
    lines_.push_back("Dock: 3x2 default");
    lines_.push_back("Console: read-only / virtualized");
    if (console_)
    {
        console_->SetItemCount(static_cast<int>(lines_.size()));
        console_->Sync();
    }
    RefreshStatus();
}

void EditorShell::SetDpiScale(float scale)
{
    dpiScale_ = scale > 0.0f && std::isfinite(scale) ? scale : 1.0f;
}

void EditorShell::Resize(float width, float height)
{
    width_ = std::max(320.0f, width / dpiScale_);
    height_ = std::max(240.0f, height / dpiScale_);
    if (initialized_)
    {
        Frame();
    }
}

void EditorShell::Frame()
{
    if (!initialized_ || !root_.IsValid())
    {
        return;
    }
    // 结构变化可能发生在事件处理期间；销毁元素统一延迟到帧边界，避免销毁正在分发的目标。
    for (const UiElementHandle element : pendingDestroy_)
    {
        if (element.IsValid())
        {
            document_.DestroyElement(element);
        }
    }
    pendingDestroy_.clear();
    if (builtDockRevision_ != dockRevision_)
    {
        RebuildGroupViews();
        builtDockRevision_ = dockRevision_;
    }
    if (modalBackdrop_.IsValid())
    {
        document_.SetParent(modalBackdrop_, root_);
    }
    if (confirmBackdrop_.IsValid())
    {
        document_.SetParent(confirmBackdrop_, root_);
    }
    UiStyle mainStyle=document_.Style(root_);
    mainStyle.position=UiPosition::Absolute;mainStyle.left=UiLength::Px(0);mainStyle.top=UiLength::Px(0);
    mainStyle.width=UiLength::Px(width_);mainStyle.height=UiLength::Px(height_);
    document_.SetStyle(root_,mainStyle);
    UpdateDockRects();
    float layoutWidth=width_,layoutHeight=height_;
    for(const auto& surface:surfaces_){layoutWidth=std::max(layoutWidth,surface.width);layoutHeight=std::max(layoutHeight,surface.height);}
    const auto panelHeight = [this](EditorPanelId panel) {
        for (const GroupView& group : groups_)
        {
            if (std::find(group.tabs.begin(), group.tabs.end(), panel) != group.tabs.end())
            {
                return std::max(0.0f, group.rect.height - kTabHeight);
            }
        }
        return 0.0f;
    };
    const auto syncContent = [this, &panelHeight](EditorPanelId panel,
                                                  const std::unique_ptr<UiListView>& list,
                                                  float reserved) {
        if (!list)
        {
            return;
        }
        list->SetViewportHeight(std::max(0.0f, panelHeight(panel) - reserved));
        list->Sync();
    };
    syncContent(EditorPanelId::Console, console_, 0.0f);
    syncContent(EditorPanelId::Hierarchy, hierarchyList_, 0.0f);
    syncContent(EditorPanelId::Assets, assetsList_, 48.0f);
    syncContent(EditorPanelId::Inspector, inspectorList_, 28.0f);
    UpdatePanelContentRects();
    RefreshStatus();
    layout_.Layout(layoutWidth, layoutHeight);
    UpdateScrollThumbs();
    // thumb 的 style 变化发生在首帧尾部；立即消化，避免下一帧空闲时仍报告 layout。
    layout_.Layout(layoutWidth, layoutHeight);
}

void EditorShell::RebuildGroupViews()
{
    for (GroupView& group : groups_)
    {
        if (group.tabBar.IsValid())
        {
            document_.DestroyElement(group.tabBar);
        }
    }
    for(const auto& slot:slots_)if(slot.panel.IsValid())document_.SetParent(slot.panel,root_);
    document_.SetParent(dropHighlight_,root_);
    document_.SetParent(tabInsertMarker_,root_);
    document_.SetParent(dragGhost_,root_);
    document_.SetParent(viewportTools_,root_);
    std::erase_if(surfaces_,[this](const auto& surface){
        if(dock_.Surface(surface.id))return false;
        document_.DestroyElement(surface.root);return true;
    });
    for(const auto& window:dock_.Windows())if(!SurfaceRoot(window.id).IsValid())
    {
        const auto element=document_.CreateElement(UiElementKind::Panel);
        document_.SetParent(element,document_.Root());
        UiStyle style=AbsoluteStyle(0,0,640,440);style.background=kContent;
        document_.SetStyle(element,style);surfaces_.push_back({window.id,element,640,440});
    }
    groups_.clear();
    std::uint32_t surface=0;
    const auto walk = [this,&surface](const auto& self, const DockNode& node, std::vector<std::size_t>& path) -> void {
        if (node.kind == DockNode::Kind::Tabs)
        {
            GroupView group;
            group.surface=surface;
            for(const auto panel:node.tabs)document_.SetParent(Slot(panel).panel,SurfaceRoot(surface));
            group.tabs = node.tabs;
            group.active = node.active;
            group.path = path;
            groups_.push_back(std::move(group));
            RebuildTabBar(groups_.back());
            return;
        }
        for (std::size_t index = 0; index < node.children.size(); ++index)
        {
            path.push_back(index);
            self(self, node.children[index], path);
            path.pop_back();
        }
    };
    std::vector<std::size_t> path;
    walk(walk, dock_.Root(), path);
    for(const auto& window:dock_.Windows()){surface=window.id;walk(walk,window.layout,path);}
    // 重新挂到末尾，保证拖影/落点提示绘制在 tab bar 之上。
    if (dropHighlight_.IsValid())
    {
        document_.SetParent(dropHighlight_, root_);
    }
    if(tabInsertMarker_.IsValid())document_.SetParent(tabInsertMarker_,root_);
    if(dragGhost_.IsValid())document_.SetParent(dragGhost_,root_);
}

void EditorShell::RebuildTabBar(GroupView& group)
{
    group.tabBar = document_.CreateElement(UiElementKind::Panel);
    if (!document_.SetParent(group.tabBar, SurfaceRoot(group.surface)))
    {
        return;
    }
    UiStyle barStyle = AbsoluteStyle(0.0f, 0.0f, 100.0f, kTabHeight);
    barStyle.background = kSurfaceRaised;
    barStyle.borderWidth = 1.0f;
    barStyle.borderColor = kBorder;
    document_.SetStyle(group.tabBar, barStyle);
    group.tabButtons.clear();
    group.closeButtons.clear();
    float x = 0.0f;
    for (const EditorPanelId panel : group.tabs)
    {
        const std::string label = PanelLabel(panel);
        const float width = TabWidth(label);
        const UiElementHandle button = document_.CreateElement(UiElementKind::Button);
        if (!document_.SetParent(button, group.tabBar))
        {
            continue;
        }
        document_.SetStyle(button, ButtonStyle(x, 0.0f, width, kTabHeight, kSurfaceRaised));
        document_.SetText(button, label);
        dispatcher_.AddHandler(button, UiEventType::PointerDown, [this, panel](UiInputEvent& event) {
            HandleTabDown(panel, event);
        });
        dispatcher_.AddHandler(button, UiEventType::PointerMove, [this](UiInputEvent& event) {
            HandleTabMove(event);
        });
        dispatcher_.AddHandler(button, UiEventType::PointerUp, [this](UiInputEvent& event) {
            HandleTabUp(event);
        });
        dispatcher_.AddHandler(button,UiEventType::PointerCaptureLost,[this](UiInputEvent&) {
            CancelTabDrag();
        });
        dispatcher_.AddHandler(button,UiEventType::FocusLost,[this](UiInputEvent&) {
            CancelTabDrag();
        });
        const UiElementHandle close = document_.CreateElement(UiElementKind::Button);
        if (document_.SetParent(close, group.tabBar))
        {
            document_.SetStyle(close, ButtonStyle(x + width - 18.0f, 2.0f, 16.0f, 20.0f, UiColor{0,0,0,0}));
            document_.SetText(close, "×");
            dispatcher_.AddHandler(close, UiEventType::PointerDown, [this, panel](UiInputEvent& event) {
                dock_.CloseTab(panel);
                if (activePanel_ == panel)
                {
                    activePanel_ = EditorPanelId::Hierarchy;
                }
                MarkDockDirty(true, true);
                event.handled = true;
            });
        }
        group.tabButtons.push_back(button);
        group.closeButtons.push_back(close);
        x += width + 2.0f;
    }
}

void EditorShell::UpdateDockRects()
{
    dockRect_ = {0.0f, menuHeight_ + toolbarHeight_, width_,
                 std::max(0.0f, height_ - menuHeight_ - toolbarHeight_ - statusHeight_)};
    splitHandles_.clear();
    std::size_t groupIndex = 0;
    std::vector<std::size_t> path;
    std::uint32_t surface=0;
    const auto walk = [this, &groupIndex, &path,&surface](const auto& self, const DockNode& node,
                                                 const UiRect& rect) -> void {
        if (node.kind == DockNode::Kind::Tabs)
        {
            if (groupIndex >= groups_.size())
            {
                return;
            }
            GroupView& group = groups_[groupIndex++];
            group.rect = rect;
            if (group.tabBar.IsValid())
            {
                UiStyle barStyle = AbsoluteStyle(rect.x, rect.y, rect.width, kTabHeight);
                barStyle.background = kSurfaceRaised;
                barStyle.borderWidth = 1.0f;
                barStyle.borderColor = kBorder;
                document_.SetStyle(group.tabBar, barStyle);
                float x = 0.0f;
                for (std::size_t index = 0; index < group.tabButtons.size(); ++index)
                {
                    const float width = TabWidth(PanelLabel(group.tabs[index]));
                    UiStyle style = document_.Style(group.tabButtons[index]);
                    style.left = UiLength::Px(x);
                    style.width = UiLength::Px(width);
                    style.background = group.tabs[index] == group.active ? kSurfaceActive : kSurfaceRaised;
                    document_.SetStyle(group.tabButtons[index], style);
                    if (index < group.closeButtons.size())
                    {
                        UiStyle closeStyle = document_.Style(group.closeButtons[index]);
                        closeStyle.left = UiLength::Px(x + width - 18.0f);
                        document_.SetStyle(group.closeButtons[index], closeStyle);
                    }
                    x += width + 2.0f;
                }
            }
            for (const EditorPanelId panel : group.tabs)
            {
                PanelSlot& slot = Slot(panel);
                if (!slot.panel.IsValid())
                {
                    continue;
                }
                UiStyle style = PanelStyle({rect.x, rect.y + kTabHeight, rect.width,
                                            std::max(0.0f, rect.height - kTabHeight)});
                if (!viewportImage_.empty() && panel == EditorPanelId::Viewport)
                {
                    style.image = viewportImage_;
                }
                document_.SetStyle(slot.panel, style);
            }
            return;
        }
        if (node.children.size() != 2)
        {
            return;
        }
        const float total = node.axis == DockAxis::Horizontal ? rect.width : rect.height;
        const auto children=SplitDockRects(node,rect);
        const UiRect first=children[0],second=children[1];
        if (node.axis == DockAxis::Horizontal)
        {
            SplitHandle handle;
            handle.surface=surface;
            handle.path = path;
            handle.axis = DockAxis::Horizontal;
            handle.rect = {second.x - 3.0f, rect.y, 6.0f, rect.height};
            handle.ratio = node.ratio;
            handle.total = total;
            splitHandles_.push_back(std::move(handle));
        }
        else
        {
            SplitHandle handle;
            handle.surface=surface;
            handle.path = path;
            handle.axis = DockAxis::Vertical;
            handle.rect = {rect.x, second.y - 3.0f, rect.width, 6.0f};
            handle.ratio = node.ratio;
            handle.total = total;
            splitHandles_.push_back(std::move(handle));
        }
        path.push_back(0);
        self(self, node.children[0], first);
        path.pop_back();
        path.push_back(1);
        self(self, node.children[1], second);
        path.pop_back();
    };
    walk(walk, dock_.Root(), dockRect_);
    for(const auto& window:dock_.Windows()){surface=window.id;walk(walk,window.layout,SurfaceDockRect(surface));}
    // 同步 active 与可见性。
    groupIndex = 0;
    for (EditorPanelId panel : {EditorPanelId::Hierarchy, EditorPanelId::Assets, EditorPanelId::Viewport,
                                EditorPanelId::UiDesigner, EditorPanelId::Inspector, EditorPanelId::Console,
                                EditorPanelId::Material, EditorPanelId::ProjectSettings, EditorPanelId::BuildExport,
                                EditorPanelId::Preferences, EditorPanelId::About, EditorPanelId::Diagnostics})
    {
        PanelSlot& slot = Slot(panel);
        if (slot.panel.IsValid())
        {
            document_.SetVisible(slot.panel, false);
        }
    }
    for (GroupView& group : groups_)
    {
        const DockNode* node = NodeAt(*dock_.Surface(group.surface), group.path);
        if (node != nullptr)
        {
            group.active = node->active;
        }
        for (std::size_t index = 0; index < group.tabButtons.size(); ++index)
        {
            UiStyle style = document_.Style(group.tabButtons[index]);
            style.background = group.tabs[index] == group.active ? kSurfaceActive : kSurfaceRaised;
            document_.SetStyle(group.tabButtons[index], style);
        }
        if (group.active != EditorPanelId::None)
        {
            PanelSlot& slot = Slot(group.active);
            if (slot.panel.IsValid())
            {
                document_.SetVisible(slot.panel, true);
            }
        }
    }
    if (statusBar_.IsValid())
    {
        UiStyle style = document_.Style(statusBar_);
        style.top = UiLength::Px(height_ - statusHeight_);
        style.width = UiLength::Percent(100.0f);
        document_.SetStyle(statusBar_, style);
    }
}

void EditorShell::UpdatePanelContentRects()
{
    const auto setContent = [this](EditorPanelId panel, float top, float height) {
        PanelSlot& slot = Slot(panel);
        if (!slot.content.IsValid())
        {
            return;
        }
        UiStyle style = AbsoluteStyle(0.0f, top, 100.0f, height);
        style.width = UiLength::Percent(100.0f);
        style.height = UiLength::Px(std::max(0.0f, height));
        style.overflow = UiOverflow::Hidden;
        style.background = kContent;
        document_.SetStyle(slot.content, style);
    };
    const auto panelHeight = [this](EditorPanelId panel) {
        for (const GroupView& group : groups_)
        {
            if (std::find(group.tabs.begin(), group.tabs.end(), panel) != group.tabs.end())
            {
                return std::max(0.0f, group.rect.height - kTabHeight);
            }
        }
        return 0.0f;
    };
    const auto panelWidth = [this](EditorPanelId panel) {
        for (const GroupView& group : groups_)
        {
            if (std::find(group.tabs.begin(), group.tabs.end(), panel) != group.tabs.end())
            {
                return std::max(0.0f, group.rect.width);
            }
        }
        return 0.0f;
    };
    setContent(EditorPanelId::Hierarchy, 0.0f, panelHeight(EditorPanelId::Hierarchy));
    setContent(EditorPanelId::Console, 0.0f, panelHeight(EditorPanelId::Console));
    setContent(EditorPanelId::Inspector, 0.0f, std::max(0.0f, panelHeight(EditorPanelId::Inspector) - 28.0f));
    setContent(EditorPanelId::Assets, 24.0f, std::max(0.0f, panelHeight(EditorPanelId::Assets) -
        (assetsPreviewImage_.empty() ? 48.0f : 104.0f)));
    for (EditorPanelId panel : {EditorPanelId::UiDesigner, EditorPanelId::Material,
                                EditorPanelId::ProjectSettings, EditorPanelId::BuildExport,
                                EditorPanelId::Preferences, EditorPanelId::About, EditorPanelId::Diagnostics})
    {
        setContent(panel, 0.0f, panelHeight(panel));
    }
    UpdateFormRects(EditorPanelId::Material);
    UpdateFormRects(EditorPanelId::ProjectSettings);
    UpdateFormRects(EditorPanelId::BuildExport);
    UpdateFormRects(EditorPanelId::Preferences);
    if (designerCanvas_.IsValid())
    {
        UiStyle style = document_.Style(designerCanvas_);
        style.left = UiLength::Px(0.0f);
        style.top = UiLength::Px(0.0f);
        style.width = UiLength::Percent(100.0f);
        style.height = UiLength::Percent(100.0f);
        document_.SetStyle(designerCanvas_, style);
    }
    if (assetsFilterElement_.IsValid())
    {
        UiStyle style = document_.Style(assetsFilterElement_);
        style.left = UiLength::Px(0.0f);
        style.top = UiLength::Px(0.0f);
        style.width = UiLength::Px(std::max(40.0f, panelWidth(EditorPanelId::Assets) - 72.0f));
        style.height = UiLength::Px(24.0f);
        document_.SetStyle(assetsFilterElement_, style);
    }
    if (assetsImportElement_.IsValid())
    {
        UiStyle style = document_.Style(assetsImportElement_);
        style.left = UiLength::Px(std::max(0.0f, panelWidth(EditorPanelId::Assets) - 66.0f));
        style.top = UiLength::Px(0.0f);
        document_.SetStyle(assetsImportElement_, style);
    }
    if (assetsPreviewImageElement_.IsValid())
    {
        UiStyle style = document_.Style(assetsPreviewImageElement_);
        style.top = UiLength::Px(std::max(0.0f, panelHeight(EditorPanelId::Assets) - 100.0f));
        style.width = UiLength::Px(std::max(0.0f, std::min(160.0f, width_ * 0.125f)));
        style.height = UiLength::Px(68.0f);
        document_.SetStyle(assetsPreviewImageElement_, style);
        document_.SetVisible(assetsPreviewImageElement_, !assetsPreviewImage_.empty());
    }
    if (assetsPreview_.IsValid())
    {
        UiStyle style = document_.Style(assetsPreview_);
        style.top = UiLength::Px(std::max(0.0f, panelHeight(EditorPanelId::Assets) - 24.0f));
        style.width = UiLength::Percent(100.0f);
        document_.SetStyle(assetsPreview_, style);
    }
    if (inspectorEditElement_.IsValid())
    {
        UiStyle style = document_.Style(inspectorEditElement_);
        style.top = UiLength::Px(std::max(0.0f, panelHeight(EditorPanelId::Inspector) - 28.0f));
        style.width = UiLength::Percent(100.0f);
        document_.SetStyle(inspectorEditElement_, style);
    }
}

void EditorShell::SetHierarchy(std::vector<EditorHierarchyNode> nodes)
{
    // EditorApplication 每帧重建同样的列表；内容未变时不要销毁行元素，
    // 否则新行在当帧没有布局矩形，会被画成零尺寸。
    if (nodes == hierarchyModel_.Nodes())
    {
        return;
    }
    hierarchyModel_.SetNodes(std::move(nodes));
    if (hierarchyList_)
    {
        hierarchyList_->Invalidate();
        hierarchyList_->SetItemCount(static_cast<int>(hierarchyModel_.VisibleRows().size()));
        hierarchyList_->Sync();
    }
}

void EditorShell::SetAssets(std::vector<EditorAssetEntry> entries)
{
    if (entries == assetsModel_.Entries())
    {
        return;
    }
    assetsModel_.SetEntries(std::move(entries));
    if (assetsList_)
    {
        assetsList_->Invalidate();
        assetsList_->SetItemCount(static_cast<int>(assetsModel_.Filtered().size()));
        assetsList_->Sync();
    }
}

void EditorShell::SetInspector(std::uint64_t id, std::string label, std::vector<EditorProperty> properties)
{
    if (id == inspectorModel_.Target() && label == inspectorModel_.TargetLabel() &&
        properties == inspectorModel_.Properties())
    {
        return;
    }
    if(id!=inspectorModel_.Target()&&hierarchyList_)hierarchyList_->Invalidate();
    inspectorModel_.SetTarget(id, std::move(label));
    inspectorModel_.SetProperties(std::move(properties));
    if (inspectorList_)
    {
        inspectorList_->SetRowBackgroundProvider([this](int index) {
            const auto& properties = inspectorModel_.Properties();
            if (index < 0 || static_cast<std::size_t>(index) >= properties.size())
            {
                return UiColor{0.0f, 0.0f, 0.0f, 0.0f};
            }
            return RowBackgroundColor(properties[static_cast<std::size_t>(index)]);
        });
        inspectorList_->Invalidate();
        inspectorList_->SetItemCount(static_cast<int>(inspectorModel_.Properties().size()));
        inspectorList_->Sync();
    }
    SetSelectionLabel(inspectorModel_.TargetLabel());
}

bool EditorShell::BeginRename()
{
    if (!inspectorEdit_ || !inspectorEditElement_.IsValid())
    {
        return false;
    }
    for (const EditorProperty& property : inspectorModel_.Properties())
    {
        if (property.key == "name" && !property.readOnly)
        {
            editingKey_ = property.key;
            inspectorEdit_->SetText(property.value);
            dispatcher_.SetFocus(inspectorEditElement_);
            return true;
        }
    }
    return false;
}

void EditorShell::SelectAsset(const std::string& path)
{
    assetsModel_.Select(path);
    if (assetsPreview_.IsValid())
    {
        document_.SetText(assetsPreview_, assetsPreviewText_.empty()
            ? Text("EditorUi.Preview", "Preview") + ": " + path
            : assetsPreviewText_);
    }
    if (assetSelectionHandler_)
    {
        assetSelectionHandler_(path);
    }
}

void EditorShell::SetAssetPreviewImage(std::string image, float width, float height)
{
    assetsPreviewImage_ = std::move(image);
    assetsPreviewImageWidth_ = width;
    assetsPreviewImageHeight_ = height;
    if (!assetsPreviewImageElement_.IsValid())
    {
        return;
    }
    if (assetsPreviewImage_.empty())
    {
        document_.SetVisible(assetsPreviewImageElement_, false);
        return;
    }
    UiStyle style = document_.Style(assetsPreviewImageElement_);
    style.image = assetsPreviewImage_;
    document_.SetStyle(assetsPreviewImageElement_, style);
    document_.SetVisible(assetsPreviewImageElement_, true);
}

void EditorShell::SetAssetsPreviewText(std::string text)
{
    assetsPreviewText_ = std::move(text);
    if (assetsPreview_.IsValid())
    {
        if (!assetsPreviewText_.empty())
        {
            document_.SetText(assetsPreview_, assetsPreviewText_);
        }
        else
        {
            const std::string path = assetsModel_.SelectedPath();
            document_.SetText(assetsPreview_, Text("EditorUi.Preview", "Preview") + ": " +
                (path.empty() ? Text("EditorUi.None", "none") : path));
        }
    }
}

void EditorShell::SetPanelText(EditorPanelId panel, std::string text)
{
    PanelSlot& slot = Slot(panel);
    if (slot.content.IsValid())
    {
        document_.SetText(slot.content, std::move(text));
    }
}

void EditorShell::SetSelectionHandler(std::function<void(std::uint64_t)> handler)
{
    selectionHandler_ = std::move(handler);
}

void EditorShell::SetPropertyHandler(std::function<bool(std::uint64_t, std::string, std::string)> handler)
{
    propertyHandler_ = std::move(handler);
}

void EditorShell::SetReparentHandler(std::function<bool(std::uint64_t, std::uint64_t)> handler)
{
    reparentHandler_ = std::move(handler);
}

void EditorShell::SetAssetSelectionHandler(std::function<void(std::string)> handler)
{
    assetSelectionHandler_ = std::move(handler);
}

void EditorShell::SetAssetImportHandler(std::function<void()> handler)
{
    assetImportHandler_ = std::move(handler);
}

void EditorShell::SetViewportImage(std::string image)
{
    viewportImage_ = std::move(image);
    if (initialized_)
    {
        Frame();
    }
}

void EditorShell::SetGizmo(float x, float y, bool visible)
{
    if (!gizmo_.IsValid())
    {
        return;
    }
    document_.SetVisible(gizmo_, visible);
    for (UiElementHandle axis : gizmoAxes_)
    {
        document_.SetVisible(axis, visible);
    }
    if (!visible)
    {
        return;
    }
    UiStyle style = document_.Style(gizmo_);
    style.left = UiLength::Px(x - 6.0f);
    style.top = UiLength::Px(y - 6.0f);
    document_.SetStyle(gizmo_, style);
    const float offsets[3][2] = {{14.0f, -4.0f}, {-4.0f, 14.0f}, {14.0f, 14.0f}};
    for (std::size_t index = 0; index < gizmoAxes_.size() && index < 3; ++index)
    {
        UiStyle axisStyle = document_.Style(gizmoAxes_[index]);
        axisStyle.left = UiLength::Px(x + offsets[index][0]);
        axisStyle.top = UiLength::Px(y + offsets[index][1]);
        document_.SetStyle(gizmoAxes_[index], axisStyle);
    }
}

void EditorShell::SetGizmoDragHandler(std::function<void(int, float, float)> handler)
{
    gizmoDragHandler_ = std::move(handler);
}

void EditorShell::SetViewportState(int tool,bool local,bool center,bool sky,bool grid,bool gizmos,bool playing)
{
    const auto surface=SurfaceRoot(dock_.SurfaceFor(EditorPanelId::Viewport));
    document_.SetParent(viewportTools_,surface.IsValid()?surface:root_);
    const auto viewport=layout_.Rect(ViewportPanel());
    document_.SetVisible(viewportTools_,!playing&&document_.Visible(ViewportPanel()));
    UiStyle bar=AbsoluteStyle(viewport.x+5,viewport.y+5,std::max(0.f,viewport.width-10),27);
    bar.background=UiInk(flare::Surface,0.95f);bar.overflow=UiOverflow::Hidden;
    const std::string labels[]={"Q","W","E","R",Text(local?"EditorUi.LocalSpace":"EditorUi.WorldSpace",local?"Local":"World"),
        Text(center?"EditorUi.Center":"EditorUi.Pivot",center?"Center":"Pivot"),Text("EditorUi.ViewportSky","Sky"),
        Text("EditorUi.ViewportGrid","Grid"),Text("EditorUi.ViewportGizmos","Gizmos"),Text("EditorUi.FrameAll","Frame All")};
    const bool active[]={tool==0,tool==1,tool==2,tool==3,local,center,sky,grid,gizmos,false};
    float x=0,y=1;
    for(std::size_t i=0;i<viewportToolButtons_.size();++i)
    {
        const float width=i<4?28.f:ToolbarWidth(labels[i]);
        if(x>0&&x+width>viewport.width-10){x=0;y+=27;}
        document_.SetText(viewportToolButtons_[i],labels[i]);
        document_.SetStyle(viewportToolButtons_[i],ButtonStyle(x,y,width,25,active[i]?kSurfaceActive:kSurfaceRaised));
        x+=width+2;
    }
    bar.height=UiLength::Px(y+26);
    document_.SetStyle(viewportTools_,bar);
}

void EditorShell::AppendConsoleLine(std::string line)
{
    lines_.push_back(std::move(line));
    if (console_)
    {
        console_->Invalidate();
        console_->SetItemCount(static_cast<int>(lines_.size()));
        console_->Sync();
    }
}

void EditorShell::SetStatus(std::string status)
{
    status_ = std::move(status);
    RefreshStatus();
}

void EditorShell::SetDirty(bool dirty)
{
    dirty_ = dirty;
    RefreshStatus();
}

void EditorShell::SetSelectionLabel(std::string label)
{
    selectionLabel_ = std::move(label);
    RefreshStatus();
}

void EditorShell::RefreshStatus()
{
    if (!statusBar_.IsValid())
    {
        return;
    }
    std::string text = status_;
    if (dirty_)
    {
        text += "   [unsaved]";
    }
    if (!selectionLabel_.empty())
    {
        text += "   |   " + selectionLabel_;
    }
    if (text == lastStatusText_)
    {
        return;
    }
    lastStatusText_ = text;
    document_.SetText(statusBar_, text);
}

void EditorShell::OpenMenu(const std::string& menu)
{
    CloseMenu();
    openMenu_ = menu;
    std::size_t first = 0;
    std::size_t count = 0;
    for (std::size_t index = 0; index < chrome_.Menus().size(); ++index)
    {
        if (chrome_.Menus()[index].menu == menu)
        {
            if (count == 0)
            {
                first = index;
            }
            ++count;
        }
    }
    if (count == 0)
    {
        openMenu_.clear();
        return;
    }
    float left = 6.0f;
    for (std::size_t index = 0; index < menuNames_.size(); ++index)
    {
        const float width = MenuWidth(menuNames_[index]);
        if (menuNames_[index] == menu)
        {
            break;
        }
        left += width + 2.0f;
    }
    const float width = 240.0f;
    const float height = 8.0f + static_cast<float>(count) * 26.0f;
    menuDropdown_ = document_.CreateElement(UiElementKind::Panel);
    if (!document_.SetParent(menuDropdown_, root_))
    {
        return;
    }
    UiStyle panelStyle = AbsoluteStyle(std::min(left, std::max(0.0f, width_ - width)), menuHeight_, width, height);
    panelStyle.background = kSurface;
    panelStyle.borderWidth = 1.0f;
    panelStyle.borderColor = kBorder;
    document_.SetStyle(menuDropdown_, panelStyle);
    dropdownIndices_.clear();
    for (std::size_t item = 0; item < count; ++item)
    {
        const std::size_t menuIndex = first + item;
        const UiElementHandle button = document_.CreateElement(UiElementKind::Button);
        if (!document_.SetParent(button, menuDropdown_))
        {
            continue;
        }
        document_.SetStyle(button, ButtonStyle(4.0f, 4.0f + static_cast<float>(item) * 26.0f, width - 8.0f, 24.0f, kSurface));
        document_.SetText(button, chrome_.Menus()[menuIndex].label);
        dispatcher_.AddHandler(button, UiEventType::PointerDown, [this, menuIndex](UiInputEvent& event) {
            if (!chrome_.ActivateMenu(menuIndex))
            {
                SetStatus(Text("EditorUi.CommandUnavailable", "Command unavailable") + ": " +
                          chrome_.Menus()[menuIndex].label);
            }
            CloseMenu();
            event.handled = true;
        });
        dropdownIndices_.push_back(menuIndex);
    }
}

void EditorShell::CloseMenu()
{
    if (menuDropdown_.IsValid())
    {
        document_.SetVisible(menuDropdown_, false);
        pendingDestroy_.push_back(menuDropdown_);
        menuDropdown_ = {};
    }
    openMenu_.clear();
    dropdownIndices_.clear();
}

bool EditorShell::RequestClose()
{
    if (closeApproved_)
    {
        return false;
    }
    if (closeModalOpen_)
    {
        return true;
    }
    if (!dirty_)
    {
        closeApproved_ = true;
        return false;
    }
    ShowCloseModal();
    return true;
}

bool EditorShell::CloseApproved() const noexcept
{
    return closeApproved_;
}

bool EditorShell::CloseModalOpen() const noexcept
{
    return closeModalOpen_;
}

UiElementHandle EditorShell::CloseModalDialog() const noexcept
{
    return modalDialog_;
}

bool EditorShell::ConsumeCloseRequest() noexcept
{
    const bool requested = closeRequested_;
    closeRequested_ = false;
    return requested;
}

void EditorShell::ShowCloseModal()
{
    if (closeModalOpen_ || !root_.IsValid())
    {
        return;
    }
    modalBackdrop_ = document_.CreateElement(UiElementKind::Panel);
    if (!document_.SetParent(modalBackdrop_, root_))
    {
        return;
    }
    UiStyle backdrop = AbsoluteStyle(0.0f, 0.0f, width_, height_);
    backdrop.width = UiLength::Percent(100.0f);
    backdrop.height = UiLength::Percent(100.0f);
    backdrop.background = UiColor{0.0f, 0.0f, 0.0f, 0.55f};
    document_.SetStyle(modalBackdrop_, backdrop);
    dispatcher_.AddHandler(modalBackdrop_, UiEventType::PointerDown, [](UiInputEvent& event) {
        event.handled = true;
        event.propagationStopped = true;
    });

    const float dialogWidth = 440.0f;
    const float dialogHeight = 196.0f;
    const float left = std::max(20.0f, (width_ - dialogWidth) * 0.5f);
    const float top = std::max(60.0f, (height_ - dialogHeight) * 0.5f);
    modalDialog_ = document_.CreateElement(UiElementKind::Panel);
    if (!document_.SetParent(modalDialog_, modalBackdrop_))
    {
        return;
    }
    UiStyle dialog = AbsoluteStyle(left, top, dialogWidth, dialogHeight);
    dialog.background = kSurface;
    dialog.borderWidth = 2.0f;
    dialog.borderColor = kAccent;
    document_.SetStyle(modalDialog_, dialog);

    const UiElementHandle title = document_.CreateElement(UiElementKind::Label);
    if (document_.SetParent(title, modalDialog_))
    {
        document_.SetStyle(title, ButtonStyle(20.0f, 20.0f, dialogWidth - 40.0f, 24.0f, kSurface));
        document_.SetText(title, Text("EditorUi.CloseUnsavedTitle", "Unsaved changes"));
    }
    const UiElementHandle message = document_.CreateElement(UiElementKind::Label);
    if (document_.SetParent(message, modalDialog_))
    {
        UiStyle style = ButtonStyle(20.0f, 56.0f, dialogWidth - 40.0f, 48.0f, kSurface);
        style.color = kMuted;
        document_.SetStyle(message, style);
        document_.SetText(message, Text("EditorUi.CloseUnsavedMessage",
                                        "The current document has unsaved changes. Save before closing?"));
    }

    const auto makeButton = [this, dialogWidth](float x, const char* label, const UiColor& color) {
        const UiElementHandle button = document_.CreateElement(UiElementKind::Button);
        if (!document_.SetParent(button, modalDialog_))
        {
            return UiElementHandle{};
        }
        document_.SetStyle(button, ButtonStyle(x, 144.0f, 120.0f, 30.0f, color));
        document_.SetText(button, label);
        return button;
    };
    modalSave_ = makeButton(20.0f, Text("EditorUi.Save", "Save").c_str(), kSurfaceActive);
    modalDiscard_ = makeButton(154.0f, Text("EditorUi.DiscardChanges", "Discard").c_str(),
                               UiColor{0.55f, 0.16f, 0.18f, 1.0f});
    modalCancel_ = makeButton(288.0f, Text("EditorUi.Cancel", "Cancel").c_str(), kSurfaceRaised);
    (void)dialogWidth;

    if (modalSave_.IsValid())
    {
        dispatcher_.AddHandler(modalSave_, UiEventType::PointerDown, [this](UiInputEvent& event) {
            if (commands_.Execute(EditorCommand{EditorCommandId::Save, {}}))
            {
                closeApproved_ = true;
                closeRequested_ = true;
                HideCloseModal();
            }
            else
            {
                SetStatus(Text("EditorUi.SaveFailed", "Save failed") + "; close cancelled");
            }
            event.handled = true;
        });
    }
    if (modalDiscard_.IsValid())
    {
        dispatcher_.AddHandler(modalDiscard_, UiEventType::PointerDown, [this](UiInputEvent& event) {
            closeApproved_ = true;
            closeRequested_ = true;
            HideCloseModal();
            event.handled = true;
        });
    }
    if (modalCancel_.IsValid())
    {
        dispatcher_.AddHandler(modalCancel_, UiEventType::PointerDown, [this](UiInputEvent& event) {
            HideCloseModal();
            event.handled = true;
        });
        dispatcher_.SetFocus(modalCancel_);
    }
    closeModalOpen_ = true;
}

void EditorShell::ShowConfirm(std::string title, std::string message, std::string primaryLabel,
    std::string secondaryLabel, std::string cancelLabel, std::function<void()> onPrimary,
    std::function<void()> onSecondary)
{
    if (confirmOpen_ || !root_.IsValid())
    {
        return;
    }
    confirmPrimaryAction_ = std::move(onPrimary);
    confirmSecondaryAction_ = std::move(onSecondary);
    confirmBackdrop_ = document_.CreateElement(UiElementKind::Panel);
    if (!document_.SetParent(confirmBackdrop_, root_))
    {
        return;
    }
    UiStyle backdrop = AbsoluteStyle(0.0f, 0.0f, width_, height_);
    backdrop.width = UiLength::Percent(100.0f);
    backdrop.height = UiLength::Percent(100.0f);
    backdrop.background = UiColor{0.0f, 0.0f, 0.0f, 0.55f};
    document_.SetStyle(confirmBackdrop_, backdrop);
    dispatcher_.AddHandler(confirmBackdrop_, UiEventType::PointerDown, [](UiInputEvent& event) {
        event.handled = true;
        event.propagationStopped = true;
    });

    const float dialogWidth = 440.0f;
    const float dialogHeight = 196.0f;
    const float left = std::max(20.0f, (width_ - dialogWidth) * 0.5f);
    const float top = std::max(60.0f, (height_ - dialogHeight) * 0.5f);
    confirmDialog_ = document_.CreateElement(UiElementKind::Panel);
    if (!document_.SetParent(confirmDialog_, confirmBackdrop_))
    {
        return;
    }
    UiStyle dialog = AbsoluteStyle(left, top, dialogWidth, dialogHeight);
    dialog.background = kSurface;
    dialog.borderWidth = 2.0f;
    dialog.borderColor = kAccent;
    document_.SetStyle(confirmDialog_, dialog);

    const UiElementHandle titleLabel = document_.CreateElement(UiElementKind::Label);
    if (document_.SetParent(titleLabel, confirmDialog_))
    {
        document_.SetStyle(titleLabel, ButtonStyle(20.0f, 20.0f, dialogWidth - 40.0f, 24.0f, kSurface));
        document_.SetText(titleLabel, title);
    }
    const UiElementHandle messageLabel = document_.CreateElement(UiElementKind::Label);
    if (document_.SetParent(messageLabel, confirmDialog_))
    {
        UiStyle style = ButtonStyle(20.0f, 56.0f, dialogWidth - 40.0f, 48.0f, kSurface);
        style.color = kMuted;
        document_.SetStyle(messageLabel, style);
        document_.SetText(messageLabel, message);
    }

    const auto makeButton = [this, dialogWidth](float x, const std::string& label, const UiColor& color) {
        const UiElementHandle button = document_.CreateElement(UiElementKind::Button);
        if (!document_.SetParent(button, confirmDialog_))
        {
            return UiElementHandle{};
        }
        document_.SetStyle(button, ButtonStyle(x, 144.0f, 120.0f, 30.0f, color));
        document_.SetText(button, label);
        return button;
    };
    confirmPrimary_ = makeButton(20.0f, primaryLabel, kSurfaceActive);
    confirmSecondary_ = makeButton(154.0f, secondaryLabel, UiColor{0.55f, 0.16f, 0.18f, 1.0f});
    confirmCancel_ = makeButton(288.0f, cancelLabel, kSurfaceRaised);
    (void)dialogWidth;
    if (secondaryLabel.empty())
    {
        document_.SetVisible(confirmSecondary_, false);
    }

    if (confirmPrimary_.IsValid())
    {
        dispatcher_.AddHandler(confirmPrimary_, UiEventType::PointerDown, [this](UiInputEvent& event) {
            auto action = confirmPrimaryAction_;
            HideConfirm();
            if (action) action();
            event.handled = true;
        });
    }
    if (confirmSecondary_.IsValid() && !secondaryLabel.empty())
    {
        dispatcher_.AddHandler(confirmSecondary_, UiEventType::PointerDown, [this](UiInputEvent& event) {
            auto action = confirmSecondaryAction_;
            HideConfirm();
            if (action) action();
            event.handled = true;
        });
    }
    if (confirmCancel_.IsValid())
    {
        dispatcher_.AddHandler(confirmCancel_, UiEventType::PointerDown, [this](UiInputEvent& event) {
            HideConfirm();
            event.handled = true;
        });
        dispatcher_.SetFocus(confirmCancel_);
    }
    confirmOpen_ = true;
}

void EditorShell::HideConfirm()
{
    if (confirmBackdrop_.IsValid())
    {
        document_.SetVisible(confirmBackdrop_, false);
        pendingDestroy_.push_back(confirmBackdrop_);
    }
    confirmBackdrop_ = {};
    confirmDialog_ = {};
    confirmPrimary_ = {};
    confirmSecondary_ = {};
    confirmCancel_ = {};
    confirmPrimaryAction_ = {};
    confirmSecondaryAction_ = {};
    confirmOpen_ = false;
    dispatcher_.ClearFocus();
}

void EditorShell::HideCloseModal()
{
    if (modalBackdrop_.IsValid())
    {
        document_.SetVisible(modalBackdrop_, false);
        pendingDestroy_.push_back(modalBackdrop_);
    }
    modalBackdrop_ = {};
    modalDialog_ = {};
    modalSave_ = {};
    modalDiscard_ = {};
    modalCancel_ = {};
    closeModalOpen_ = false;
    dispatcher_.ClearFocus();
}

void EditorShell::SelectPanel(EditorPanelId panel, bool save)
{
    activePanel_ = panel;
    dock_.SelectTab(panel);
    if (initialized_)
    {
        UpdateDockRects();
        UpdatePanelContentRects();
    }
    if (save)
    {
        SaveLayoutIfConfigured();
    }
}

void EditorShell::TogglePanel(EditorPanelId panel)
{
    if (panel == EditorPanelId::None)
    {
        return;
    }
    if (dock_.Find(panel) != nullptr)
    {
        dock_.CloseTab(panel);
        if (activePanel_ == panel)
        {
            activePanel_ = EditorPanelId::Hierarchy;
        }
    }
    else
    {
        EditorPanelId nearPanel = Slot(panel).openNear;
        if (panel == EditorPanelId::Preferences || panel == EditorPanelId::About ||
            panel == EditorPanelId::Diagnostics)
        {
            nearPanel = EditorPanelId::Viewport;
        }
        dock_.OpenTabNear(panel, nearPanel);
        activePanel_ = panel;
    }
    MarkDockDirty(true, true);
}

void EditorShell::FocusPanel(EditorPanelId panel)
{
    if (panel == EditorPanelId::None)
    {
        return;
    }
    bool added = false;
    if (dock_.Find(panel) == nullptr)
    {
        EditorPanelId nearPanel = Slot(panel).openNear;
        if (panel == EditorPanelId::Preferences || panel == EditorPanelId::About ||
            panel == EditorPanelId::Diagnostics)
        {
            nearPanel = EditorPanelId::Viewport;
        }
        added = dock_.OpenTabNear(panel, nearPanel);
    }
    SelectPanel(panel, true);
    if (added)
    {
        MarkDockDirty(true, false);
    }
}

bool EditorShell::TextFieldFocused() const noexcept
{
    const UiElement* element = document_.Element(dispatcher_.Focus());
    return element != nullptr && element->Kind() == UiElementKind::TextEdit;
}

bool EditorShell::RouteShortcut(const UiInputEvent& event, bool textFieldFocused)
{
    return router_.RouteKey(event, textFieldFocused);
}

void EditorShell::MarkDockDirty(bool structural, bool save)
{
    if (structural)
    {
        ++dockRevision_;
    }
    if (save)
    {
        SaveLayoutIfConfigured();
    }
}

void EditorShell::SetLayoutPath(std::filesystem::path path)
{
    layoutPath_ = std::move(path);
    if (initialized_ && !layoutPath_.empty())
    {
        LoadLayout();
        Frame();
    }
}

bool EditorShell::LoadLayout()
{
    if (layoutPath_.empty())
    {
        return false;
    }
    EditorDock loaded;
    std::string error;
    if (EditorDockState::Load(layoutPath_, loaded, error))
    {
        dock_ = std::move(loaded);
        MarkDockDirty(true, false);
        SetStatus(Text("EditorUi.LayoutLoaded", "Layout loaded"));
        return true;
    }
    if (error != "dock layout not found")
    {
        SetStatus(Text("EditorUi.LayoutFallback", "Layout fallback") + ": " + error);
    }
    MarkDockDirty(true, false);
    return false;
}

bool EditorShell::SaveLayout()
{
    if (layoutPath_.empty())
    {
        return false;
    }
    std::string error;
    if (!EditorDockState::Save(dock_, layoutPath_, error))
    {
        SetStatus(Text("EditorUi.LayoutSaveFailed", "Layout save failed") + ": " + error);
        return false;
    }
    return true;
}

void EditorShell::SaveLayoutIfConfigured()
{
    if (!layoutPath_.empty())
    {
        SaveLayout();
    }
}

std::uint32_t EditorShell::FloatPanel(EditorPanelId panel,int x,int y,int width,int height)
{
    const auto id=dock_.FloatTab(panel,x,y,width,height);
    if(id)MarkDockDirty(true,true);
    return id;
}
void EditorShell::CloseFloatingWindow(std::uint32_t id)
{if(dock_.CloseWindow(id))MarkDockDirty(true,true);}
void EditorShell::SetFloatingBounds(std::uint32_t id,int x,int y,int width,int height)
{if(dock_.SetWindowBounds(id,x,y,width,height))SaveLayoutIfConfigured();}
void EditorShell::SetSurfaceSize(std::uint32_t id,float width,float height)
{
    for(auto& surface:surfaces_)if(surface.id==id)
    {
        surface.width=std::max(160.0f,width);surface.height=std::max(120.0f,height);
        auto style=document_.Style(surface.root);style.width=UiLength::Px(surface.width);style.height=UiLength::Px(surface.height);
        document_.SetStyle(surface.root,style);return;
    }
}
void EditorShell::SetInputSurface(std::uint32_t id)
{inputSurface_=id;dispatcher_.SetInputRoot(SurfaceRoot(id));}
UiElementHandle EditorShell::SurfaceRoot(std::uint32_t id) const
{
    if(!id)return root_;
    for(const auto& surface:surfaces_)if(surface.id==id)return surface.root;
    return {};
}
std::uint32_t EditorShell::SurfaceForElement(UiElementHandle element)const
{
    while(const auto* node=document_.Element(element))
    {
        if(element==root_)return 0;
        for(const auto& surface:surfaces_)if(element==surface.root)return surface.id;
        element=node->Parent();
    }
    return 0;
}
UiRect EditorShell::SurfaceDockRect(std::uint32_t id) const
{
    if(!id)return dockRect_;
    for(const auto& surface:surfaces_)if(surface.id==id)return {0,0,surface.width,surface.height};
    return {};
}
std::string EditorShell::WindowLabel(std::uint32_t id) const
{
    const auto* tree=dock_.Surface(id);if(!tree)return {};
    while(tree->kind==DockNode::Kind::Split&&!tree->children.empty())tree=&tree->children.front();
    return PanelLabel(tree->active);
}

void EditorShell::HandleTabDown(EditorPanelId panel, UiInputEvent& event)
{
    if(event.button!=0)return;
    activePanel_ = panel;
    dragTabPanel_ = panel;
    tabDragging_ = false;
    tabDragStartX_ = event.x;
    tabDragStartY_ = event.y;
    if (dock_.SelectTab(panel))
    {
        UpdateDockRects();
        UpdatePanelContentRects();
    }
    event.handled = true;
    event.StopPropagation();
}

void EditorShell::HandleTabMove(UiInputEvent& event)
{
    if (dragTabPanel_ == EditorPanelId::None)
    {
        return;
    }
    event.handled=true;
    event.StopPropagation();
    const float dx=event.x-tabDragStartX_,dy=event.y-tabDragStartY_;
    if (!tabDragging_ && dx*dx+dy*dy>36.0f)
    {
        tabDragging_ = true;
        if (dragGhost_.IsValid())
        {
            document_.SetVisible(dragGhost_, true);
            document_.SetText(dragGhost_, PanelLabel(dragTabPanel_));
        }
    }
    if (!tabDragging_)
    {
        return;
    }
    dropTarget_=EditorPanelId::None;
    dropZone_=0;
    dropBefore_=false;
    for (std::size_t index = 0; index < groups_.size(); ++index)
    {
        const auto& group=groups_[index];
        if(group.surface!=inputSurface_)continue;
        const UiRect rect = group.rect;
        if (event.x < rect.x || event.x >= rect.x + rect.width ||
            event.y < rect.y || event.y >= rect.y + rect.height)
        {
            continue;
        }
        const bool onTabs=event.y<rect.y+kTabHeight;
        if(group.tabs.empty()&&!inputSurface_){dropTarget_=dragTabPanel_;break;}
        const bool own=std::find(group.tabs.begin(),group.tabs.end(),dragTabPanel_)!=group.tabs.end();
        if(own && group.tabs.size()==1)break;
        if(onTabs)
        {
            for(size_t tab=0;tab<group.tabs.size();++tab)
            {
                if(group.tabs[tab]==dragTabPanel_)continue;
                dropTarget_=group.tabs[tab];
                const auto button=layout_.Rect(group.tabButtons[tab]);
                dropBefore_=event.x<button.x+button.width*0.5f;
                if(dropBefore_)break;
            }
        }
        else
        {
            // 上侧以内容区为起点，避免被 24 DIP 的页签栏吞掉。
            const float contentTop=rect.y+kTabHeight;
            const float horizontalBand=std::clamp(rect.width*0.22f,24.0f,64.0f);
            const float verticalBand=std::clamp((rect.height-kTabHeight)*0.22f,24.0f,64.0f);
            const float edges[]={event.x-rect.x,rect.x+rect.width-event.x,
                                 event.y-contentTop,rect.y+rect.height-event.y};
            const float bands[]={horizontalBand,horizontalBand,verticalBand,verticalBand};
            float nearest=1.0f;
            for(int edge=0;edge<4;++edge)
                if(edges[edge]/bands[edge]<nearest)
                {
                    nearest=edges[edge]/bands[edge];
                    dropZone_=edge+1;
                }
            if(own && dropZone_==0)break;
            if((dropZone_==1 || dropZone_==2) && rect.width<2*kMinimumGroupSize)break;
            if((dropZone_==3 || dropZone_==4) && rect.height<2*kMinimumGroupSize)break;
            for(const auto panel:group.tabs)
                if(panel!=dragTabPanel_)dropTarget_=panel;
        }
        break;
    }

    EditorDock preview=dock_;
    const bool valid=ApplyTabDrop(preview) && preview!=dock_;
    if(!valid)dropTarget_=EditorPanelId::None;
    const auto targetRoot=SurfaceRoot(inputSurface_);
    if(targetRoot.IsValid()){document_.SetParent(dropHighlight_,targetRoot);document_.SetParent(tabInsertMarker_,targetRoot);document_.SetParent(dragGhost_,targetRoot);}
    document_.SetVisible(dragGhost_,targetRoot.IsValid());
    document_.SetVisible(dropHighlight_,valid);
    document_.SetVisible(tabInsertMarker_,valid && dropZone_==0);
    if(valid)
    {
        // 和提交使用同一树变换，再按真实分隔比例计算落点，覆盖空组折叠后的面积。
        const UiRect rect=DockPanelRect(*preview.Surface(inputSurface_),dragTabPanel_,SurfaceDockRect(inputSurface_));
        UiStyle style=document_.Style(dropHighlight_);
        style.left=UiLength::Px(rect.x);style.top=UiLength::Px(rect.y);
        style.width=UiLength::Px(rect.width);style.height=UiLength::Px(rect.height);
        document_.SetStyle(dropHighlight_,style);
        if(dropZone_==0)
        {
            float x=rect.x;
            for(const auto panel:preview.Find(dragTabPanel_)->tabs)
            {
                if(panel==dragTabPanel_)break;
                x+=TabWidth(PanelLabel(panel))+2.0f;
            }
            UiStyle marker=document_.Style(tabInsertMarker_);
            marker.left=UiLength::Px(x);marker.top=UiLength::Px(rect.y+2);
            marker.height=UiLength::Px(kTabHeight-4);
            document_.SetStyle(tabInsertMarker_,marker);
        }
    }
    if(dragGhost_.IsValid())
    {
        const char* keys[]={"EditorUi.DockMerge","EditorUi.DockLeft","EditorUi.DockRight","EditorUi.DockTop","EditorUi.DockBottom"};
        const char* labels[]={"Merge as tab","Dock left","Dock right","Dock above","Dock below"};
        const std::string label=PanelLabel(dragTabPanel_)+(valid?" · "+Text(keys[dropZone_],labels[dropZone_]):"");
        document_.SetText(dragGhost_,label);
        UiStyle style=document_.Style(dragGhost_);
        const auto area=SurfaceDockRect(inputSurface_);
        const float surfaceWidth=std::max(1.0f,area.width),surfaceHeight=std::max(24.0f,area.y+area.height);
        const float width=std::min(surfaceWidth,std::max(220.0f,MeasuredWidth(label,13.0f)+16.0f));
        style.width=UiLength::Px(width);
        style.left=UiLength::Px(std::clamp(event.x+10,0.0f,surfaceWidth-width));
        style.top=UiLength::Px(std::clamp(event.y+8,0.0f,surfaceHeight-24.0f));
        document_.SetStyle(dragGhost_,style);
    }
}

void EditorShell::HandleTabUp(UiInputEvent& event)
{
    if(dragTabPanel_==EditorPanelId::None || event.button!=0)return;
    if (tabDragging_)
    {
        HandleTabMove(event);
        if(inputSurface_==UINT32_MAX&&floatRequest_)floatRequest_(dragTabPanel_);
        else ApplyTabDrop();
    }
    event.handled=true;
    event.StopPropagation();
    CancelTabDrag();
}

void EditorShell::CancelTabDrag()
{
    const bool tabPressed=dragTabPanel_!=EditorPanelId::None;
    dragTabPanel_ = EditorPanelId::None;
    tabDragging_ = false;
    dropTarget_=EditorPanelId::None;
    document_.SetVisible(tabInsertMarker_,false);
    if (dragGhost_.IsValid())
    {
        document_.SetVisible(dragGhost_, false);
    }
    if (dropHighlight_.IsValid())
    {
        document_.SetVisible(dropHighlight_, false);
    }
    if(tabPressed && dispatcher_.PointerCapture().IsValid())dispatcher_.ReleasePointerCapture();
}

bool EditorShell::ApplyTabDrop(EditorDock& dock) const
{
    if(dragTabPanel_==EditorPanelId::None || dropTarget_==EditorPanelId::None)return false;
    if(!inputSurface_&&dock.Root().kind==DockNode::Kind::Tabs&&dock.Root().tabs.empty())
    {return dock.CloseTab(dragTabPanel_)&&dock.OpenTab(dragTabPanel_);}
    if(dropZone_==0)return dock.MoveTab(dragTabPanel_,dropTarget_,dropBefore_);
    const auto axis=dropZone_<=2?DockAxis::Horizontal:DockAxis::Vertical;
    return dock.SplitTab(dragTabPanel_,dropTarget_,axis,dropZone_==1 || dropZone_==3);
}

void EditorShell::ApplyTabDrop()
{
    EditorDock next=dock_;
    if(ApplyTabDrop(next) && next!=dock_)
    {
        dock_=std::move(next);
        activePanel_ = dragTabPanel_;
        MarkDockDirty(true, true);
    }
}

void EditorShell::BeginSplitter(float x, float y)
{
    for (const SplitHandle& handle : splitHandles_)
    {
        if(handle.surface!=inputSurface_)continue;
        if (x < handle.rect.x || x >= handle.rect.x + handle.rect.width ||
            y < handle.rect.y || y >= handle.rect.y + handle.rect.height)
        {
            continue;
        }
        splitterActive_ = 1;
        splitterSurface_=handle.surface;
        splitterPath_ = handle.path;
        splitterAxis_ = handle.axis;
        splitterStartRatio_ = handle.ratio;
        splitterStartCoordinate_ = handle.axis == DockAxis::Horizontal ? x : y;
        splitterTotal_ = std::max(1.0f, handle.total);
        return;
    }
}

void EditorShell::DragSplitter(float x, float y)
{
    if (splitterActive_ == 0)
    {
        return;
    }
    const float coordinate = splitterAxis_ == DockAxis::Horizontal ? x : y;
    const float minimum = std::min(0.45f, kMinimumGroupSize / splitterTotal_);
    float ratio = splitterStartRatio_ + (coordinate - splitterStartCoordinate_) / splitterTotal_;
    ratio = std::clamp(ratio, minimum, 1.0f - minimum);
    if (dock_.SetSplitRatio(splitterSurface_,splitterPath_, ratio) && initialized_)
    {
        UpdateDockRects();
        UpdatePanelContentRects();
    }
}

void EditorShell::EndSplitter()
{
    splitterActive_ = 0;
    splitterPath_.clear();
    SaveLayoutIfConfigured();
}

UiDocument& EditorShell::Document() noexcept
{
    return document_;
}

UiLayoutEngine& EditorShell::Layout() noexcept
{
    return layout_;
}

const UiLayoutEngine& EditorShell::Layout() const noexcept
{
    return layout_;
}

UiDispatcher& EditorShell::Dispatcher() noexcept
{
    return dispatcher_;
}

UiTextService& EditorShell::TextService() noexcept
{
    return *text_;
}

UiListView& EditorShell::ConsoleList() noexcept
{
    return *console_;
}

UiListView& EditorShell::HierarchyList() noexcept
{
    return *hierarchyList_;
}

UiListView& EditorShell::AssetsList() noexcept
{
    return *assetsList_;
}

UiListView& EditorShell::InspectorList() noexcept
{
    return *inspectorList_;
}

EditorDock& EditorShell::Dock() noexcept
{
    return dock_;
}

EditorChrome& EditorShell::Chrome() noexcept
{
    return chrome_;
}

EditorCommandBus& EditorShell::Commands() noexcept
{
    return commands_;
}

EditorCommandRouter& EditorShell::Router() noexcept
{
    return router_;
}

} // namespace se::ui::editor

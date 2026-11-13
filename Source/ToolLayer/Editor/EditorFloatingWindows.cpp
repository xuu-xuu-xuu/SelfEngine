#include "EditorFloatingWindows.h"
#include "EditorWindowStyle.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "PlatformLayer/Windows/AppBranding.h"
#include "CoreLayer/Utf.h"
#include "CoreLayer/Log.h"
#include "CoreLayer/Localization.h"
#include "PlatformLayer/Windows/WindowsInput.h"
#include <algorithm>
#include <windowsx.h>

namespace se
{
using namespace ui::editor;
namespace
{
constexpr wchar_t WindowClass[]=L"FlareFloatingPanel";
std::wstring Wide(const std::string& value)
{
    const int count=MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0);
    std::wstring result(count,L'\0');
    MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),count);return result;
}
RECT VisibleBounds(RECT rect)
{
    MONITORINFO monitor{sizeof(monitor)};
    if(GetMonitorInfoW(MonitorFromRect(&rect,MONITOR_DEFAULTTONEAREST),&monitor))
    {
        const int width=std::min(rect.right-rect.left,monitor.rcWork.right-monitor.rcWork.left);
        const int height=std::min(rect.bottom-rect.top,monitor.rcWork.bottom-monitor.rcWork.top);
        rect.left=std::clamp(rect.left,monitor.rcWork.left,monitor.rcWork.right-width);
        rect.top=std::clamp(rect.top,monitor.rcWork.top,monitor.rcWork.bottom-height);
        rect.right=rect.left+width;rect.bottom=rect.top+height;
    }
    return rect;
}
}
struct EditorFloatingWindows::Entry
{
    EditorFloatingWindows* host=nullptr;
    std::uint32_t id=0;
    HWND window=nullptr;
    bool scene=false;
    EditorInputAdapter input;
    InputState rawInput;
    UiDirectWriteTextService text{L"Segoe UI"};
    std::unique_ptr<SandboxRenderer> renderer;
    std::vector<unsigned char> font;
    std::uint64_t frame=0;
};
EditorFloatingWindows::EditorFloatingWindows(HWND owner,EditorShell& shell,const Localization& text,
    std::filesystem::path resources,MessageHandler handler,bool visible)
    :owner_(owner),shell_(shell),text_(text),resources_(std::move(resources)),handler_(std::move(handler)),visible_(visible)
{
    WNDCLASSEXW type{sizeof(type)};type.lpfnWndProc=WindowProc;type.hInstance=GetModuleHandleW(nullptr);
    type.lpszClassName=WindowClass;type.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));type.hIcon=AppIcon(32);type.hIconSm=AppIcon(16);
    RegisterClassExW(&type);
    shell_.SetFloatRequestHandler([this](auto panel){RequestFloat(panel);});
}
EditorFloatingWindows::~EditorFloatingWindows()
{
    shuttingDown_=true;shell_.SetFloatRequestHandler({});
    for(auto& entry:entries_)Release(*entry);
}
void EditorFloatingWindows::Release(Entry& entry)noexcept
{
    if(entry.renderer){entry.renderer->Shutdown();entry.renderer.reset();}
    entry.input.SetWindow(nullptr);
    if(entry.window){DestroyWindow(entry.window);entry.window=nullptr;}
}
bool EditorFloatingWindows::Synchronize()
{
    std::erase_if(entries_,[this](auto& e){if(shell_.Dock().Surface(e->id))return false;Release(*e);return true;});
    const auto windows=shell_.Dock().Windows(); // CreateWindow 可重入消息，不能借用可变容器。
    for(const auto& saved:windows)
    {
        Entry* entry=nullptr;
        for(auto& e:entries_)if(e->id==saved.id)entry=e.get();
        if(!entry)
        {
            auto next=std::make_unique<Entry>();next->host=this;next->id=saved.id;
            RECT bounds{saved.x,saved.y,saved.x+saved.width,saved.y+saved.height};
            if(!GetEnvironmentVariableW(L"SELFENGINE_TEST_OFFSCREEN",nullptr,0))bounds=VisibleBounds(bounds);
            const auto title=Wide(shell_.WindowLabel(saved.id)+" — "+text_.GetText("Editor.WindowTitle"));
            entry=next.get();
            entry->window=CreateWindowExW(WS_EX_TOOLWINDOW,WindowClass,title.c_str(),WS_OVERLAPPEDWINDOW,
                bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,owner_,nullptr,GetModuleHandleW(nullptr),entry);
            if(!entry->window)return false;
            entry->input.SetWindow(entry->window);ApplyAppIcon(entry->window);flare::StyleTitleBar(entry->window);
            entries_.push_back(std::move(next));
            if(visible_)ShowWindow(entry->window,SW_SHOWNOACTIVATE);
        }
        const float dpi=GetDpiForWindow(entry->window)/96.0f;
        entry->input.SetDpiScale(dpi);entry->text.SetRasterScale(dpi);
        RECT area{};GetClientRect(entry->window,&area);
        shell_.SetSurfaceSize(saved.id,area.right/dpi,area.bottom/dpi);
        const bool scene=shell_.Dock().SurfaceFor(EditorPanelId::Viewport)==saved.id;
        if(!entry->renderer||entry->scene!=scene)
        {
            if(entry->renderer)entry->renderer->Shutdown();
            entry->scene=scene;
            entry->renderer=std::make_unique<SandboxRenderer>(scene?SandboxScene::BasicLit:SandboxScene::Clear,resources_);
            if(!entry->renderer->Initialize({entry->window,static_cast<unsigned>(std::max(1L,area.right)),static_cast<unsigned>(std::max(1L,area.bottom))},text_))return false;
            Log(LogLevel::Info,"EditorUi.FloatingReady: "+text_.GetText("EditorUi.FloatingReady")+" "+std::to_string(entry->id));
        }
        const auto title=Wide(shell_.WindowLabel(saved.id)+" — "+text_.GetText("Editor.WindowTitle"));
        SetWindowTextW(entry->window,title.c_str());
        const bool enabled=!shell_.CloseModalOpen()&&!shell_.ConfirmOpen();
        if((IsWindowEnabled(entry->window)!=FALSE)!=enabled)
        {
            if(!enabled&&GetForegroundWindow()==entry->window)SetForegroundWindow(owner_);
            EnableWindow(entry->window,enabled);
        }
    }
    shell_.Frame();return true;
}
bool EditorFloatingWindows::Render(const FrameData& base,ui::UiImageProvider& images,const ui::UiDisplayList& overlay)
{
    for(auto& entry:entries_)
    {
        if(IsIconic(entry->window)||!entry->renderer)continue;
        RECT area{};GetClientRect(entry->window,&area);if(area.right<1||area.bottom<1)continue;
        const float dpi=entry->input.DpiScale();
        ui::UiDisplayList display;ui::UiPaintStats stats;
        if(!ui::UiPainter(shell_.Document(),shell_.Layout()).Build(shell_.SurfaceRoot(entry->id),display,stats))return false;
        if(entry->scene)
            for(auto it=display.commands.begin();it!=display.commands.end();++it)
                if(it->kind==ui::UiPaintCommandKind::Image&&it->image=="scene")
                {display.commands.insert(it+1,overlay.commands.begin(),overlay.commands.end());break;}
        FrameData data=base;ui::UiGeometryStats geometryStats;std::vector<std::string> errors;
        ui::UiGeometryOptions options;options.whiteTexture=3;options.fontPageBase=1;options.textPixelScale={dpi,dpi};
        if(!ui::UiGeometryBuilder{}.Build(display,&entry->text,&images,options,data.editorGui,geometryStats,errors))return false;
        int width=0,height=0;std::uint64_t revision=0;
        if(!entry->text.FontAtlas(entry->font,width,height,revision))return false;
        data.editorFontPixels=entry->font.data();data.editorFontWidth=width;data.editorFontHeight=height;data.editorFontRevision=revision;
        data.editorGui.framebufferScale={dpi,dpi};
        if(!entry->scene)data.sceneViewport={0,0,1,1};
        const RenderFrame frame{{entry->window,static_cast<unsigned>(area.right),static_cast<unsigned>(area.bottom)},0,0,GetTickCount64()/1000.0,++entry->frame,&data};
        if(entry->renderer->Render(frame,text_)==RenderResult::Failed)return false;
    }
    for(auto& entry:entries_)entry->rawInput.ClearEvents();
    return true;
}
bool EditorFloatingWindows::Owns(HWND window)const
{return window&&(window==owner_||std::any_of(entries_.begin(),entries_.end(),[window](const auto& e){return e->window==window;}));}
bool EditorFloatingWindows::Empty()const{return entries_.empty();}
EditorInputAdapter* EditorFloatingWindows::Input(HWND window)const
{for(const auto& e:entries_)if(e->window==window)return &e->input;return nullptr;}
HWND EditorFloatingWindows::Window(std::uint32_t id)const
{if(!id)return owner_;for(const auto& e:entries_)if(e->id==id)return e->window;return nullptr;}
const InputState* EditorFloatingWindows::GameplayInput(float& dpi)const
{
    const auto id=shell_.Dock().SurfaceFor(EditorPanelId::Viewport);
    for(const auto& e:entries_)if(e->id==id){dpi=e->input.DpiScale();return &e->rawInput;}
    return nullptr;
}
void EditorFloatingWindows::SelectSurface(HWND window)
{
    if(window==owner_){shell_.SetInputSurface(0);return;}
    for(const auto& e:entries_)if(e->window==window){shell_.SetInputSurface(e->id);return;}
    shell_.SetInputSurface(UINT32_MAX);
}
HWND EditorFloatingWindows::RoutePointer(HWND source,UINT message,LPARAM& position)
{
    if(message!=WM_MOUSEMOVE&&message!=WM_LBUTTONUP&&message!=WM_LBUTTONDOWN)return source;
    POINT screen{GET_X_LPARAM(position),GET_Y_LPARAM(position)};ClientToScreen(source,&screen);dropPoint_=screen;
    if(shell_.DraggingPanel()==EditorPanelId::None)return source;
    const auto contains=[screen](HWND window){
        if(!window||IsIconic(window))return false;
        RECT area{};GetClientRect(window,&area);POINT p=screen;ScreenToClient(window,&p);return PtInRect(&area,p)!=FALSE;
    };
    HWND target=GetAncestor(WindowFromPoint(screen),GA_ROOT);
    if(!Owns(target)||!contains(target))target=nullptr;
    if(!target&&GetEnvironmentVariableW(L"SELFENGINE_TEST_OFFSCREEN",nullptr,0))
    {
        for(auto it=entries_.rbegin();it!=entries_.rend();++it)if(contains((*it)->window)){target=(*it)->window;break;}
        if(!target&&contains(owner_))target=owner_;
    }
    if(target){POINT p=screen;ScreenToClient(target,&p);position=MAKELPARAM(p.x,p.y);}
    return target;
}
void EditorFloatingWindows::RequestFloat(EditorPanelId panel)
{
    shell_.FloatPanel(panel,dropPoint_.x-32,dropPoint_.y-12);
}
void EditorFloatingWindows::SetCaptureOutput(std::uint32_t id,const std::filesystem::path& path,std::uint64_t frame)
{for(auto& e:entries_)if(e->id==id&&e->renderer)e->renderer->SetCaptureOutput(path,frame);}
void EditorFloatingWindows::CaptureNext(std::uint32_t id,const std::filesystem::path& path)
{for(auto& e:entries_)if(e->id==id&&e->renderer)e->renderer->SetCaptureOutput(path,e->frame+1);}
LRESULT CALLBACK EditorFloatingWindows::WindowProc(HWND window,UINT message,WPARAM w,LPARAM l)
{
    auto* entry=reinterpret_cast<Entry*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE)
    {
        entry=static_cast<Entry*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);entry->window=window;
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(entry));
    }
    if(entry&&entry->host)
    {
        try{if(auto result=entry->host->Message(*entry,message,w,l))return *result;}
        catch(const std::exception& e){Log(LogLevel::Error,e.what());PostMessageW(entry->host->owner_,WM_CLOSE,0,0);return 0;}
    }
    if(message==WM_NCDESTROY)SetWindowLongPtrW(window,GWLP_USERDATA,0);
    return DefWindowProcW(window,message,w,l);
}
std::optional<LRESULT> EditorFloatingWindows::Message(Entry& entry,UINT message,WPARAM w,LPARAM l)
{
    if(shuttingDown_)return {};
    if(message==WM_CLOSE){shell_.CloseFloatingWindow(entry.id);return 0;}
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_PAINT){PAINTSTRUCT paint;BeginPaint(entry.window,&paint);EndPaint(entry.window,&paint);return 0;}
    if(message==WM_GETMINMAXINFO)
    {auto* info=reinterpret_cast<MINMAXINFO*>(l);info->ptMinTrackSize={240,160};return 0;}
    if(message==WM_DPICHANGED)
    {
        const auto* bounds=reinterpret_cast<const RECT*>(l);
        if(bounds)SetWindowPos(entry.window,nullptr,bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top,SWP_NOZORDER|SWP_NOACTIVATE);
        entry.input.SetDpiScale(HIWORD(w)/96.0f);entry.text.SetRasterScale(entry.input.DpiScale());return 0;
    }
    if(message==WM_EXITSIZEMOVE)
    {RECT bounds{};GetWindowRect(entry.window,&bounds);shell_.SetFloatingBounds(entry.id,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top);}
    if(message==WM_SIZE)return 0;
    if(handler_&&message!=WM_NCCREATE&&message!=WM_NCDESTROY)
    {
        const auto handled=handler_(entry.window,message,w,l);
        const auto raw=ProcessWindowsInput(entry.window,message,w,l,entry.rawInput);
        if(raw==InputMessageResult::Handled)return handled.value_or(0);
        return handled;
    }
    return {};
}
}

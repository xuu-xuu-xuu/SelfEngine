#include "EditorLoadingScreen.h"
#include "EditorWindowStyle.h"
#include "LoadingRibbon.h"
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include "PlatformLayer/Windows/AppBranding.h"
#include "CoreLayer/Localization.h"
#include <array>
#include <numbers>
#include <thread>

namespace se
{
namespace
{
constexpr int WindowWidth=656, WindowHeight=460, Margin=40;
constexpr ULONGLONG CycleMs=7200;
// 全部坐标按客户区和实际 DPI 计算，绘制与命中共享取消按钮区域。
RECT CancelRect(HWND hwnd)
{
    RECT client{};GetClientRect(hwnd,&client);
    const UINT dpi=GetDpiForWindow(hwnd);
    const auto n=[dpi](int value){return MulDiv(value,dpi,96);};
    return {client.right-n(Margin+76),client.bottom-n(62),
            client.right-n(Margin-8),client.bottom-n(20)};
}
std::wstring Wide(const std::string& text)
{
    if(text.empty())return {};
    const int size=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
    std::wstring result(size,L'\0');if(size)MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),size);return result;
}
}
struct EditorLoadingScreen::Impl
{
    EditorLoadingScreen& owner;bool hidden;
    std::atomic<HWND> window=nullptr;
    std::atomic<DWORD> threadId=0;
    std::thread thread;
    std::array<std::wstring,static_cast<size_t>(StartupStage::Ready)+1> stages;
    std::wstring title,project,cancel,cancelling,subtitle;
    HFONT heading=nullptr,label=nullptr,detailFont=nullptr;
    ULONG_PTR graphicsToken=0;
    ULONGLONG animationStart=0;
    bool animate=true;
    void UpdateMotion(HWND hwnd)
    {
        BOOL enabled=TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&enabled,0);
        animate=enabled!=FALSE;
        SetTimer(hwnd,1,animate?33:100,nullptr);
    }
    enum class Press { None, Move, Cancel };
    Press press=Press::None;
    POINT dragOffset{};
    static bool OverCancel(HWND window, POINT point)
    {
        RECT area=CancelRect(window);
        return PtInRect(&area,point)!=FALSE;
    }
    explicit Impl(EditorLoadingScreen& value,bool invisible):owner(value),hidden(invisible){}
    void Fonts(UINT dpi)
    {
        for(const auto font:{heading,label,detailFont})if(font)DeleteObject(font);
        heading=CreateFontW(-MulDiv(30,dpi,96),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        label=CreateFontW(-MulDiv(16,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        detailFont=CreateFontW(-MulDiv(12,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    }
    void Paint(HWND hwnd,HDC target=nullptr)
    {
        PAINTSTRUCT paint{};const auto destination=target?target:BeginPaint(hwnd,&paint);RECT client{};GetClientRect(hwnd,&client);
        const auto dc=CreateCompatibleDC(destination);const auto bitmap=CreateCompatibleBitmap(destination,client.right,client.bottom);
        const auto oldBitmap=SelectObject(dc,bitmap);const auto brush=CreateSolidBrush(flare::Native(flare::Background));FillRect(dc,&client,brush);DeleteObject(brush);
        const UINT dpi=GetDpiForWindow(hwnd);const auto n=[&](int value){return MulDiv(value,dpi?dpi:96,96);};
        const auto text=[&](const std::wstring& value,HFONT font,COLORREF color,int x,int y,int width,int height,UINT flags=DT_LEFT){
            RECT rect{n(x),n(y),n(x+width),n(y+height)};SelectObject(dc,font);SetTextColor(dc,color);SetBkMode(dc,TRANSPARENT);
            DrawTextW(dc,value.c_str(),static_cast<int>(value.size()),&rect,flags|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
        };
        const int width=MulDiv(client.right,96,dpi),height=MulDiv(client.bottom,96,dpi);
        const int logo=n(164);
        if(const auto icon=AppIcon(logo))DrawIconEx(dc,(client.right-logo)/2,n(40),icon,logo,logo,0,nullptr,DI_NORMAL);
        text(L"Flare",heading,flare::Native(flare::Heading),Margin,207,width-2*Margin,42,DT_CENTER);
        const auto progress=owner.channel_.Read();
        const bool cancelled=owner.Cancelled() || progress.cancelled;
        const auto& stage=stages[static_cast<size_t>(progress.stage)];
        text(cancelled?cancelling:subtitle,label,flare::Native(flare::Heading),Margin,height-202,width-2*Margin,28,DT_CENTER);
        text(std::to_wstring(progress.percent)+L"%",detailFont,flare::Native(flare::Gold),Margin,height-171,width-2*Margin,20,DT_CENTER);

        RECT ribbon{n(Margin),client.bottom-n(143),client.right-n(Margin),client.bottom-n(79)};
        const float phase=animate ? static_cast<float>((GetTickCount64()-animationStart)%CycleMs)
                                   *2.0f*std::numbers::pi_v<float>/static_cast<float>(CycleMs) : 0.0f;
        if(graphicsToken)DrawLoadingRibbon(dc,ribbon,progress.percent,phase);
        else
        {
            // 系统绘图服务不可用时仍显示真实的填充长度和读数。
            RECT track{ribbon.left,(ribbon.top+ribbon.bottom)/2,ribbon.right,(ribbon.top+ribbon.bottom)/2+n(3)};
            const auto trackBrush=static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
            SetDCBrushColor(dc,flare::Native(flare::Border));FillRect(dc,&track,trackBrush);
            track.right=track.left+MulDiv(track.right-track.left,progress.percent,100);
            SetDCBrushColor(dc,flare::Native(flare::Gold));FillRect(dc,&track,trackBrush);
        }
        text(project,detailFont,flare::Native(flare::Text),Margin,height-64,width-2*Margin-110,22);
        text(stage,detailFont,flare::Native(flare::Muted),Margin,height-42,width-2*Margin-110,20);
        RECT action=CancelRect(hwnd);
        action.right-=n(8);
        SelectObject(dc,detailFont);SetTextColor(dc,flare::Native(cancelled?flare::Disabled:flare::Muted));SetBkMode(dc,TRANSPARENT);
        DrawTextW(dc,cancel.c_str(),-1,&action,DT_RIGHT|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
        BitBlt(destination,0,0,client.right,client.bottom,dc,0,0,SRCCOPY);
        SelectObject(dc,oldBitmap);DeleteObject(bitmap);DeleteDC(dc);if(!target)EndPaint(hwnd,&paint);
    }
    static LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM wp,LPARAM lp)
    {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(window,message,wp,lp);
        switch(message)
        {
        case WM_PAINT:self->Paint(window);return 0;
        case WM_PRINTCLIENT:self->Paint(window,reinterpret_cast<HDC>(wp));return 0;
        case WM_ERASEBKGND:return 1;
        case WM_SETTINGCHANGE:self->UpdateMotion(window);return 0;
        case WM_CLOSE:self->owner.cancelled_=true;self->owner.channel_.Cancel();InvalidateRect(window,nullptr,FALSE);return 0;
        case WM_LBUTTONDOWN:
        {
            POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
            self->press=OverCancel(window,point)?Press::Cancel:Press::Move;
            RECT bounds{};GetWindowRect(window,&bounds);ClientToScreen(window,&point);
            self->dragOffset={point.x-bounds.left,point.y-bounds.top};
            SetCapture(window);
            return 0;
        }
        case WM_MOUSEMOVE:
            if(self->press==Press::Move && GetCapture()==window)
            {
                POINT cursor{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ClientToScreen(window,&cursor);
                SetWindowPos(window,nullptr,cursor.x-self->dragOffset.x,cursor.y-self->dragOffset.y,
                    0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            }
            return 0;
        case WM_LBUTTONUP:
        {
            const bool cancel=self->press==Press::Cancel && OverCancel(window,{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)});
            self->press=Press::None;
            if(GetCapture()==window)ReleaseCapture();
            if(cancel)SendMessageW(window,WM_CLOSE,0,0);
            return 0;
        }
        case WM_CAPTURECHANGED:self->press=Press::None;return 0;
        case WM_CANCELMODE:
            self->press=Press::None;
            if(GetCapture()==window)ReleaseCapture();
            return 0;
        case WM_DPICHANGED:
        {
            const auto* rect=reinterpret_cast<RECT*>(lp);self->Fonts(HIWORD(wp));
            SetWindowPos(window,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);ApplyAppIcon(window);return 0;
        }
        case WM_TIMER:
        {
            const auto progress=self->owner.channel_.Read();
            if(progress.stage==StartupStage::Ready){DestroyWindow(window);return 0;}
            if(progress.cancelled)self->owner.cancelled_=true;
            InvalidateRect(window,nullptr,FALSE);return 0;
        }
        case WM_DESTROY:self->window=nullptr;PostQuitMessage(0);return 0;
        default:return DefWindowProcW(window,message,wp,lp);
        }
    }
    void Run(HANDLE ready)
    {
        threadId=GetCurrentThreadId();MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const auto instance=GetModuleHandleW(nullptr);WNDCLASSW type{};type.hInstance=instance;type.lpfnWndProc=Procedure;
        type.lpszClassName=L"FlareLoadingScreen";type.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));type.hIcon=AppIcon(32);
        if(!RegisterClassW(&type)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS){SetEvent(ready);return;}
        const UINT dpi=GetDpiForSystem();Fonts(dpi);const int width=MulDiv(WindowWidth,dpi,96),height=MulDiv(WindowHeight,dpi,96);
        RECT area{};SystemParametersInfoW(SPI_GETWORKAREA,0,&area,0);
        wchar_t offscreen[8]{};const bool testOffscreen=GetEnvironmentVariableW(L"SELFENGINE_TEST_OFFSCREEN",offscreen,8)>0;
        window=CreateWindowExW(WS_EX_APPWINDOW,type.lpszClassName,title.c_str(),WS_POPUP|WS_BORDER,
            testOffscreen?-30000:area.left+(area.right-area.left-width)/2,testOffscreen?-30000:area.top+(area.bottom-area.top-height)/2,width,height,nullptr,nullptr,instance,this);
        if(window)
        {
            Gdiplus::GdiplusStartupInput input;
            if(Gdiplus::GdiplusStartup(&graphicsToken,&input,nullptr)!=Gdiplus::Ok)graphicsToken=0;
            animationStart=GetTickCount64();
            // 创建位置所在屏幕的实际 DPI 可能与系统 DPI 不同（含屏外自动测试）。
            const UINT actual=GetDpiForWindow(window);Fonts(actual?actual:96);
            const int actualWidth=MulDiv(WindowWidth,actual?actual:96,96),actualHeight=MulDiv(WindowHeight,actual?actual:96,96);
            SetWindowPos(window,nullptr,testOffscreen?-30000:area.left+(area.right-area.left-actualWidth)/2,
                testOffscreen?-30000:area.top+(area.bottom-area.top-actualHeight)/2,actualWidth,actualHeight,SWP_NOZORDER|SWP_NOACTIVATE);
            ApplyAppIcon(window);UpdateMotion(window);ShowWindow(window,SW_SHOW);UpdateWindow(window);
        }
        SetEvent(ready);
        if(window)while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
        if(window)DestroyWindow(window);
        if(graphicsToken){Gdiplus::GdiplusShutdown(graphicsToken);graphicsToken=0;}
        for(const auto font:{heading,label,detailFont})if(font)DeleteObject(font);heading=label=detailFont=nullptr;
    }
};
EditorLoadingScreen::EditorLoadingScreen(const Localization& text,const std::filesystem::path& project,bool hidden)
    :impl_(std::make_unique<Impl>(*this,hidden))
{
    for(size_t i=0;i<impl_->stages.size();++i)impl_->stages[i]=Wide(text.GetText(StartupStageKey(static_cast<StartupStage>(i))));
    impl_->title=Wide(text.GetText("Editor.Loading.Title"));impl_->subtitle=Wide(text.GetText("Editor.Loading.Subtitle"));
    impl_->cancel=Wide(text.GetText("EditorUi.Cancel"));impl_->cancelling=Wide(text.GetText("Editor.Loading.Cancelling"));
    impl_->project=project.filename().wstring();Restart();
}
EditorLoadingScreen::~EditorLoadingScreen(){Stop();}
void EditorLoadingScreen::Stop()
{
    if(!impl_->thread.joinable())return;
    const auto id=impl_->threadId.load();if(id)PostThreadMessageW(id,WM_QUIT,0,0);impl_->thread.join();impl_->threadId=0;
}
void EditorLoadingScreen::Restart()
{
    Stop();cancelled_=false;channel_.Reset();if(impl_->hidden||!channel_.Valid())return;
    const auto ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!ready)return;
    impl_->thread=std::thread([this,ready]{impl_->Run(ready);});WaitForSingleObject(ready,INFINITE);CloseHandle(ready);
}
HWND EditorLoadingScreen::Window() const noexcept {return impl_->window.load();}
}

#include "LauncherStyle.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <string>

namespace se::launcher_style
{
void Fill(HDC dc, RECT rect, flare::Color color)
{
    SetDCBrushColor(dc, flare::Native(color));
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

LRESULT ControlColors(UINT message, HDC dc, bool muted)
{
    const auto background = message==WM_CTLCOLORSTATIC ? flare::Background : flare::Surface;
    SetTextColor(dc,flare::Native(muted ? flare::Muted : flare::Text));
    SetBkColor(dc,flare::Native(background));
    SetDCBrushColor(dc,flare::Native(background));
    return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
}

namespace
{
LRESULT CALLBACK ButtonHover(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if(message==WM_MOUSEMOVE && !GetPropW(hwnd,L"FlareHover"))
    {
        TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,hwnd,0};
        if(TrackMouseEvent(&tracking))
        {
            SetPropW(hwnd,L"FlareHover",reinterpret_cast<HANDLE>(1));
            InvalidateRect(hwnd,nullptr,FALSE);
        }
    }
    if(message==WM_MOUSELEAVE || message==WM_NCDESTROY)
    {
        RemovePropW(hwnd,L"FlareHover");
        if(message==WM_MOUSELEAVE)InvalidateRect(hwnd,nullptr,FALSE);
    }
    return DefSubclassProc(hwnd,message,wp,lp);
}
}

void ThemeControl(HWND control)
{
    wchar_t type[32]{};
    GetClassNameW(control,type,32);
    if(lstrcmpiW(type,L"BUTTON")==0)
        SetWindowSubclass(control,ButtonHover,1,0);
    else if(lstrcmpiW(type,L"LISTBOX")==0 || lstrcmpiW(type,L"COMBOBOX")==0)
        SetWindowTheme(control,L"DarkMode_Explorer",nullptr);
}

bool DrawControl(const DRAWITEMSTRUCT& item)
{
    const bool button=item.CtlType==ODT_BUTTON;
    const bool list=item.CtlType==ODT_LISTBOX;
    if(!button && !list && item.CtlType!=ODT_COMBOBOX)return false;
    HDC dc=item.hDC;
    const int saved=SaveDC(dc);
    RECT row=item.rcItem;
    const bool selected=(item.itemState&ODS_SELECTED)!=0;
    const bool disabled=(item.itemState&ODS_DISABLED)!=0;
    const bool focused=(item.itemState&(ODS_FOCUS|ODS_NOFOCUSRECT))==ODS_FOCUS;
    const bool hovered=button && GetPropW(item.hwndItem,L"FlareHover");
    const auto bg=selected ? flare::Selected : hovered && !disabled ? flare::Hover : button ? flare::Raised : flare::Surface;
    Fill(dc,row,bg);
    if(button || focused)
    {
        SetDCBrushColor(dc,flare::Native(focused ? flare::Silver : flare::Border));
        FrameRect(dc,&row,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    const auto font=SendMessageW(item.hwndItem,WM_GETFONT,0,0);
    if(font)SelectObject(dc,reinterpret_cast<HFONT>(font));
    SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc,flare::Native(disabled ? flare::Disabled : flare::Heading));
    InflateRect(&row,-9,0);
    const UINT flags=DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX;
    if(button)
    {
        wchar_t label[200]{};
        GetWindowTextW(item.hwndItem,label,200);
        DrawTextW(dc,label,-1,&row,flags|DT_CENTER|DT_END_ELLIPSIS);
    }
    else if(item.itemID!=static_cast<UINT>(-1))
    {
        const auto length=SendMessageW(item.hwndItem,list?LB_GETTEXTLEN:CB_GETLBTEXTLEN,item.itemID,0);
        if(length>=0)
        {
            std::wstring label(static_cast<size_t>(length)+1,L'\0');
            SendMessageW(item.hwndItem,list?LB_GETTEXT:CB_GETLBTEXT,item.itemID,reinterpret_cast<LPARAM>(label.data()));
            label.resize(static_cast<size_t>(length));
            const auto split=list ? label.rfind(L"  —  ") : std::wstring::npos;
            if(split!=std::wstring::npos)
            {
                RECT name=row;
                name.right=row.left+(row.right-row.left)/3;
                DrawTextW(dc,label.data(),static_cast<int>(split),&name,flags|DT_END_ELLIPSIS);
                row.left=name.right+8;
                SetTextColor(dc,flare::Native(flare::Muted));
                DrawTextW(dc,label.c_str()+split+5,-1,&row,flags|DT_PATH_ELLIPSIS);
            }
            else DrawTextW(dc,label.c_str(),-1,&row,flags|DT_END_ELLIPSIS);
        }
    }
    RestoreDC(dc,saved);
    return true;
}
}

#include "scrollbar.h"
#include "theme.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <stdexcept>

namespace talktext {
namespace {
void paint(HWND window,HDC dc,COLORREF background) {
    RECT rect{};GetClientRect(window,&rect);
    Gdiplus::Graphics graphics(dc);
    graphics.Clear(Gdiplus::Color(255,GetRValue(background),GetGValue(background),GetBValue(background)));
    SCROLLBARINFO info{sizeof(info)};if(!GetScrollBarInfo(window,OBJID_CLIENT,&info))return;
    const float scale=GetDpiForWindow(window)/96.0f;
    const float width=std::max(3.0f,4.0f*scale);
    const float x=(rect.right-width)/2,y=static_cast<float>(info.xyThumbTop);
    const float height=static_cast<float>(info.xyThumbBottom-info.xyThumbTop);
    if(height<width)return;
    POINT cursor{};GetCursorPos(&cursor);ScreenToClient(window,&cursor);
    const bool hover=PtInRect(&rect,cursor)!=FALSE;
    const bool active=GetCapture()==window;
    const auto& c=theme::colours;
    const COLORREF tone=active?c.text:hover?c.muted:theme::mix(c.border,c.muted,0.25f);
    Gdiplus::SolidBrush ink(Gdiplus::Color(255,GetRValue(tone),GetGValue(tone),GetBValue(tone)));
    Gdiplus::GraphicsPath path;
    path.AddArc(x,y,width,width,180,180);
    path.AddArc(x,y+height-width,width,width,0,180);path.CloseFigure();
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.FillPath(&ink,&path);
}
LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR color) {
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_APP+1){InvalidateRect(hwnd,nullptr,FALSE);UpdateWindow(hwnd);return 0;}
    if(msg==WM_PAINT) {
        PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);paint(hwnd,dc,static_cast<COLORREF>(color));EndPaint(hwnd,&ps);return 0;
    }
    if(msg==WM_PRINTCLIENT){paint(hwnd,reinterpret_cast<HDC>(wp),static_cast<COLORREF>(color));return 0;}
    const LRESULT result=DefSubclassProc(hwnd,msg,wp,lp);
    if(msg==WM_MOUSEMOVE) {
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0};TrackMouseEvent(&track);
    }
    if(msg==WM_MOUSEMOVE||msg==WM_MOUSELEAVE||msg==WM_LBUTTONDOWN||msg==WM_LBUTTONUP||msg==WM_KEYDOWN) {
        InvalidateRect(hwnd,nullptr,FALSE);UpdateWindow(hwnd);
    }
    return result;
}
}
void Scrollbar::create(HWND parent,int id,COLORREF background) {
    // Layered so it draws above the parent's composition visual; the keyed
    // track lets the frosted panel show and leaves only the thumb.
    window=CreateWindowExW(WS_EX_LAYERED,L"SCROLLBAR",nullptr,WS_CHILD|SBS_VERT,0,0,1,1,parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    if(!window)throw std::runtime_error("Cannot create scroll control");
    SetWindowTheme(window,L"",L"");
    recolour(background);
}
void Scrollbar::recolour(COLORREF background) {
    if(!window)return;
    SetWindowSubclass(window,proc,1,background);
    SetLayeredWindowAttributes(window,background,255,LWA_COLORKEY);
    InvalidateRect(window,nullptr,FALSE);
}
void Scrollbar::layout(int x,int y,int width,int height) {
    if(!window)return;
    const int arrow=GetSystemMetricsForDpi(SM_CYVSCROLL,GetDpiForWindow(window));
    MoveWindow(window,x,y-arrow,width,std::max(1,height)+2*arrow,TRUE);
    // Clip native line buttons outside the visible/hit-testable track. Native
    // thumb geometry, capture, page clicks, keyboard and UIA stay in Windows.
    HRGN region=CreateRectRgn(0,arrow,width,arrow+std::max(1,height));
    if(!SetWindowRgn(window,region,TRUE))DeleteObject(region);
}
void Scrollbar::update(int position,int count,int page) {
    if(!window)return;
    SCROLLINFO old{sizeof(old),SIF_RANGE|SIF_PAGE|SIF_POS};GetScrollInfo(window,SB_CTL,&old);
    SCROLLINFO next{sizeof(next),SIF_RANGE|SIF_PAGE|SIF_POS,0,std::max(0,count-1),static_cast<UINT>(std::max(1,page)),position};
    const bool changed=old.nMax!=next.nMax||old.nPage!=next.nPage||old.nPos!=next.nPos;
    if(changed)SetScrollInfo(window,SB_CTL,&next,TRUE);
    const bool visible=count>page;
    const bool visibilityChanged=((GetWindowLongPtrW(window,GWL_STYLE)&WS_VISIBLE)!=0)!=visible;
    if(visibilityChanged)ShowWindow(window,visible?SW_SHOWNOACTIVATE:SW_HIDE);
    // The native control also paints directly from SetScrollInfo and ShowWindow.
    // Repaint after those calls, including its modal thumb-tracking loop.
    if(changed||visibilityChanged)PostMessageW(window,WM_APP+1,0,0);
}
int Scrollbar::destination(UINT code,int lineStep) const {
    SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(window,SB_CTL,&info);
    int value=info.nPos;
    switch(code) {
    case SB_LINEUP:value-=lineStep;break;case SB_LINEDOWN:value+=lineStep;break;
    case SB_PAGEUP:value-=info.nPage;break;case SB_PAGEDOWN:value+=info.nPage;break;
    case SB_THUMBTRACK:case SB_THUMBPOSITION:value=info.nTrackPos;break;
    case SB_TOP:value=info.nMin;break;case SB_BOTTOM:value=info.nMax;break;
    }
    return std::clamp(value,info.nMin,std::max(info.nMin,info.nMax-static_cast<int>(info.nPage)+1));
}
}

#include "theme.h"
#include "platform.h"
#include "speech.h"
#include "visuals.h"
#include "gpu_visuals.h"
#include "scrollbar.h"
#include <windows.h>
#include <shellapi.h>
#include <ole2.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <shellscalingapi.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <mmsystem.h>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace talktext;
namespace {
constexpr UINT TrayMessage = WM_APP + 20;
constexpr UINT DeliveryTimer = 2;
constexpr int PopupWidth = 180, PopupHeight = 48;
constexpr int HeaderHeight = 48, RowHeight = 40, HistoryRow = 52;
enum Control { Record = 100, Startup, Tray, Hidden, Mode, Enter, Bind, Method, Limit, History, Copy, Delete, Clear, Preview, Status, Quit, Recover, Theme, Glass, LastControl = Glass };
// Identities of the composition-drawn text and images; each owns one cached raster.
struct InkKeys { char logo, title, tagline, input, delivery, appearance, behavior, history, limit, helper, status; } keys;
struct App {
    HWND window{}, popup{}, target{};
    HFONT font{}, readingFont{}, smallFont{}, labelFont{}, titleFont{};
    HICON logo{};
    // Native panel backgrounds in the active theme; applyAppearance() owns them.
    HBRUSH canvas{}, surface{};
    Settings settings;
    std::vector<HistoryEntry> history;
    Hotkeys hotkeys;
    SpeechSession speech;
    bool recording{}, finishing{}, cancelling{}, binding{}, trayAdded{};
    std::vector<UINT> captured;
    std::wstring pending, status{L"Preparing Nemotron once. Model will stay ready while TalkText is open."};
    ULONGLONG deliveryStarted{};
    float level{}, visualLevel{}, centroid{}, activeSeconds{};
    std::array<float,8> spectrum{}, visualSpectrum{};
    ULONGLONG audioTick{};
    std::chrono::steady_clock::time_point frameTick{},hoverTick{};
    bool motion{true},visualDirty{true},animating{};
    UINT dpi{96}, popupDpi{96};
    int scroll{}, contentHeight{};
    int hovered{};
    std::array<float,120> hoverLight{},togglePosition{};
    bool visualSettingsReady{};
    HWND controls[120]{};
    std::array<Scrollbar,3> scrollbars;
    std::array<int,2> wheelRemainder{};
    // Client-space geometry from layout(), drawn by renderMain().
    std::array<RECT,4> cards{};
    RECT historyPanel{},previewPanel{},limitBox{};
    ~App() {
        for(HFONT f:{font,readingFont,smallFont,labelFont,titleFont})DeleteObject(f);
        if(logo)DestroyIcon(logo);DeleteObject(canvas);DeleteObject(surface);
    }
} app;

std::wstring widen(const char* value) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
    if (!n) return L"Unknown error";
    std::wstring result(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value, -1, result.data(), n);
    result.pop_back(); return result;
}
int px(int value) { return MulDiv(value,static_cast<int>(app.dpi),96); }
void layout();
enum class Cue : WORD { Start=301,Stopped=302,Complete=303 };
void play(Cue cue) {
    PlaySoundW(MAKEINTRESOURCEW(static_cast<WORD>(cue)),GetModuleHandleW(nullptr),SND_RESOURCE|SND_ASYNC|SND_NODEFAULT);
}
void updateFonts() {
    // Orchard type scale: 14px body, 12px labels (medium), Segoe UI as on Windows Orchard.
    auto make=[](int size,int weight) {
        return CreateFontW(-px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    };
    HFONT font=make(14,FW_NORMAL),reading=make(14,FW_NORMAL),metadata=make(12,FW_NORMAL);
    HFONT label=make(12,FW_SEMIBOLD),title=make(14,FW_SEMIBOLD);
    for(HWND child:app.controls)if(child)SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    for(int id:{History,Preview})if(app.controls[id])
        SendMessageW(app.controls[id],WM_SETFONT,reinterpret_cast<WPARAM>(reading),TRUE);
    if(app.controls[Status])SendMessageW(app.controls[Status],WM_SETFONT,reinterpret_cast<WPARAM>(metadata),TRUE);
    for(HFONT f:{app.font,app.readingFont,app.smallFont,app.labelFont,app.titleFont})DeleteObject(f);
    app.font=font;app.readingFont=reading;app.smallFont=metadata;app.labelFont=label;app.titleFont=title;
    if(app.logo)DestroyIcon(app.logo);
    app.logo=static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1),IMAGE_ICON,px(20),px(20),LR_DEFAULTCOLOR));
    app.visualDirty=true;
    if(app.controls[History])SendMessageW(app.controls[History],LB_SETITEMHEIGHT,0,px(HistoryRow));
}
void animate() {
    if(!app.animating)app.hoverTick=std::chrono::steady_clock::now();
    app.animating=true;app.visualDirty=true;
}
void syncScrollbars() {
    if(!app.scrollbars[0].window)return;
    RECT client{};GetClientRect(app.window,&client);
    app.scrollbars[0].update(app.scroll,app.contentHeight,client.bottom);
    GetClientRect(app.controls[History],&client);
    app.scrollbars[1].update(static_cast<int>(SendMessageW(app.controls[History],LB_GETTOPINDEX,0,0)),
        static_cast<int>(app.history.size()),std::max(1,static_cast<int>(client.bottom)/px(HistoryRow)));
    GetClientRect(app.controls[Preview],&client);
    HDC dc=GetDC(app.controls[Preview]);auto old=SelectObject(dc,app.readingFont);
    TEXTMETRICW metrics{};GetTextMetricsW(dc,&metrics);SelectObject(dc,old);ReleaseDC(app.controls[Preview],dc);
    app.scrollbars[2].update(static_cast<int>(SendMessageW(app.controls[Preview],EM_GETFIRSTVISIBLELINE,0,0)),
        static_cast<int>(SendMessageW(app.controls[Preview],EM_GETLINECOUNT,0,0)),
        std::max(1,static_cast<int>(client.bottom)/std::max(1,static_cast<int>(metrics.tmHeight))));
}
LRESULT CALLBACK controlProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
    const int id=GetDlgCtrlID(hwnd);
    if(msg==WM_MOUSEWHEEL&&(id==History||id==Preview)) {
        const int index=id==History?0:1;
        SCROLLINFO info{sizeof(info),SIF_ALL};
        GetScrollInfo(app.scrollbars[index+1].window,SB_CTL,&info);
        UINT lines=3;SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
        const int step=lines==WHEEL_PAGESCROLL?static_cast<int>(info.nPage):static_cast<int>(lines);
        int& remainder=app.wheelRemainder[index];
        remainder+=GET_WHEEL_DELTA_WPARAM(wp)*step;
        const int delta=remainder/WHEEL_DELTA;remainder%=WHEEL_DELTA;
        const int next=std::clamp(info.nPos-delta,info.nMin,
            std::max(info.nMin,info.nMax-static_cast<int>(info.nPage)+1));
        if(id==History)SendMessageW(hwnd,LB_SETTOPINDEX,next,0);
        else SendMessageW(hwnd,EM_LINESCROLL,0,next-info.nPos);
        return 0;
    }
    if(msg==WM_MOUSEMOVE&&app.hovered!=id) {
        app.hovered=id;
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0};TrackMouseEvent(&track);
        animate();
    } else if(msg==WM_MOUSELEAVE) {
        if(app.hovered==id)app.hovered=0;
        animate();
    } else if(msg==WM_SETFOCUS) {
        RECT r{};GetWindowRect(hwnd,&r);MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT*>(&r),2);
        RECT client{};GetClientRect(app.window,&client);
        if(r.top<0||r.bottom>client.bottom) {
            app.scroll+=r.top<0?r.top:r.bottom-client.bottom;
            layout();
        }
    }
    const LRESULT result=DefSubclassProc(hwnd,msg,wp,lp);
    if(msg==WM_PAINT&&id==History&&app.history.empty()) {
        HDC dc=GetDC(hwnd);RECT r{};GetClientRect(hwnd,&r);InflateRect(&r,-px(12),-px(12));
        auto previous=SelectObject(dc,app.readingFont);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,theme::colours.muted);
        DrawTextW(dc,L"No transcriptions yet.\nYour completed words will be saved here.",-1,&r,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
        SelectObject(dc,previous);ReleaseDC(hwnd,dc);
    }
    if((id==History||id==Preview)&&(msg==WM_PAINT||msg==WM_MOUSEWHEEL||msg==WM_KEYDOWN||
       msg==WM_SETTEXT||msg==EM_LINESCROLL||msg==LB_SETTOPINDEX))syncScrollbars();
    return result;
}
void setStatus(const std::wstring& value) {
    app.status = value;
    SetWindowTextW(app.controls[Status], value.c_str());
    app.visualDirty=true;
    if(app.controls[Recover]) {
        const bool recover=hasPendingClipboard();
        if((IsWindowVisible(app.controls[Recover])!=FALSE)!=recover) {
            ShowWindow(app.controls[Recover],recover?SW_SHOW:SW_HIDE);layout();
        }
    }
}
void error(const std::exception& e) { play(Cue::Stopped);setStatus(widen(e.what())); }
// History, Preview and Limit stay native for editing, selection and UI Automation;
// buttons and the status line are native proxies for input and accessibility.
// Every child is layered so it composites above the window's composition visual
// instead of punching a hole through it; proxies are kept at the minimum alpha
// that still receives input, because the composition frame draws them.
HWND control(int id, const wchar_t* cls, const wchar_t* text, DWORD style) {
    HWND h = CreateWindowExW(WS_EX_LAYERED, cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
        0,0,0,0,app.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    if (!h) throw std::runtime_error("Cannot create window control");
    const bool panel=id==History||id==Preview||id==Limit;
    SetLayeredWindowAttributes(h,0,panel?255:1,LWA_ALPHA);
    SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(app.font),TRUE);
    app.controls[id] = h;
    SetWindowSubclass(h,controlProc,1,0);
    return h;
}
void button(int id, const wchar_t* text) { control(id,L"BUTTON",text,BS_OWNERDRAW); }
bool checked(int id) {
    switch(id) {
    case Startup:return app.settings.startWithWindows;case Tray:return app.settings.keepInTray;
    case Hidden:return app.settings.startHidden;case Enter:return app.settings.autoEnter;
    default:return false;
    }
}
void refreshSettings() {
    auto label = [](int id, bool value, const wchar_t* text) {
        std::wstring s=text; s+=value?L" (on)":L" (off)";
        SetWindowTextW(app.controls[id],s.c_str());
    };
    label(Startup,app.settings.startWithWindows,L"Start with Windows");
    label(Tray,app.settings.keepInTray,L"Keep in system tray");
    label(Hidden,app.settings.startHidden,L"Start hidden");
    label(Enter,app.settings.autoEnter,L"Press Enter after insertion");
    SetWindowTextW(app.controls[Mode],app.settings.pushToTalk ? L"Mode: hold to talk" : L"Mode: toggle to talk");
    SetWindowTextW(app.controls[Bind],(L"Hotkey: " + chordName(app.settings.hotkey)).c_str());
    const wchar_t* methods[] = {L"Delivery: paste + restore clipboard",L"Delivery: type Unicode characters",L"Delivery: clipboard only"};
    SetWindowTextW(app.controls[Method],methods[static_cast<int>(app.settings.method)]);
    SetWindowTextW(app.controls[Limit],std::to_wstring(app.settings.historyLimit).c_str());
    SetWindowTextW(app.controls[Theme],(std::wstring(L"Theme: ")+theme::presets[app.settings.theme].name).c_str());
    SetWindowTextW(app.controls[Glass],app.settings.frostedGlass?
        (L"Frosted glass: "+std::to_wstring(app.settings.frostedGlass)+L"%").c_str():L"Frosted glass: Off");
    for(int id:{Startup,Tray,Hidden,Enter}) {
        if(!app.visualSettingsReady||!app.motion)app.togglePosition[id]=checked(id)?1.0f:0.0f;
    }
    app.visualSettingsReady=true;
    animate();
}
// Orchard's frosted glass: the same acrylic accent policy GPUI applies for
// WindowBackgroundAppearance::Blurred, under a translucent canvas.
void setAcrylic(HWND window,bool enabled) {
    struct AccentPolicy { DWORD state, flags, gradient, animation; };
    struct Attribute { DWORD attribute; void* data; SIZE_T size; };
    using Set=BOOL(WINAPI*)(HWND,Attribute*);
    static const auto set=reinterpret_cast<Set>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetWindowCompositionAttribute"));
    if(!set)return;
    AccentPolicy accent{enabled?4u:0u,0,enabled?0x01000000u:0u,0};
    Attribute data{0x13,&accent,sizeof(accent)};
    set(window,&data);
}
void applyAppearance() {
    theme::select(app.settings.theme,app.settings.frostedGlass);
    const auto& c=theme::colours;
    const bool glass=theme::glass>0;
    for(HBRUSH brush:{app.canvas,app.surface})if(brush)DeleteObject(brush);
    app.canvas=CreateSolidBrush(c.canvas);app.surface=CreateSolidBrush(c.surface);
    BOOL dark=c.dark;DwmSetWindowAttribute(app.window,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
    const COLORREF caption=glass?DWMWA_COLOR_DEFAULT:c.canvas,ink=c.text,border=c.border;
    DwmSetWindowAttribute(app.window,DWMWA_CAPTION_COLOR,&caption,sizeof(caption));
    DwmSetWindowAttribute(app.window,DWMWA_TEXT_COLOR,&ink,sizeof(ink));
    DwmSetWindowAttribute(app.window,DWMWA_BORDER_COLOR,&border,sizeof(border));
    setAcrylic(app.window,glass);
    // Native panels float over the pane: nearly opaque while frosted, as Orchard's popovers.
    const BYTE alpha=glass?static_cast<BYTE>(theme::FloatingAlpha*255):255;
    for(int id:{History,Preview,Limit}) {
        SetWindowTheme(app.controls[id],c.dark?L"DarkMode_Explorer":L"Explorer",nullptr);
        SetLayeredWindowAttributes(app.controls[id],0,alpha,LWA_ALPHA);
        // Layered children miss the parent's RDW_ALLCHILDREN pass.
        RedrawWindow(app.controls[id],nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME);
    }
    app.scrollbars[0].recolour(c.canvas);app.scrollbars[1].recolour(c.surface);app.scrollbars[2].recolour(c.surface);
    RedrawWindow(app.window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN);
    app.visualDirty=true;
}
// Shells (Explorer restarts, replacement bars) ask notification-area owners
// to re-register through this broadcast; ignoring it loses the icon.
const UINT TaskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
void updateTray() {
    NOTIFYICONDATAW ni{sizeof(ni)}; ni.hWnd=app.window; ni.uID=1;
    if (!app.settings.keepInTray && !app.settings.startHidden) {
        if(app.trayAdded) Shell_NotifyIconW(NIM_DELETE,&ni);
        app.trayAdded=false; return;
    }
    ni.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP; ni.uCallbackMessage=TrayMessage;
    ni.hIcon=static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1),IMAGE_ICON,32,32,LR_DEFAULTCOLOR|LR_SHARED));
    if (!ni.hIcon) ni.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    wcscpy_s(ni.szTip,L"TalkText - local voice to text");
    // Before the shell is ready (early sign-in) both fail; TaskbarCreated retries.
    // A re-registration request may come while the shell still holds the icon.
    const DWORD first=app.trayAdded?NIM_MODIFY:NIM_ADD,second=app.trayAdded?NIM_ADD:NIM_MODIFY;
    app.trayAdded=Shell_NotifyIconW(first,&ni)||Shell_NotifyIconW(second,&ni);
}
void save() { saveSettings(app.settings); updateTray(); }
void historyList() {
    HWND h=app.controls[History]; SendMessageW(h,LB_RESETCONTENT,0,0);
    for(const auto& entry:app.history) {
        std::wstring line=entry.timestamp+L"   "+entry.text.substr(0,120);
        std::replace(line.begin(),line.end(),L'\n',L' ');
        SendMessageW(h,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(line.c_str()));
    }
    InvalidateRect(h,nullptr,TRUE);
    SetWindowTextW(app.controls[Preview],L"Speak to see live text, or select a history entry.");
}
void addHistory(const std::wstring& text) {
    if(!app.settings.historyLimit) return;
    SYSTEMTIME st{}; GetLocalTime(&st); wchar_t date[40];
    swprintf_s(date,L"%04u-%02u-%02u %02u:%02u:%02u",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
    app.history.insert(app.history.begin(),HistoryEntry{date,text});
    if(app.history.size()>app.settings.historyLimit) app.history.resize(app.settings.historyLimit);
    saveHistory(app.history); historyList();
}
// Orchard grid: 24px page padding, 40px form rows on 6px cards, 32px controls.
void layout() {
    RECT r{};GetClientRect(app.window,&r);
    app.contentHeight=std::max(static_cast<int>(r.bottom),px(hasPendingClipboard()?844:796));
    app.scroll=std::clamp(app.scroll,0,std::max(0,app.contentHeight-static_cast<int>(r.bottom)));
    const int left=px(24),gap=px(24),lw=(r.right-px(48)-gap)*45/100;
    const int right=left+lw+gap,rw=r.right-right-px(24),h=app.contentHeight;
    auto at=[](int x,int y,int width,int height){return RECT{x,y-app.scroll,x+width,y-app.scroll+height};};
    auto place=[](int id,const RECT& box){
        MoveWindow(app.controls[id],box.left,box.top,box.right-box.left,box.bottom-box.top,TRUE);
    };
    place(Quit,at(r.right-px(24)-px(128),(px(HeaderHeight)-px(32))/2,px(128),px(32)));
    place(Record,at(left,px(72),lw,px(40)));
    // Each card holds RowHeight rows inside a 4px inset.
    const int cardTops[4]{px(152),px(280),px(408),px(536)},cardRows[4]{2,2,2,4};
    for(int i=0;i<4;++i)app.cards[i]=at(left,cardTops[i],lw,px(8)+cardRows[i]*px(RowHeight));
    auto row=[&](int id,int card,int index){
        place(id,at(left+px(4),cardTops[card]+px(4)+index*px(RowHeight),lw-px(8),px(RowHeight)));
    };
    row(Mode,0,0);row(Bind,0,1);
    row(Method,1,0);row(Enter,1,1);
    row(Theme,2,0);row(Glass,2,1);
    row(Startup,3,0);row(Tray,3,1);row(Hidden,3,2);
    app.limitBox=at(left+lw-px(16)-px(72),cardTops[3]+px(4)+3*px(RowHeight)+px(4),px(72),px(32));
    HDC editDC=GetDC(app.controls[Limit]);auto previousFont=SelectObject(editDC,app.font);
    TEXTMETRICW metrics{};GetTextMetricsW(editDC,&metrics);
    SelectObject(editDC,previousFont);ReleaseDC(app.controls[Limit],editDC);
    const RECT& box=app.limitBox;
    const int editTop=(box.top+box.bottom-static_cast<int>(metrics.tmHeight))/2;
    place(Limit,RECT{box.left+px(10),editTop,box.right-px(10),editTop+static_cast<int>(metrics.tmHeight)});
    place(Recover,at(left,px(744),lw,px(32)));
    app.historyPanel=at(right,px(96),rw,h-px(380));
    app.previewPanel=at(right,h-px(268),rw,px(120));
    const RECT& list=app.historyPanel;const RECT& preview=app.previewPanel;
    place(History,RECT{list.left+px(4),list.top+px(4),list.right-px(18),list.bottom-px(4)});
    place(Preview,RECT{preview.left+px(12),preview.top+px(10),preview.right-px(22),preview.bottom-px(10)});
    place(Copy,at(right,h-px(132),(rw-px(8))/2,px(32)));
    place(Delete,at(right+(rw+px(8))/2,h-px(132),rw-(rw+px(8))/2,px(32)));
    place(Clear,at(right,h-px(92),rw,px(32)));
    place(Status,at(left,h-px(32),r.right-px(48),px(20)));
    app.scrollbars[0].layout(r.right-px(12),px(8),px(8),r.bottom-px(16));
    app.scrollbars[1].layout(list.right-px(12),list.top+px(8),px(6),list.bottom-list.top-px(16));
    app.scrollbars[2].layout(preview.right-px(12),preview.top+px(8),px(6),preview.bottom-preview.top-px(16));
    // Layered children are not repainted by a move or resize alone.
    for(int id:{History,Preview,Limit})RedrawWindow(app.controls[id],nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME);
    syncScrollbars();
    app.visualDirty=true;
    InvalidateRect(app.window,nullptr,FALSE);
}
void showMain() { ShowWindow(app.window,SW_RESTORE); SetForegroundWindow(app.window); }
void showPopup() {
    HMONITOR monitor=MonitorFromWindow(app.target,MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(monitor,&mi);
    UINT xdpi=96,ydpi=96;GetDpiForMonitor(monitor,MDT_EFFECTIVE_DPI,&xdpi,&ydpi);
    app.popupDpi=xdpi;
    const int w=MulDiv(PopupWidth,xdpi,96),h=MulDiv(PopupHeight,xdpi,96);
    SetWindowPos(app.popup,HWND_TOPMOST,(mi.rcWork.left+mi.rcWork.right-w)/2,mi.rcWork.top+MulDiv(16,xdpi,96),w,h,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    app.frameTick=std::chrono::steady_clock::now();app.audioTick=GetTickCount64();
}
void stop(bool cancel) {
    if(!app.recording&&!app.finishing) return;
    const bool newlyCancelled=cancel&&!app.cancelling;
    app.recording=false; app.finishing=true; app.cancelling=app.cancelling||cancel;
    app.level=0;app.spectrum.fill(0);
    app.speech.stop(app.cancelling);
    if(newlyCancelled)play(Cue::Stopped);
    SetWindowTextW(app.controls[Record],app.cancelling?L"Cancelling...":L"Finishing...");
    setStatus(app.cancelling?L"Cancelling transcription...":L"Finishing last words...");
}
void start() {
    if(app.recording||app.finishing||!app.pending.empty()||app.binding) return;
    app.target=GetForegroundWindow();
    // A click inside TalkText has no external insertion target; preserve result in history.
    if(app.target==app.window) app.target=nullptr;
    if(!app.speech.ready())app.speech.prepare(app.window,appDirectory());
    app.speech.start();
    app.recording=true;app.cancelling=false;app.level=0;app.visualLevel=0;app.centroid=0;app.activeSeconds=0;
    app.spectrum.fill(0);app.visualSpectrum.fill(0);
    play(Cue::Start);
    SetWindowTextW(app.controls[Record],L"Stop transcription");
    setStatus(app.speech.ready()?L"Starting microphone...":L"Capturing audio while Nemotron prepares...");
    showPopup();
}
void finishBinding() {
    if(!app.binding) return;
    app.hotkeys.capture(false); app.binding=false;
    if(!app.captured.empty()) {app.settings.hotkey=app.captured;app.hotkeys.setChord(app.settings.hotkey);save();}
    refreshSettings(); setStatus(L"Hotkey saved. Escape always cancels transcription.");
}
void quit() {
    if(hasPendingClipboard()) {
        std::wstring result=restorePendingClipboard();
        if(hasPendingClipboard()){showMain();setStatus(result+L" Close the app holding the clipboard, then click Restore clipboard.");return;}
    }
    DestroyWindow(app.window);
}
void command(int id,int notification) {
    // Owner-drawn buttons also send focus/paint notifications, not just clicks.
    if(id!=History&&id!=Limit&&notification!=BN_CLICKED)return;
    switch(id) {
    case Record: if(app.recording) stop(false); else start(); break;
    case Startup: setStartup(!app.settings.startWithWindows);app.settings.startWithWindows=!app.settings.startWithWindows;save();break;
    case Tray: app.settings.keepInTray=!app.settings.keepInTray;save();break;
    case Hidden: app.settings.startHidden=!app.settings.startHidden;save();break;
    case Enter: app.settings.autoEnter=!app.settings.autoEnter;save();break;
    case Mode: app.settings.pushToTalk=!app.settings.pushToTalk;save();break;
    case Method: app.settings.method=static_cast<InsertMethod>((static_cast<int>(app.settings.method)+1)%3);save();break;
    case Theme: app.settings.theme=(app.settings.theme+1)%static_cast<uint32_t>(theme::presets.size());save();applyAppearance();break;
    case Glass: app.settings.frostedGlass=(app.settings.frostedGlass+20)%100;save();applyAppearance();break;
    case Bind:
        if(app.recording||app.finishing) {setStatus(L"Stop transcription before changing hotkey.");break;}
        if(app.binding) {finishBinding();break;}
        app.binding=true;app.captured.clear();app.hotkeys.capture(true);
        SetWindowTextW(app.controls[Bind],L"Press keys; click here to save; Esc discards");
        setStatus(L"Press desired keys or mouse buttons. Click hotkey button to save. Escape is reserved."); return;
    case Limit:
        if(notification==EN_KILLFOCUS) {
            wchar_t value[32]{};GetWindowTextW(app.controls[Limit],value,32);
            wchar_t* end{};unsigned long n=wcstoul(value,&end,10);
            if(!*value||*end||n>100000) {setStatus(L"History limit must be 0 through 100000; 0 disables history.");break;}
            app.settings.historyLimit=n;
            if(app.history.size()>n) app.history.resize(n);
            saveHistory(app.history);save();historyList();
        } else return;
        break;
    case History:
        if(notification==LBN_SELCHANGE) {
            LRESULT selected=SendMessageW(app.controls[History],LB_GETCURSEL,0,0);
            if(selected>=0&&static_cast<size_t>(selected)<app.history.size()) SetWindowTextW(app.controls[Preview],app.history[selected].text.c_str());
        } return;
    case Copy: {
        LRESULT selected=SendMessageW(app.controls[History],LB_GETCURSEL,0,0);
        if(selected<0) {setStatus(L"Select a history entry first.");break;}
        std::wstring result=insertText(nullptr,app.history[selected].text,InsertMethod::ClipboardOnly,false);
        setStatus(result.empty()?L"Copied selected transcription.":result); break;
    }
    case Delete: {
        LRESULT selected=SendMessageW(app.controls[History],LB_GETCURSEL,0,0);
        if(selected>=0) {app.history.erase(app.history.begin()+selected);saveHistory(app.history);historyList();} break;
    }
    case Clear: app.history.clear();saveHistory(app.history);historyList();break;
    case Quit: quit();return;
    case Recover: {
        std::wstring result=restorePendingClipboard();
        setStatus(result.empty()?L"Original clipboard restored.":result);
        ShowWindow(app.controls[Recover],hasPendingClipboard()?SW_SHOW:SW_HIDE);return;
    }
    default:return;
    }
    if(!app.binding) refreshSettings();
}
void speechEvent(SpeechEvent* raw) {
    std::unique_ptr<SpeechEvent> event(raw);
    if(event->kind==SpeechEvent::Kind::Level) {
        if(app.recording){app.level=event->level;app.spectrum=event->spectrum;app.audioTick=GetTickCount64();}
        return;
    }
    if(event->kind==SpeechEvent::Kind::Partial) {
        if(!app.cancelling&&(app.recording||app.finishing))SetWindowTextW(app.controls[Preview],event->text.c_str());
        return;
    }
    if(event->kind==SpeechEvent::Kind::Listening) {
        if(app.recording)setStatus(app.speech.ready()?L"Listening. Nemotron streaming on GPU.":L"Microphone live. Preparing Nemotron once...");
        return;
    }
    if(event->kind==SpeechEvent::Kind::Ready) {
        if(!app.finishing)setStatus(app.recording?L"Listening. Nemotron streaming on GPU.":L"Ready. Nemotron kept warm for instant recording.");
        return;
    }
    if(event->kind==SpeechEvent::Kind::Status) {if(!app.cancelling)setStatus(event->text);return;}
    const bool cancelled=app.cancelling;
    app.recording=false;app.finishing=false;app.cancelling=false;ShowWindow(app.popup,SW_HIDE);
    SetWindowTextW(app.controls[Record],L"Start transcription");
    if(event->kind==SpeechEvent::Kind::Error) {if(!cancelled)play(Cue::Stopped);setStatus(event->text);return;}
    if(event->kind==SpeechEvent::Kind::Cancelled) {
        if(!cancelled)play(Cue::Stopped);
        setStatus(app.speech.ready()?L"Cancelled. Nemotron stays ready.":L"Cancelled. Speech engine preparing in background.");return;
    }
    if(event->text.empty()) {play(Cue::Stopped);setStatus(L"No speech detected. Nemotron stays ready.");return;}
    SetWindowTextW(app.controls[Preview],event->text.c_str());
    try {addHistory(event->text);} catch(const std::exception& e) {error(e);}
    SetWindowTextW(app.controls[Preview],event->text.c_str());
    if(!app.target&&app.settings.method!=InsertMethod::ClipboardOnly) {play(Cue::Complete);setStatus(L"Saved. Start with hotkey from another app for automatic insertion.");return;}
    app.pending=event->text;app.deliveryStarted=GetTickCount64();SetTimer(app.window,DeliveryTimer,25,nullptr);
}
void delivery() {
    bool down=false;
    for(UINT key:app.settings.hotkey) down=down||((GetAsyncKeyState(key)&0x8000)!=0);
    for(int key:{VK_CONTROL,VK_SHIFT,VK_MENU,VK_LWIN,VK_RWIN}) down=down||((GetAsyncKeyState(key)&0x8000)!=0);
    if(down && GetTickCount64()-app.deliveryStarted<10000) return;
    KillTimer(app.window,DeliveryTimer);
    if(down) {app.pending.clear();play(Cue::Stopped);setStatus(L"Insertion skipped: release hotkey/modifier keys. Text remains in preview.");return;}
    std::wstring result=insertText(app.target,app.pending,app.settings.method,app.settings.autoEnter);
    play(result.empty()?Cue::Complete:Cue::Stopped);
    app.pending.clear();setStatus(result.empty()?L"Done. Nemotron stays ready.":result+L" Text remains in preview.");
}
void updateVoiceVisual() {
    const ULONGLONG now=GetTickCount64();
    const auto tick=std::chrono::steady_clock::now();
    const float dt=std::min(0.1f,std::chrono::duration<float>(tick-app.frameTick).count());
    app.frameTick=tick;
    if(now-app.audioTick>160){app.level=0;app.spectrum.fill(0);}
    // PCM RMS is already x4. Map normal speech to a useful visual range,
    // suppress low-level mic noise, attack promptly, and let the tail settle.
    const float amplitude=std::clamp((app.level-0.008f)*6.0f,0.0f,1.0f);
    const float response=amplitude>app.visualLevel?32.0f:6.25f;
    app.visualLevel+=(amplitude-app.visualLevel)*(1.0f-std::exp(-dt*response));
    if(app.visualLevel>0.002f&&!app.finishing&&app.motion)app.activeSeconds+=dt*(0.5f+1.5f*app.visualLevel);
    const float blend=1.0f-std::exp(-dt*18.0f);
    float energy=0,weighted=0;
    for(size_t i=0;i<app.spectrum.size();++i) {
        app.visualSpectrum[i]+=(app.spectrum[i]-app.visualSpectrum[i])*blend;
        energy+=app.visualSpectrum[i];weighted+=app.visualSpectrum[i]*static_cast<float>(i)/7.0f;
    }
    if(energy>0.005f)app.centroid+=(weighted/energy-app.centroid)*blend;
}
void paintPopup(HWND hwnd) {
    gpu::VoiceFrame frame;
    frame.amplitude=app.visualLevel;frame.centroid=app.centroid;frame.seconds=app.activeSeconds;
    frame.finishing=app.finishing;frame.reducedMotion=!app.motion;frame.bands=app.visualSpectrum;
    gpu::overlay(hwnd,app.popupDpi/96.0f,frame);
}
LRESULT CALLBACK popupProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    try {
        switch(msg) {
        case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
        case WM_ERASEBKGND:return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};BeginPaint(hwnd,&ps);EndPaint(hwnd,&ps);
            return 0;
        }
        }
    } catch(const std::exception& e) {
        ShowWindow(hwnd,SW_HIDE);
        setStatus(L"Recording visualization unavailable: "+widen(e.what())+L". Transcription remains active.");
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
bool settingsRow(int id) {
    return id==Mode||id==Bind||id==Method||id==Enter||id==Theme||id==Glass||id==Startup||id==Tray||id==Hidden;
}
void paintControl(const DRAWITEMSTRUCT& d) {
    if(d.CtlID==History) {
        FillRect(d.hDC,&d.rcItem,app.surface);
        if(d.itemID>=app.history.size())return;
        RECT r=d.rcItem;InflateRect(&r,0,-px(2));
        if(d.itemState&ODS_SELECTED)visuals::selection(d.hDC,r,app.dpi/96.0f,(d.itemState&ODS_FOCUS)!=0);
        SetTextColor(d.hDC,theme::colours.text);SetBkMode(d.hDC,TRANSPARENT);SelectObject(d.hDC,app.readingFont);
        const auto& entry=app.history[d.itemID];
        std::wstring text=entry.text.substr(0,240);
        std::replace(text.begin(),text.end(),L'\n',L' ');
        InflateRect(&r,-px(12),0);r.top+=px(5);r.bottom=r.top+px(20);
        DrawTextW(d.hDC,text.c_str(),-1,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
        r.top=r.bottom;r.bottom=r.top+px(18);
        SelectObject(d.hDC,app.smallFont);SetTextColor(d.hDC,theme::colours.muted);
        DrawTextW(d.hDC,entry.timestamp.c_str(),-1,&r,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
        return;
    }
    // Owner-drawn buttons are input and accessibility proxies; the composition frame draws them.
    app.visualDirty=true;
}
void renderControl(int id) {
    HWND control=app.controls[id];
    if(!IsWindowVisible(control))return;
    RECT rect{};GetWindowRect(control,&rect);MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT*>(&rect),2);
    const LRESULT native=SendMessageW(control,BM_GETSTATE,0,0);
    using visuals::Icon;
    Icon icon=Icon::None;
    switch(id) {
    case Record:icon=Icon::Microphone;break;case Quit:icon=Icon::Power;break;
    case Copy:case Recover:icon=Icon::Copy;break;case Delete:case Clear:icon=Icon::Trash;break;
    }
    visuals::ButtonState state;
    state.pressed=(native&BST_PUSHED)!=0;
    state.focused=GetFocus()==control;
    state.disabled=!IsWindowEnabled(control);
    state.primary=id==Record;
    state.row=settingsRow(id);
    state.toggle=id==Startup||id==Tray||id==Hidden||id==Enter;
    state.position=app.togglePosition[id];
    state.light=app.hoverLight[id];
    wchar_t text[512]{};GetWindowTextW(control,text,512);
    // Accessible native text retains explicit on/off; visual switch owns that cue.
    if(state.toggle)if(wchar_t* suffix=wcsrchr(text,L'(');suffix&&suffix>text)*(suffix-1)=0;
    visuals::button(control,rect,text,app.font,icon,state);
}
// The whole client is one composition frame (WS_EX_NOREDIRECTIONBITMAP): canvas,
// header, cards and controls. Only layered native panels draw above it.
void renderMain() {
    // Native/COM calls may dispatch resize or paint messages during this frame.
    // Preserve those invalidations for the next frame rather than clearing them.
    app.visualDirty=false;
    const auto& c=theme::colours;
    const bool glass=theme::glass>0;
    const float scale=app.dpi/96.0f;
    RECT r{};GetClientRect(app.window,&r);
    gpu::begin(app.window);
    auto fill=[](const RECT& rect,COLORREF colour,float alpha,float radius,bool outlined) {
        gpu::Shape shape;shape.radius=radius;shape.fill=colour;shape.fillAlpha=alpha;
        if(outlined){shape.border=theme::colours.border;shape.borderWidth=1.0f;}
        gpu::material(rect,shape);
    };
    auto surface=[&](const RECT& rect,COLORREF colour,float radius) {
        const theme::Fill layer=theme::layer(colour);fill(rect,layer.colour,layer.alpha,radius,true);
    };
    auto divider=[&](int y){fill(RECT{0,y,r.right,y+std::max(1,px(1))},c.border,1.0f,0,false);};
    constexpr UINT line=DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS;
    fill(r,c.canvas,1.0f-theme::glass,0,false);
    // Header: brand mark and product name over a hairline, as Orchard's header.
    const int left=px(24),right=app.historyPanel.left,header=px(HeaderHeight)-app.scroll;
    const int logoTop=header-(px(HeaderHeight)+px(20))/2;
    visuals::logo(&keys.logo,RECT{left,logoTop,left+px(20),logoTop+px(20)},app.logo);
    HDC measure=GetDC(app.window);auto previous=SelectObject(measure,app.titleFont);
    SIZE title{};GetTextExtentPoint32W(measure,L"TalkText",8,&title);
    SelectObject(measure,previous);ReleaseDC(app.window,measure);
    const int titleLeft=left+px(30),quitLeft=app.historyPanel.right-px(128);
    visuals::text(&keys.title,RECT{titleLeft,header-px(HeaderHeight),titleLeft+title.cx+px(2),header},L"TalkText",app.titleFont,c.text,line);
    visuals::text(&keys.tagline,RECT{titleLeft+title.cx+px(10),header-px(HeaderHeight),quitLeft-px(16),header},
        L"Local voice to text",app.smallFont,c.muted,line);
    divider(header-std::max(1,px(1)));
    // Section labels: 12px medium, muted, uppercase, 20px above their surface.
    auto section=[&](const void* key,int x,int top,const wchar_t* text) {
        visuals::text(key,RECT{x,top-px(22),x+px(240),top-px(4)},text,app.labelFont,c.muted,line);
    };
    section(&keys.input,left,app.cards[0].top,L"INPUT");
    section(&keys.delivery,left,app.cards[1].top,L"TEXT DELIVERY");
    section(&keys.appearance,left,app.cards[2].top,L"APPEARANCE");
    section(&keys.behavior,left,app.cards[3].top,L"APP BEHAVIOR");
    section(&keys.history,right,app.historyPanel.top,L"HISTORY");
    for(const RECT& card:app.cards)surface(card,c.surface,theme::RadiusSurface*scale);
    // Native panels sit inside these; they match the panels' near-opaque floating fill.
    const float floating=glass?theme::FloatingAlpha:1.0f;
    for(const RECT& panel:{app.historyPanel,app.previewPanel})fill(panel,c.surface,floating,theme::RadiusSurface*scale,true);
    fill(app.limitBox,c.canvas,floating,theme::RadiusControl*scale,true);
    const RECT& limits=app.cards[3];
    visuals::text(&keys.limit,RECT{left+px(16),limits.bottom-px(4)-px(RowHeight),app.limitBox.left-px(8),limits.bottom-px(4)},
        L"History limit",app.font,c.text,line);
    visuals::text(&keys.helper,RECT{left,limits.bottom+px(6),left+(app.cards[3].right-left),limits.bottom+px(24)},
        L"0 turns history off. Stored only on this computer.",app.smallFont,c.muted,line);
    divider(app.contentHeight-app.scroll-px(44));
    visuals::text(&keys.status,RECT{left,app.contentHeight-app.scroll-px(34),r.right-px(24),app.contentHeight-app.scroll-px(10)},
        app.status,app.smallFont,c.muted,line);
    for(int id:{Record,Mode,Bind,Method,Enter,Theme,Glass,Startup,Tray,Hidden,Quit,Copy,Delete,Clear,Recover})renderControl(id);
    gpu::present(app.window);
}
void paintMain(HWND hwnd) {
    PAINTSTRUCT ps{};BeginPaint(hwnd,&ps);EndPaint(hwnd,&ps);
    app.visualDirty=true;
}
void updateHover() {
    const auto now=std::chrono::steady_clock::now();
    const float blend=app.motion?1.0f-std::exp(-std::chrono::duration<float>(now-app.hoverTick).count()/0.055f):1.0f;
    app.hoverTick=now;
    bool moving=false;
    for(int i=Record;i<=LastControl;++i) {
        const float target=app.hovered==i?1.0f:0.0f;
        bool changed=false;
        if(app.hoverLight[i]!=target) {
            app.hoverLight[i]+=(target-app.hoverLight[i])*blend;
            if(std::abs(target-app.hoverLight[i])<0.01f)app.hoverLight[i]=target;
            else moving=true;
            changed=true;
        }
        const float position=checked(i)?1.0f:0.0f;
        if(app.togglePosition[i]!=position) {
            app.togglePosition[i]+=(position-app.togglePosition[i])*blend;
            if(std::abs(position-app.togglePosition[i])<0.002f)app.togglePosition[i]=position;
            else moving=true;
            changed=true;
        }
        if(changed)app.visualDirty=true;
    }
    app.animating=moving;
}
LRESULT CALLBACK windowProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    try {
        if(msg==TaskbarCreated){app.trayAdded=false;updateTray();return 0;}
        switch(msg) {
        case WM_CREATE:
            app.window=hwnd;app.dpi=GetDpiForWindow(hwnd);
            // An elevated TalkText must still hear a non-elevated shell's broadcast.
            ChangeWindowMessageFilterEx(hwnd,TaskbarCreated,MSGFLT_ALLOW,nullptr);
            return 0;
        case WM_SIZE: if(app.controls[Record])layout();return 0;
        case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize={px(820),px(420)};return 0;
        case WM_DPICHANGED: {
            app.dpi=HIWORD(wp);updateFonts();
            auto* bounds=reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd,nullptr,bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top,SWP_NOZORDER|SWP_NOACTIVATE);
            layout();return 0;
        }
        case WM_SETTINGCHANGE: {
            BOOL motion=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&motion,0);app.motion=motion!=FALSE;
            animate();return 0;
        }
        case WM_VSCROLL: {
            const HWND source=reinterpret_cast<HWND>(lp);
            if(source==app.scrollbars[1].window) {
                SendMessageW(app.controls[History],LB_SETTOPINDEX,app.scrollbars[1].destination(LOWORD(wp)),0);
            } else if(source==app.scrollbars[2].window) {
                const int first=static_cast<int>(SendMessageW(app.controls[Preview],EM_GETFIRSTVISIBLELINE,0,0));
                SendMessageW(app.controls[Preview],EM_LINESCROLL,0,app.scrollbars[2].destination(LOWORD(wp))-first);
            } else {app.scroll=app.scrollbars[0].destination(LOWORD(wp),px(40));layout();}
            syncScrollbars();return 0;
        }
        case WM_MOUSEWHEEL:app.scroll-=GET_WHEEL_DELTA_WPARAM(wp)*px(60)/WHEEL_DELTA;layout();return 0;
        case WM_ERASEBKGND:return 1;
        case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX: {
            const int id=GetDlgCtrlID(reinterpret_cast<HWND>(lp));
            const bool panel=id==Preview||id==History;
            HDC dc=reinterpret_cast<HDC>(wp);
            SetTextColor(dc,id==Status?theme::colours.muted:theme::colours.text);SetBkColor(dc,panel?theme::colours.surface:theme::colours.canvas);
            return reinterpret_cast<LRESULT>(panel?app.surface:app.canvas);
        }
        case WM_MEASUREITEM:reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight=px(HistoryRow);return TRUE;
        case WM_DRAWITEM:paintControl(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));return TRUE;
        case WM_PAINT:paintMain(hwnd);return 0;
        case WM_COMMAND:command(LOWORD(wp),HIWORD(wp));return 0;
        case WM_SPEECH:speechEvent(reinterpret_cast<SpeechEvent*>(lp));return 0;
        case WM_HOTKEY_CAPTURE:
            if(app.binding) {
                if(std::find(app.captured.begin(),app.captured.end(),static_cast<UINT>(wp))==app.captured.end()) {
                    POINT cursor{};GetCursorPos(&cursor);
                    if(wp==VK_LBUTTON&&WindowFromPoint(cursor)==app.controls[Bind])return 0;
                    app.captured.push_back(static_cast<UINT>(wp));SetWindowTextW(app.controls[Bind],(L"Save chord: "+chordName(app.captured)).c_str());
                }
            } return 0;
        case WM_HOTKEY_EVENT:
            if(wp==3) {
                if(app.binding){app.hotkeys.capture(false);app.binding=false;refreshSettings();setStatus(L"Hotkey change discarded.");}
                if(!app.pending.empty()){KillTimer(hwnd,DeliveryTimer);app.pending.clear();play(Cue::Stopped);setStatus(L"Insertion cancelled. Text remains in preview.");}
                stop(true);
            }
            else if(wp==1) {if(app.recording&&!app.settings.pushToTalk)stop(false);else start();}
            else if(wp==2&&app.settings.pushToTalk)stop(false);
            return 0;
        case WM_TIMER:if(wp==DeliveryTimer)delivery();return 0;
        case TrayMessage:
            if(lp==WM_LBUTTONUP||lp==WM_LBUTTONDBLCLK)showMain();
            else if(lp==WM_RBUTTONUP) {
                HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,1,L"Open TalkText");AppendMenuW(menu,MF_STRING,2,L"Quit");
                POINT p{};GetCursorPos(&p);SetForegroundWindow(hwnd);int choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,hwnd,nullptr);DestroyMenu(menu);
                if(choice==1)showMain();else if(choice==2)quit();
            }return 0;
        case WM_CLOSE:if(app.settings.keepInTray&&app.trayAdded){ShowWindow(hwnd,SW_HIDE);return 0;}quit();return 0;
        case WM_DESTROY: {
            app.hotkeys.uninstall();app.speech.shutdown();gpu::release(app.popup);DestroyWindow(app.popup);gpu::release(hwnd);
            NOTIFYICONDATAW ni{sizeof(ni)};ni.hWnd=hwnd;ni.uID=1;Shell_NotifyIconW(NIM_DELETE,&ni);PostQuitMessage(0);return 0;
        }
        }
    } catch(const std::exception& e) {error(e);}
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void createWindows(HINSTANCE instance) {
        app.dpi=GetDpiForSystem();updateFonts();
        WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=windowProc;wc.lpszClassName=L"TalkText.Main";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));RegisterClassW(&wc);
        RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
        const int width=std::min(px(960),static_cast<int>(work.right-work.left));
        const int height=std::min(px(860),static_cast<int>(work.bottom-work.top));
        // No GDI surface: the composition frame is the client, so frosted glass shows through it.
        CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP,wc.lpszClassName,L"TalkText",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
            work.left+(work.right-work.left-width)/2,work.top+(work.bottom-work.top-height)/2,width,height,nullptr,nullptr,instance,nullptr);
        if(!app.window)throw std::runtime_error("Cannot create TalkText window");
        // The in-client header carries the brand; the caption keeps only window controls.
        WTA_OPTIONS caption{WTNCA_NODRAWCAPTION|WTNCA_NODRAWICON,WTNCA_NODRAWCAPTION|WTNCA_NODRAWICON};
        SetWindowThemeAttribute(app.window,WTA_NONCLIENT,&caption,sizeof(caption));
        BOOL motion=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&motion,0);app.motion=motion!=FALSE;
        button(Record,L"Start transcription");button(Mode,L"");button(Bind,L"");button(Method,L"");button(Enter,L"");
        button(Theme,L"");button(Glass,L"");button(Startup,L"");button(Tray,L"");button(Hidden,L"");
        control(Limit,L"EDIT",L"100",ES_NUMBER|ES_AUTOHSCROLL);
        control(History,L"LISTBOX",L"",LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS);
        control(Preview,L"EDIT",L"",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL);
        button(Recover,L"Restore clipboard");ShowWindow(app.controls[Recover],SW_HIDE);
        button(Copy,L"Copy selected");button(Delete,L"Delete selected");button(Clear,L"Clear history");button(Quit,L"Quit TalkText");
        control(Status,L"STATIC",app.status.c_str(),SS_LEFT);
        SendMessageW(app.controls[Status],WM_SETFONT,reinterpret_cast<WPARAM>(app.smallFont),TRUE);
        updateFonts();
        wc.lpfnWndProc=popupProc;wc.lpszClassName=L"TalkText.Overlay";wc.hbrBackground=nullptr;RegisterClassW(&wc);
        app.popup=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP,wc.lpszClassName,L"TalkText recording",WS_POPUP,0,0,PopupWidth,PopupHeight,app.window,nullptr,instance,nullptr);
        app.scrollbars[0].create(app.window,200,theme::colours.canvas);
        app.scrollbars[1].create(app.window,201,theme::colours.surface);
        app.scrollbars[2].create(app.window,202,theme::colours.surface);
        applyAppearance();
        refreshSettings();historyList();layout();
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    struct GraphicsSession {
        ULONG_PTR token{};
        GraphicsSession(){Gdiplus::GdiplusStartupInput input;Gdiplus::GdiplusStartup(&token,&input,nullptr);}
        ~GraphicsSession(){gpu::shutdown();if(token)Gdiplus::GdiplusShutdown(token);}
    } graphics;
    int argc{};LPWSTR* args=CommandLineToArgvW(GetCommandLineW(),&argc);
    try {
        if(argc>=4&&std::wstring(args[1])==L"--transcribe") {
            std::wstring text=SpeechSession::transcribeFile(appDirectory(),args[2]);
            int n=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
            std::string utf8(n,'\0');WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),utf8.data(),n,nullptr,nullptr);
            std::ofstream out(std::filesystem::path(args[3]),std::ios::binary);out<<utf8;if(!out)throw std::runtime_error("Cannot write transcription output");
            LocalFree(args);OleUninitialize();return 0;
        }
        HANDLE singleton=CreateMutexW(nullptr,FALSE,L"Local\\TalkText.App");
        if(GetLastError()==ERROR_ALREADY_EXISTS) {
            HWND existing=FindWindowW(L"TalkText.Main",nullptr);if(existing){ShowWindow(existing,SW_RESTORE);SetForegroundWindow(existing);}
            if(singleton)CloseHandle(singleton);LocalFree(args);OleUninitialize();return 0;
        }
        gpu::initialize();
        app.settings=loadSettings();app.history=loadHistory();
        if(app.settings.startWithWindows)setStartup(true);
        if(app.history.size()>app.settings.historyLimit){app.history.resize(app.settings.historyLimit);saveHistory(app.history);}
        createWindows(instance);
        updateTray();
        app.speech.prepare(app.window,appDirectory());
        app.hotkeys.install(app.window,app.settings.hotkey);
        if(!app.settings.startHidden)ShowWindow(app.window,SW_SHOW);
        MSG msg{};bool running=true;
        while(running) {
            while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
                if(msg.message==WM_QUIT){running=false;break;}
                if(msg.message==WM_KEYDOWN&&msg.wParam==VK_ESCAPE&&!app.binding){stop(true);continue;}
                if(app.binding||!IsDialogMessageW(app.window,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}
            }
            if(!running)break;
            HANDLE handles[2]{};HWND windows[2]{};DWORD count=0;
            if(IsWindowVisible(app.window)&&!IsIconic(app.window)&&(app.visualDirty||app.animating)) {
                windows[count]=app.window;handles[count++]=gpu::frameReady(app.window);
            }
            if(IsWindowVisible(app.popup)) {
                windows[count]=app.popup;handles[count++]=gpu::frameReady(app.popup);
            }
            const DWORD ready=MsgWaitForMultipleObjectsEx(count,handles,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            if(ready==WAIT_FAILED)throw std::runtime_error("Cannot wait for display frame");
            if(ready>=WAIT_OBJECT_0&&ready<WAIT_OBJECT_0+count) {
                // Service both ready surfaces; a continuously active main surface
                // must not starve the microphone overlay on the same display.
                for(DWORD i=0;i<count;++i)if(i==ready-WAIT_OBJECT_0||WaitForSingleObject(handles[i],0)==WAIT_OBJECT_0) {
                    if(windows[i]==app.window){updateHover();renderMain();}
                    else {updateVoiceVisual();paintPopup(app.popup);}
                }
                gpu::synchronize();
            }
        }
        if(singleton)CloseHandle(singleton);LocalFree(args);OleFlushClipboard();OleUninitialize();return 0;
    } catch(const std::exception& e) {
        if(argc>=4&&std::wstring(args[1])==L"--transcribe") {std::ofstream out{std::filesystem::path(args[3])};out<<"ERROR: "<<e.what();}
        else MessageBoxW(nullptr,widen(e.what()).c_str(),L"TalkText",MB_OK|MB_ICONERROR);
        LocalFree(args);OleUninitialize();return 1;
    }
}

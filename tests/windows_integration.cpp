#include "platform.h"
#include <windows.h>
#include <ole2.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstring>
#include <vector>
#include <thread>
#include <future>
#include <atomic>
using namespace talktext;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void pump(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
void put(UINT format,const void* data,size_t n){HGLOBAL h=GlobalAlloc(GMEM_MOVEABLE,n);require(h!=nullptr,"allocation");void* p=GlobalLock(h);std::memcpy(p,data,n);GlobalUnlock(h);require(SetClipboardData(format,h)!=nullptr,"set clipboard");}
void check(UINT format,const void* data,size_t n){HANDLE h=GetClipboardData(format);require(h!=nullptr,"clipboard format missing");void* p=GlobalLock(h);require(p&&GlobalSize(h)>=n,"clipboard media invalid");bool equal=std::memcmp(p,data,n)==0;GlobalUnlock(h);require(equal,"clipboard payload changed");}
std::atomic<int> clipboardScenario{0};
WNDPROC originalEditProc{};
LRESULT CALLBACK testEditProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    LRESULT result=CallWindowProcW(originalEditProc,hwnd,message,wp,lp);
    if(message==WM_PASTE||(message==WM_CHAR&&wp==22)) {
        int scenario=clipboardScenario.exchange(0);
        if(scenario&&OpenClipboard(hwnd)) {
            if(scenario==1)Sleep(1700);
            else {const wchar_t replacement[]=L"New user clipboard";EmptyClipboard();put(CF_UNICODETEXT,replacement,sizeof(replacement));}
            CloseClipboard();
        }
    }
    return result;
}
class TargetWindow {
public:
    HWND window{}, edit{};
    std::thread thread;
    TargetWindow() {
        std::promise<void> ready;auto started=ready.get_future();
        thread=std::thread([this,&ready] {
            window=CreateWindowExW(0,L"STATIC",L"TalkText delivery regression",WS_OVERLAPPEDWINDOW|WS_VISIBLE,100,100,640,200,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_BORDER|ES_MULTILINE,10,10,600,120,window,nullptr,GetModuleHandleW(nullptr),nullptr);
            originalEditProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(edit,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(testEditProc)));
            SetWindowPos(window,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
            DWORD foregroundThread=GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
            BOOL attached=AttachThreadInput(GetCurrentThreadId(),foregroundThread,TRUE);
            SetForegroundWindow(window);SetFocus(edit);
            if(attached)AttachThreadInput(GetCurrentThreadId(),foregroundThread,FALSE);
            ready.set_value();
            MSG m{};while(IsWindow(window)&&GetMessageW(&m,nullptr,0,0)>0){TranslateMessage(&m);DispatchMessageW(&m);}
        });
        started.wait();
    }
    ~TargetWindow(){PostMessageW(window,WM_CLOSE,0,0);thread.join();}
};
class ClipboardBackup {
    std::vector<std::pair<UINT,HANDLE>> formats_;
    HWND window_;
public:
    explicit ClipboardBackup(HWND window):window_(window) {
        require(OpenClipboard(window)!=0,"cannot snapshot existing clipboard");
        for(UINT format=EnumClipboardFormats(0);format;format=EnumClipboardFormats(format)) {
            HANDLE copy=OleDuplicateData(GetClipboardData(format),static_cast<CLIPFORMAT>(format),0);
            if(!copy){CloseClipboard();throw std::runtime_error("existing clipboard cannot be safely preserved; regression not run");}
            formats_.emplace_back(format,copy);
        }
        CloseClipboard();
    }
    ~ClipboardBackup() {
        CloseClipboard();
        if(OpenClipboard(window_)) {
            EmptyClipboard();
            for(auto [format,handle]:formats_)SetClipboardData(format,handle);
            CloseClipboard();
        }
    }
};
}
int main(){
    OleInitialize(nullptr);
    HWND window{};
    try {
        TargetWindow target;window=target.window;HWND edit=target.edit;
        SetForegroundWindow(window);Sleep(150);
        require(GetForegroundWindow()==window,"test window could not get foreground");
        ClipboardBackup backup(window);
        const wchar_t oldText[]=L"Original clipboard text";
        const unsigned char custom[]={0,1,2,3,254,255};
        struct Dib {BITMAPINFOHEADER header;DWORD pixel;} dib{};
        dib.header.biSize=sizeof(BITMAPINFOHEADER);dib.header.biWidth=1;dib.header.biHeight=1;dib.header.biPlanes=1;dib.header.biBitCount=32;dib.header.biSizeImage=4;dib.pixel=0x00123456;
        UINT format=RegisterClipboardFormatW(L"TalkText.Regression.Binary");
        require(OpenClipboard(window)!=0,"open clipboard");EmptyClipboard();put(CF_UNICODETEXT,oldText,sizeof(oldText));put(CF_DIB,&dib,sizeof(dib));put(format,custom,sizeof(custom));CloseClipboard();
        std::wstring result=insertText(window,L"Paste result.",InsertMethod::Paste,false);
        if(!result.empty())std::wcerr<<result<<L'\n';
        require(result.empty(),"paste delivery reported failure");pump();
        wchar_t text[512]{};GetWindowTextW(edit,text,512);require(std::wstring(text)==L"Paste result.","paste text not delivered");
        require(OpenClipboard(window)!=0,"open restored clipboard");check(CF_UNICODETEXT,oldText,sizeof(oldText));check(CF_DIB,&dib,sizeof(dib));check(format,custom,sizeof(custom));CloseClipboard();
        std::cout<<"Normal paste restored all formats.\n";
        clipboardScenario=1;
        result=insertText(window,L"Locked clipboard.",InsertMethod::Paste,false);
        require(!result.empty()&&hasPendingClipboard(),"locked clipboard did not retain recovery snapshot");
        Sleep(900);
        result=restorePendingClipboard();
        require(result.empty()&&!hasPendingClipboard(),"clipboard recovery failed after lock released");
        require(OpenClipboard(window)!=0,"open recovered clipboard");check(CF_UNICODETEXT,oldText,sizeof(oldText));check(CF_DIB,&dib,sizeof(dib));check(format,custom,sizeof(custom));CloseClipboard();
        std::cout<<"Locked clipboard recovered all formats.\n";
        clipboardScenario=2;
        result=insertText(window,L"Replacement race.",InsertMethod::Paste,false);
        std::wcerr<<L"Replacement status: "<<result<<L'\n';
        require(!hasPendingClipboard(),"user replacement left stale recovery pending");
        const wchar_t replacement[]=L"New user clipboard";
        require(OpenClipboard(window)!=0,"open user clipboard");check(CF_UNICODETEXT,replacement,sizeof(replacement));CloseClipboard();
        SetWindowTextW(edit,L"");
        result=insertText(window,L"Unicode: caf\u00e9 \u4e16\u754c \U0001f642",InsertMethod::Type,false);
        require(result.empty(),"typing reported failure");Sleep(100);pump();
        GetWindowTextW(edit,text,512);require(std::wstring(text)==L"Unicode: caf\u00e9 \u4e16\u754c \U0001f642","Unicode typing corrupted text");
        SetWindowTextW(edit,L"");
        result=insertText(window,L"Submit",InsertMethod::Type,true);
        require(result.empty(),"auto-enter reported failure");Sleep(100);
        GetWindowTextW(edit,text,512);require(std::wstring(text)==L"Submit\r\n","auto-enter did not follow delivered text");
        HWND other=CreateWindowExW(0,L"STATIC",L"Wrong target",WS_OVERLAPPEDWINDOW|WS_VISIBLE,800,100,300,150,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        SetForegroundWindow(other);pump();result=insertText(window,L"must not insert",InsertMethod::Type,false);require(!result.empty(),"wrong foreground accepted");DestroyWindow(other);
        std::cout<<"PASS: native EDIT paste; text/image/custom format preservation; locked clipboard recovery; newer clipboard protected; Unicode typing; auto-enter; wrong-target rejection.\n";return 0;
    } catch(const std::exception& e){
        CloseClipboard();OleUninitialize();std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;
    }
}

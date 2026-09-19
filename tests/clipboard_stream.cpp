// Exercise the private capture/restore boundary without injecting keys into the
// user's desktop. The separate window station owns an entirely separate clipboard.
#include "../src/platform.cpp"
#include <gdiplus.h>
#include <iostream>
using namespace talktext;
namespace {
void require(bool ok,const char* message) {
    if(!ok)throw std::runtime_error(message);
}
void verifyPng(CLIPFORMAT format) {
    ComPtr<IDataObject> clipboard;
    requireHr(OleGetClipboard(&clipboard),L"Read restored image");
    FORMATETC requested{format,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL|TYMED_ISTREAM};
    Medium medium;
    requireHr(clipboard->GetData(&requested,&medium.value),L"Retrieve restored PNG");
    ComPtr<IStream> stream;
    if(medium.value.tymed==TYMED_HGLOBAL)
        requireHr(CreateStreamOnHGlobal(medium.value.hGlobal,FALSE,&stream),L"Open PNG memory");
    else {
        require(medium.value.tymed==TYMED_ISTREAM,"Unexpected image medium");
        stream=medium.value.pstm;
    }
    LARGE_INTEGER zero{};
    requireHr(stream->Seek(zero,STREAM_SEEK_SET,nullptr),L"Read image from start");
    Gdiplus::Bitmap image(stream.Get());
    require(image.GetLastStatus()==Gdiplus::Ok&&image.GetWidth()==2&&image.GetHeight()==2,
        "Restored PNG is not a decodable 2x2 image");
    Gdiplus::Color pixel;
    require(image.GetPixel(0,0,&pixel)==Gdiplus::Ok&&pixel.GetValue()==Gdiplus::Color::Red,
        "Restored image pixel changed");
}
void roundTrip(bool streamBacked,DWORD delay) {
    const auto format=static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"PNG"));
    ComPtr<IStream> png;
    requireHr(CreateStreamOnHGlobal(nullptr,TRUE,&png),L"Create PNG stream");
    Gdiplus::Bitmap image(2,2);
    require(image.SetPixel(0,0,Gdiplus::Color::Red)==Gdiplus::Ok,"Set image pixel");
    CLSID encoder{0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
    require(image.Save(png.Get(),&encoder,nullptr)==Gdiplus::Ok,"Encode PNG");
    auto original=textClipboard(L"Original text beside image");
    ClipboardEntry entry;
    entry.format={format,nullptr,DVASPECT_CONTENT,-1,static_cast<DWORD>(streamBacked?TYMED_ISTREAM:TYMED_HGLOBAL)};
    entry.medium.value.tymed=entry.format.tymed;
    if(streamBacked) {
        entry.medium.value.pstm=png.Get();png->AddRef();
    } else {
        HGLOBAL memory{};
        requireHr(GetHGlobalFromStream(png.Get(),&memory),L"Get PNG memory");
        entry.medium.value.hGlobal=OleDuplicateData(memory,format,0);
        require(entry.medium.value.hGlobal!=nullptr,"Copy PNG memory");
    }
    original->entries.push_back(std::move(entry));
    requireHr(OleSetClipboard(original.Get()),L"Seed isolated image");
    original.Reset();png.Reset();
    for(int cycle=0;cycle<3;++cycle) {
        DWORD sequence{};
        pendingClipboard.snapshot=captureClipboard(sequence);
        pendingClipboard.temporary=textClipboard(L"Temporary transcription");
        requireHr(OleSetClipboard(pendingClipboard.temporary.Get()),L"Publish temporary text");
        pendingClipboard.owner=GetClipboardOwner();
        pendingClipboard.phase=PendingClipboard::Phase::Temporary;
        const auto result=restoreClipboard();
        if(!result.empty())std::wcerr<<result<<L'\n';
        require(result.empty()&&!hasPendingClipboard(),"Image restoration failed");
        verifyPng(format);
        if(cycle==0&&delay) {
            const auto end=GetTickCount64()+delay;
            while(GetTickCount64()<end) {
                MSG message{};
                while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                    TranslateMessage(&message);DispatchMessageW(&message);
                }
                Sleep(20);
            }
        }
    }
    requireHr(OleSetClipboard(nullptr),L"Clear isolated clipboard");
}
}
int main(int argc,char** argv) {
    int result=1;
    try {
        HWINSTA station=CreateWindowStationW(nullptr,0,WINSTA_ALL_ACCESS,nullptr);
        require(station&&SetProcessWindowStation(station),"Cannot isolate clipboard window station");
        HDESK desktop=CreateDesktopW(L"TalkTextClipboardRegression",nullptr,nullptr,0,GENERIC_ALL,nullptr);
        require(desktop&&SetThreadDesktop(desktop),"Cannot create isolated desktop");
        requireHr(OleInitialize(nullptr),L"Initialize OLE");
        Gdiplus::GdiplusStartupInput startup;
        ULONG_PTR token{};
        require(Gdiplus::GdiplusStartup(&token,&startup,nullptr)==Gdiplus::Ok,"Initialize PNG decoder");
        try {
            const DWORD delay=argc>1&&std::string(argv[1])=="--delay"?125000:0;
            roundTrip(true,delay);
            roundTrip(false,0);
            std::cout<<"PASS: stream/memory PNG survive three captures, restores and image decodes"
                <<(delay?" with a 125-second gap":"")<<"; live clipboard untouched.\n";
            result=0;
        } catch(const std::exception& error) {
            std::cerr<<"FAIL: "<<error.what()<<'\n';
        }
        pendingClipboard.clear();
        OleSetClipboard(nullptr);
        OleUninitialize();
        Gdiplus::GdiplusShutdown(token);
        // Desktop/station handles are process-scoped and close on test exit.
    } catch(const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';
    }
    return result;
}

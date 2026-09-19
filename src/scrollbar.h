#pragma once
#include <windows.h>

namespace talktext {
// Native SCROLLBAR interaction and accessibility; custom paint, arrowless track.
// The parent HWND owns lifetime and routes WM_VSCROLL to its content control.
struct Scrollbar {
    HWND window{};
    void create(HWND parent,int id,COLORREF background);
    void layout(int x,int y,int width,int height);
    void update(int position,int count,int page);
    int destination(UINT code,int lineStep=1) const;
};
}

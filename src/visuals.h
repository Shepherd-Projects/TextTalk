#pragma once
#include <windows.h>
#include <string_view>

namespace talktext::visuals {

enum class Icon { None, Microphone, Power, Copy, Trash };

// Orchard control roles: `primary` is the one accent action, `row` a borderless
// settings row on a card, anything else a hairline secondary button.
struct ButtonState {
    bool pressed{},focused{},disabled{},primary{},toggle{},row{};
    float light{},position{};
};

// Device-pixel geometry in the active theme. Caller owns GDI+ startup/shutdown
// and all interaction state.
// Native owner-drawn list selection (GDI).
void selection(HDC,const RECT&,float scale,bool focused);
// Composition-frame drawing between gpu::begin and gpu::present; `key` owns the cached raster.
void button(HWND, const RECT&, const wchar_t*, HFONT, Icon, const ButtonState&);
void text(const void* key,const RECT&,std::wstring_view,HFONT,COLORREF,UINT format);
void logo(const void* key,const RECT&,HICON);

}

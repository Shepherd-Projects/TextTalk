#pragma once
#include <windows.h>

namespace talktext::visuals {

enum class Icon { None, Microphone, Wave, Keyboard, Text, Windows, Tray, Hidden, Power, Copy, Trash };

struct ButtonState {
    bool pressed{},focused{},disabled{},primary{},toggle{};
    float light{},position{},seconds{};
};

// Device-pixel geometry. The caller owns GDI+ startup/shutdown and all interaction state.
void surface(HDC, const RECT&, float radius, bool raised, float light = 0.0f);
void selection(HDC,const RECT&,float scale,bool focused);
void button(HWND, const RECT&, const wchar_t*, HFONT, Icon, const ButtonState&);
void icon(HDC, const RECT&, Icon, COLORREF);

}

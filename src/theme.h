#pragma once
#include <windows.h>
#include <algorithm>
#include <array>

// Orchard GUI visual language (groundstate/gui/DESIGN.md): flat, compact,
// editorial. Presets and frosted glass mirror gui/src/theme.rs: greys stay
// pure except the warm Sunburnt and Paper; hue lives in accent and focus.
namespace talktext::theme {
struct Palette {
    const wchar_t* name;
    bool dark;
    COLORREF canvas, surface, elevated, border, text, muted, accent, focus;
    // Orchard text_on_accent: black on a light accent, white on a dark one.
    COLORREF onAccent;
};

inline constexpr std::array<Palette, 6> presets{{
    {L"Light", false, RGB(0xFF,0xFF,0xFF), RGB(0xF6,0xF6,0xF6), RGB(0xF0,0xF0,0xF0), RGB(0xD0,0xD0,0xD0),
     RGB(0x1F,0x1F,0x1F), RGB(0x65,0x65,0x65), RGB(0x09,0x69,0xDA), RGB(0x05,0x50,0xAE), RGB(0xFF,0xFF,0xFF)},
    {L"Dark", true, RGB(0x1E,0x1E,0x1E), RGB(0x25,0x25,0x25), RGB(0x2E,0x2E,0x2E), RGB(0x3A,0x3A,0x3A),
     RGB(0xED,0xED,0xED), RGB(0xA0,0xA0,0xA0), RGB(0x58,0xA6,0xFF), RGB(0x79,0xC0,0xFF), RGB(0x00,0x00,0x00)},
    {L"OLED", true, RGB(0x00,0x00,0x00), RGB(0x0B,0x0B,0x0B), RGB(0x16,0x16,0x16), RGB(0x26,0x26,0x26),
     RGB(0xE6,0xE6,0xE6), RGB(0x9A,0x9A,0x9A), RGB(0xA7,0x8B,0xFA), RGB(0xC4,0xB5,0xFD), RGB(0x00,0x00,0x00)},
    {L"Sunburnt", true, RGB(0x1A,0x16,0x13), RGB(0x24,0x1E,0x1A), RGB(0x2E,0x26,0x21), RGB(0x3B,0x31,0x2A),
     RGB(0xF3,0xE7,0xDB), RGB(0xB3,0xA0,0x8F), RGB(0xE8,0x74,0x3B), RGB(0xF5,0x9A,0x66), RGB(0x00,0x00,0x00)},
    {L"Paper", false, RGB(0xF6,0xF1,0xE7), RGB(0xFB,0xF8,0xF2), RGB(0xEE,0xE7,0xDA), RGB(0xDC,0xD2,0xC0),
     RGB(0x2E,0x29,0x24), RGB(0x6F,0x65,0x5A), RGB(0xA3,0x54,0x2A), RGB(0x7F,0x3E,0x1C), RGB(0xFF,0xFF,0xFF)},
    {L"High Contrast", true, RGB(0x00,0x00,0x00), RGB(0x0A,0x0A,0x0A), RGB(0x1A,0x1A,0x1A), RGB(0xA0,0xA0,0xA0),
     RGB(0xFF,0xFF,0xFF), RGB(0xE0,0xE0,0xE0), RGB(0xFF,0xE1,0x00), RGB(0xFF,0xF2,0x00), RGB(0x00,0x00,0x00)},
}};

// Active appearance; UI thread only. Set through select().
inline Palette colours = presets[2];
// Share of the blurred desktop shown through the canvas (0 = opaque window).
inline float glass = 0.0f;

inline void select(unsigned preset, unsigned glassPercent) {
    colours = presets[std::min<size_t>(preset, presets.size() - 1)];
    glass = static_cast<float>(std::min(glassPercent, 80u)) / 100.0f;
}

// State fills are the text colour over the underlying fill, never another grey.
constexpr float HoverTint = 0.05f;
constexpr float SelectedTint = 0.08f;
constexpr float PressedTint = 0.11f;

constexpr float RadiusControl = 4.0f;
constexpr float RadiusSurface = 6.0f;
constexpr float FocusWidth = 2.0f;

// Least alpha of a frosted layer, and of native panels floating over the pane.
constexpr float LayerAlpha = 0.25f;
constexpr float FloatingAlpha = 0.94f;

// `from` moved `amount` (0-1) towards `to` per sRGB channel, so greys mix to greys.
constexpr COLORREF mix(COLORREF from, COLORREF to, float amount) {
    auto channel = [amount](int a, int b) { return static_cast<BYTE>(a + (b - a) * amount + 0.5f); };
    return RGB(channel(GetRValue(from), GetRValue(to)), channel(GetGValue(from), GetGValue(to)),
               channel(GetBValue(from), GetBValue(to)));
}

struct Fill {
    COLORREF colour;
    float alpha;
};

// Orchard frost(): while glass is on, `fill` becomes a translucent layer that
// composites to exactly `fill` over the opaque canvas colour, so nested
// surfaces read as denser frost of one pane. Opaque while glass is off.
inline Fill layer(COLORREF fill) {
    if (glass <= 0.0f) return {fill, 1.0f};
    const COLORREF canvas = colours.canvas;
    const float base[3]{GetRValue(canvas) / 255.0f, GetGValue(canvas) / 255.0f, GetBValue(canvas) / 255.0f};
    const float goal[3]{GetRValue(fill) / 255.0f, GetGValue(fill) / 255.0f, GetBValue(fill) / 255.0f};
    float alpha = LayerAlpha;
    for (int i = 0; i < 3; ++i) {
        const float delta = goal[i] - base[i];
        if (delta > 0) alpha = std::max(alpha, delta / (1.0f - base[i]));
        if (delta < 0) alpha = std::max(alpha, -delta / base[i]);
    }
    alpha = std::min(alpha, 1.0f);
    auto channel = [&](int i) {
        return static_cast<BYTE>(std::clamp(base[i] + (goal[i] - base[i]) / alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return {RGB(channel(0), channel(1), channel(2)), alpha};
}
}

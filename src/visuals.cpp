#include "visuals.h"
#include "gpu_visuals.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace talktext::visuals {
namespace {
using namespace Gdiplus;
constexpr float Pi = 3.14159265358979323846f;

float unit(float value) {
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

RectF bounds(const RECT& rect) {
    return {static_cast<float>(rect.left), static_cast<float>(rect.top),
        static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top)};
}

RectF inset(RectF rect, float amount) {
    rect.Inflate(-amount, -amount);
    return rect;
}

void rounded(GraphicsPath& path, const RectF& rect, float radius) {
    if (rect.Width <= 0 || rect.Height <= 0) return;
    radius = std::clamp(radius, 0.0f, (std::min)(rect.Width, rect.Height) * 0.5f);
    if (radius < 0.1f) {
        path.AddRectangle(rect);
        return;
    }
    const float diameter = radius * 2;
    const float right = rect.GetRight() - diameter;
    const float bottom = rect.GetBottom() - diameter;
    path.StartFigure();
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180, 90);
    path.AddArc(right, rect.Y, diameter, diameter, 270, 90);
    path.AddArc(right, bottom, diameter, diameter, 0, 90);
    path.AddArc(rect.X, bottom, diameter, diameter, 90, 90);
    path.CloseFigure();
}

void prepare(Graphics& graphics, const RectF& clip) {
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    graphics.SetCompositingQuality(CompositingQualityHighQuality);
    graphics.SetClip(clip, CombineModeIntersect);
}

float fontScale(HDC dc, HFONT font, float height) {
    LOGFONTW description{};
    if (font && GetObjectW(font, sizeof(description), &description) && description.lfHeight) {
        return (std::max)(0.5f, std::abs(static_cast<float>(description.lfHeight)) / 16.0f);
    }
    TEXTMETRICW metrics{};
    if (GetTextMetricsW(dc, &metrics) && metrics.tmHeight > 0)
        return (std::max)(0.5f, static_cast<float>(metrics.tmHeight) / 19.0f);
    return (std::max)(0.5f, height / 36.0f);
}

void material(Graphics& graphics,const RectF& rect,float radius,bool raised,
              float light,float scale,bool pressed=false,bool disabled=false,
              bool =false,int kind=-1,float position=0.0f) {
    RECT bounds{static_cast<LONG>(std::round(rect.X)),static_cast<LONG>(std::round(rect.Y)),
        static_cast<LONG>(std::round(rect.GetRight())),static_cast<LONG>(std::round(rect.GetBottom()))};
    graphics.Flush(FlushIntentionSync);
    HDC dc=graphics.GetHDC();
    try {gpu::material(dc,bounds,radius,scale,light,pressed,disabled,kind<0?(raised?0:1):kind,position);}
    catch(...) {graphics.ReleaseHDC(dc);throw;}
    graphics.ReleaseHDC(dc);
}

// Lucide's original 24-unit SVG geometry, embedded for native drawing.
// Circular SVG arcs are converted to GDI+ arcs; no SVG decoder/runtime assets.
// Sources, modifications and complete notices: assets/NOTICE.txt.
struct IconPath {
    GraphicsPath path;
    PointF current{};

    void move(float x, float y) {
        path.StartFigure();
        current = PointF(x, y);
    }
    void line(float x, float y) {
        const PointF next(x, y);
        path.AddLine(current, next);
        current = next;
    }
    void curve(float x1, float y1, float x2, float y2, float x, float y) {
        const PointF next(x, y);
        path.AddBezier(current, PointF(x1, y1), PointF(x2, y2), next);
        current = next;
    }
    void arc(float radius, bool large, bool clockwise, float x, float y) {
        const float dx = (current.X - x) * 0.5f;
        const float dy = (current.Y - y) * 0.5f;
        const float distance = dx * dx + dy * dy;
        if (distance == 0) return;
        radius = (std::max)(radius, std::sqrt(distance));
        float factor = std::sqrt((std::max)(0.0f, (radius * radius - distance) / distance));
        if (large == clockwise) factor = -factor;
        const float cx = (current.X + x) * 0.5f + factor * dy;
        const float cy = (current.Y + y) * 0.5f - factor * dx;
        const float start = std::atan2(current.Y - cy, current.X - cx);
        float sweep = std::atan2(y - cy, x - cx) - start;
        if (clockwise && sweep < 0) sweep += 2 * Pi;
        if (!clockwise && sweep > 0) sweep -= 2 * Pi;
        path.AddArc(cx - radius, cy - radius, 2 * radius, 2 * radius,
                    start * 180 / Pi, sweep * 180 / Pi);
        current = PointF(x, y);
    }
};

void drawIcon(Graphics& graphics, const RectF& rect, Icon which, Color color) {
    if (which == Icon::None || rect.Width <= 0 || rect.Height <= 0) return;
    IconPath p;
    switch (which) {
    case Icon::Microphone:
        p.move(12, 19); p.line(12, 22);
        p.move(19, 10); p.line(19, 12); p.arc(7, false, true, 5, 12); p.line(5, 10);
        rounded(p.path, RectF(9, 2, 6, 13), 3);
        break;
    case Icon::Wave:
        p.move(2, 10); p.line(2, 13);
        p.move(6, 6); p.line(6, 17);
        p.move(10, 3); p.line(10, 21);
        p.move(14, 8); p.line(14, 15);
        p.move(18, 5); p.line(18, 18);
        p.move(22, 10); p.line(22, 13);
        break;
    case Icon::Keyboard:
        for (float x : {6.0f, 10.0f, 14.0f, 18.0f}) { p.move(x, 8); p.line(x + 0.01f, 8); }
        for (float x : {8.0f, 12.0f, 16.0f}) { p.move(x, 12); p.line(x + 0.01f, 12); }
        p.move(7, 16); p.line(17, 16);
        rounded(p.path, RectF(2, 4, 20, 16), 2);
        break;
    case Icon::Text:
        p.move(12,4);p.line(12,20);
        p.move(4,7);p.line(4,5);p.arc(1,false,true,5,4);
        p.line(19,4);p.arc(1,false,true,20,5);p.line(20,7);
        p.move(9,20);p.line(15,20);
        break;
    case Icon::Windows:
        rounded(p.path, RectF(3, 3, 18, 18), 2);
        p.move(3, 9); p.line(21, 9);
        p.move(9, 21); p.line(9, 9);
        break;
    case Icon::Tray:
        p.move(22, 12); p.line(16, 12); p.line(14, 15); p.line(10, 15); p.line(8, 12); p.line(2, 12);
        p.move(5.45f, 5.11f); p.line(2, 12); p.line(2, 18); p.arc(2, false, false, 4, 20);
        p.line(20, 20); p.arc(2, false, false, 22, 18); p.line(22, 12); p.line(18.55f, 5.11f);
        p.arc(2, false, false, 16.76f, 4); p.line(7.24f, 4); p.arc(2, false, false, 5.45f, 5.11f);
        p.path.CloseFigure();
        break;
    case Icon::Hidden:
        p.move(10.733f, 5.076f); p.arc(10.744f, false, true, 21.938f, 11.651f);
        p.arc(1, false, true, 21.938f, 12.347f); p.arc(10.747f, false, true, 20.494f, 14.837f);
        p.move(14.084f, 14.158f); p.arc(3, false, true, 9.842f, 9.916f);
        p.move(17.479f, 17.499f); p.arc(10.75f, false, true, 2.062f, 12.348f);
        p.arc(1, false, true, 2.062f, 11.652f); p.arc(10.75f, false, true, 6.508f, 6.509f);
        p.move(2, 2); p.line(22, 22);
        break;
    case Icon::Power:
        p.move(12, 2); p.line(12, 12);
        p.move(18.4f, 6.6f); p.arc(9, true, true, 5.63f, 6.64f);
        break;
    case Icon::Copy:
        rounded(p.path, RectF(8, 8, 14, 14), 2);
        p.move(4, 16); p.curve(2.9f, 16, 2, 15.1f, 2, 14);
        p.line(2, 4); p.curve(2, 2.9f, 2.9f, 2, 4, 2);
        p.line(14, 2); p.curve(15.1f, 2, 16, 2.9f, 16, 4);
        break;
    case Icon::Trash:
        p.move(10, 11); p.line(10, 17);
        p.move(14, 11); p.line(14, 17);
        p.move(19, 6); p.line(19, 20); p.arc(2, false, true, 17, 22);
        p.line(7, 22); p.arc(2, false, true, 5, 20); p.line(5, 6);
        p.move(3, 6); p.line(21, 6);
        p.move(8, 6); p.line(8, 4); p.arc(2, false, true, 10, 2);
        p.line(14, 2); p.arc(2, false, true, 16, 4); p.line(16, 6);
        break;
    case Icon::None:
        return;
    }
    const float scale = (std::min)(rect.Width, rect.Height) / 24;
    Matrix transform(scale, 0, 0, scale,
        rect.X + (rect.Width - 24 * scale) * 0.5f, rect.Y + (rect.Height - 24 * scale) * 0.5f);
    p.path.Transform(&transform);
    Pen pen(color, 2 * scale);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    graphics.DrawPath(&pen, &p.path);
}

struct InkBitmap {
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};
    void* pixels{};
    InkBitmap(int width,int height) {
        dc=CreateCompatibleDC(nullptr);
        if(!dc)throw std::runtime_error("Cannot create native text context");
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!bitmap){DeleteDC(dc);throw std::runtime_error("Cannot allocate native text bitmap");}
        previous=SelectObject(dc,bitmap);
        std::memset(pixels,0,static_cast<size_t>(width)*height*4);
    }
    ~InkBitmap(){SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);}
};
}

void surface(HDC dc, const RECT& rect, float radius, bool raised, float light) {
    if (!dc || rect.right <= rect.left || rect.bottom <= rect.top) return;
    Graphics graphics(dc);
    const RectF box = bounds(rect);
    prepare(graphics, box);
    const float scale = fontScale(dc, nullptr, box.Height);
    material(graphics, box, radius, raised, unit(light), scale);
}

void selection(HDC dc,const RECT& rect,float scale,bool focused) {
    Graphics graphics(dc);const RectF box=bounds(rect);prepare(graphics,box);
    GraphicsPath path;rounded(path,inset(box,0.5f*scale),8*scale);
    SolidBrush fill(Color(255,24,31,37));graphics.FillPath(&fill,&path);
    if(focused) {
        Pen outline(Color(255,112,132,147),scale);
        graphics.DrawPath(&outline,&path);
    }
}

void icon(HDC dc, const RECT& rect, Icon which, COLORREF color) {
    if (!dc || rect.right <= rect.left || rect.bottom <= rect.top) return;
    Graphics graphics(dc);
    const RectF box = bounds(rect);
    prepare(graphics, box);
    drawIcon(graphics, box, which, Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
}

void button(HWND window,const RECT& rect,const wchar_t* label,HFONT font,Icon which,const ButtonState& state) {
    const int width=rect.right-rect.left,height=rect.bottom-rect.top;
    if(width<=0||height<=0)return;
    const float scale=GetDpiForWindow(window)/96.0f;
    const RectF box(0,0,static_cast<float>(width),static_cast<float>(height));
    const float radius=12*scale;
    const float light=state.disabled?0:unit(state.light);
    auto metal=[&](const RectF& shape,float rounding,int kind,float position=0.0f) {
        RECT at{rect.left+static_cast<LONG>(std::round(shape.X)),rect.top+static_cast<LONG>(std::round(shape.Y)),
            rect.left+static_cast<LONG>(std::round(shape.GetRight())),rect.top+static_cast<LONG>(std::round(shape.GetBottom()))};
        gpu::material(nullptr,at,rounding,scale,light,state.pressed&&!state.disabled,state.disabled,kind,position,state.seconds);
    };
    metal(box,radius,0);
    RectF content=inset(box,16*scale);
    content.Y=0;content.Height=box.Height;
    if(state.toggle) {
        const float h=24*scale,w=46*scale;
        RectF track(content.GetRight()-w,(box.Height-h)/2,w,h);
        metal(track,8*scale,2,state.position);
        const float diameter=h-6*scale;
        RectF thumb(track.X+3*scale+state.position*(w-diameter-6*scale),track.Y+3*scale,diameter,diameter);
        metal(thumb,5*scale,3,state.position);
        content.Width-=w+10*scale;
    }
    RectF iconBox;
    if(which!=Icon::None) {
        const float size=(state.primary?22.0f:20.0f)*scale;
        iconBox=RectF(content.X,(box.Height-size)/2,size,size);
        content.X+=size+9*scale;content.Width-=size+9*scale;
    }
    struct CachedInk {
        std::wstring label;
        HFONT font{};
        int width{},height{};
        bool disabled{},focused{};
        Icon icon{};
        unsigned revision{};
        std::vector<unsigned char> pixels;
    };
    static std::unordered_map<HWND,CachedInk> cache;
    auto& ink=cache[window];
    const std::wstring_view text=label?label:L"";
    if(ink.width!=width||ink.height!=height||ink.label!=text||ink.font!=font||
       ink.disabled!=state.disabled||ink.focused!=state.focused||ink.icon!=which) {
        InkBitmap bitmap(width,height);
        {
            Graphics graphics(bitmap.dc);prepare(graphics,box);
            if(which!=Icon::None)drawIcon(graphics,iconBox,which,Color(255,255,255,255));
            if(state.focused&&!state.disabled) {
                GraphicsPath focus;rounded(focus,inset(box,5*scale),radius-5*scale);
                Pen ring(Color(255,170,170,170),scale);
                graphics.DrawPath(&ring,&focus);
            }
        }
        if(!text.empty()&&content.Width>0) {
            SelectObject(bitmap.dc,font);SetTextColor(bitmap.dc,RGB(255,255,255));SetBkColor(bitmap.dc,RGB(0,0,0));
            SetBkMode(bitmap.dc,OPAQUE);
            RECT textRect{static_cast<LONG>(std::round(content.X)),0,
                static_cast<LONG>(std::round(content.GetRight())),height};
            const UINT alignment=which==Icon::None&&!state.toggle?DT_CENTER:DT_LEFT;
            DrawTextW(bitmap.dc,label,-1,&textRect,alignment|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
        }
        GdiFlush();
        ink.pixels.resize(static_cast<size_t>(width)*height*4);
        std::memcpy(ink.pixels.data(),bitmap.pixels,ink.pixels.size());
        ink.label=text;ink.font=font;ink.width=width;ink.height=height;ink.disabled=state.disabled;
        ink.focused=state.focused;ink.icon=which;++ink.revision;
    }
    gpu::ink(window,rect,ink.revision,ink.pixels.data(),static_cast<UINT>(width)*4,state.disabled);
}
} // namespace talktext::visuals

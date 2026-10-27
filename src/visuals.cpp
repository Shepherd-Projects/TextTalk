#include "visuals.h"
#include "gpu_visuals.h"
#include "theme.h"
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

Color opaque(COLORREF colour) {
    return Color(255,GetRValue(colour),GetGValue(colour),GetBValue(colour));
}

// Cached white-on-black ClearType coverage, re-rasterized only when its inputs change.
struct CachedInk {
    std::wstring text;
    HFONT font{};
    int width{},height{};
    unsigned flags{},revision{};
    std::vector<unsigned char> pixels;
};
template<class Paint>
const CachedInk& cachedInk(const void* key,std::wstring_view text,HFONT font,int width,int height,unsigned flags,Paint paint) {
    static std::unordered_map<const void*,CachedInk> cache;
    auto& ink=cache[key];
    if(ink.width!=width||ink.height!=height||ink.text!=text||ink.font!=font||ink.flags!=flags) {
        InkBitmap bitmap(width,height);
        SelectObject(bitmap.dc,font);SetBkMode(bitmap.dc,TRANSPARENT);
        paint(bitmap.dc);
        GdiFlush();
        ink.pixels.resize(static_cast<size_t>(width)*height*4);
        std::memcpy(ink.pixels.data(),bitmap.pixels,ink.pixels.size());
        ink.text=text;ink.font=font;ink.width=width;ink.height=height;ink.flags=flags;++ink.revision;
    }
    return ink;
}

RECT offset(const RECT& origin,const RectF& at) {
    return {origin.left+static_cast<LONG>(std::round(at.X)),origin.top+static_cast<LONG>(std::round(at.Y)),
        origin.left+static_cast<LONG>(std::round(at.GetRight())),origin.top+static_cast<LONG>(std::round(at.GetBottom()))};
}
}

void selection(HDC dc,const RECT& rect,float scale,bool focused) {
    const auto& c=theme::colours;
    Graphics graphics(dc);const RectF box=bounds(rect);prepare(graphics,box);
    GraphicsPath path;rounded(path,box,theme::RadiusControl*scale);
    SolidBrush brush(opaque(theme::mix(c.surface,c.text,theme::SelectedTint)));graphics.FillPath(&brush,&path);
    if(focused) {
        const float width=theme::FocusWidth*scale;
        GraphicsPath ring;rounded(ring,inset(box,width*0.5f),(theme::RadiusControl-1)*scale);
        Pen outline(opaque(c.focus),width);
        graphics.DrawPath(&outline,&ring);
    }
}

void text(const void* key,const RECT& rect,std::wstring_view value,HFONT font,COLORREF colour,UINT format) {
    const int width=rect.right-rect.left,height=rect.bottom-rect.top;
    if(width<=0||height<=0||value.empty())return;
    const auto& ink=cachedInk(key,value,font,width,height,format,[&](HDC dc) {
        RECT box{0,0,width,height};SetTextColor(dc,RGB(255,255,255));
        DrawTextW(dc,value.data(),static_cast<int>(value.size()),&box,format|DT_NOPREFIX);
    });
    gpu::ink(key,rect,ink.revision,ink.pixels.data(),static_cast<UINT>(width)*4,colour);
}

void logo(const void* key,const RECT& rect,HICON icon) {
    const int width=rect.right-rect.left,height=rect.bottom-rect.top;
    ICONINFO info{};
    if(width<=0||height<=0||!icon||!GetIconInfo(icon,&info))return;
    struct Image {
        HICON icon{};
        int width{},height{};
        unsigned revision{};
        std::vector<unsigned char> pixels;
    };
    static std::unordered_map<const void*,Image> cache;
    auto& image=cache[key];
    if(image.icon!=icon||image.width!=width||image.height!=height) {
        // The resource's 32-bit colour plane carries straight alpha; the compositor wants it premultiplied.
        BITMAPINFO bits{};bits.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        bits.bmiHeader.biWidth=width;bits.bmiHeader.biHeight=-height;bits.bmiHeader.biPlanes=1;
        bits.bmiHeader.biBitCount=32;bits.bmiHeader.biCompression=BI_RGB;
        image.pixels.assign(static_cast<size_t>(width)*height*4,0);
        HDC dc=GetDC(nullptr);
        GetDIBits(dc,info.hbmColor,0,height,image.pixels.data(),&bits,DIB_RGB_COLORS);
        ReleaseDC(nullptr,dc);
        for(size_t i=0;i<image.pixels.size();i+=4)for(size_t k=0;k<3;++k)
            image.pixels[i+k]=static_cast<unsigned char>(image.pixels[i+k]*image.pixels[i+3]/255);
        image.icon=icon;image.width=width;image.height=height;++image.revision;
    }
    DeleteObject(info.hbmColor);DeleteObject(info.hbmMask);
    gpu::image(key,rect,image.revision,image.pixels.data(),static_cast<UINT>(width)*4);
}

void button(HWND window,const RECT& rect,const wchar_t* label,HFONT font,Icon which,const ButtonState& state) {
    const int width=rect.right-rect.left,height=rect.bottom-rect.top;
    if(width<=0||height<=0)return;
    const auto& c=theme::colours;
    const float scale=GetDpiForWindow(window)/96.0f;
    const RectF box(0,0,static_cast<float>(width),static_cast<float>(height));
    const float light=state.disabled?0:unit(state.light);
    const bool pressed=state.pressed&&!state.disabled;
    auto draw=[&](const RectF& at,gpu::Shape shape) {
        shape.opacity=state.disabled?0.5f:1.0f;
        gpu::material(offset(rect,at),shape);
    };
    gpu::Shape face;
    face.radius=theme::RadiusControl*scale;
    if(state.primary) {
        face.fill=pressed?theme::mix(c.accent,c.canvas,0.15f):theme::mix(c.accent,c.text,0.15f*light);
    } else {
        const float tint=pressed?theme::PressedTint:theme::HoverTint*light;
        const theme::Fill fill=theme::layer(theme::mix(state.row?c.surface:c.canvas,c.text,tint));
        face.fill=fill.colour;face.fillAlpha=fill.alpha;
        if(!state.row){face.border=c.border;face.borderWidth=std::round(scale);}
    }
    if(state.focused&&!state.disabled){face.ring=c.focus;face.ringWidth=theme::FocusWidth*scale;}
    draw(box,face);
    RectF content=inset(box,12*scale);
    content.Y=0;content.Height=box.Height;
    if(state.toggle) {
        const float h=20*scale,w=36*scale,position=unit(state.position);
        const RectF track(content.GetRight()-w,std::round((box.Height-h)/2),w,h);
        gpu::Shape rail;rail.radius=h/2;rail.fill=theme::mix(c.border,c.accent,position);
        draw(track,rail);
        const float diameter=h-6*scale;
        gpu::Shape thumb;thumb.radius=diameter/2;thumb.fill=theme::mix(c.muted,c.onAccent,position);
        draw(RectF(track.X+3*scale+position*(w-diameter-6*scale),track.Y+3*scale,diameter,diameter),thumb);
        content.Width-=w+12*scale;
    }
    const std::wstring_view text=label?label:L"";
    const unsigned flags=static_cast<unsigned>(which)|(state.primary?0x100u:0)|(state.row?0x200u:0)|(state.toggle?0x400u:0);
    const auto& ink=cachedInk(window,text,font,width,height,flags,[&](HDC dc) {
        // White coverage takes the ink colour; grey coverage reads as muted over the fill.
        constexpr BYTE quiet=170;
        // Settings rows read `Name: value`; the value sits muted at the right edge.
        std::wstring_view name=text,value;
        if(state.row&&!state.toggle)if(const size_t split=text.find(L": ");split!=text.npos) {
            name=text.substr(0,split);value=text.substr(split+2);
        }
        const float iconSize=16*scale,iconGap=which==Icon::None?0:8*scale;
        const float iconWidth=which==Icon::None?0:iconSize;
        float x=content.X,right=content.GetRight();
        if(!state.row) {
            SIZE extent{};GetTextExtentPoint32W(dc,name.data(),static_cast<int>(name.size()),&extent);
            x=std::max(content.X,(box.Width-(iconWidth+iconGap+extent.cx))/2);
        }
        if(which!=Icon::None) {
            Graphics graphics(dc);prepare(graphics,box);
            const BYTE tone=state.primary?255:quiet;
            drawIcon(graphics,RectF(std::round(x),std::round((box.Height-iconSize)/2),iconSize,iconSize),which,Color(255,tone,tone,tone));
        }
        x+=iconWidth+iconGap;
        if(!value.empty()) {
            RECT valueRect{static_cast<LONG>(std::round(x)),0,static_cast<LONG>(std::round(right)),height};
            SetTextColor(dc,RGB(quiet,quiet,quiet));
            DrawTextW(dc,value.data(),static_cast<int>(value.size()),&valueRect,DT_RIGHT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
            SIZE extent{};GetTextExtentPoint32W(dc,value.data(),static_cast<int>(value.size()),&extent);
            right=std::max(x,right-extent.cx-16*scale);
        }
        RECT nameRect{static_cast<LONG>(std::round(x)),0,static_cast<LONG>(std::round(right)),height};
        if(!name.empty()&&nameRect.right>nameRect.left) {
            SetTextColor(dc,RGB(255,255,255));
            DrawTextW(dc,name.data(),static_cast<int>(name.size()),&nameRect,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
        }
    });
    const COLORREF colour=state.disabled?c.muted:state.primary?c.onAccent:c.text;
    gpu::ink(window,rect,ink.revision,ink.pixels.data(),static_cast<UINT>(width)*4,colour);
}
} // namespace talktext::visuals

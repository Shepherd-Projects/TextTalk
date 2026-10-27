#pragma once
#include <windows.h>
#include <array>

namespace talktext::gpu {

// UI-thread-owned Direct3D11 shaders; no application/control state is owned here.
void initialize();
void shutdown();
// Composition frames are premultiplied and paced by the swap chain's waitable handle.
// The window's visual sits behind its child windows: native controls that must
// stay visible are layered children composited above it.
HANDLE frameReady(HWND window);
void begin(HWND window);
void present(HWND window);
void synchronize();
void release(HWND window);
// Draws below go into the frame opened by begin(); `key` identifies a cached upload.
// White-on-black LCD coverage tinted with `color`.
void ink(const void* key,const RECT& bounds,unsigned revision,const void* pixels,UINT stride,COLORREF color);
// Premultiplied BGRA image.
void image(const void* key,const RECT& bounds,unsigned revision,const void* pixels,UINT stride);
// Flat rounded rectangle in device pixels: fill (with its own alpha), hairline
// border and inner focus ring; `opacity` fades all of it.
struct Shape {
    float radius{};
    COLORREF fill{},border{},ring{};
    float fillAlpha{1.0f},borderWidth{},ringWidth{},opacity{1.0f};
};
void material(const RECT& bounds,const Shape& shape);
struct VoiceFrame {
    float amplitude{},centroid{},seconds{};
    bool finishing{},reducedMotion{};
    std::array<float,8> bands{};
};
void overlay(HWND window,float scale,const VoiceFrame& frame);

} // namespace talktext::gpu

#pragma once
#include <windows.h>
#include <array>

namespace talktext::gpu {

// UI-thread-owned Direct3D11 shaders; no application/control state is owned here.
void initialize();
void shutdown();
// Composition frames are premultiplied and paced by the swap chain's waitable handle.
HANDLE frameReady(HWND window);
void begin(HWND window);
void present(HWND window);
void synchronize();
void release(HWND window);
void ink(HWND key,const RECT& bounds,unsigned revision,const void* pixels,UINT stride,bool disabled);
void material(HDC destination,const RECT& bounds,float radius,float scale,
              float hover,bool pressed,bool disabled,int kind,float position=0.0f,float seconds=0.0f);
struct VoiceFrame {
    float amplitude{},centroid{},seconds{};
    bool finishing{},reducedMotion{};
    std::array<float,8> bands{};
};
void overlay(HWND window,float scale,const VoiceFrame& frame);

} // namespace talktext::gpu

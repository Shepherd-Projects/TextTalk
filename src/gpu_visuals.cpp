#include "gpu_visuals.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace talktext::gpu {
namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT result,const char* operation) {
    if(FAILED(result))throw std::runtime_error(std::string(operation)+" failed ("+std::to_string(static_cast<unsigned long>(result))+")");
}
struct Parameters {
    float viewport[4]{};
    float material[4]{};
    float state[4]{};
    float sound[4]{};
    float bands0[4]{},bands1[4]{};
};
ComPtr<ID3DBlob> compile(const char* source,size_t length,const char* name,const char* profile) {
    ComPtr<ID3DBlob> code,errors;
    const HRESULT result=D3DCompile(source,length,name,nullptr,nullptr,"main",profile,
        D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if(FAILED(result))throw std::runtime_error(errors?static_cast<const char*>(errors->GetBufferPointer()):"Cannot compile visual shader");
    return code;
}
ComPtr<ID3DBlob> resourceShader(int id,const char* name) {
    HMODULE module=GetModuleHandleW(nullptr);
    HRSRC resource=FindResourceW(module,MAKEINTRESOURCEW(id),RT_RCDATA);
    HGLOBAL loaded=resource?LoadResource(module,resource):nullptr;
    const char* source=loaded?static_cast<const char*>(LockResource(loaded)):nullptr;
    if(!source)throw std::runtime_error("Missing embedded visual shader");
    return compile(source,SizeofResource(module,resource),name,"ps_5_0");
}
struct Surface {
    ComPtr<IDXGISwapChain2> chain;
    ComPtr<ID3D11RenderTargetView> view;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    HANDLE ready{};
    UINT width{},height{};
    ~Surface(){if(ready)CloseHandle(ready);}
};
struct Ink {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    unsigned revision{};
};
class Renderer {
public:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> metal,voice,text;
    ComPtr<ID3D11BlendState> blend,lcdBlend;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDCompositionDevice> composition;
    std::unordered_map<HWND,std::unique_ptr<Surface>> surfaces;
    std::unordered_map<HWND,Ink> inks;
    Surface* active{};
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11Texture2D> target,staging;
    ComPtr<ID3D11RenderTargetView> targetView;
    int width{},height{};
    HDC memory{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};
    void* pixels{};

    Renderer() {
        const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0};
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_SINGLETHREADED,levels,1,D3D11_SDK_VERSION,
            &device,nullptr,&context),"Create Direct3D11 visual device");
        constexpr char source[]="struct V {float4 p:SV_POSITION;float2 uv:TEXCOORD0;}; V main(uint id:SV_VertexID) {float2 p=float2((id<<1)&2,id&2); V v;v.p=float4(p*float2(2,-2)+float2(-1,1),0,1);v.uv=p;return v;}";
        const auto vs=compile(source,sizeof(source)-1,"fullscreen triangle","vs_5_0");
        check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex),"Create visual vertex shader");
        const auto materialCode=resourceShader(201,"material.hlsl");
        check(device->CreatePixelShader(materialCode->GetBufferPointer(),materialCode->GetBufferSize(),nullptr,&metal),"Create material shader");
        const auto voiceCode=resourceShader(202,"overlay.hlsl");
        check(device->CreatePixelShader(voiceCode->GetBufferPointer(),voiceCode->GetBufferSize(),nullptr,&voice),"Create voice shader");
        D3D11_BUFFER_DESC description{};description.ByteWidth=sizeof(Parameters);
        description.Usage=D3D11_USAGE_DEFAULT;description.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&description,nullptr,&constants),"Create shader parameters");
        ComPtr<IDXGIDevice> dxgi;check(device.As(&dxgi),"Query composition device");
        ComPtr<IDXGIAdapter> adapter;check(dxgi->GetAdapter(&adapter),"Query display adapter");
        check(adapter->GetParent(IID_PPV_ARGS(&factory)),"Query swap chain factory");
        check(DCompositionCreateDevice(dxgi.Get(),IID_PPV_ARGS(&composition)),"Create compositor");
        constexpr char inkSource[]=
            "cbuffer P:register(b0){float4 viewport;float4 material;float4 inkColor;};"
            "Texture2D image:register(t0);"
            "struct O{float4 color:SV_Target0;float4 coverage:SV_Target1;};"
            "O main(float4 p:SV_POSITION,float2 uv:TEXCOORD0){"
            "float3 c=image.Load(int3(int2(uv*viewport.xy),0)).rgb;"
            "float a=max(c.r,max(c.g,c.b));O o;"
            "o.color=float4(c*inkColor.rgb,a);o.coverage=float4(c,a);return o;}";
        const auto inkCode=compile(inkSource,sizeof(inkSource)-1,"cached ink","ps_5_0");
        check(device->CreatePixelShader(inkCode->GetBufferPointer(),inkCode->GetBufferSize(),nullptr,&text),"Create ink shader");
        D3D11_BLEND_DESC blending{};auto& b=blending.RenderTarget[0];b.BlendEnable=TRUE;
        b.SrcBlend=D3D11_BLEND_ONE;b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D11_BLEND_OP_ADD;
        b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOpAlpha=D3D11_BLEND_OP_ADD;
        b.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        check(device->CreateBlendState(&blending,&blend),"Create premultiplied blending");
        b.DestBlend=D3D11_BLEND_INV_SRC1_COLOR;
        b.DestBlendAlpha=D3D11_BLEND_INV_SRC1_ALPHA;
        check(device->CreateBlendState(&blending,&lcdBlend),"Create native LCD coverage blending");
        memory=CreateCompatibleDC(nullptr);
        if(!memory)throw std::runtime_error("Cannot create native shader drawing context");
    }
    ~Renderer() {
        if(bitmap){SelectObject(memory,previous);DeleteObject(bitmap);}
        DeleteDC(memory);
    }
    Surface& surface(HWND window) {
        RECT rect{};GetClientRect(window,&rect);
        const UINT w=std::max(1L,rect.right),h=std::max(1L,rect.bottom);
        auto& slot=surfaces[window];
        if(!slot) {
            auto next=std::make_unique<Surface>();
            DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=w;desc.Height=h;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;
            desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
            desc.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            ComPtr<IDXGISwapChain1> chain;
            check(factory->CreateSwapChainForComposition(device.Get(),&desc,nullptr,&chain),"Create composition swap chain");
            check(chain.As(&next->chain),"Query frame pacing");
            check(next->chain->SetMaximumFrameLatency(1),"Set frame latency");
            next->ready=next->chain->GetFrameLatencyWaitableObject();
            if(!next->ready)throw std::runtime_error("Cannot obtain display pacing handle");
            check(composition->CreateTargetForHwnd(window,TRUE,&next->target),"Create window composition target");
            check(composition->CreateVisual(&next->visual),"Create window visual");
            check(next->visual->SetContent(next->chain.Get()),"Attach swap chain");
            check(next->target->SetRoot(next->visual.Get()),"Attach window visual");
            check(composition->Commit(),"Commit composition visual");
            next->width=w;next->height=h;slot=std::move(next);
        }
        auto& s=*slot;
        if(s.width!=w||s.height!=h) {
            context->OMSetRenderTargets(0,nullptr,nullptr);s.view.Reset();
            check(s.chain->ResizeBuffers(0,w,h,DXGI_FORMAT_UNKNOWN,DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT),"Resize visual");
            s.width=w;s.height=h;
        }
        if(!s.view) {
            ComPtr<ID3D11Texture2D> buffer;check(s.chain->GetBuffer(0,IID_PPV_ARGS(&buffer)),"Acquire visual buffer");
            check(device->CreateRenderTargetView(buffer.Get(),nullptr,&s.view),"Create composition target view");
        }
        return s;
    }
    void reserve(int w,int h) {
        if(w<=width&&h<=height)return;
        context->OMSetRenderTargets(0,nullptr,nullptr);
        targetView.Reset();target.Reset();staging.Reset();
        width=std::max(width,(w+63)&~63);height=std::max(height,(h+63)&~63);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=width;desc.Height=height;desc.MipLevels=1;desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
        desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        check(device->CreateTexture2D(&desc,nullptr,&target),"Allocate material target");
        check(device->CreateRenderTargetView(target.Get(),nullptr,&targetView),"Create material target view");
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&desc,nullptr,&staging),"Allocate material readback");
        if(bitmap){SelectObject(memory,previous);DeleteObject(bitmap);bitmap=nullptr;}
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        bitmap=CreateDIBSection(memory,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!bitmap)throw std::runtime_error("Cannot allocate native material surface");
        previous=SelectObject(memory,bitmap);
    }
    void draw(ID3D11RenderTargetView* view,ID3D11PixelShader* shader,const Parameters& p,float x=0,float y=0,ID3D11BlendState* blending=nullptr) {
        context->UpdateSubresource(constants.Get(),0,nullptr,&p,0,0);
        context->OMSetRenderTargets(1,&view,nullptr);
        D3D11_VIEWPORT viewport{x,y,p.viewport[0],p.viewport[1],0,1};
        context->RSSetViewports(1,&viewport);
        context->OMSetBlendState(blending?blending:(active?blend.Get():nullptr),nullptr,0xffffffff);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(),nullptr,0);
        context->PSSetShader(shader,nullptr,0);
        ID3D11Buffer* buffer=constants.Get();context->PSSetConstantBuffers(0,1,&buffer);
        context->Draw(3,0);
    }
    void readback(int w,int h) {
        D3D11_BOX box{0,0,0,static_cast<UINT>(w),static_cast<UINT>(h),1};
        context->CopySubresourceRegion(staging.Get(),0,0,0,0,target.Get(),0,&box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Read shader surface");
        for(int y=0;y<h;++y)std::memcpy(static_cast<char*>(pixels)+static_cast<size_t>(y)*width*4,
            static_cast<const char*>(mapped.pData)+static_cast<size_t>(y)*mapped.RowPitch,static_cast<size_t>(w)*4);
        context->Unmap(staging.Get(),0);
    }
};
std::unique_ptr<Renderer> renderer;
Renderer& current() {
    if(renderer&&FAILED(renderer->device->GetDeviceRemovedReason()))renderer.reset();
    if(!renderer)renderer=std::make_unique<Renderer>();
    return *renderer;
}
}
void initialize(){current();}
void shutdown(){renderer.reset();}
HANDLE frameReady(HWND window){return current().surface(window).ready;}
void begin(HWND window) {
    auto& gpu=current();gpu.active=&gpu.surface(window);
    constexpr float transparent[4]{};
    gpu.context->ClearRenderTargetView(gpu.active->view.Get(),transparent);
}
void present(HWND window) {
    auto& gpu=current();auto& surface=*gpu.surfaces.at(window);
    // Present exactly the buffer drawn by begin(); never resize it here.
    gpu.active=nullptr;
    check(surface.chain->Present(1,0),"Present composition frame");
}
void synchronize() {
    // Composition swap-chain readiness means queue space, not a displayed frame.
    // One compositor barrier after the whole window batch prevents discarded,
    // thousands-per-second submissions while following the actual refresh rate.
    check(DwmFlush(),"Synchronize compositor frame");
}
void release(HWND window) {
    if(renderer){renderer->active=nullptr;renderer->surfaces.erase(window);}
}
void ink(HWND key,const RECT& bounds,unsigned revision,const void* pixels,UINT stride,bool disabled) {
    auto& gpu=current();if(!gpu.active)throw std::runtime_error("Ink requires an active composition frame");
    auto& image=gpu.inks[key];
    if(!image.texture||image.revision!=revision) {
        image.view.Reset();image.texture.Reset();
        D3D11_TEXTURE2D_DESC desc{};desc.Width=bounds.right-bounds.left;desc.Height=bounds.bottom-bounds.top;
        desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
        desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels,stride,0};
        check(gpu.device->CreateTexture2D(&desc,&data,&image.texture),"Upload cached control ink");
        check(gpu.device->CreateShaderResourceView(image.texture.Get(),nullptr,&image.view),"Create ink view");
        image.revision=revision;
    }
    auto* view=image.view.Get();
    gpu.context->PSSetShaderResources(0,1,&view);
    Parameters p{};p.viewport[0]=static_cast<float>(bounds.right-bounds.left);p.viewport[1]=static_cast<float>(bounds.bottom-bounds.top);
    p.state[0]=(disabled?132.0f:238.0f)/255.0f;
    p.state[1]=(disabled?140.0f:241.0f)/255.0f;
    p.state[2]=(disabled?148.0f:243.0f)/255.0f;
    gpu.draw(gpu.active->view.Get(),gpu.text.Get(),p,static_cast<float>(bounds.left),static_cast<float>(bounds.top),gpu.lcdBlend.Get());
    view=nullptr;gpu.context->PSSetShaderResources(0,1,&view);
}
void material(HDC destination,const RECT& bounds,float radius,float scale,
              float hover,bool pressed,bool disabled,int kind,float position,float seconds) {
    const int w=bounds.right-bounds.left,h=bounds.bottom-bounds.top;
    if(w<=0||h<=0)return;
    auto& gpu=current();
    Parameters p{};p.viewport[0]=static_cast<float>(w);p.viewport[1]=static_cast<float>(h);p.viewport[2]=scale;p.viewport[3]=seconds;
    p.material[0]=radius;p.material[1]=hover;p.material[2]=pressed?1.0f:0.0f;p.material[3]=static_cast<float>(kind);
    p.state[0]=position;p.state[3]=disabled?1.0f:0.0f;
    if(!destination) {
        if(!gpu.active)throw std::runtime_error("Material requires an active composition frame");
        gpu.draw(gpu.active->view.Get(),gpu.metal.Get(),p,static_cast<float>(bounds.left),static_cast<float>(bounds.top));
        return;
    }
    gpu.reserve(w,h);
    gpu.draw(gpu.targetView.Get(),gpu.metal.Get(),p);
    gpu.readback(w,h);
    BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    AlphaBlend(destination,bounds.left,bounds.top,w,h,gpu.memory,0,0,w,h,blend);
    GdiFlush();
}
void overlay(HWND window,float scale,const VoiceFrame& frame) {
    RECT rect{};GetClientRect(window,&rect);
    if(rect.right<=0||rect.bottom<=0||!IsWindowVisible(window))return;
    begin(window);auto& gpu=current();
    Parameters p{};p.viewport[0]=static_cast<float>(rect.right);p.viewport[1]=static_cast<float>(rect.bottom);
    p.viewport[2]=scale;p.viewport[3]=frame.seconds;p.material[0]=16*scale;
    p.sound[0]=frame.amplitude;p.sound[1]=frame.centroid;
    p.sound[2]=frame.finishing?1.0f:0.0f;p.sound[3]=frame.reducedMotion?1.0f:0.0f;
    std::copy_n(frame.bands.begin(),4,p.bands0);std::copy_n(frame.bands.begin()+4,4,p.bands1);
    gpu.draw(gpu.active->view.Get(),gpu.voice.Get(),p);
    present(window);
}
} // namespace talktext::gpu

// nr-bridge core: routes VR eye images through a second D3D11 device that owns a hidden
// DXGI swap chain, so that ReShade (dxgi.dll mode) and the RenoDX DLSS add-on treat it like a
// desktop window and apply Neural Rendering to it; the result is copied back into the eye images.
//
//   device A (game)                        device B (ours, one hidden swap chain)
//   eye images --copy--> sharedIn  ==fence==>  sharedIn --copy/blit--> backbuffer
//                                                                    Present()  <- ReShade + RenoDX NR here
//   eye images <--copy-- sharedOut <==fence==  sharedOut <--copy/blit-- backbuffer
//
// All synchronisation is GPU-side via shared D3D11.4 fences; the CPU never blocks on the GPU
// (unless the driver refuses plain NT-handle sharing, in which case a keyed-mutex fallback is used).
// Author: LowPopLabs
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <algorithm>

// Provided by the host (layer DLL or test program).
void BridgeLog(const char *fmt, ...);

using Microsoft::WRL::ComPtr;

struct BridgeView
{
    ID3D11Texture2D *texture; // XR swap chain image on device A
    UINT arrayIndex;          // texture-array slice (single-pass instanced: 0 = left, 1 = right)
    UINT left, top, right, bottom;
};

struct BridgeConfig
{
    float scale = 1.0f;          // NR work resolution as a fraction of the eye-pair size (0.25 .. 1.0)
    bool window_visible = false; // show the hidden window (debugging only)
    bool skip_present = false;   // copy in/out but never Present (isolates the copy path from ReShade)
    int log_frames = 3;          // log this many frames in detail
};

inline DXGI_FORMAT BridgeTypedFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

class Bridge
{
public:
    bool Init(ID3D11Device *devA, const BridgeConfig &cfg);
    void Shutdown(bool leakDeviceB = false);
    // Returns false when the frame was left untouched (caller just forwards it).
    bool Process(const BridgeView *views, uint32_t count);
    void MarkFailed(const char *why) { if (!failed_) BridgeLog("bridge: DISABLED: %s", why); failed_ = true; }
    bool failed() const { return failed_; }
    uint64_t frames() const { return frames_; }
    double lastCpuMs() const { return lastMs_; }
    double avgCpuMs() const { return frames_ ? totalMs_ / (double)frames_ : 0.0; }
    bool keyedMutexFallback() const { return keyed_; }

private:
    bool CreateDeviceB();
    bool CreateFences();
    bool CreateBlit();
    bool EnsureSized(UINT w, UINT h, DXGI_FORMAT fmt);
    bool CreateSharedPair(UINT w, UINT h, DXGI_FORMAT fmt);
    bool CreateSwapchainB(UINT w, UINT h, DXGI_FORMAT fmt);
    void ReleaseSized();
    void Blit(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst, UINT w, UINT h);
    bool Fail(const char *what, HRESULT hr);
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

    BridgeConfig cfg_;
    bool failed_ = false, keyed_ = false, scaled_ = false;
    uint64_t frames_ = 0; double lastMs_ = 0, totalMs_ = 0; LARGE_INTEGER qpf_ = {};

    // device A (game) -- pointers come through ReShade's proxy when ReShade is installed
    ComPtr<ID3D11Device> devA_; ComPtr<ID3D11Device1> devA1_; ComPtr<ID3D11Device5> devA5_;
    ComPtr<ID3D11DeviceContext> ctxA_; ComPtr<ID3D11DeviceContext4> ctxA4_;
    // device B (ours)
    ComPtr<ID3D11Device> devB_; ComPtr<ID3D11Device1> devB1_; ComPtr<ID3D11Device5> devB5_;
    ComPtr<ID3D11DeviceContext> ctxB_; ComPtr<ID3D11DeviceContext4> ctxB4_;
    // fences: AB signalled by A / waited by B, BA the other way round
    ComPtr<ID3D11Fence> fenceAB_A_, fenceAB_B_, fenceBA_A_, fenceBA_B_;
    UINT64 vIn_ = 0, vOut_ = 0;
    // sized resources
    UINT W_ = 0, H_ = 0, workW_ = 0, workH_ = 0; DXGI_FORMAT fmt_ = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Texture2D> sharedInB_, sharedOutB_, sharedInA_, sharedOutA_;
    ComPtr<IDXGIKeyedMutex> kmInA_, kmInB_, kmOutA_, kmOutB_;
    ComPtr<ID3D11ShaderResourceView> srvInB_, srvBB_;
    ComPtr<ID3D11RenderTargetView> rtvOutB_, rtvBB_;
    HWND hwnd_ = nullptr; ATOM wndClass_ = 0;
    ComPtr<IDXGISwapChain1> swapB_; ComPtr<ID3D11Texture2D> bbB_;
    // blit
    ComPtr<ID3D11VertexShader> vs_; ComPtr<ID3D11PixelShader> ps_; ComPtr<ID3D11SamplerState> samp_; ComPtr<ID3D11RasterizerState> rs_;
};

// ------------------------------------------------------------------------------------------------

inline bool Bridge::Fail(const char *what, HRESULT hr)
{
    char buf[256]; snprintf(buf, sizeof(buf), "%s failed, hr=0x%08lX", what, (unsigned long)hr);
    MarkFailed(buf);
    return false;
}

inline bool Bridge::Init(ID3D11Device *devA, const BridgeConfig &cfg)
{
    cfg_ = cfg;
    if (cfg_.scale < 0.25f) cfg_.scale = 0.25f;
    if (cfg_.scale > 1.0f) cfg_.scale = 1.0f;
    scaled_ = cfg_.scale < 0.999f;
    QueryPerformanceFrequency(&qpf_);

    devA_ = devA;
    devA_->GetImmediateContext(&ctxA_);
    if (FAILED(devA_.As(&devA1_))) return Fail("device A ID3D11Device1", E_NOINTERFACE);
    if (FAILED(devA_.As(&devA5_))) return Fail("device A ID3D11Device5 (needs D3D11.4 for shared fences)", E_NOINTERFACE);
    if (FAILED(ctxA_.As(&ctxA4_))) return Fail("device A ID3D11DeviceContext4", E_NOINTERFACE);

    if (!CreateDeviceB() || !CreateFences() || !CreateBlit()) return false;

    bool reshade = false;
    HMODULE mods[512]; DWORD needed = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
    {
        const DWORD n = std::min<DWORD>(needed / sizeof(HMODULE), 512);
        for (DWORD i = 0; i < n; ++i) if (GetProcAddress(mods[i], "ReShadeVersion")) { reshade = true; break; }
    }
    BridgeLog("bridge: init ok. scale=%.2f (%s) skip_present=%d ReShade-in-process=%s",
              cfg_.scale, scaled_ ? "blit" : "copy", cfg_.skip_present ? 1 : 0, reshade ? "yes" : "NO (nothing will apply NR)");
    return true;
}

inline bool Bridge::CreateDeviceB()
{
    ComPtr<IDXGIDevice> dxgiA; ComPtr<IDXGIAdapter> adapter;
    if (FAILED(devA_.As(&dxgiA)) || FAILED(dxgiA->GetAdapter(&adapter))) return Fail("adapter of device A", E_FAIL);
    DXGI_ADAPTER_DESC ad = {}; adapter->GetDesc(&ad);
    BridgeLog("bridge: adapter '%ls' luid=%08lX%08lX", ad.Description, ad.AdapterLuid.HighPart, ad.AdapterLuid.LowPart);

    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    // Goes through d3d11.dll's exported D3D11CreateDevice, which ReShade hooks, so device B comes back
    // as a ReShade proxy and the swap chain created on it gets a ReShade runtime + add-on events.
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   fls, 2, D3D11_SDK_VERSION, &devB_, &got, &ctxB_);
    if (FAILED(hr)) return Fail("D3D11CreateDevice (device B)", hr);
    if (FAILED(devB_.As(&devB1_)) || FAILED(devB_.As(&devB5_)) || FAILED(ctxB_.As(&ctxB4_))) return Fail("device B D3D11.4 interfaces", E_NOINTERFACE);
    BridgeLog("bridge: device B created, feature level 0x%X, device=%p ctx=%p", got, devB_.Get(), ctxB_.Get());
    return true;
}

inline bool Bridge::CreateFences()
{
    auto make = [&](ComPtr<ID3D11Fence> &owner, ComPtr<ID3D11Fence> &opened, const char *name) -> bool {
        HRESULT hr = devB5_->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&owner));
        if (FAILED(hr)) return Fail(name, hr);
        HANDLE h = nullptr;
        hr = owner->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h);
        if (FAILED(hr)) return Fail("fence CreateSharedHandle", hr);
        hr = devA5_->OpenSharedFence(h, IID_PPV_ARGS(&opened));
        CloseHandle(h);
        if (FAILED(hr)) return Fail("device A OpenSharedFence", hr);
        return true;
    };
    return make(fenceAB_B_, fenceAB_A_, "CreateFence AB") && make(fenceBA_B_, fenceBA_A_, "CreateFence BA");
}

inline bool Bridge::CreateBlit()
{
    static const char *src =
        "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
        "V VS(uint id : SV_VertexID) { V o; float2 uv = float2((id << 1) & 2, id & 2);"
        " o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
        "Texture2D t : register(t0); SamplerState s : register(s0);\n"
        "float4 PS(V i) : SV_Target { return t.Sample(s, i.uv); }\n";
    ComPtr<ID3DBlob> vsb, psb, err;
    HRESULT hr = D3DCompile(src, strlen(src), "blit", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) { if (err) BridgeLog("VS: %s", (const char *)err->GetBufferPointer()); return Fail("D3DCompile VS", hr); }
    hr = D3DCompile(src, strlen(src), "blit", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psb, &err);
    if (FAILED(hr)) { if (err) BridgeLog("PS: %s", (const char *)err->GetBufferPointer()); return Fail("D3DCompile PS", hr); }
    if (FAILED(hr = devB_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_))) return Fail("CreateVertexShader", hr);
    if (FAILED(hr = devB_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_))) return Fail("CreatePixelShader", hr);
    D3D11_SAMPLER_DESC sd = {}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(hr = devB_->CreateSamplerState(&sd, &samp_))) return Fail("CreateSamplerState", hr);
    D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    if (FAILED(hr = devB_->CreateRasterizerState(&rd, &rs_))) return Fail("CreateRasterizerState", hr);
    return true;
}

inline bool Bridge::CreateSharedPair(UINT w, UINT h, DXGI_FORMAT fmt)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    HRESULT hr = devB_->CreateTexture2D(&d, nullptr, &sharedInB_);
    if (FAILED(hr))
    {
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        hr = devB_->CreateTexture2D(&d, nullptr, &sharedInB_);
        if (FAILED(hr)) return Fail("CreateTexture2D shared (both flag variants)", hr);
        keyed_ = true;
        BridgeLog("bridge: plain NT-handle sharing refused, using keyed-mutex fallback (adds CPU waits)");
    }
    if (FAILED(hr = devB_->CreateTexture2D(&d, nullptr, &sharedOutB_))) return Fail("CreateTexture2D sharedOut", hr);

    auto open = [&](ComPtr<ID3D11Texture2D> &b, ComPtr<ID3D11Texture2D> &a, ComPtr<IDXGIKeyedMutex> &kmA, ComPtr<IDXGIKeyedMutex> &kmB) -> bool {
        ComPtr<IDXGIResource1> r; HANDLE hnd = nullptr;
        if (FAILED(b.As(&r))) return Fail("IDXGIResource1", E_NOINTERFACE);
        HRESULT hr2 = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &hnd);
        if (FAILED(hr2)) return Fail("texture CreateSharedHandle", hr2);
        hr2 = devA1_->OpenSharedResource1(hnd, IID_PPV_ARGS(&a));
        CloseHandle(hnd);
        if (FAILED(hr2)) return Fail("device A OpenSharedResource1", hr2);
        if (keyed_) { if (FAILED(a.As(&kmA)) || FAILED(b.As(&kmB))) return Fail("IDXGIKeyedMutex", E_NOINTERFACE); }
        return true;
    };
    if (!open(sharedInB_, sharedInA_, kmInA_, kmInB_) || !open(sharedOutB_, sharedOutA_, kmOutA_, kmOutB_)) return false;
    if (FAILED(hr = devB_->CreateShaderResourceView(sharedInB_.Get(), nullptr, &srvInB_))) return Fail("SRV sharedIn", hr);
    if (FAILED(hr = devB_->CreateRenderTargetView(sharedOutB_.Get(), nullptr, &rtvOutB_))) return Fail("RTV sharedOut", hr);
    return true;
}

inline bool Bridge::CreateSwapchainB(UINT w, UINT h, DXGI_FORMAT fmt)
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&Bridge::WndProc, &self);
    if (!wndClass_)
    {
        WNDCLASSEXW wc = { sizeof(wc) }; wc.lpfnWndProc = &Bridge::WndProc; wc.hInstance = self; wc.lpszClassName = L"NRBridgeWindow";
        wndClass_ = RegisterClassExW(&wc);
        if (!wndClass_ && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return Fail("RegisterClassExW", HRESULT_FROM_WIN32(GetLastError()));
    }
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"NRBridgeWindow", L"NR bridge",
                            WS_POPUP, 0, 0, (int)w, (int)h, nullptr, nullptr, self, nullptr);
    if (!hwnd_) return Fail("CreateWindowExW", HRESULT_FROM_WIN32(GetLastError()));
    if (cfg_.window_visible) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

    // CreateDXGIFactory1 is hooked by ReShade too -> proxy factory -> the swap chain gets wrapped.
    ComPtr<IDXGIFactory2> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return Fail("CreateDXGIFactory1", hr);
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = w; sd.Height = h; sd.Format = fmt; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    sd.BufferCount = 1;
    sd.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL; // bit-blt model: back buffer contents survive Present
    sd.Scaling = DXGI_SCALING_STRETCH;
    hr = factory->CreateSwapChainForHwnd(devB_.Get(), hwnd_, &sd, nullptr, nullptr, &swapB_);
    if (FAILED(hr)) return Fail("CreateSwapChainForHwnd", hr);
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    if (FAILED(hr = swapB_->GetBuffer(0, IID_PPV_ARGS(&bbB_)))) return Fail("GetBuffer", hr);
    if (FAILED(hr = devB_->CreateRenderTargetView(bbB_.Get(), nullptr, &rtvBB_))) return Fail("RTV backbuffer", hr);
    if (FAILED(hr = devB_->CreateShaderResourceView(bbB_.Get(), nullptr, &srvBB_))) return Fail("SRV backbuffer", hr);
    BridgeLog("bridge: hidden swap chain %ux%u format %d on device B (hwnd=%p)", w, h, (int)fmt, hwnd_);
    return true;
}

inline void Bridge::ReleaseSized()
{
    srvInB_.Reset(); srvBB_.Reset(); rtvOutB_.Reset(); rtvBB_.Reset(); bbB_.Reset(); swapB_.Reset();
    kmInA_.Reset(); kmInB_.Reset(); kmOutA_.Reset(); kmOutB_.Reset();
    sharedInA_.Reset(); sharedOutA_.Reset(); sharedInB_.Reset(); sharedOutB_.Reset();
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    W_ = H_ = workW_ = workH_ = 0; fmt_ = DXGI_FORMAT_UNKNOWN;
}

inline bool Bridge::EnsureSized(UINT w, UINT h, DXGI_FORMAT fmt)
{
    if (w == W_ && h == H_ && fmt == fmt_) return true;
    if (W_) BridgeLog("bridge: layout changed %ux%u -> %ux%u, rebuilding", W_, H_, w, h);
    ReleaseSized();
    UINT ww = std::max<UINT>(2, (UINT)(w * cfg_.scale) & ~1u), wh = std::max<UINT>(2, (UINT)(h * cfg_.scale) & ~1u);
    if (!scaled_) { ww = w; wh = h; }
    if (!CreateSharedPair(w, h, fmt) || !CreateSwapchainB(ww, wh, fmt)) return false;
    W_ = w; H_ = h; workW_ = ww; workH_ = wh; fmt_ = fmt;
    BridgeLog("bridge: eye pair %ux%u, NR work size %ux%u, format %d", w, h, ww, wh, (int)fmt);
    return true;
}

inline void Bridge::Blit(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst, UINT w, UINT h)
{
    ctx->ClearState();
    ctx->OMSetRenderTargets(1, &dst, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 }; ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rs_.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.Get(), nullptr, 0); ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &src); ID3D11SamplerState *s = samp_.Get(); ctx->PSSetSamplers(0, 1, &s);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView *nul = nullptr; ctx->PSSetShaderResources(0, 1, &nul);
    ID3D11RenderTargetView *nrt = nullptr; ctx->OMSetRenderTargets(1, &nrt, nullptr);
}

inline bool Bridge::Process(const BridgeView *views, uint32_t count)
{
    if (failed_ || count == 0 || count > 8) return false;
    UINT W = 0, H = 0;
    for (uint32_t i = 0; i < count; ++i) { W += views[i].right - views[i].left; H = std::max(H, views[i].bottom - views[i].top); }
    if (W == 0 || H == 0) return false;
    D3D11_TEXTURE2D_DESC td = {}; views[0].texture->GetDesc(&td);
    if (td.SampleDesc.Count > 1) { MarkFailed("multisampled XR images are not supported"); return false; }
    const DXGI_FORMAT fmt = BridgeTypedFormat(td.Format);
    if (fmt == DXGI_FORMAT_UNKNOWN) { char b[96]; snprintf(b, sizeof(b), "unsupported XR image format %d", (int)td.Format); MarkFailed(b); return false; }
    if (!EnsureSized(W, H, fmt)) return false;

    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
    const bool verbose = frames_ < (uint64_t)cfg_.log_frames;

    // ---- A: gather eyes side by side into sharedIn ----
    if (keyed_ && FAILED(kmInA_->AcquireSync(0, 2000))) return Fail("keyed mutex in (A)", E_FAIL);
    UINT x = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const BridgeView &v = views[i];
        D3D11_TEXTURE2D_DESC vd = {}; v.texture->GetDesc(&vd);
        D3D11_BOX box = { v.left, v.top, 0, v.right, v.bottom, 1 };
        ctxA_->CopySubresourceRegion(sharedInA_.Get(), 0, x, 0, 0, v.texture, D3D11CalcSubresource(0, v.arrayIndex, vd.MipLevels), &box);
        if (verbose) BridgeLog("bridge: frame %llu view %u: tex=%p slice %u rect %u,%u-%u,%u -> x=%u (image %ux%u fmt %d arr %u)",
                               (unsigned long long)frames_, i, v.texture, v.arrayIndex, v.left, v.top, v.right, v.bottom, x, vd.Width, vd.Height, (int)vd.Format, vd.ArraySize);
        x += v.right - v.left;
    }
    if (keyed_) kmInA_->ReleaseSync(1);
    ctxA4_->Signal(fenceAB_A_.Get(), ++vIn_);
    ctxA_->Flush();

    // ---- B: into the hidden swap chain, Present (ReShade + RenoDX run here), back out ----
    ctxB4_->Wait(fenceAB_B_.Get(), vIn_);
    if (keyed_ && FAILED(kmInB_->AcquireSync(1, 2000))) return Fail("keyed mutex in (B)", E_FAIL);
    if (scaled_) Blit(ctxB_.Get(), srvInB_.Get(), rtvBB_.Get(), workW_, workH_);
    else ctxB_->CopyResource(bbB_.Get(), sharedInB_.Get());
    if (keyed_) kmInB_->ReleaseSync(0);

    if (!cfg_.skip_present)
    {
        HRESULT hr = swapB_->Present(0, 0);
        if (FAILED(hr)) return Fail("Present (device B)", hr);
        if (verbose && hr != S_OK) BridgeLog("bridge: Present returned 0x%08lX (occluded is expected for a hidden window)", (unsigned long)hr);
    }

    if (keyed_ && FAILED(kmOutB_->AcquireSync(0, 2000))) return Fail("keyed mutex out (B)", E_FAIL);
    if (scaled_) Blit(ctxB_.Get(), srvBB_.Get(), rtvOutB_.Get(), W_, H_);
    else ctxB_->CopyResource(sharedOutB_.Get(), bbB_.Get());
    if (keyed_) kmOutB_->ReleaseSync(1);
    ctxB4_->Signal(fenceBA_B_.Get(), ++vOut_);
    ctxB_->Flush();

    // ---- A: scatter the processed pair back into the eye images ----
    ctxA4_->Wait(fenceBA_A_.Get(), vOut_);
    if (keyed_ && FAILED(kmOutA_->AcquireSync(1, 2000))) return Fail("keyed mutex out (A)", E_FAIL);
    x = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const BridgeView &v = views[i];
        D3D11_TEXTURE2D_DESC vd = {}; v.texture->GetDesc(&vd);
        const UINT w = v.right - v.left, h = v.bottom - v.top;
        D3D11_BOX box = { x, 0, 0, x + w, h, 1 };
        ctxA_->CopySubresourceRegion(v.texture, D3D11CalcSubresource(0, v.arrayIndex, vd.MipLevels), v.left, v.top, 0, sharedOutA_.Get(), 0, &box);
        x += w;
    }
    if (keyed_) kmOutA_->ReleaseSync(0);
    ctxA_->Flush();

    QueryPerformanceCounter(&t1);
    lastMs_ = (t1.QuadPart - t0.QuadPart) * 1000.0 / (double)qpf_.QuadPart; totalMs_ += lastMs_;
    ++frames_;
    if (verbose) BridgeLog("bridge: frame %llu done, cpu %.3f ms", (unsigned long long)frames_, lastMs_);
    else if (frames_ % 600 == 0) BridgeLog("bridge: %llu frames, avg cpu %.3f ms (last %.3f)", (unsigned long long)frames_, avgCpuMs(), lastMs_);
    return true;
}

inline void Bridge::Shutdown(bool leakDeviceB)
{
    if (leakDeviceB)
    {
        // Tearing down device B makes ReShade destroy its runtime and the RenoDX add-on destroy its
        // D3D12 proxy, which was observed to crash inside the add-on. The game is exiting anyway, so
        // drop only the game-side references and let device B, its swap chain and window leak.
        sharedInA_.Reset(); sharedOutA_.Reset(); kmInA_.Reset(); kmOutA_.Reset();
        fenceAB_A_.Reset(); fenceBA_A_.Reset();
        ctxA4_.Reset(); ctxA_.Reset(); devA5_.Reset(); devA1_.Reset(); devA_.Reset();
        srvInB_.Detach(); srvBB_.Detach(); rtvOutB_.Detach(); rtvBB_.Detach(); bbB_.Detach(); swapB_.Detach();
        kmInB_.Detach(); kmOutB_.Detach(); sharedInB_.Detach(); sharedOutB_.Detach();
        vs_.Detach(); ps_.Detach(); samp_.Detach(); rs_.Detach();
        fenceAB_B_.Detach(); fenceBA_B_.Detach();
        ctxB4_.Detach(); ctxB_.Detach(); devB5_.Detach(); devB1_.Detach(); devB_.Detach();
        hwnd_ = nullptr;
        BridgeLog("bridge: detached after %llu frames (device B intentionally left alive until process exit)", (unsigned long long)frames_);
        return;
    }
    ReleaseSized();
    vs_.Reset(); ps_.Reset(); samp_.Reset(); rs_.Reset();
    fenceAB_A_.Reset(); fenceAB_B_.Reset(); fenceBA_A_.Reset(); fenceBA_B_.Reset();
    ctxB4_.Reset(); ctxB_.Reset(); devB5_.Reset(); devB1_.Reset(); devB_.Reset();
    ctxA4_.Reset(); ctxA_.Reset(); devA5_.Reset(); devA1_.Reset(); devA_.Reset();
    BridgeLog("bridge: shut down after %llu frames", (unsigned long long)frames_);
}

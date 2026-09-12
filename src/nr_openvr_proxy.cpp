// nr-bridge: OpenVR front end. A drop-in replacement for a game's openvr_api.dll that forwards
// every export to the original (renamed openvr_api.orig.dll next to it) and hooks IVRCompositor::Submit.
// The left eye's Submit is held back; when the right eye arrives both eyes go through the direct engine
// (nr_direct.h) in one pass, exactly like the OpenXR layer's xrEndFrame path, and are then submitted.
//
// Hooking: VR_GetGenericInterface is intercepted, and the vtable (or FnTable) of every IVRCompositor the
// app requests gets its Submit slot patched. Slot numbers per interface version (pinned against the SDK
// headers v1.0.10 .. v2.5.1 and master):
//   IVRCompositor_0xx <= 028: Submit = 5, SubmitWithArrayIndex = 6 (from 027)
//   IVRCompositor_029+:       GetSubmitTexture was inserted at 5, so Submit = 6, SubmitWithArrayIndex = 7
//   GetLastPoses = 3 in every version. IVRSystem_022 (requested by us for poses/FOV): GetProjectionRaw = 2,
//   GetEyeToHeadTransform = 4.
// Author: LowPopLabs
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "nr_direct.h"
#include "nr_common.h"
#include <mutex>
#include <vector>
#include <cstring>
#include <cmath>

#define PROXY_VERSION "0.2.0"

// ---------------------------------------------------------------- minimal OpenVR types (no SDK dependency)
namespace vr
{
enum EVREye { Eye_Left = 0, Eye_Right = 1 };
enum ETextureType { TextureType_DirectX = 0, TextureType_DirectX12 = 4 };
enum EVRSubmitFlags { Submit_TextureWithPose = 0x08, Submit_TextureWithDepth = 0x10 };
enum EVRCompositorError { VRCompositorError_None = 0, VRCompositorError_InvalidTexture = 102 };
struct Texture_t { void *handle; int eType; int eColorSpace; };
struct VRTextureBounds_t { float uMin, vMin, uMax, vMax; };
struct HmdMatrix34_t { float m[3][4]; };
struct TrackedDevicePose_t { HmdMatrix34_t mDeviceToAbsoluteTracking; float vVelocity[3]; float vAngularVelocity[3]; int eTrackingResult; bool bPoseIsValid; bool bDeviceIsConnected; };
static const size_t kTexPoseBytes = sizeof(HmdMatrix34_t);          // VRTextureWithPose_t adds this
static const size_t kTexDepthBytes = 8 + 64 + sizeof(VRTextureBounds_t); // VRTextureDepthInfo_t: handle, 4x4 projection, bounds
}

// ---------------------------------------------------------------- the original DLL
static HMODULE g_module = nullptr, g_real = nullptr;
static std::mutex g_mtx;
static bool g_realTried = false;

static FARPROC Real(const char *name)
{
    if (!g_real && !g_realTried)
    {
        g_realTried = true;
        wchar_t p[MAX_PATH]; swprintf_s(p, L"%s\\openvr_api.orig.dll", g_dir);
        g_real = LoadLibraryExW(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        BridgeLog("openvr proxy " PROXY_VERSION ": original %ls -> %p%s", p, g_real, g_real ? "" : " (NOT FOUND: rename the game's openvr_api.dll to openvr_api.orig.dll next to this one)");
    }
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}

// ---------------------------------------------------------------- hook registry
typedef int (*PFN_SubmitVt)(void *self, int eye, const vr::Texture_t *tex, const vr::VRTextureBounds_t *bounds, int flags);
typedef int (*PFN_SubmitFn)(int eye, const vr::Texture_t *tex, const vr::VRTextureBounds_t *bounds, int flags);
typedef int (*PFN_SubmitArrayVt)(void *self, int eye, const vr::Texture_t *tex, uint32_t arrayIndex, const vr::VRTextureBounds_t *bounds, int flags);
typedef int (*PFN_SubmitArrayFn)(int eye, const vr::Texture_t *tex, uint32_t arrayIndex, const vr::VRTextureBounds_t *bounds, int flags);
typedef int (*PFN_GetLastPosesVt)(void *self, vr::TrackedDevicePose_t *render, uint32_t nRender, vr::TrackedDevicePose_t *game, uint32_t nGame);
typedef int (*PFN_GetLastPosesFn)(vr::TrackedDevicePose_t *render, uint32_t nRender, vr::TrackedDevicePose_t *game, uint32_t nGame);
typedef void (*PFN_GetProjectionRawVt)(void *self, int eye, float *l, float *r, float *t, float *b);
typedef vr::HmdMatrix34_t *(*PFN_GetEyeToHeadVt)(void *self, vr::HmdMatrix34_t *ret, int eye);   // struct return: hidden pointer in RDX
typedef void *(*PFN_GetGenericInterface)(const char *, int *);

struct Hooked
{
    void **table = nullptr; void *self = nullptr; bool fnTable = false; int version = 0;
    int submitIdx = 5, arrayIdx = -1;
    void *origSubmit = nullptr, *origSubmitArray = nullptr;
};
static std::vector<Hooked> g_hooks;   // at most a few entries: one per (interface version, table kind)

static bool PatchSlot(void **table, int idx, void *fn, void **old)
{
    DWORD prot = 0;
    if (!VirtualProtect(&table[idx], sizeof(void *), PAGE_EXECUTE_READWRITE, &prot)) return false;
    if (old) *old = table[idx];
    table[idx] = fn;
    VirtualProtect(&table[idx], sizeof(void *), prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}

// ---------------------------------------------------------------- engine state
static DirectNR *g_direct = nullptr;
static bool g_directFailed = false, g_bypass = false, g_keyWasDown = false, g_reloadKeyWasDown = false;
static uint64_t g_frames = 0, g_skipped = 0, g_submits = 0;
static bool g_warnedMsaa = false, g_warnedType = false, g_warnedFlip = false, g_warnedSize = false;
static void *g_system = nullptr; bool g_systemTried = false;   // our own IVRSystem_022 for poses and FOV
static uint32_t g_appSystemVersion = 0;

struct Pending
{
    bool valid = false;
    unsigned char tex[16 + vr::kTexPoseBytes + vr::kTexDepthBytes]; size_t texBytes = 0;
    vr::VRTextureBounds_t bounds; bool hadBounds = false; int flags = 0;
    ID3D11Texture2D *texture = nullptr; UINT arrayIndex = 0;
    bool viaArray = false; uint32_t arrayIndexArg = 0;
};
static Pending g_left;

static bool SafeProcessDirect(DirectNR *d, const DirectView *v, uint32_t n, unsigned long *code)
{
    __try { return d->Process(v, n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return false; }
}

static void MaybeReloadCfg()
{
    bool reload = false;
    if (g_cfg.reload_vk)
    {
        const bool down = (GetAsyncKeyState(g_cfg.reload_vk) & 0x8000) != 0;
        if (down && !g_reloadKeyWasDown) reload = true;
        g_reloadKeyWasDown = down;
    }
    if (!reload && (g_frames % 60) == 0)
    {
        FILETIME ft = {};
        if (CfgFileTime(&ft) && (ft.dwLowDateTime != g_cfgTime.dwLowDateTime || ft.dwHighDateTime != g_cfgTime.dwHighDateTime)) reload = true;
    }
    if (!reload) return;
    LoadCfg(true);
    BridgeLog("cfg reloaded from disk");
    if (g_direct) { DirectConfig dc = g_cfg.direct; dc.log_frames = g_cfg.log_frames; g_direct->Reconfigure(dc); }
}

// quaternion (x,y,z,w) from the 3x3 part of an OpenVR row-major 3x4 matrix
static void QuatFrom34(const vr::HmdMatrix34_t &m, float q[4])
{
    const float tr = m.m[0][0] + m.m[1][1] + m.m[2][2];
    if (tr > 0.0f) { const float s = sqrtf(tr + 1.0f) * 2.0f; q[3] = 0.25f * s; q[0] = (m.m[2][1] - m.m[1][2]) / s; q[1] = (m.m[0][2] - m.m[2][0]) / s; q[2] = (m.m[1][0] - m.m[0][1]) / s; }
    else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) { const float s = sqrtf(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.0f; q[3] = (m.m[2][1] - m.m[1][2]) / s; q[0] = 0.25f * s; q[1] = (m.m[0][1] + m.m[1][0]) / s; q[2] = (m.m[0][2] + m.m[2][0]) / s; }
    else if (m.m[1][1] > m.m[2][2]) { const float s = sqrtf(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.0f; q[3] = (m.m[0][2] - m.m[2][0]) / s; q[0] = (m.m[0][1] + m.m[1][0]) / s; q[1] = 0.25f * s; q[2] = (m.m[1][2] + m.m[2][1]) / s; }
    else { const float s = sqrtf(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.0f; q[3] = (m.m[1][0] - m.m[0][1]) / s; q[0] = (m.m[0][2] + m.m[2][0]) / s; q[1] = (m.m[1][2] + m.m[2][1]) / s; q[2] = 0.25f * s; }
    const float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]); if (n > 0) for (int i = 0; i < 4; ++i) q[i] /= n;
}
static vr::HmdMatrix34_t Mul34(const vr::HmdMatrix34_t &a, const vr::HmdMatrix34_t &b)
{
    vr::HmdMatrix34_t r = {};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) { r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + (j == 3 ? a.m[i][3] : 0.0f); }
    return r;
}

// poses and FOV for the two views: HMD render pose from the app's compositor (GetLastPoses, slot 3 in every version)
// times the eye-to-head transform, and the projection tangents, both from our own IVRSystem_022
static void FillPoses(const Hooked &h, DirectView *v)
{
    for (int i = 0; i < 2; ++i) { v[i].q[0] = v[i].q[1] = v[i].q[2] = 0; v[i].q[3] = 1; v[i].p[0] = v[i].p[1] = v[i].p[2] = 0; v[i].fovL = -0.9f; v[i].fovR = 0.9f; v[i].fovU = 0.9f; v[i].fovD = -0.9f; }
    if (!g_systemTried)
    {
        g_systemTried = true;
        int err = 0; PFN_GetGenericInterface gi = (PFN_GetGenericInterface)Real("VR_GetGenericInterface");
        g_system = gi ? gi("IVRSystem_022", &err) : nullptr;
        BridgeLog("openvr proxy: IVRSystem_022 for poses/FOV -> %p (err %d)%s", g_system, err, g_system ? "" : "; motion vectors will use zero rotation");
    }
    vr::TrackedDevicePose_t pose = {};
    int pr = h.fnTable ? ((PFN_GetLastPosesFn)h.table[3])(&pose, 1, nullptr, 0) : ((PFN_GetLastPosesVt)h.table[3])(h.self, &pose, 1, nullptr, 0);
    if (!g_system) return;
    void **sysVt = *(void ***)g_system;
    for (int i = 0; i < 2; ++i)
    {
        float l = -1, r = 1, t = -1, b = 1;
        ((PFN_GetProjectionRawVt)sysVt[2])(g_system, i, &l, &r, &t, &b);
        // OpenVR's raw projection: left/top negative, right/bottom positive (y grows downward); OpenXR's fov: up positive, down negative
        v[i].fovL = atanf(l); v[i].fovR = atanf(r); v[i].fovU = atanf(-t); v[i].fovD = atanf(-b);
        vr::HmdMatrix34_t e2h = {}; ((PFN_GetEyeToHeadVt)sysVt[4])(g_system, &e2h, i);
        if (pr == 0 && pose.bPoseIsValid)
        {
            const vr::HmdMatrix34_t eye = Mul34(pose.mDeviceToAbsoluteTracking, e2h);
            QuatFrom34(eye, v[i].q); v[i].p[0] = eye.m[0][3]; v[i].p[1] = eye.m[1][3]; v[i].p[2] = eye.m[2][3];
        }
    }
}

static int CallOrigSubmit(const Hooked &h, int eye, const vr::Texture_t *tex, const vr::VRTextureBounds_t *bounds, int flags)
{
    return h.fnTable ? ((PFN_SubmitFn)h.origSubmit)(eye, tex, bounds, flags) : ((PFN_SubmitVt)h.origSubmit)(h.self, eye, tex, bounds, flags);
}
static int CallOrigSubmitArray(const Hooked &h, int eye, const vr::Texture_t *tex, uint32_t idx, const vr::VRTextureBounds_t *bounds, int flags)
{
    return h.fnTable ? ((PFN_SubmitArrayFn)h.origSubmitArray)(eye, tex, idx, bounds, flags) : ((PFN_SubmitArrayVt)h.origSubmitArray)(h.self, eye, tex, idx, bounds, flags);
}
static int FlushPending(const Hooked &h)
{
    if (!g_left.valid) return 0;
    g_left.valid = false;
    const vr::Texture_t *t = (const vr::Texture_t *)g_left.tex; const vr::VRTextureBounds_t *b = g_left.hadBounds ? &g_left.bounds : nullptr;
    return g_left.viaArray ? CallOrigSubmitArray(h, vr::Eye_Left, t, g_left.arrayIndexArg, b, g_left.flags) : CallOrigSubmit(h, vr::Eye_Left, t, b, g_left.flags);
}

static size_t TexBytes(int flags)
{
    size_t n = sizeof(vr::Texture_t);
    if (flags & vr::Submit_TextureWithPose) n += vr::kTexPoseBytes;
    if (flags & vr::Submit_TextureWithDepth) n += vr::kTexDepthBytes;
    return n;
}

// Both eyes are in hand: run the pair through the engine (in place, into the app's textures), then submit both.
static int ProcessPairAndSubmit(const Hooked &h, const vr::Texture_t *rtex, uint32_t rArrayIdx, bool rViaArray, const vr::VRTextureBounds_t *rbounds, int rflags)
{
    ++g_frames;
    MaybeReloadCfg();
    if (g_cfg.toggle_vk)
    {
        const bool down = (GetAsyncKeyState(g_cfg.toggle_vk) & 0x8000) != 0;
        if (down && !g_keyWasDown) { g_bypass = !g_bypass; BridgeLog("toggle: bridge %s", g_bypass ? "BYPASSED" : "active"); if (!g_bypass && g_direct) g_direct->RequestReset(); }
        g_keyWasDown = down;
    }
    ID3D11Texture2D *rt = (ID3D11Texture2D *)rtex->handle;
    bool ok = !g_bypass && g_cfg.enabled && g_left.texture && rt;
    D3D11_TEXTURE2D_DESC ld = {}, rd = {};
    if (ok)
    {
        g_left.texture->GetDesc(&ld); rt->GetDesc(&rd);
        if (ld.SampleDesc.Count > 1 || rd.SampleDesc.Count > 1) { ok = false; if (!g_warnedMsaa) { g_warnedMsaa = true; BridgeLog("openvr proxy: the app submits MSAA textures (%u samples); not supported, passing through", ld.SampleDesc.Count); } }
    }
    if (ok && !g_direct && !g_directFailed)
    {
        ComPtr<ID3D11Device> dev; rt->GetDevice(&dev);
        wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH); if (wchar_t *sl = wcsrchr(exe, L'\\')) *sl = 0;
        DirectConfig dc = g_cfg.direct; dc.log_frames = g_cfg.log_frames;
        g_direct = new DirectNR();
        if (!g_direct->Init(dev.Get(), dc, exe, g_home)) { g_direct->Shutdown(true); delete g_direct; g_direct = nullptr; g_directFailed = true; BridgeLog("openvr proxy: direct engine init FAILED, passing through"); }
        else BridgeLog("openvr proxy: direct engine ACTIVE on device %p (IVRCompositor_%03d %s), %s home %ls%s", dev.Get(), h.version, h.fnTable ? "FnTable" : "vtable",
                       g_central ? "central" : "standalone", g_home, g_gameCfgFound ? " + per-game cfg" : "");
    }
    if (ok && g_direct && !g_direct->failed())
    {
        DirectView v[2] = {};
        // the two eyes share one texture side by side when they use the same texture and slice
        v[0].texture = g_left.texture; v[0].arrayIndex = g_left.arrayIndex;
        v[1].texture = rt; v[1].arrayIndex = rViaArray ? rArrayIdx : (rd.ArraySize > 1 ? 1u : 0u);
        const bool sideBySide = v[0].texture == v[1].texture && v[0].arrayIndex == v[1].arrayIndex;
        auto rectOf = [&](const D3D11_TEXTURE2D_DESC &d, const vr::VRTextureBounds_t *b, bool had, DirectView &dv) {
            float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
            if (had) { u0 = b->uMin; u1 = b->uMax; v0 = b->vMin; v1 = b->vMax; }
            if (v0 > v1) { std::swap(v0, v1); dv.flipY = true; if (!g_warnedFlip) { g_warnedFlip = true; BridgeLog("openvr proxy: texture bounds are vertically flipped (vMin > vMax): the eyes are processed mirrored so the model sees them upright"); } }
            if (u0 > u1) std::swap(u0, u1);
            dv.left = (UINT)lroundf(u0 * d.Width); dv.right = (UINT)lroundf(u1 * d.Width); dv.top = (UINT)lroundf(v0 * d.Height); dv.bottom = (UINT)lroundf(v1 * d.Height);
            dv.right = std::min(dv.right, d.Width); dv.bottom = std::min(dv.bottom, d.Height);
            // dynamic viewport (SteamVR adaptive resolution shrinks the bounds per frame): size the engine from the whole texture
            dv.maxW = sideBySide ? d.Width / 2 : d.Width; dv.maxH = d.Height;
        };
        rectOf(ld, &g_left.bounds, g_left.hadBounds, v[0]); rectOf(rd, rbounds, rbounds != nullptr, v[1]);
        if (v[0].right > v[0].left && v[0].bottom > v[0].top && v[1].right > v[1].left && v[1].bottom > v[1].top)
        {
            FillPoses(h, v);
            if (g_frames <= 2)
                BridgeLog("openvr proxy: pair L tex=%p slice %u rect %u,%u-%u,%u | R tex=%p slice %u rect %u,%u-%u,%u | %ux%u arr %u fmt %d%s%s | q=(%.3f %.3f %.3f %.3f) fov=(%.3f %.3f %.3f %.3f)",
                          v[0].texture, v[0].arrayIndex, v[0].left, v[0].top, v[0].right, v[0].bottom, v[1].texture, v[1].arrayIndex, v[1].left, v[1].top, v[1].right, v[1].bottom,
                          rd.Width, rd.Height, rd.ArraySize, (int)rd.Format, sideBySide ? " side-by-side" : "", v[0].flipY ? " flipped" : "", v[0].q[0], v[0].q[1], v[0].q[2], v[0].q[3], v[0].fovL, v[0].fovR, v[0].fovU, v[0].fovD);
            unsigned long code = 0;
            if (!SafeProcessDirect(g_direct, v, 2, &code)) { ++g_skipped; if (code) { char b[96]; snprintf(b, sizeof(b), "exception 0x%08lX inside direct Process", code); g_direct->MarkFailed(b); } }
        }
        else if (!g_warnedSize) { g_warnedSize = true; BridgeLog("openvr proxy: empty eye rect from bounds, passing through"); }
    }
    const int rl = FlushPending(h);
    const int rr = rViaArray ? CallOrigSubmitArray(h, vr::Eye_Right, rtex, rArrayIdx, rbounds, rflags) : CallOrigSubmit(h, vr::Eye_Right, rtex, rbounds, rflags);
    return rr != 0 ? rr : rl;
}

static int OnSubmit(Hooked &h, int eye, const vr::Texture_t *tex, uint32_t arrayIdx, bool viaArray, const vr::VRTextureBounds_t *bounds, int flags)
{
    ++g_submits;
    if (!tex || tex->eType != vr::TextureType_DirectX || !tex->handle)
    {
        if (!g_warnedType && tex) { g_warnedType = true; BridgeLog("openvr proxy: texture type %d is not D3D11; passing through", tex->eType); }
        FlushPending(h);
        return viaArray ? CallOrigSubmitArray(h, eye, tex, arrayIdx, bounds, flags) : CallOrigSubmit(h, eye, tex, bounds, flags);
    }
    if (eye == vr::Eye_Left)
    {
        FlushPending(h);   // two lefts in a row: let the first one through untouched
        g_left.valid = true; g_left.texBytes = std::min(TexBytes(flags), sizeof(g_left.tex)); memcpy(g_left.tex, tex, g_left.texBytes);
        g_left.hadBounds = bounds != nullptr; if (bounds) g_left.bounds = *bounds; else g_left.bounds = { 0, 0, 1, 1 };
        g_left.flags = flags; g_left.texture = (ID3D11Texture2D *)tex->handle; g_left.viaArray = viaArray; g_left.arrayIndexArg = arrayIdx;
        D3D11_TEXTURE2D_DESC d = {}; g_left.texture->GetDesc(&d);
        g_left.arrayIndex = viaArray ? arrayIdx : (d.ArraySize > 1 ? 0u : 0u);
        return vr::VRCompositorError_None;
    }
    if (!g_left.valid) return viaArray ? CallOrigSubmitArray(h, eye, tex, arrayIdx, bounds, flags) : CallOrigSubmit(h, eye, tex, bounds, flags);
    return ProcessPairAndSubmit(h, tex, arrayIdx, viaArray, bounds, flags);
}

// ---------------------------------------------------------------- trampolines (one pair per registry slot; slots are found by table)
static Hooked *FindByTable(void **table) { for (auto &h : g_hooks) if (h.table == table) return &h; return nullptr; }
static Hooked *FindBySelf(void *self) { void **t = *(void ***)self; return FindByTable(t); }

static int HookSubmitVt(void *self, int eye, const vr::Texture_t *tex, const vr::VRTextureBounds_t *bounds, int flags)
{
    Hooked *h = FindBySelf(self); if (!h) return vr::VRCompositorError_InvalidTexture;
    return OnSubmit(*h, eye, tex, 0, false, bounds, flags);
}
static int HookSubmitArrayVt(void *self, int eye, const vr::Texture_t *tex, uint32_t idx, const vr::VRTextureBounds_t *bounds, int flags)
{
    Hooked *h = FindBySelf(self); if (!h) return vr::VRCompositorError_InvalidTexture;
    return OnSubmit(*h, eye, tex, idx, true, bounds, flags);
}
// FnTable variants carry no `this`; only one FnTable compositor per process is supported (the first registered)
static Hooked *FnTableHook() { for (auto &h : g_hooks) if (h.fnTable) return &h; return nullptr; }
static int HookSubmitFn(int eye, const vr::Texture_t *tex, const vr::VRTextureBounds_t *bounds, int flags)
{
    Hooked *h = FnTableHook(); if (!h) return vr::VRCompositorError_InvalidTexture;
    return OnSubmit(*h, eye, tex, 0, false, bounds, flags);
}
static int HookSubmitArrayFn(int eye, const vr::Texture_t *tex, uint32_t idx, const vr::VRTextureBounds_t *bounds, int flags)
{
    Hooked *h = FnTableHook(); if (!h) return vr::VRCompositorError_InvalidTexture;
    return OnSubmit(*h, eye, tex, idx, true, bounds, flags);
}

static void HookCompositor(void *iface, int version, bool fnTable)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    void **table = fnTable ? (void **)iface : *(void ***)iface;
    if (Hooked *e = FindByTable(table)) { e->self = iface; return; }   // same interface handed out again
    if (fnTable && FnTableHook()) { BridgeLog("openvr proxy: a second FnTable compositor (IVRCompositor_%03d) is not supported, left unhooked", version); return; }
    Hooked h; h.table = table; h.self = iface; h.fnTable = fnTable; h.version = version;
    h.submitIdx = version >= 29 ? 6 : 5; h.arrayIdx = version >= 29 ? 7 : (version >= 27 ? 6 : -1);
    if (!PatchSlot(table, h.submitIdx, fnTable ? (void *)HookSubmitFn : (void *)HookSubmitVt, &h.origSubmit)) { BridgeLog("openvr proxy: VirtualProtect failed on IVRCompositor_%03d table %p", version, table); return; }
    if (h.arrayIdx >= 0) PatchSlot(table, h.arrayIdx, fnTable ? (void *)HookSubmitArrayFn : (void *)HookSubmitArrayVt, &h.origSubmitArray);
    g_hooks.push_back(h);
    BridgeLog("openvr proxy: hooked IVRCompositor_%03d %s at %p: Submit slot %d (orig %p)%s", version, fnTable ? "FnTable" : "vtable", table, h.submitIdx, h.origSubmit,
              h.arrayIdx >= 0 ? " + SubmitWithArrayIndex" : "");
}

static void UnhookAll()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto &h : g_hooks)
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(h.table, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT)
        {
            void *cur = nullptr;
            if (h.origSubmit) PatchSlot(h.table, h.submitIdx, h.origSubmit, &cur);
            if (h.arrayIdx >= 0 && h.origSubmitArray) PatchSlot(h.table, h.arrayIdx, h.origSubmitArray, &cur);
        }
    }
    g_hooks.clear(); g_left.valid = false; g_system = nullptr; g_systemTried = false;
}

// ---------------------------------------------------------------- exports (same set as openvr_api.dll)
extern "C"
{
__declspec(dllexport) void *VR_GetGenericInterface(const char *ver, int *err)
{
    LoadCfg();
    PFN_GetGenericInterface f = (PFN_GetGenericInterface)Real("VR_GetGenericInterface");
    if (!f) { if (err) *err = 100; return nullptr; }   // VRInitError_Init_InstallationNotFound
    void *p = f(ver, err);
    if (p && ver)
    {
        if (strncmp(ver, "IVRCompositor_", 14) == 0) HookCompositor(p, atoi(ver + 14), false);
        else if (strncmp(ver, "FnTable:IVRCompositor_", 22) == 0) HookCompositor(p, atoi(ver + 22), true);
        else if (strncmp(ver, "IVRSystem_", 10) == 0) g_appSystemVersion = (uint32_t)atoi(ver + 10);
        else if (strncmp(ver, "FnTable:IVRSystem_", 18) == 0) g_appSystemVersion = (uint32_t)atoi(ver + 18);
    }
    return p;
}
__declspec(dllexport) uint32_t VR_InitInternal2(int *err, int appType, const char *startupInfo)
{
    LoadCfg();
    typedef uint32_t (*F)(int *, int, const char *); F f = (F)Real("VR_InitInternal2");
    if (!f) { typedef uint32_t (*F1)(int *, int); F1 f1 = (F1)Real("VR_InitInternal"); if (!f1) { if (err) *err = 100; return 0; } const uint32_t t = f1(err, appType); BridgeLog("VR_InitInternal(app type %d) -> token %u err %d", appType, t, err ? *err : -1); return t; }
    const uint32_t t = f(err, appType, startupInfo);
    BridgeLog("VR_InitInternal2(app type %d) -> token %u err %d", appType, t, err ? *err : -1);
    return t;
}
__declspec(dllexport) uint32_t VR_InitInternal(int *err, int appType)
{
    LoadCfg();
    typedef uint32_t (*F)(int *, int); F f = (F)Real("VR_InitInternal");
    if (!f) { if (err) *err = 100; return 0; }
    const uint32_t t = f(err, appType);
    BridgeLog("VR_InitInternal(app type %d) -> token %u err %d", appType, t, err ? *err : -1);
    return t;
}
__declspec(dllexport) void VR_ShutdownInternal()
{
    BridgeLog("VR_ShutdownInternal: %llu frames processed, %llu skipped, %llu submits", (unsigned long long)g_frames, (unsigned long long)g_skipped, (unsigned long long)g_submits);
    UnhookAll();
    if (g_direct) { g_direct->Shutdown(true); delete g_direct; g_direct = nullptr; }
    g_directFailed = false;
    typedef void (*F)(); F f = (F)Real("VR_ShutdownInternal"); if (f) f();
}
__declspec(dllexport) bool VR_IsHmdPresent() { typedef bool (*F)(); F f = (F)Real("VR_IsHmdPresent"); return f ? f() : false; }
__declspec(dllexport) bool VR_IsRuntimeInstalled() { typedef bool (*F)(); F f = (F)Real("VR_IsRuntimeInstalled"); return f ? f() : false; }
__declspec(dllexport) const char *VR_RuntimePath() { typedef const char *(*F)(); F f = (F)Real("VR_RuntimePath"); return f ? f() : nullptr; }
__declspec(dllexport) bool VR_GetRuntimePath(char *buf, uint32_t size, uint32_t *req) { typedef bool (*F)(char *, uint32_t, uint32_t *); F f = (F)Real("VR_GetRuntimePath"); return f ? f(buf, size, req) : false; }
__declspec(dllexport) const char *VR_GetVRInitErrorAsSymbol(int e) { typedef const char *(*F)(int); F f = (F)Real("VR_GetVRInitErrorAsSymbol"); return f ? f(e) : "VRInitError_Unknown"; }
__declspec(dllexport) const char *VR_GetVRInitErrorAsEnglishDescription(int e) { typedef const char *(*F)(int); F f = (F)Real("VR_GetVRInitErrorAsEnglishDescription"); return f ? f(e) : "nr-bridge: openvr_api.orig.dll not found"; }
__declspec(dllexport) const char *VR_GetStringForHmdError(int e) { typedef const char *(*F)(int); F f = (F)Real("VR_GetStringForHmdError"); return f ? f(e) : "nr-bridge: openvr_api.orig.dll not found"; }
__declspec(dllexport) bool VR_IsInterfaceVersionValid(const char *ver) { typedef bool (*F)(const char *); F f = (F)Real("VR_IsInterfaceVersionValid"); return f ? f(ver) : false; }
__declspec(dllexport) uint32_t VR_GetInitToken() { typedef uint32_t (*F)(); F f = (F)Real("VR_GetInitToken"); return f ? f() : 0; }
// internal helpers exported by the real DLL (no arguments, return an interface pointer)
#define BNR_FWD0(name) __declspec(dllexport) void *name() { typedef void *(*F)(); F f = (F)Real(#name); return f ? f() : nullptr; }
BNR_FWD0(LiquidVR) BNR_FWD0(VRControlPanel) BNR_FWD0(VRHeadsetView) BNR_FWD0(VRPaths) BNR_FWD0(VRVirtualDisplay)
BNR_FWD0(VRCompositorSystemInternal) BNR_FWD0(VRDashboardManager) BNR_FWD0(VROculusDirect) BNR_FWD0(VRRenderModelsInternal) BNR_FWD0(VRTrackedCameraInternal)

// test hooks (not part of the OpenVR surface): let a harness inspect the registry without a runtime
__declspec(dllexport) int bnr_test_hook_count() { std::lock_guard<std::mutex> lk(g_mtx); return (int)g_hooks.size(); }
__declspec(dllexport) unsigned long long bnr_test_frames() { return g_frames; }
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
        InitPaths(module);
    }
    return TRUE;
}

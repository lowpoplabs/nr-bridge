// nr-bridge: OpenXR API layer. Sits between the game and the OpenXR runtime, and at every
// xrEndFrame runs the projection layer's eye images through the direct engine (nr_direct.h) or the
// ReShade bridge (nr_bridge_core.h).
//
// Author: LowPopLabs
// Build: see build.cmd. Register: nr-bridge.json as an implicit layer (install-bridge.cmd).
#include "nr_bridge_core.h"
#include "nr_direct.h"
#include "nr_common.h"
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"
#include "openxr/openxr_loader_negotiation.h"
#include <mutex>
#include <unordered_map>
#include <deque>
#include <string>
#include <cstdarg>
#include <cstring>

#define BRIDGE_VERSION "0.2.1"
static const char *const kLayerName = "XR_APILAYER_LOWPOPLABS_nr_bridge";
static HMODULE g_module = nullptr;

// ---------------------------------------------------------------- layer state
struct InstanceRec
{
    PFN_xrGetInstanceProcAddr nextGIPA = nullptr;
    PFN_xrDestroyInstance DestroyInstance = nullptr;
    PFN_xrCreateSession CreateSession = nullptr;
    PFN_xrDestroySession DestroySession = nullptr;
    PFN_xrCreateSwapchain CreateSwapchain = nullptr;
    PFN_xrDestroySwapchain DestroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;
    PFN_xrEndFrame EndFrame = nullptr;
    bool active = false;
};
struct SessionRec
{
    XrInstance instance = XR_NULL_HANDLE;
    ComPtr<ID3D11Device> devA;
    Bridge *bridge = nullptr;    // reshade mode
    DirectNR *direct = nullptr;  // direct mode
};
struct SwapchainRec
{
    XrSession session = XR_NULL_HANDLE;
    XrSwapchainCreateFlags createFlags = 0;
    std::vector<ComPtr<ID3D11Texture2D>> images;
    std::deque<uint32_t> acquired;
    uint32_t lastReleased = 0;
};

static std::mutex g_mtx;
static std::unordered_map<XrInstance, InstanceRec> g_instances;
static std::unordered_map<XrSession, SessionRec> g_sessions;
static std::unordered_map<XrSwapchain, SwapchainRec> g_swapchains;
static PFN_xrGetInstanceProcAddr g_lastNextGIPA = nullptr;
static bool g_bypass = false, g_keyWasDown = false, g_reloadKeyWasDown = false;
static uint64_t g_endFrames = 0, g_skipped = 0;

// Re-reads the cfg when it changed on disk (checked every 60 frames) or when the reload key is pressed,
// and pushes the new values into the direct engine without a restart.
static void MaybeReloadCfg(DirectNR *direct)
{
    bool reload = false;
    if (g_cfg.reload_vk)
    {
        const bool down = (GetAsyncKeyState(g_cfg.reload_vk) & 0x8000) != 0;
        if (down && !g_reloadKeyWasDown) reload = true;
        g_reloadKeyWasDown = down;
    }
    if (!reload && (g_endFrames % 60) == 0)
    {
        FILETIME ft = {};
        if (CfgFileTime(&ft) && (ft.dwLowDateTime != g_cfgTime.dwLowDateTime || ft.dwHighDateTime != g_cfgTime.dwHighDateTime)) reload = true;
    }
    if (!reload) return;
    LoadCfg(true);
    BridgeLog("cfg reloaded from disk");
    if (direct) { DirectConfig dc = g_cfg.direct; dc.log_frames = g_cfg.log_frames; direct->Reconfigure(dc); }
}

static XrResult XRAPI_CALL Layer_xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *function);

static InstanceRec *FindInstance(XrInstance i) { auto it = g_instances.find(i); return it == g_instances.end() ? nullptr : &it->second; }
static SessionRec *FindSession(XrSession s) { auto it = g_sessions.find(s); return it == g_sessions.end() ? nullptr : &it->second; }

// ---------------------------------------------------------------- SEH guard (no C++ objects in here)
static bool SafeProcess(Bridge *b, const BridgeView *v, uint32_t n, unsigned long *code)
{
    __try { return b->Process(v, n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return false; }
}
static bool SafeProcessDirect(DirectNR *d, const DirectView *v, uint32_t n, unsigned long *code)
{
    __try { return d->Process(v, n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return false; }
}

// ---------------------------------------------------------------- intercepted functions
static XrResult XRAPI_CALL Layer_xrDestroyInstance(XrInstance instance)
{
    PFN_xrDestroyInstance next = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (InstanceRec *r = FindInstance(instance)) { next = r->DestroyInstance; g_instances.erase(instance); }
    }
    BridgeLog("xrDestroyInstance(%llx)", (unsigned long long)instance);
    return next ? next(instance) : XR_ERROR_HANDLE_INVALID;
}

static XrResult XRAPI_CALL Layer_xrCreateSession(XrInstance instance, const XrSessionCreateInfo *info, XrSession *session)
{
    InstanceRec rec;
    { std::lock_guard<std::mutex> lk(g_mtx); InstanceRec *r = FindInstance(instance); if (!r) return XR_ERROR_HANDLE_INVALID; rec = *r; }
    const XrResult res = rec.CreateSession(instance, info, session);
    if (XR_FAILED(res)) { BridgeLog("xrCreateSession failed: %d", (int)res); return res; }

    ID3D11Device *dev = nullptr; const char *binding = "none/unknown";
    for (const XrBaseInStructure *p = (const XrBaseInStructure *)info->next; p; p = p->next)
    {
        if (p->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) { dev = ((const XrGraphicsBindingD3D11KHR *)p)->device; binding = "D3D11"; break; }
        if (p->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) { binding = "D3D12"; break; }
        if (p->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) { binding = "Vulkan"; break; }
        if (p->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR) { binding = "OpenGL"; break; }
    }
    SessionRec s; s.instance = instance; s.devA = dev;
    if (rec.active && dev)
    {
        if (g_cfg.mode == "reshade")
        {
            BridgeConfig bc; bc.scale = g_cfg.scale; bc.window_visible = g_cfg.window_visible; bc.skip_present = g_cfg.skip_present; bc.log_frames = g_cfg.log_frames;
            s.bridge = new Bridge();
            if (!s.bridge->Init(dev, bc)) { s.bridge->Shutdown(); delete s.bridge; s.bridge = nullptr; }
        }
        else
        {
            wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH); if (wchar_t *sl = wcsrchr(exe, L'\\')) *sl = 0;
            DirectConfig dc = g_cfg.direct; dc.log_frames = g_cfg.log_frames;
            s.direct = new DirectNR();
            if (!s.direct->Init(dev, dc, exe, g_home)) { s.direct->Shutdown(true); delete s.direct; s.direct = nullptr; }
        }
    }
    BridgeLog("xrCreateSession -> %llx, graphics binding %s, device=%p, mode=%s, engine=%s", (unsigned long long)*session, binding, dev, g_cfg.mode.c_str(),
              s.direct ? "DIRECT ACTIVE" : s.bridge ? "RESHADE-BRIDGE ACTIVE" : (rec.active ? "inactive (init failed or not D3D11)" : "inactive (layer not active for this app)"));
    std::lock_guard<std::mutex> lk(g_mtx);
    g_sessions[*session] = s;
    return res;
}

static XrResult XRAPI_CALL Layer_xrDestroySession(XrSession session)
{
    PFN_xrDestroySession next = nullptr; Bridge *b = nullptr; DirectNR *d = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (SessionRec *s = FindSession(session))
        {
            if (InstanceRec *r = FindInstance(s->instance)) next = r->DestroySession;
            b = s->bridge; d = s->direct; g_sessions.erase(session);
        }
        for (auto it = g_swapchains.begin(); it != g_swapchains.end();) { if (it->second.session == session) it = g_swapchains.erase(it); else ++it; }
    }
    if (b) { b->Shutdown(true); delete b; }
    if (d) { d->Shutdown(true); delete d; }
    BridgeLog("xrDestroySession(%llx) after %llu xrEndFrame calls (%llu skipped)", (unsigned long long)session, (unsigned long long)g_endFrames, (unsigned long long)g_skipped);
    return next ? next(session) : XR_ERROR_HANDLE_INVALID;
}

static XrResult XRAPI_CALL Layer_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo *info, XrSwapchain *swapchain)
{
    InstanceRec rec; bool bridged = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        SessionRec *s = FindSession(session); if (!s) return XR_ERROR_HANDLE_INVALID;
        InstanceRec *r = FindInstance(s->instance); if (!r) return XR_ERROR_HANDLE_INVALID;
        rec = *r; bridged = s->bridge != nullptr || s->direct != nullptr;
    }
    XrSwapchainCreateInfo ci = *info;
    if (bridged) ci.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    const XrResult res = rec.CreateSwapchain(session, &ci, swapchain);
    if (XR_FAILED(res)) { BridgeLog("xrCreateSwapchain failed: %d", (int)res); return res; }

    SwapchainRec sc; sc.session = session; sc.createFlags = info->createFlags;
    if (bridged)
    {
        uint32_t n = 0;
        rec.EnumerateSwapchainImages(*swapchain, 0, &n, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        if (n && XR_SUCCEEDED(rec.EnumerateSwapchainImages(*swapchain, n, &n, (XrSwapchainImageBaseHeader *)imgs.data())))
            for (uint32_t i = 0; i < n; ++i) sc.images.push_back(imgs[i].texture);
        D3D11_TEXTURE2D_DESC d = {}; if (!sc.images.empty()) sc.images[0]->GetDesc(&d);
        BridgeLog("xrCreateSwapchain -> %llx: %ux%u format %lld arraySize %u samples %u faces %u mips %u usage 0x%llx flags 0x%llx, %u images (d3d: %ux%u fmt %d arr %u bind 0x%x misc 0x%x)",
                  (unsigned long long)*swapchain, info->width, info->height, (long long)info->format, info->arraySize, info->sampleCount, info->faceCount, info->mipCount,
                  (unsigned long long)info->usageFlags, (unsigned long long)info->createFlags, n, d.Width, d.Height, (int)d.Format, d.ArraySize, d.BindFlags, d.MiscFlags);
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    g_swapchains[*swapchain] = std::move(sc);
    return res;
}

static XrResult XRAPI_CALL Layer_xrDestroySwapchain(XrSwapchain swapchain)
{
    PFN_xrDestroySwapchain next = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_swapchains.find(swapchain);
        if (it != g_swapchains.end())
        {
            if (SessionRec *s = FindSession(it->second.session)) if (InstanceRec *r = FindInstance(s->instance)) next = r->DestroySwapchain;
            g_swapchains.erase(it);
        }
    }
    BridgeLog("xrDestroySwapchain(%llx)", (unsigned long long)swapchain);
    return next ? next(swapchain) : XR_ERROR_HANDLE_INVALID;
}

static XrResult XRAPI_CALL Layer_xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo *info, uint32_t *index)
{
    PFN_xrAcquireSwapchainImage next = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_swapchains.find(swapchain);
        if (it != g_swapchains.end()) if (SessionRec *s = FindSession(it->second.session)) if (InstanceRec *r = FindInstance(s->instance)) next = r->AcquireSwapchainImage;
    }
    if (!next) return XR_ERROR_HANDLE_INVALID;
    const XrResult res = next(swapchain, info, index);
    if (XR_SUCCEEDED(res))
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_swapchains.find(swapchain);
        if (it != g_swapchains.end()) it->second.acquired.push_back(*index);
    }
    return res;
}

static XrResult XRAPI_CALL Layer_xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *info)
{
    PFN_xrReleaseSwapchainImage next = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_swapchains.find(swapchain);
        if (it != g_swapchains.end()) if (SessionRec *s = FindSession(it->second.session)) if (InstanceRec *r = FindInstance(s->instance)) next = r->ReleaseSwapchainImage;
    }
    if (!next) return XR_ERROR_HANDLE_INVALID;
    const XrResult res = next(swapchain, info);
    if (XR_SUCCEEDED(res))
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_swapchains.find(swapchain);
        if (it != g_swapchains.end() && !it->second.acquired.empty()) { it->second.lastReleased = it->second.acquired.front(); it->second.acquired.pop_front(); }
    }
    return res;
}

static XrResult XRAPI_CALL Layer_xrEndFrame(XrSession session, const XrFrameEndInfo *frameEndInfo)
{
    PFN_xrEndFrame next = nullptr; Bridge *bridge = nullptr; DirectNR *direct = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        SessionRec *s = FindSession(session);
        if (s) { if (InstanceRec *r = FindInstance(s->instance)) next = r->EndFrame; bridge = s->bridge; direct = s->direct; }
    }
    if (!next) return XR_ERROR_HANDLE_INVALID;
    ++g_endFrames;
    if (direct) MaybeReloadCfg(direct);

    const bool engineOk = (bridge && !bridge->failed()) || (direct && !direct->failed());
    if (engineOk)
    {
        if (g_cfg.toggle_vk)
        {
            const bool down = (GetAsyncKeyState(g_cfg.toggle_vk) & 0x8000) != 0;
            if (down && !g_keyWasDown) { g_bypass = !g_bypass; BridgeLog("toggle: bridge %s", g_bypass ? "BYPASSED" : "active"); if (!g_bypass && direct) direct->RequestReset(); }
            g_keyWasDown = down;
        }
        if (!g_bypass && frameEndInfo)
        {
            for (uint32_t li = 0; li < frameEndInfo->layerCount; ++li)
            {
                const XrCompositionLayerBaseHeader *L = frameEndInfo->layers[li];
                if (!L || L->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
                const XrCompositionLayerProjection *proj = (const XrCompositionLayerProjection *)L;
                BridgeView views[8]; DirectView dviews[8]; uint32_t n = 0; bool ok = proj->viewCount > 0 && proj->viewCount <= 4;
                {
                    std::lock_guard<std::mutex> lk(g_mtx);
                    for (uint32_t v = 0; ok && v < proj->viewCount; ++v)
                    {
                        const XrCompositionLayerProjectionView &pv = proj->views[v];
                        const XrSwapchainSubImage &sub = pv.subImage;
                        auto it = g_swapchains.find(sub.swapchain);
                        if (it == g_swapchains.end() || it->second.images.empty() || (it->second.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ||
                            it->second.lastReleased >= it->second.images.size() || sub.imageRect.extent.width <= 0 || sub.imageRect.extent.height <= 0)
                        { ok = false; break; }
                        BridgeView &bv = views[n]; DirectView &dv = dviews[n]; ++n;
                        bv.texture = it->second.images[it->second.lastReleased].Get();
                        bv.arrayIndex = sub.imageArrayIndex;
                        bv.left = (UINT)sub.imageRect.offset.x; bv.top = (UINT)sub.imageRect.offset.y;
                        bv.right = bv.left + (UINT)sub.imageRect.extent.width; bv.bottom = bv.top + (UINT)sub.imageRect.extent.height;
                        dv.texture = bv.texture; dv.arrayIndex = bv.arrayIndex; dv.left = bv.left; dv.top = bv.top; dv.right = bv.right; dv.bottom = bv.bottom;
                        dv.q[0] = pv.pose.orientation.x; dv.q[1] = pv.pose.orientation.y; dv.q[2] = pv.pose.orientation.z; dv.q[3] = pv.pose.orientation.w;
                        dv.p[0] = pv.pose.position.x; dv.p[1] = pv.pose.position.y; dv.p[2] = pv.pose.position.z;
                        dv.fovL = pv.fov.angleLeft; dv.fovR = pv.fov.angleRight; dv.fovU = pv.fov.angleUp; dv.fovD = pv.fov.angleDown;
                    }
                }
                if (ok)
                {
                    unsigned long code = 0;
                    if (direct)
                    {
                        if (g_endFrames <= 2) BridgeLog("view 0 pose q=(%.3f %.3f %.3f %.3f) p=(%.2f %.2f %.2f) fov=(%.3f %.3f %.3f %.3f)", dviews[0].q[0], dviews[0].q[1], dviews[0].q[2], dviews[0].q[3], dviews[0].p[0], dviews[0].p[1], dviews[0].p[2], dviews[0].fovL, dviews[0].fovR, dviews[0].fovU, dviews[0].fovD);
                        if (!SafeProcessDirect(direct, dviews, n, &code)) { ++g_skipped; if (code) { char b[96]; snprintf(b, sizeof(b), "exception 0x%08lX inside direct Process", code); direct->MarkFailed(b); } }
                    }
                    else if (!SafeProcess(bridge, views, n, &code)) { ++g_skipped; if (code) { char b[96]; snprintf(b, sizeof(b), "exception 0x%08lX inside Process", code); bridge->MarkFailed(b); } }
                }
                else ++g_skipped;
                break; // only the first projection layer
            }
        }
    }
    return next(session, frameEndInfo);
}

// ---------------------------------------------------------------- dispatch
static XrResult XRAPI_CALL Layer_xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *function)
{
    struct E { const char *n; PFN_xrVoidFunction f; };
    static const E table[] = {
        { "xrGetInstanceProcAddr", (PFN_xrVoidFunction)Layer_xrGetInstanceProcAddr },
        { "xrDestroyInstance", (PFN_xrVoidFunction)Layer_xrDestroyInstance },
        { "xrCreateSession", (PFN_xrVoidFunction)Layer_xrCreateSession },
        { "xrDestroySession", (PFN_xrVoidFunction)Layer_xrDestroySession },
        { "xrCreateSwapchain", (PFN_xrVoidFunction)Layer_xrCreateSwapchain },
        { "xrDestroySwapchain", (PFN_xrVoidFunction)Layer_xrDestroySwapchain },
        { "xrAcquireSwapchainImage", (PFN_xrVoidFunction)Layer_xrAcquireSwapchainImage },
        { "xrReleaseSwapchainImage", (PFN_xrVoidFunction)Layer_xrReleaseSwapchainImage },
        { "xrEndFrame", (PFN_xrVoidFunction)Layer_xrEndFrame },
    };
    for (const E &e : table) if (strcmp(name, e.n) == 0) { *function = e.f; return XR_SUCCESS; }
    PFN_xrGetInstanceProcAddr nextGIPA = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (InstanceRec *r = FindInstance(instance)) nextGIPA = r->nextGIPA; else nextGIPA = g_lastNextGIPA;
    }
    if (!nextGIPA) { *function = nullptr; return XR_ERROR_HANDLE_INVALID; }
    return nextGIPA(instance, name, function);
}

static XrResult XRAPI_CALL Layer_xrCreateApiLayerInstance(const XrInstanceCreateInfo *info, const XrApiLayerCreateInfo *layerInfo, XrInstance *instance)
{
    if (!layerInfo || layerInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO || layerInfo->structVersion != XR_API_LAYER_CREATE_INFO_STRUCT_VERSION ||
        layerInfo->structSize != sizeof(XrApiLayerCreateInfo) || !layerInfo->nextInfo || layerInfo->nextInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
        layerInfo->nextInfo->structVersion != XR_API_LAYER_NEXT_INFO_STRUCT_VERSION || layerInfo->nextInfo->structSize != sizeof(XrApiLayerNextInfo) ||
        !layerInfo->nextInfo->nextGetInstanceProcAddr || !layerInfo->nextInfo->nextCreateApiLayerInstance)
        return XR_ERROR_INITIALIZATION_FAILED;

    const XrApiLayerNextInfo *next = layerInfo->nextInfo;
    XrApiLayerCreateInfo li = *layerInfo; li.nextInfo = next->next;
    g_lastNextGIPA = next->nextGetInstanceProcAddr;
    const XrResult res = next->nextCreateApiLayerInstance(info, &li, instance);
    if (XR_FAILED(res)) { BridgeLog("xrCreateApiLayerInstance: next layer/runtime failed with %d", (int)res); return res; }

    InstanceRec rec; rec.nextGIPA = next->nextGetInstanceProcAddr;
#define LOAD(fn) rec.nextGIPA(*instance, "xr" #fn, (PFN_xrVoidFunction *)&rec.fn)
    LOAD(DestroyInstance); LOAD(CreateSession); LOAD(DestroySession); LOAD(CreateSwapchain); LOAD(DestroySwapchain);
    LOAD(EnumerateSwapchainImages); LOAD(AcquireSwapchainImage); LOAD(ReleaseSwapchainImage); LOAD(EndFrame);
#undef LOAD

    wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring exeName = exe; size_t sl = exeName.find_last_of(L"\\/"); if (sl != std::wstring::npos) exeName = exeName.substr(sl + 1);
    std::wstring up = exeName; for (auto &c : up) c = (wchar_t)towupper(c);
    std::wstring match(g_cfg.app_match.begin(), g_cfg.app_match.end()); for (auto &c : match) c = (wchar_t)towupper(c);
    // active when the installer enabled this game (games\<exe>.cfg exists), or by the standalone name filter
    rec.active = g_cfg.enabled && (g_gameCfgFound || g_cfg.any_app || (!match.empty() && up.find(match) != std::wstring::npos));

    BridgeLog("xrCreateApiLayerInstance -> %llx for '%ls' (app '%s', api %u.%u.%u): layer %s", (unsigned long long)*instance, exeName.c_str(),
              info->applicationInfo.applicationName, XR_VERSION_MAJOR(info->applicationInfo.apiVersion), XR_VERSION_MINOR(info->applicationInfo.apiVersion),
              XR_VERSION_PATCH(info->applicationInfo.apiVersion), rec.active ? "ACTIVE" : "passive (app name does not match)");
    std::lock_guard<std::mutex> lk(g_mtx);
    g_instances[*instance] = rec;
    return res;
}

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo *loaderInfo, const char *layerName, XrNegotiateApiLayerRequest *req)
{
    LoadCfg();
    if (!loaderInfo || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO || loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo) || !req || req->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        req->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION || req->structSize != sizeof(XrNegotiateApiLayerRequest))
    { BridgeLog("negotiate: bad loader structs"); return XR_ERROR_INITIALIZATION_FAILED; }
    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION || loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
    { BridgeLog("negotiate: loader interface version %u..%u unsupported", loaderInfo->minInterfaceVersion, loaderInfo->maxInterfaceVersion); return XR_ERROR_INITIALIZATION_FAILED; }
    XrVersion api = XR_CURRENT_API_VERSION;
    if (api > loaderInfo->maxApiVersion) api = loaderInfo->maxApiVersion;
    if (api < loaderInfo->minApiVersion) { BridgeLog("negotiate: loader api range unsupported"); return XR_ERROR_INITIALIZATION_FAILED; }
    req->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    req->layerApiVersion = api;
    req->getInstanceProcAddr = Layer_xrGetInstanceProcAddr;
    req->createApiLayerInstance = Layer_xrCreateApiLayerInstance;
    BridgeLog("negotiate ok: nr-bridge " BRIDGE_VERSION " by LowPopLabs - ko-fi.com/lowpoplabs (%s home %ls%s) as '%s', api %u.%u.%u (loader %u.%u.%u..%u.%u.%u), cfg enabled=%d scale=%.2f any_app=%d",
              g_central ? "central" : "standalone", g_home, g_gameCfgFound ? ", per-game cfg" : "",
              layerName ? layerName : kLayerName, XR_VERSION_MAJOR(api), XR_VERSION_MINOR(api), XR_VERSION_PATCH(api),
              XR_VERSION_MAJOR(loaderInfo->minApiVersion), XR_VERSION_MINOR(loaderInfo->minApiVersion), XR_VERSION_PATCH(loaderInfo->minApiVersion),
              XR_VERSION_MAJOR(loaderInfo->maxApiVersion), XR_VERSION_MINOR(loaderInfo->maxApiVersion), XR_VERSION_PATCH(loaderInfo->maxApiVersion),
              g_cfg.enabled ? 1 : 0, g_cfg.scale, g_cfg.any_app ? 1 : 0);
    return XR_SUCCESS;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
        InitPaths(module);
    }
    return TRUE;
}

// Exercises the OpenVR proxy (build\openvr\openvr_api.dll) against the fake runtime (openvr_api.orig.dll built from
// test_fake_openvr.cpp) placed in the same directory. Checks: exports forward, IVRCompositor tables get hooked at the
// right slot per version (022 vtable, 029 vtable, 022 FnTable), the left eye is deferred and both eyes reach the
// original Submit in order with the app's own arguments, the pair goes through the direct engine (ngx=3 passthrough
// must be bit-exact, ngx=4 half must halve the eyes), and VR_ShutdownInternal restores the tables.
//   test_openvr.exe <dir with openvr_api.dll + openvr_api.orig.dll> [ngx 3|4] [layout: array|sbs|two] [iface: 022|029|fn022]
//   test_openvr.exe real <dir with the proxy + a REAL openvr_api.orig.dll>   (init is expected to fail without an HMD; checks forwarding)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
using Microsoft::WRL::ComPtr;

struct Texture_t { void *handle; int eType; int eColorSpace; };
struct VRTextureBounds_t { float uMin, vMin, uMax, vMax; };
struct SubmitRec { int eye; void *handle; VRTextureBounds_t bounds; bool hadBounds; int flags; int viaArray; uint32_t arrayIndex; int viaFnTable; };
typedef int (*PFN_SubmitVt)(void *, int, const Texture_t *, const VRTextureBounds_t *, int);
typedef int (*PFN_SubmitFn)(int, const Texture_t *, const VRTextureBounds_t *, int);

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } else { printf("ok: " __VA_ARGS__); printf("\n"); } } while (0)

// central layout under <dir>\central: the global cfg says 1 pass, games\test_openvr.cfg overrides to `passes`; the proxy
// finds the home through NR_BRIDGE_HOME, so the per-game override and the logs\ location are exercised too
static void WriteCfg(const std::string &dir, int ngx, float scale, float fovea, float outer, int passes)
{
    const std::string home = dir + "\\central";
    CreateDirectoryA(home.c_str(), nullptr); CreateDirectoryA((home + "\\games").c_str(), nullptr);
    FILE *f = fopen((home + "\\nr-bridge.cfg").c_str(), "w");
    fprintf(f, "enabled=1\nmode=direct\nscale=%.2f\nfovea=%.2f\nouter_scale=%.2f\nouter_pack=1\npasses=1\nresidual=1\nmv=1\nngx=%s\ntoggle_vk=0\nreload_vk=0\nlog_frames=3\napp_match=\n", scale, fovea, outer, ngx == 3 ? "none" : "half");
    fclose(f);
    f = fopen((home + "\\games\\test_openvr.cfg").c_str(), "w");
    fprintf(f, "# per-game override\npasses=%d\n", passes);
    fclose(f);
    SetEnvironmentVariableA("NR_BRIDGE_HOME", home.c_str());
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { printf("usage: see source\n"); return 1; }
    const bool real = !strcmp(argv[1], "real");
    const std::string dir = argv[real ? 2 : 1];
    const int ngx = argc > 2 && !real ? atoi(argv[2]) : 3;
    const std::string layout = argc > 3 && !real ? argv[3] : "array";
    const std::string iface = argc > 4 && !real ? argv[4] : "022";
    if (!real) WriteCfg(dir, ngx, 0.6f, 0.6f, 0.25f, 2);
    SetDllDirectoryA(dir.c_str());
    HMODULE proxy = LoadLibraryA((dir + "\\openvr_api.dll").c_str());
    CHECK(proxy != nullptr, "proxy loaded from %s", dir.c_str());
    if (!proxy) return 2;
    auto P = [&](const char *n) { FARPROC f = GetProcAddress(proxy, n); if (!f) { ++fails; printf("FAIL: export %s missing\n", n); } return f; };
    typedef uint32_t (*PFN_Init2)(int *, int, const char *); typedef void (*PFN_Shutdown)(); typedef void *(*PFN_GGI)(const char *, int *);
    typedef bool (*PFN_Bool)(); typedef const char *(*PFN_ErrStr)(int); typedef bool (*PFN_IsValid)(const char *);
    PFN_Init2 init2 = (PFN_Init2)P("VR_InitInternal2"); PFN_Shutdown shutdown = (PFN_Shutdown)P("VR_ShutdownInternal"); PFN_GGI ggi = (PFN_GGI)P("VR_GetGenericInterface");
    PFN_Bool isInstalled = (PFN_Bool)P("VR_IsRuntimeInstalled"); PFN_ErrStr errStr = (PFN_ErrStr)P("VR_GetVRInitErrorAsSymbol"); PFN_IsValid isValid = (PFN_IsValid)P("VR_IsInterfaceVersionValid");
    for (const char *n : { "VR_InitInternal", "VR_IsHmdPresent", "VR_RuntimePath", "VR_GetRuntimePath", "VR_GetVRInitErrorAsEnglishDescription", "VR_GetStringForHmdError", "VR_GetInitToken", "VRControlPanel", "LiquidVR", "VRPaths" }) P(n);
    if (fails) return 3;

    if (real)
    {
        int err = -1; const uint32_t tok = init2(&err, 1, nullptr);
        printf("real runtime: VR_InitInternal2 -> token %u, error %d (%s), runtime installed=%d, IVRCompositor_022 valid=%d\n", tok, err, errStr(err), (int)isInstalled(), (int)isValid("IVRCompositor_022"));
        CHECK(err == 0 || err == 108 || err == 126 || err == 110 || err == 100, "init returns a sensible error without an HMD (got %d)", err);
        if (err == 0) { void *c = ggi("IVRCompositor_022", &err); printf("compositor %p err %d\n", c, err); shutdown(); }
        printf("%s\n", fails ? "REAL: FAIL" : "REAL: PASS");
        return fails ? 5 : 0;
    }

    int err = -1; const uint32_t tok = init2(&err, 1, nullptr);   // first forwarded call: the proxy loads the original here
    HMODULE fake = GetModuleHandleA("openvr_api.orig.dll");
    CHECK(fake != nullptr, "fake original loaded by the proxy");
    if (!fake) return 4;
    typedef int (*PFN_Int)(); typedef const void *(*PFN_Rec)(int); typedef void (*PFN_Void)();
    PFN_Int fakeSubmits = (PFN_Int)GetProcAddress(fake, "bnr_fake_submits"); PFN_Rec fakeSubmit = (PFN_Rec)GetProcAddress(fake, "bnr_fake_submit"); PFN_Void fakeReset = (PFN_Void)GetProcAddress(fake, "bnr_fake_reset");
    PFN_Int fakeInits = (PFN_Int)GetProcAddress(fake, "bnr_fake_inits"); PFN_Int fakeShutdowns = (PFN_Int)GetProcAddress(fake, "bnr_fake_shutdowns");
    PFN_Int hookCount = (PFN_Int)GetProcAddress(proxy, "bnr_test_hook_count");

    CHECK(tok == 7 && err == 0 && fakeInits() == 1, "VR_InitInternal2 forwarded (token %u err %d)", tok, err);
    const bool fn = iface == "fn022"; const int version = iface == "029" ? 29 : 22;
    const std::string name = (fn ? "FnTable:IVRCompositor_022" : (version == 29 ? "IVRCompositor_029" : "IVRCompositor_022"));
    void *comp = ggi(name.c_str(), &err);
    CHECK(comp != nullptr && err == 0, "GetGenericInterface(%s) -> %p", name.c_str(), comp);
    void *comp2 = ggi(name.c_str(), &err);
    CHECK(comp2 == comp && hookCount() == 1, "second request returns the same object and does not double-hook (hooks=%d)", hookCount());
    void **table = fn ? (void **)comp : *(void ***)comp;
    const int submitIdx = version >= 29 ? 6 : 5;
    HMODULE proxyOfSlot = nullptr; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)table[submitIdx], &proxyOfSlot);
    CHECK(proxyOfSlot == proxy, "Submit slot %d now points into the proxy", submitIdx);
    HMODULE fakeOfOther = nullptr; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)table[3], &fakeOfOther);
    CHECK(fakeOfOther == fake, "GetLastPoses slot 3 untouched");

    // D3D11 eye textures with the test_direct pattern
    const UINT W = 2008, H = 2166;
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) { printf("D3D11CreateDevice failed\n"); return 1; }
    const bool array = layout == "array", sbs = layout == "sbs";
    const UINT TW = sbs ? W * 2 : W;
    D3D11_TEXTURE2D_DESC td = {}; td.Width = TW; td.Height = H; td.MipLevels = 1; td.ArraySize = array ? 2 : 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    // pattern per eye: x&255, y&255, eye tag
    std::vector<uint32_t> pat[2]; for (int s = 0; s < 2; ++s) { pat[s].resize((size_t)W * H); for (UINT y = 0; y < H; ++y) for (UINT x = 0; x < W; ++x) pat[s][(size_t)y * W + x] = (x & 255) | ((y & 255) << 8) | ((s ? 200u : 40u) << 16) | 0xFF000000u; }
    std::vector<uint32_t> full((size_t)TW * H);
    ComPtr<ID3D11Texture2D> texA, texB;   // texA holds the left eye (and right when array/sbs); texB = right eye for the "two" layout
    auto fillSlice = [&](std::vector<uint32_t> &dst, UINT dstW, UINT x0, const std::vector<uint32_t> &src) { for (UINT y = 0; y < H; ++y) memcpy(&dst[(size_t)y * dstW + x0], &src[(size_t)y * W], W * 4); };
    if (array)
    {
        D3D11_SUBRESOURCE_DATA init[2] = { { pat[0].data(), W * 4, 0 }, { pat[1].data(), W * 4, 0 } };
        dev->CreateTexture2D(&td, init, &texA);
    }
    else if (sbs)
    {
        fillSlice(full, TW, 0, pat[0]); fillSlice(full, TW, W, pat[1]);
        D3D11_SUBRESOURCE_DATA init = { full.data(), TW * 4, 0 }; dev->CreateTexture2D(&td, &init, &texA);
    }
    else
    {
        D3D11_SUBRESOURCE_DATA i0 = { pat[0].data(), W * 4, 0 }, i1 = { pat[1].data(), W * 4, 0 };
        dev->CreateTexture2D(&td, &i0, &texA); dev->CreateTexture2D(&td, &i1, &texB);
    }
    CHECK(texA != nullptr, "eye texture(s) created (%s)", layout.c_str());

    Texture_t tl = { texA.Get(), 0, 1 }, tr = { (sbs || array) ? texA.Get() : texB.Get(), 0, 1 };
    VRTextureBounds_t bl = { 0, 0, sbs ? 0.5f : 1.0f, 1 }, br = { sbs ? 0.5f : 0.0f, 0, 1, 1 };
    PFN_SubmitVt submitVt = (PFN_SubmitVt)table[submitIdx]; PFN_SubmitFn submitFn = (PFN_SubmitFn)table[submitIdx];
    auto submit = [&](int eye, const Texture_t *t, const VRTextureBounds_t *b) { return fn ? submitFn(eye, t, b, 0) : submitVt(comp, eye, t, b, 0); };
    const int frames = 12;
    fakeReset();
    for (int f = 0; f < frames; ++f)
    {
        // re-upload the pattern each frame (the engine writes its result back into the app's textures)
        if (array) { for (int s = 0; s < 2; ++s) ctx->UpdateSubresource(texA.Get(), D3D11CalcSubresource(0, s, 1), nullptr, pat[s].data(), W * 4, 0); }
        else if (sbs) ctx->UpdateSubresource(texA.Get(), 0, nullptr, full.data(), TW * 4, 0);
        else { ctx->UpdateSubresource(texA.Get(), 0, nullptr, pat[0].data(), W * 4, 0); ctx->UpdateSubresource(texB.Get(), 0, nullptr, pat[1].data(), W * 4, 0); }
        const int r1 = submit(0, &tl, &bl);
        CHECK(r1 == 0 && fakeSubmits() == f * 2, "frame %d: left eye deferred (fake saw %d submits)", f, fakeSubmits());
        const int r2 = submit(1, &tr, &br);
        CHECK(r2 == 0 && fakeSubmits() == f * 2 + 2, "frame %d: right eye releases both (fake saw %d submits)", f, fakeSubmits());
        if (fails) break;
    }
    if (!fails)
    {
        const SubmitRec *a = (const SubmitRec *)fakeSubmit(0), *b = (const SubmitRec *)fakeSubmit(1);
        CHECK(a->eye == 0 && b->eye == 1, "order is left then right");
        CHECK(a->handle == tl.handle && b->handle == tr.handle, "texture handles forwarded unchanged");
        CHECK(a->hadBounds && memcmp(&a->bounds, &bl, sizeof(bl)) == 0 && memcmp(&b->bounds, &br, sizeof(br)) == 0, "bounds forwarded unchanged");
        CHECK(a->viaFnTable == (fn ? 1 : 0), "reached the %s original", fn ? "FnTable" : "vtable");
    }
    typedef unsigned long long (*PFN_U64)(); PFN_U64 framesDone = (PFN_U64)GetProcAddress(proxy, "bnr_test_frames");
    CHECK(framesDone() == (unsigned long long)frames, "engine processed %llu pairs", framesDone());
    CHECK(GetFileAttributesA((dir + "\\central\\logs\\nr-bridge-test_openvr.log").c_str()) != INVALID_FILE_ATTRIBUTES, "log written to the central logs folder under the exe name");

    // read back and compare: ngx=3 bit-exact, ngx=4 fovea x0.25 (2 passes) / outer x0.5 with the feather blend -> just check the eye centre and a corner
    D3D11_TEXTURE2D_DESC sd = td; sd.BindFlags = 0; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    auto readEye = [&](int eye, std::vector<uint32_t> &out) {
        ID3D11Texture2D *src = (eye == 0 || sbs || array) ? texA.Get() : texB.Get();
        ComPtr<ID3D11Texture2D> stage; dev->CreateTexture2D(&sd, nullptr, &stage); ctx->CopyResource(stage.Get(), src);
        const UINT sub = array ? D3D11CalcSubresource(0, eye, 1) : 0; D3D11_MAPPED_SUBRESOURCE m = {}; ctx->Map(stage.Get(), sub, D3D11_MAP_READ, 0, &m);
        out.resize((size_t)W * H); const UINT x0 = (sbs && eye == 1) ? W : 0;
        for (UINT y = 0; y < H; ++y) memcpy(&out[(size_t)y * W], (const uint8_t *)m.pData + (size_t)y * m.RowPitch + (size_t)x0 * 4, W * 4);
        ctx->Unmap(stage.Get(), sub);
    };
    for (int eye = 0; eye < 2; ++eye)
    {
        std::vector<uint32_t> got; readEye(eye, got);
        if (ngx == 3)
        {
            size_t mism = 0; for (size_t i = 0; i < got.size(); ++i) if (got[i] != pat[eye][i]) ++mism;
            CHECK(mism == 0, "eye %d passthrough bit-exact (%zu mismatches)", eye, mism);
        }
        else
        {
            auto ch = [](uint32_t p, int c) { return (int)((p >> (c * 8)) & 255); };
            const size_t centre = (size_t)(H / 2) * W + W / 2, corner = (size_t)8 * W + 8;
            const uint32_t gc = got[centre], ec = pat[eye][centre], gk = got[corner], ek = pat[eye][corner];
            bool okc = true, okk = true;
            for (int c = 0; c < 3; ++c) { okc &= abs(ch(gc, c) - (int)(ch(ec, c) * 0.25 + 0.5)) <= 2; okk &= abs(ch(gk, c) - (int)(ch(ek, c) * 0.5 + 0.5)) <= 2; }
            CHECK(okc, "eye %d centre is the 2-pass fovea result (x0.25): got %06X expected from %06X", eye, gc & 0xFFFFFF, ec & 0xFFFFFF);
            CHECK(okk, "eye %d corner is the 1-pass outer result (x0.5): got %06X expected from %06X", eye, gk & 0xFFFFFF, ek & 0xFFFFFF);
        }
    }

    shutdown();
    CHECK(fakeShutdowns() == 1 && hookCount() == 0, "VR_ShutdownInternal forwarded and hooks removed (hooks=%d)", hookCount());
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)table[submitIdx], &fakeOfOther);
    CHECK(fakeOfOther == fake, "Submit slot restored to the original");
    printf("%s (%s, ngx %d, %s)\n", fails ? "OPENVR TEST: FAIL" : "OPENVR TEST: PASS", iface.c_str(), ngx, layout.c_str());
    return fails ? 5 : 0;
}

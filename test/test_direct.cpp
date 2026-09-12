// Exercises DirectNR without OpenXR. With ngx=3 (passthrough) the round trip must be bit-exact at
// scale 1.0 and close at lower scales; with ngx=0 it reports how far the NGX init/create gets on this
// machine (the feature itself is unsupported on the dev GPU).
#include "../src/nr_direct.h"
#include <cstdarg>
#include <cstdlib>

void BridgeLog(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); }

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    DirectConfig cfg;
    cfg.scale = argc > 1 ? (float)atof(argv[1]) : 1.0f;
    cfg.ngx = argc > 2 ? atoi(argv[2]) : 3;
    const int frames = argc > 3 ? atoi(argv[3]) : 60;
    cfg.fovea = argc > 4 ? (float)atof(argv[4]) : 1.0f;
    cfg.passes = argc > 5 ? atoi(argv[5]) : 1;
    const bool reconfigureMidRun = argc > 6 && atoi(argv[6]) != 0; // switch to scale 0.5 / fovea 0.5 / 2 passes halfway
    cfg.outer_scale = argc > 7 ? (float)atof(argv[7]) : 0.0f;
    cfg.outer_pack = argc > 8 ? atoi(argv[8]) != 0 : true;   // 1 = outer tier packed into the fovea work texture (one evaluate per pass), 0 = separate
    const bool dynamic = argc > 9 && atoi(argv[9]) != 0;      // dynamic viewport: the eye rect shrinks and grows per frame inside a fixed texture
    const bool flip = argc > 10 && atoi(argv[10]) != 0;       // eyes stored upside down (OpenVR flipped bounds)
    const UINT W = 2008, H = 2166;

    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) { printf("D3D11CreateDevice failed\n"); return 1; }
    D3D11_TEXTURE2D_DESC td = {}; td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 2; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint32_t> pat[2]; D3D11_SUBRESOURCE_DATA init[2];
    // pattern: x&255, y&255, eye tag. With flip the texture holds the pattern upside down (row y = upright row H-1-y).
    for (int s = 0; s < 2; ++s) { pat[s].resize((size_t)W * H); for (UINT y = 0; y < H; ++y) for (UINT x = 0; x < W; ++x) { const UINT uy = flip ? H - 1 - y : y; pat[s][(size_t)y * W + x] = (x & 255) | ((uy & 255) << 8) | ((s ? 200u : 40u) << 16) | 0xFF000000u; } init[s].pSysMem = pat[s].data(); init[s].SysMemPitch = W * 4; init[s].SysMemSlicePitch = 0; }
    ComPtr<ID3D11Texture2D> eyes; if (FAILED(dev->CreateTexture2D(&td, init, &eyes))) { printf("CreateTexture2D failed\n"); return 1; }

    wchar_t dir[MAX_PATH]; GetModuleFileNameW(nullptr, dir, MAX_PATH); if (wchar_t *s = wcsrchr(dir, L'\\')) *s = 0;
    DirectNR nr;
    if (!nr.Init(dev.Get(), cfg, L"E:\\Steam\\steamapps\\common\\BONELAB", dir)) { printf("FAIL: Init\n"); return 2; }

    DirectView v[2] = {};
    for (int i = 0; i < 2; ++i)
    {
        v[i].texture = eyes.Get(); v[i].arrayIndex = i; v[i].left = 0; v[i].top = 0; v[i].right = W; v[i].bottom = H;
        v[i].q[3] = 1.0f; v[i].fovL = -0.86f; v[i].fovR = 0.75f; v[i].fovU = 0.83f; v[i].fovD = -0.86f;
        v[i].flipY = flip; if (dynamic) { v[i].maxW = W; v[i].maxH = H; }
    }
    // dynamic viewport: cycle the rect through these fractions of the texture; the last frame uses the last entry
    const float dynScale[] = { 1.0f, 0.7f, 0.5f, 0.85f, 0.6f, 0.41f, 0.9f, 0.75f };
    float sLast = 1.0f;
    for (int f = 0; f < frames; ++f)
    {
        if (dynamic) { sLast = dynScale[f % 8]; for (int i = 0; i < 2; ++i) { v[i].right = (UINT)(W * sLast); v[i].bottom = (UINT)(H * sLast); } }
        if (reconfigureMidRun && f == frames / 2) { DirectConfig c2 = cfg; c2.scale = 0.5f; c2.fovea = 0.5f; c2.passes = 2; c2.intensity = 0.7f; nr.Reconfigure(c2); }
        // small yaw each frame so the motion-vector pass has something to do
        const float yaw = 0.002f * f; v[0].q[1] = v[1].q[1] = sinf(yaw * 0.5f); v[0].q[3] = v[1].q[3] = cosf(yaw * 0.5f);
        for (int s = 0; s < 2; ++s) ctx->UpdateSubresource(eyes.Get(), D3D11CalcSubresource(0, s, 1), nullptr, pat[s].data(), W * 4, 0);
        if (!nr.Process(v, 2)) { printf("Process returned false at frame %d (see log above)\n", f); return 3; }
    }
    printf("%d frames, avg cpu %.3f ms%s\n", frames, nr.avgCpuMs(), dynamic ? " (dynamic viewport)" : "");
    td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage; dev->CreateTexture2D(&td, nullptr, &stage); ctx->CopyResource(stage.Get(), eyes.Get());
    // ngx=4 expectation: the fovea gets every pass (x 0.5^passes), the outer tier exactly one (x 0.5), nothing outside the tiers
    // (x 1); the resolve blends fovea and outer with the same elliptical/rect feather weight the shader uses.
    DirectConfig fin = cfg; if (reconfigureMidRun) { fin.scale = 0.5f; fin.fovea = 0.5f; fin.passes = 2; }
    const bool whole = fin.fovea >= 0.999f;
    const double fovF = pow(0.5, fin.passes), outF = whole ? fovF : fin.outer_scale > 0.0f ? 0.5 : 1.0;
    // the rect in use on the last frame (dynamic) and the fovea crop centred in it; pixels outside the rect are untouched
    const UINT RW = dynamic ? (UINT)(W * sLast) : W, RH = dynamic ? (UINT)(H * sLast) : H;
    const float cw = whole ? (float)RW : floorf(RW * fin.fovea), ch = whole ? (float)RH : floorf(RH * fin.fovea), cx = floorf((RW - cw) * 0.5f), cy = floorf((RH - ch) * 0.5f);
    auto weight = [&](float px, float py) -> double {
        if (whole) return 1.0;
        if (fin.fovea_shape != 0) { const float hx = cw * 0.5f, hy = ch * 0.5f, r = sqrtf(powf((px - cx - hx) / hx, 2) + powf((py - cy - hy) / hy, 2)), fr = fin.feather / std::min(hx, hy), e0 = std::max(0.0f, 1.0f - fr); const float t = std::min(1.0f, std::max(0.0f, (r - e0) / (1.0f - e0))); return 1.0 - t * t * (3 - 2 * t); }
        if (px < cx || px >= cx + cw || py < cy || py >= cy + ch) return 0.0;
        if (fin.feather <= 0.0f) return 1.0;
        const float d = std::min(std::min(px - cx, cx + cw - px), std::min(py - cy, cy + ch - py)), t = std::min(1.0f, std::max(0.0f, d / fin.feather)); return t * t * (3 - 2 * t);
    };
    int rc = 0;
    for (int s = 0; s < 2; ++s)
    {
        D3D11_MAPPED_SUBRESOURCE m = {}; if (FAILED(ctx->Map(stage.Get(), D3D11CalcSubresource(0, s, 1), D3D11_MAP_READ, 0, &m))) return 4;
        uint64_t sum = 0; uint32_t mism = 0, mx = 0;
        for (UINT y = 0; y < H; ++y) { const uint32_t *row = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch); for (UINT x = 0; x < W; ++x) { const uint32_t a = row[x]; uint32_t e = pat[s][(size_t)y * W + x];
            double factor = 1.0;
            if (x < RW && y < RH)
            {
                if (cfg.ngx == 4) { const double w = weight(x + 0.5f, y + 0.5f); factor = w * fovF + (1.0 - w) * outF; }
                // ngx=5 (whole eye, scale < 1): the upright top half is halved; with flip that is the bottom half of the texture
                else if (cfg.ngx == 5) { const bool uprightTop = flip ? (y >= RH / 2) : (y < RH / 2); factor = uprightTop ? 0.5 : 1.0; }
            }
            if (factor != 1.0) { uint32_t h = 0; for (int c = 0; c < 3; ++c) h |= (uint32_t)(int)(((e >> (c * 8)) & 255) * factor + 0.5) << (c * 8); e = h | 0xFF000000u; }
            if (a != e) ++mism; for (int c = 0; c < 3; ++c) { const int d = abs((int)((a >> (c * 8)) & 255) - (int)((e >> (c * 8)) & 255)); sum += d; if ((uint32_t)d > mx) mx = d; } } }
        ctx->Unmap(stage.Get(), D3D11CalcSubresource(0, s, 1));
        const double mean = (double)sum / ((double)W * H * 3);
        // ngx=3: the round trip must reproduce the input. ngx=4: the per-pixel expectation above, within filtering error
        const bool pass = cfg.ngx == 3 ? (cfg.scale >= 0.999f ? mism == 0 : mean < 6.0) : (cfg.ngx == 4 || cfg.ngx == 5) ? mean < 6.0 : true;
        printf("slice %d: mismatched %u, mean abs diff %.3f, max %u -> %s\n", s, mism, mean, mx, pass ? "PASS" : "FAIL"); if (!pass) rc = 5;
    }
    nr.Shutdown(false);
    return rc;
}

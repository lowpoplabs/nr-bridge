// Exercises Bridge without OpenXR or ReShade: two "eye" slices of a texture array round-trip through
// device B's hidden swap chain. Without ReShade nothing modifies the frame, so at scale 1.0 the
// output must be bit-identical to the input; at lower scales it must be a close approximation.
#include "../src/nr_bridge_core.h"
#include <cstdarg>
#include <cstdlib>
#include <cmath>

void BridgeLog(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); }

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const float scale = argc > 1 ? (float)atof(argv[1]) : 1.0f;
    const bool skipPresent = argc > 2 && atoi(argv[2]) != 0;
    const bool leakB = argc > 3 && atoi(argv[3]) != 0; // mirror the layer's behaviour at session end
    const UINT W = 2064, H = 2208; // Quest 3 default per-eye size at 1.0x

    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDevice failed 0x%08lX\n", hr); return 1; }

    // texture array like Unity single-pass-instanced XR swap chain images
    D3D11_TEXTURE2D_DESC td = {}; td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 2; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint32_t> pattern[2];
    D3D11_SUBRESOURCE_DATA init[2];
    for (int s = 0; s < 2; ++s)
    {
        pattern[s].resize((size_t)W * H);
        for (UINT y = 0; y < H; ++y) for (UINT x = 0; x < W; ++x)
            pattern[s][(size_t)y * W + x] = (x & 255) | ((y & 255) << 8) | ((s ? 200u : 40u) << 16) | (0xFFu << 24);
        init[s].pSysMem = pattern[s].data(); init[s].SysMemPitch = W * 4; init[s].SysMemSlicePitch = 0;
    }
    ComPtr<ID3D11Texture2D> eyes;
    if (FAILED(hr = dev->CreateTexture2D(&td, init, &eyes))) { printf("CreateTexture2D failed 0x%08lX\n", hr); return 1; }

    Bridge b; BridgeConfig cfg; cfg.scale = scale; cfg.skip_present = skipPresent; cfg.log_frames = 2;
    if (!b.Init(dev.Get(), cfg)) { printf("FAIL: Init\n"); return 2; }

    BridgeView views[2] = { { eyes.Get(), 0, 0, 0, W, H }, { eyes.Get(), 1, 0, 0, W, H } };
    const int frames = 120;
    for (int f = 0; f < frames; ++f)
    {
        // re-seed every frame so a stale copy would be caught
        for (int s = 0; s < 2; ++s) ctx->UpdateSubresource(eyes.Get(), D3D11CalcSubresource(0, s, 1), nullptr, pattern[s].data(), W * 4, 0);
        if (!b.Process(views, 2)) { printf("FAIL: Process at frame %d\n", f); return 3; }
    }
    printf("%d frames, avg cpu %.3f ms/frame, keyed-mutex fallback=%d\n", frames, b.avgCpuMs(), b.keyedMutexFallback() ? 1 : 0);

    // read back and compare
    printf("reading back (leakB=%d)\n", leakB ? 1 : 0);
    td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage; dev->CreateTexture2D(&td, nullptr, &stage);
    ctx->CopyResource(stage.Get(), eyes.Get());
    int rc = 0;
    for (int s = 0; s < 2; ++s)
    {
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(ctx->Map(stage.Get(), D3D11CalcSubresource(0, s, 1), D3D11_MAP_READ, 0, &m))) { printf("Map failed\n"); return 4; }
        uint64_t sumAbs = 0; uint32_t maxAbs = 0, mismatched = 0;
        for (UINT y = 0; y < H; ++y)
        {
            const uint32_t *row = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
            for (UINT x = 0; x < W; ++x)
            {
                const uint32_t a = row[x], e = pattern[s][(size_t)y * W + x];
                if (a != e) ++mismatched;
                for (int c = 0; c < 3; ++c) { const int d = abs((int)((a >> (c * 8)) & 255) - (int)((e >> (c * 8)) & 255)); sumAbs += d; if ((uint32_t)d > maxAbs) maxAbs = d; }
            }
        }
        if (argc > 4 && atoi(argv[4]) != 0) // dump the slice as raw RGBA for inspection
        {
            char name[32]; snprintf(name, sizeof(name), "slice%d.rgba", s);
            if (FILE *f = fopen(name, "wb")) { for (UINT y = 0; y < H; ++y) fwrite((const uint8_t *)m.pData + (size_t)y * m.RowPitch, 4, W, f); fclose(f); printf("wrote %s (%ux%u RGBA8)\n", name, W, H); }
        }
        ctx->Unmap(stage.Get(), D3D11CalcSubresource(0, s, 1));
        const double mean = (double)sumAbs / ((double)W * H * 3);
        const bool pass = scale >= 0.999f ? mismatched == 0 : mean < 6.0;
        printf("slice %d: mismatched pixels %u, mean abs diff %.3f, max %u -> %s\n", s, mismatched, mean, maxAbs, pass ? "PASS" : "FAIL");
        if (!pass) rc = 5;
    }
    fflush(stdout);
    b.Shutdown(leakB);
    return rc;
}

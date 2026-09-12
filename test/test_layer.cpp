// Loads the game's own openxr_loader.dll with our manifest exposed as an explicit layer through
// XR_API_LAYER_PATH, creates an instance with the layer enabled and reports what happened.
// Usage: test_layer.exe [runtime.json]   (runtime.json overrides the active runtime via XR_RUNTIME_JSON)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include "../src/openxr/openxr.h"

int wmain(int argc, wchar_t **argv)
{
    wchar_t dir[MAX_PATH]; GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (wchar_t *s = wcsrchr(dir, L'\\')) *s = 0;
    SetEnvironmentVariableW(L"XR_API_LAYER_PATH", dir);
    SetEnvironmentVariableW(L"XR_LOADER_DEBUG", L"warning");
    if (argc > 1) SetEnvironmentVariableW(L"XR_RUNTIME_JSON", argv[1]);

    std::wstring loaderPath = std::wstring(dir) + L"\\openxr_loader.dll";
    HMODULE loader = LoadLibraryW(loaderPath.c_str());
    if (!loader) { printf("cannot load %ls (copy the game's openxr_loader.dll next to this exe)\n", loaderPath.c_str()); return 1; }
    auto gipa = (PFN_xrGetInstanceProcAddr)GetProcAddress(loader, "xrGetInstanceProcAddr");
    PFN_xrEnumerateApiLayerProperties enumLayers = nullptr; PFN_xrCreateInstance createInstance = nullptr;
    gipa(XR_NULL_HANDLE, "xrEnumerateApiLayerProperties", (PFN_xrVoidFunction *)&enumLayers);
    gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&createInstance);

    uint32_t n = 0; enumLayers(0, &n, nullptr);
    std::vector<XrApiLayerProperties> props(n, { XR_TYPE_API_LAYER_PROPERTIES });
    enumLayers(n, &n, props.data());
    bool found = false;
    printf("layers visible to the loader: %u\n", n);
    for (auto &p : props) { printf("  %s (spec %u.%u.%u, layer v%u) %s\n", p.layerName, XR_VERSION_MAJOR(p.specVersion), XR_VERSION_MINOR(p.specVersion), XR_VERSION_PATCH(p.specVersion), p.layerVersion, p.description); if (strstr(p.layerName, "nr_bridge")) found = true; }
    if (!found) { printf("FAIL: our layer was not enumerated\n"); return 2; }

    const char *layerNames[] = { "XR_APILAYER_LOWPOPLABS_nr_bridge" };
    const char *extNames[] = { "XR_KHR_D3D11_enable" };
    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy_s(ci.applicationInfo.applicationName, "nr-bridge-test");
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "test");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledApiLayerCount = 1; ci.enabledApiLayerNames = layerNames;
    ci.enabledExtensionCount = 1; ci.enabledExtensionNames = extNames;
    XrInstance inst = XR_NULL_HANDLE;
    XrResult r = createInstance(&ci, &inst);
    printf("xrCreateInstance with the layer enabled -> %d (%s)\n", (int)r, XR_SUCCEEDED(r) ? "success: layer negotiated and chained" : "failed (a runtime failure here is still fine if the layer log shows the chain was reached)");
    if (XR_SUCCEEDED(r))
    {
        PFN_xrGetInstanceProperties gip = nullptr; gipa(inst, "xrGetInstanceProperties", (PFN_xrVoidFunction *)&gip);
        XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
        if (gip && XR_SUCCEEDED(gip(inst, &ip))) printf("runtime: %s %u.%u.%u\n", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
        PFN_xrDestroyInstance di = nullptr; gipa(inst, "xrDestroyInstance", (PFN_xrVoidFunction *)&di);
        if (di) di(inst);
    }
    return XR_SUCCEEDED(r) ? 0 : 3;
}

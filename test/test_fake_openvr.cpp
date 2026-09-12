// A stand-in for the real openvr_api.dll, built as openvr_api.orig.dll for test_openvr.exe. It serves fake
// IVRCompositor (vtable and FnTable, versions 022 and 029 with their different Submit slots) and IVRSystem_022
// objects, records every Submit call, and exposes the record through bnr_fake_* exports.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>

struct Texture_t { void *handle; int eType; int eColorSpace; };
struct VRTextureBounds_t { float uMin, vMin, uMax, vMax; };
struct HmdMatrix34_t { float m[3][4]; };
struct TrackedDevicePose_t { HmdMatrix34_t mDeviceToAbsoluteTracking; float vVelocity[3]; float vAngularVelocity[3]; int eTrackingResult; bool bPoseIsValid; bool bDeviceIsConnected; };

struct SubmitRec { int eye; void *handle; VRTextureBounds_t bounds; bool hadBounds; int flags; int viaArray; uint32_t arrayIndex; int viaFnTable; };
static SubmitRec g_recs[4096]; static int g_nrec = 0;
static void Record(int eye, const Texture_t *t, const VRTextureBounds_t *b, int flags, int viaArray, uint32_t idx, int fn)
{
    if (g_nrec >= 4096) return;
    SubmitRec &r = g_recs[g_nrec++]; r.eye = eye; r.handle = t ? t->handle : nullptr; r.hadBounds = b != nullptr; if (b) r.bounds = *b; else r.bounds = { 0, 0, 0, 0 }; r.flags = flags; r.viaArray = viaArray; r.arrayIndex = idx; r.viaFnTable = fn;
}

// ---- vtable compositors: a C++ class with the real slot order ----
struct Comp022
{
    virtual void SetTrackingSpace(int) {}
    virtual int GetTrackingSpace() { return 0; }
    virtual int WaitGetPoses(TrackedDevicePose_t *r, uint32_t n, TrackedDevicePose_t *, uint32_t) { return GetLastPoses(r, n, nullptr, 0); }
    virtual int GetLastPoses(TrackedDevicePose_t *r, uint32_t n, TrackedDevicePose_t *, uint32_t)
    {
        if (n < 1) return 0;
        memset(r, 0, sizeof(*r)); r->bPoseIsValid = true; r->bDeviceIsConnected = true;
        // 10 degrees yaw, 1.6 m up
        const float a = 10.0f * 3.14159265f / 180.0f; r->mDeviceToAbsoluteTracking.m[0][0] = cosf(a); r->mDeviceToAbsoluteTracking.m[0][2] = sinf(a); r->mDeviceToAbsoluteTracking.m[1][1] = 1; r->mDeviceToAbsoluteTracking.m[2][0] = -sinf(a); r->mDeviceToAbsoluteTracking.m[2][2] = cosf(a); r->mDeviceToAbsoluteTracking.m[1][3] = 1.6f;
        return 0;
    }
    virtual int GetLastPoseForTrackedDeviceIndex(uint32_t, TrackedDevicePose_t *, TrackedDevicePose_t *) { return 0; }
    virtual int Submit(int eye, const Texture_t *t, const VRTextureBounds_t *b, int flags) { Record(eye, t, b, flags, 0, 0, 0); return 0; }
    virtual int SubmitWithArrayIndex(int eye, const Texture_t *t, uint32_t idx, const VRTextureBounds_t *b, int flags) { Record(eye, t, b, flags, 1, idx, 0); return 0; }
    virtual void ClearLastSubmittedFrame() {}
    virtual void PostPresentHandoff() {}
};
struct Comp029
{
    virtual void SetTrackingSpace(int) {}
    virtual int GetTrackingSpace() { return 0; }
    virtual int WaitGetPoses(TrackedDevicePose_t *r, uint32_t n, TrackedDevicePose_t *, uint32_t) { return GetLastPoses(r, n, nullptr, 0); }
    virtual int GetLastPoses(TrackedDevicePose_t *r, uint32_t n, TrackedDevicePose_t *, uint32_t) { if (n < 1) return 0; memset(r, 0, sizeof(*r)); r->mDeviceToAbsoluteTracking.m[0][0] = r->mDeviceToAbsoluteTracking.m[1][1] = r->mDeviceToAbsoluteTracking.m[2][2] = 1; r->bPoseIsValid = true; return 0; }
    virtual int GetLastPoseForTrackedDeviceIndex(uint32_t, TrackedDevicePose_t *, TrackedDevicePose_t *) { return 0; }
    virtual int GetSubmitTexture(Texture_t *, bool *, int, int) { return 0; }
    virtual int Submit(int eye, const Texture_t *t, const VRTextureBounds_t *b, int flags) { Record(eye, t, b, flags, 0, 0, 0); return 0; }
    virtual int SubmitWithArrayIndex(int eye, const Texture_t *t, uint32_t idx, const VRTextureBounds_t *b, int flags) { Record(eye, t, b, flags, 1, idx, 0); return 0; }
    virtual void ClearLastSubmittedFrame() {}
    virtual void PostPresentHandoff() {}
};
struct Sys022
{
    virtual void GetRecommendedRenderTargetSize(uint32_t *w, uint32_t *h) { *w = 2008; *h = 2166; }
    virtual void GetProjectionMatrix(void *, int, float, float) {}
    virtual void GetProjectionRaw(int eye, float *l, float *r, float *t, float *b) { *l = eye == 0 ? -1.30f : -1.10f; *r = eye == 0 ? 1.10f : 1.30f; *t = -1.15f; *b = 1.20f; }
    virtual bool ComputeDistortion(int, float, float, void *) { return false; }
    virtual HmdMatrix34_t GetEyeToHeadTransform(int eye) { HmdMatrix34_t m = {}; m.m[0][0] = m.m[1][1] = m.m[2][2] = 1; m.m[0][3] = eye == 0 ? -0.032f : 0.032f; return m; }
    virtual bool GetTimeSinceLastVsync(float *, uint64_t *) { return false; }
};
static Comp022 g_c022; static Comp029 g_c029; static Sys022 g_sys;

// ---- FnTable compositor (022 layout): plain functions, no this ----
static void FnSetTrackingSpace(int) {}
static int FnGetTrackingSpace() { return 0; }
static int FnGetLastPoses(TrackedDevicePose_t *r, uint32_t n, TrackedDevicePose_t *, uint32_t) { return g_c022.GetLastPoses(r, n, nullptr, 0); }
static int FnWaitGetPoses(TrackedDevicePose_t *r, uint32_t n, TrackedDevicePose_t *, uint32_t) { return g_c022.GetLastPoses(r, n, nullptr, 0); }
static int FnGetLastPoseFor(uint32_t, TrackedDevicePose_t *, TrackedDevicePose_t *) { return 0; }
static int FnSubmit(int eye, const Texture_t *t, const VRTextureBounds_t *b, int flags) { Record(eye, t, b, flags, 0, 0, 1); return 0; }
static int FnSubmitArray(int eye, const Texture_t *t, uint32_t idx, const VRTextureBounds_t *b, int flags) { Record(eye, t, b, flags, 1, idx, 1); return 0; }
static void FnClear() {}
static void *g_fnTable[16] = { (void *)FnSetTrackingSpace, (void *)FnGetTrackingSpace, (void *)FnWaitGetPoses, (void *)FnGetLastPoses, (void *)FnGetLastPoseFor, (void *)FnSubmit, (void *)FnSubmitArray, (void *)FnClear };

static int g_initCalls = 0, g_shutdownCalls = 0;
extern "C"
{
__declspec(dllexport) uint32_t VR_InitInternal2(int *err, int, const char *) { ++g_initCalls; if (err) *err = 0; return 7; }
__declspec(dllexport) uint32_t VR_InitInternal(int *err, int) { ++g_initCalls; if (err) *err = 0; return 7; }
__declspec(dllexport) void VR_ShutdownInternal() { ++g_shutdownCalls; }
__declspec(dllexport) bool VR_IsHmdPresent() { return true; }
__declspec(dllexport) bool VR_IsRuntimeInstalled() { return true; }
__declspec(dllexport) const char *VR_RuntimePath() { return "fake"; }
__declspec(dllexport) bool VR_GetRuntimePath(char *b, uint32_t n, uint32_t *req) { if (req) *req = 5; if (b && n >= 5) { memcpy(b, "fake", 5); return true; } return false; }
__declspec(dllexport) const char *VR_GetVRInitErrorAsSymbol(int) { return "VRInitError_None"; }
__declspec(dllexport) const char *VR_GetVRInitErrorAsEnglishDescription(int) { return "No error"; }
__declspec(dllexport) const char *VR_GetStringForHmdError(int) { return "No error"; }
__declspec(dllexport) bool VR_IsInterfaceVersionValid(const char *v) { return strstr(v, "022") || strstr(v, "029"); }
__declspec(dllexport) uint32_t VR_GetInitToken() { return 7; }
__declspec(dllexport) void *VR_GetGenericInterface(const char *ver, int *err)
{
    if (err) *err = 0;
    if (!strcmp(ver, "IVRCompositor_022")) return &g_c022;
    if (!strcmp(ver, "IVRCompositor_029")) return &g_c029;
    if (!strcmp(ver, "FnTable:IVRCompositor_022")) return g_fnTable;
    if (!strcmp(ver, "IVRSystem_022")) return &g_sys;
    if (err) *err = 105; return nullptr;   // VRInitError_Init_InterfaceNotFound
}
__declspec(dllexport) void *VRControlPanel() { return nullptr; }
__declspec(dllexport) void *LiquidVR() { return nullptr; }
// test access
__declspec(dllexport) int bnr_fake_submits() { return g_nrec; }
__declspec(dllexport) const void *bnr_fake_submit(int i) { return i < g_nrec ? &g_recs[i] : nullptr; }
__declspec(dllexport) void bnr_fake_reset() { g_nrec = 0; }
__declspec(dllexport) int bnr_fake_inits() { return g_initCalls; }
__declspec(dllexport) int bnr_fake_shutdowns() { return g_shutdownCalls; }
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

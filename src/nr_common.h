// nr-bridge: paths, logging and cfg parsing shared by the OpenXR layer (nr_bridge_layer.cpp) and the OpenVR
// proxy (nr_openvr_proxy.cpp). Everything is static: exactly one translation unit per DLL includes this header.
//
// Layout. Standalone: the DLL's own folder holds nr-bridge.cfg and the logs. Central (what the installer sets
// up): one home folder (env NR_BRIDGE_HOME, else HKCU\Software\LowPopLabs\nr-bridge\Home) holds the global cfg,
// games\<exe stem>.cfg per game (its presence enables the layer for that game and overrides the global keys),
// logs\nr-bridge-<exe stem>.log, the forwarder and the model DLL. The OpenVR proxy must live in the game's plugin
// folder but reads the same central files.
// Author: LowPopLabs
#pragma once
#include "nr_direct.h"
#include <mutex>
#include <string>
#include <cstdarg>
#include <cstdio>

// ---------------------------------------------------------------- paths
static wchar_t g_dir[MAX_PATH] = {};       // this DLL's folder
static wchar_t g_home[MAX_PATH] = {};      // central home, or g_dir when standalone
static wchar_t g_exeStem[128] = {};        // the game exe without extension
static wchar_t g_cfgGlobal[MAX_PATH] = {}, g_cfgGame[MAX_PATH] = {}, g_logDir[MAX_PATH] = {};
static bool g_central = false, g_gameCfgFound = false;

static void InitPaths(HMODULE module)
{
    GetModuleFileNameW(module, g_dir, MAX_PATH);
    if (wchar_t *s = wcsrchr(g_dir, L'\\')) *s = 0;
    wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t *name = wcsrchr(exe, L'\\'); name = name ? name + 1 : exe;
    wcscpy_s(g_exeStem, name); if (wchar_t *dot = wcsrchr(g_exeStem, L'.')) *dot = 0;
    if (!g_exeStem[0]) wcscpy_s(g_exeStem, L"unknown");
    // central home: environment first (tests), then the installer's registry value
    wchar_t home[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"NR_BRIDGE_HOME", home, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        home[0] = 0; HKEY k = nullptr; DWORD sz = sizeof(home);
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\LowPopLabs\\nr-bridge", 0, KEY_READ, &k) == ERROR_SUCCESS)
        { if (RegQueryValueExW(k, L"Home", nullptr, nullptr, (LPBYTE)home, &sz) != ERROR_SUCCESS) home[0] = 0; RegCloseKey(k); }
    }
    const DWORD attr = home[0] ? GetFileAttributesW(home) : INVALID_FILE_ATTRIBUTES;
    g_central = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    wcscpy_s(g_home, g_central ? home : g_dir);
    swprintf_s(g_cfgGlobal, L"%s\\nr-bridge.cfg", g_home);
    // the cfg was called bonelab-nr-bridge.cfg before the rename: carry an existing one over (or read it in place if
    // the rename is refused), so no install silently falls back to the built-in defaults
    if (GetFileAttributesW(g_cfgGlobal) == INVALID_FILE_ATTRIBUTES)
    {
        wchar_t old[MAX_PATH]; swprintf_s(old, L"%s\\bonelab-nr-bridge.cfg", g_home);
        if (GetFileAttributesW(old) != INVALID_FILE_ATTRIBUTES && !MoveFileW(old, g_cfgGlobal)) wcscpy_s(g_cfgGlobal, old);
    }
    swprintf_s(g_cfgGame, L"%s\\games\\%s.cfg", g_home, g_exeStem);
    if (g_central) { swprintf_s(g_logDir, L"%s\\logs", g_home); CreateDirectoryW(g_logDir, nullptr); }
    else wcscpy_s(g_logDir, g_dir);
}

// ---------------------------------------------------------------- logging
static FILE *g_log = nullptr;
static std::mutex g_logmtx;

void BridgeLog(const char *fmt, ...)
{
    std::lock_guard<std::mutex> lk(g_logmtx);
    if (!g_log)
    {
        // one log per game, so logs from different games never overwrite each other
        wchar_t p[MAX_PATH]; swprintf_s(p, L"%s\\nr-bridge-%s.log", g_logDir[0] ? g_logDir : g_dir, g_exeStem[0] ? g_exeStem : L"unknown");
        g_log = _wfopen(p, L"a");
        if (!g_log) return;
    }
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "%02d:%02d:%02d.%03d [%5lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

// ---------------------------------------------------------------- config
struct Cfg
{
    bool enabled = true;
    std::string mode = "direct";   // direct (own D3D12 + NGX) or reshade (hidden swap chain for ReShade + RenoDX)
    float scale = 1.0f;
    int toggle_vk = VK_F10;
    int reload_vk = VK_F9;         // re-read this file and apply it without restarting (the file is also watched)
    bool any_app = false;
    bool window_visible = false;
    bool skip_present = false;
    int log_frames = 3;
    std::string app_match = "BONELAB";
    DirectConfig direct;           // direct-mode settings (mv, tuning, ngx path)
};
static Cfg g_cfg;
static bool g_cfg_loaded = false;
static FILETIME g_cfgTime = {};

// the later of the global and the per-game cfg write times (either changing means "reload")
static bool CfgFileTime(FILETIME *ft)
{
    WIN32_FILE_ATTRIBUTE_DATA a = {}, b = {};
    const bool ha = GetFileAttributesExW(g_cfgGlobal, GetFileExInfoStandard, &a), hb = GetFileAttributesExW(g_cfgGame, GetFileExInfoStandard, &b);
    if (!ha && !hb) return false;
    if (ha && hb) *ft = CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) >= 0 ? a.ftLastWriteTime : b.ftLastWriteTime;
    else *ft = ha ? a.ftLastWriteTime : b.ftLastWriteTime;
    return true;
}

static void WriteDefaultCfg(const wchar_t *path)
{
    FILE *f = _wfopen(path, L"w");
    if (!f) return;
    fputs("# nr-bridge settings (read at game start, watched while it runs)\n"
          "enabled=1\n"
          "# direct = this layer runs DLSS 5 Neural Rendering itself (no ReShade needed)\n"
          "# reshade = route the eyes through a hidden swap chain for ReShade + RenoDX\n"
          "mode=direct\n"
          "# Neural Rendering work resolution as a fraction of the eye-pair size, 0.25 .. 1.0\n"
          "scale=0.65\n"
          "# direct mode: when scale < 1 transfer only the model's edit onto the full-res frame\n"
          "residual=1\n"
          "# fovea: fraction of each eye around the centre the full-quality passes process; elliptical, feathered\n"
          "fovea=0.65\nfovea_shape=ellipse\nfeather=400\n"
          "# outer tier: the whole eye pair at this scale for lighting/tone outside the fovea; packed = one evaluate per pass\n"
          "outer_scale=0.25\nouter_pack=1\n"
          "# direct mode: motion vectors from head rotation (0 = none), sign flip and scale for experiments\n"
          "mv=1\nmv_sign=1\nmv_scale=1.0\n"
          "# direct mode: constant depth handed to the model\n"
          "depth_value=0.5\n"
          "# direct mode model tuning (same meaning as the RenoDX panel)\n"
          "intensity=0.8\nlocal_structure=0.8\nlocal_tone=0.8\nskin_structure=-1\nstyle=1\npreset=0\nauto_mask=1\n"
          "# direct mode: model passes per frame (each pass re-processes the previous pass's output; cost multiplies)\n"
          "passes=2\n"
          "# direct mode NGX path: auto | core | snippet | none (none = debug passthrough, no model)\n"
          "ngx=auto\n"
          "# key that re-reads this file in-game (0x78 = F9); the file is also watched for changes\n"
          "reload_vk=0x78\n"
          "# virtual-key code that toggles the bridge at runtime (0x79 = F10, 0x78 = F9). 0 = none\n"
          "toggle_vk=0x79\n"
          "# OpenXR layer activation: a games\\<exe>.cfg file enables a game (the installer manages these); app_match is the\n"
          "# standalone fallback (activate when the exe name contains this text), any_app=1 activates for every OpenXR app\n"
          "app_match=BONELAB\n"
          "any_app=0\n"
          "# debugging: show the hidden window / skip the Present call (pure copy test)\n"
          "window_visible=0\n"
          "skip_present=0\n"
          "# frames to log in detail\n"
          "log_frames=3\n", f);
    fclose(f);
}

// parses one cfg file over the current g_cfg (keys not present keep their current value); false = file not there
static bool ParseCfg(const wchar_t *path)
{
    FILE *f = _wfopen(path, L"r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        char *s = line; while (*s == ' ' || *s == '\t') ++s;
        if (*s == '#' || *s == ';' || *s == '\r' || *s == '\n' || *s == 0) continue;
        char *eq = strchr(s, '='); if (!eq) continue;
        *eq = 0; std::string key(s), val(eq + 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ' || val.back() == '\t')) val.pop_back();
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(0, 1);
        if (key == "enabled") g_cfg.enabled = atoi(val.c_str()) != 0;
        else if (key == "mode") g_cfg.mode = val;
        else if (key == "scale") { g_cfg.scale = (float)atof(val.c_str()); g_cfg.direct.scale = g_cfg.scale; }
        else if (key == "residual") g_cfg.direct.residual = atoi(val.c_str()) != 0;
        else if (key == "fovea") g_cfg.direct.fovea = (float)atof(val.c_str());
        else if (key == "feather") g_cfg.direct.feather = (float)atof(val.c_str());
        else if (key == "fovea_shape") g_cfg.direct.fovea_shape = (val == "rect" || val == "0") ? 0 : 1;
        else if (key == "outer_scale") g_cfg.direct.outer_scale = (float)atof(val.c_str());
        else if (key == "outer_pack") g_cfg.direct.outer_pack = atoi(val.c_str()) != 0;
        else if (key == "viewport_ref") g_cfg.direct.viewport_ref = (float)atof(val.c_str());
        else if (key == "mv") g_cfg.direct.mv = atoi(val.c_str()) != 0;
        else if (key == "mv_sign") g_cfg.direct.mv_sign = (float)atof(val.c_str());
        else if (key == "mv_scale") g_cfg.direct.mv_scale = (float)atof(val.c_str());
        else if (key == "depth_value") g_cfg.direct.depth_value = (float)atof(val.c_str());
        else if (key == "intensity") g_cfg.direct.intensity = (float)atof(val.c_str());
        else if (key == "local_structure") g_cfg.direct.local_structure = (float)atof(val.c_str());
        else if (key == "local_tone") g_cfg.direct.local_tone = (float)atof(val.c_str());
        else if (key == "skin_structure") g_cfg.direct.skin_structure = (float)atof(val.c_str());
        else if (key == "style") g_cfg.direct.style = (unsigned)atoi(val.c_str());
        else if (key == "preset") g_cfg.direct.preset = (unsigned)atoi(val.c_str());
        else if (key == "auto_mask") g_cfg.direct.auto_mask = atoi(val.c_str()) != 0;
        else if (key == "passes") g_cfg.direct.passes = atoi(val.c_str());
        else if (key == "reload_vk") g_cfg.reload_vk = (int)strtol(val.c_str(), nullptr, 0);
        else if (key == "ngx") g_cfg.direct.ngx = (val == "core") ? 1 : (val == "snippet") ? 2 : (val == "none") ? 3 : (val == "half") ? 4 : (val == "tophalf") ? 5 : 0;
        else if (key == "app_id") g_cfg.direct.app_id = strtoull(val.c_str(), nullptr, 0);
        else if (key == "toggle_vk") g_cfg.toggle_vk = (int)strtol(val.c_str(), nullptr, 0);
        else if (key == "any_app") g_cfg.any_app = atoi(val.c_str()) != 0;
        else if (key == "window_visible") g_cfg.window_visible = atoi(val.c_str()) != 0;
        else if (key == "skip_present") g_cfg.skip_present = atoi(val.c_str()) != 0;
        else if (key == "log_frames") g_cfg.log_frames = atoi(val.c_str());
        else if (key == "app_match") g_cfg.app_match = val;
    }
    fclose(f);
    return true;
}

static void LoadCfg(bool force = false)
{
    if (g_cfg_loaded && !force) return;
    g_cfg_loaded = true;
    g_cfg = Cfg(); // every key not in the files goes back to its default
    if (!ParseCfg(g_cfgGlobal)) { WriteDefaultCfg(g_cfgGlobal); ParseCfg(g_cfgGlobal); }
    g_gameCfgFound = ParseCfg(g_cfgGame);   // per-game overrides win
    CfgFileTime(&g_cfgTime);
}

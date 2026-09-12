// nr-bridge-setup: standalone installer for the DLSS 5 Neural Rendering bridge. Lists the VR games found in the
// Steam libraries (plus folders added by hand), tells OpenXR games from OpenVR games, and enables or disables the
// bridge per game with one click, ReShade style.
//
// Central layout (%LOCALAPPDATA%\nr-bridge): the OpenXR layer DLL + manifest registered ONCE for the current user,
// the forwarder, the model DLL, the global cfg, games\<exe>.cfg per enabled game (overrides), logs\. The OpenVR
// proxy is copied into each OpenVR game's plugin folder (the game's openvr_api.dll is renamed openvr_api.orig.dll)
// and reads the same central files through HKCU\Software\LowPopLabs\nr-bridge\Home.
//
// Command line (also usable from scripts): --scan | --sync | --enable <exe stem> | --disable <exe stem> | --notes | --home
// (--notes rewrites the comment block on top of every per-game cfg without touching the settings in it)
// Author: LowPopLabs
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <io.h>
#include <fcntl.h>
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define SETUP_VERSION L"0.2.0"
using std::wstring;

struct Game
{
    wstring name, dir, stem, pluginDir;
    bool openxr = false, openvr = false, unity = false;
    bool xrEnabled = false, vrInstalled = false, cfgOff = false;
    wstring api() const { return openxr && openvr ? L"OpenXR (+OpenVR)" : openxr ? L"OpenXR" : openvr ? L"OpenVR" : L"-"; }
    bool usesXr() const { return openxr; }
    wstring status() const
    {
        if (openxr) return xrEnabled ? L"enabled" : cfgOff ? L"off (settings kept)" : L"off";
        if (openvr) return vrInstalled ? (xrEnabled ? L"installed" : L"installed (no cfg)") : cfgOff ? L"off (settings kept)" : L"off";
        return L"not a VR game";
    }
};

static wstring g_setupDir, g_home;
static std::vector<Game> g_games;
static wstring g_headline;

// ---------------------------------------------------------------- small helpers
static bool Exists(const wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool IsDir(const wstring &p) { const DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static wstring Join(const wstring &a, const wstring &b) { return a.empty() ? b : (a.back() == L'\\' ? a + b : a + L"\\" + b); }
static wstring Lower(wstring s) { for (auto &c : s) c = (wchar_t)towlower(c); return s; }
static bool EndsWithI(const wstring &s, const wstring &suf) { return s.size() >= suf.size() && Lower(s.substr(s.size() - suf.size())) == Lower(suf); }
static wstring Parent(const wstring &p) { const size_t i = p.find_last_of(L"\\/"); return i == wstring::npos ? L"" : p.substr(0, i); }
static wstring Leaf(const wstring &p) { const size_t i = p.find_last_of(L"\\/"); return i == wstring::npos ? p : p.substr(i + 1); }
static std::string ReadFile(const wstring &p) { std::string s; FILE *f = _wfopen(p.c_str(), L"rb"); if (!f) return s; char b[4096]; size_t n; while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n); fclose(f); return s; }
static bool WriteFile(const wstring &p, const std::string &s) { FILE *f = _wfopen(p.c_str(), L"wb"); if (!f) return false; fwrite(s.data(), 1, s.size(), f); fclose(f); return true; }
static wstring Widen(const std::string &s) { if (s.empty()) return L""; int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0); wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n); return w; }
static std::string Narrow(const wstring &w) { if (w.empty()) return ""; int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr); std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr); return s; }
static bool SameFile(const wstring &a, const wstring &b)
{
    WIN32_FILE_ATTRIBUTE_DATA x = {}, y = {};
    if (!GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &x) || !GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &y)) return false;
    return x.nFileSizeLow == y.nFileSizeLow && x.nFileSizeHigh == y.nFileSizeHigh && CompareFileTime(&x.ftLastWriteTime, &y.ftLastWriteTime) == 0;
}
static bool CopyIfDifferent(const wstring &src, const wstring &dst) { if (!Exists(src)) return false; if (SameFile(src, dst)) return true; return CopyFileW(src.c_str(), dst.c_str(), FALSE) != 0; }
static void ForEachEntry(const wstring &dir, const wstring &pattern, bool dirs, void (*fn)(const wstring &, void *), void *ctx)
{
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(Join(dir, pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir == dirs) fn(Join(dir, fd.cFileName), ctx);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static wstring HomeDir() { wchar_t p[MAX_PATH] = {}; SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, p); return Join(p, L"nr-bridge"); }
static wstring GameCfg(const Game &g) { return Join(Join(g_home, L"games"), g.stem + L".cfg"); }

// ---------------------------------------------------------------- Steam libraries and manifests
static std::string Quoted(const std::string &line, int index)   // the index-th "quoted" string on the line
{
    size_t p = 0; std::string last;
    for (int i = 0; i <= index; ++i)
    {
        const size_t a = line.find('"', p); if (a == std::string::npos) return "";
        const size_t b = line.find('"', a + 1); if (b == std::string::npos) return "";
        last = line.substr(a + 1, b - a - 1); p = b + 1;
    }
    return last;
}
static std::string Unescape(std::string s) { std::string o; for (size_t i = 0; i < s.size(); ++i) { if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '\\') { o += '\\'; ++i; } else o += s[i]; } return o; }
static std::vector<wstring> SteamLibraries()
{
    std::vector<wstring> libs;
    wchar_t sp[MAX_PATH] = {}; DWORD sz = sizeof(sp); HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &k) == ERROR_SUCCESS) { RegQueryValueExW(k, L"SteamPath", nullptr, nullptr, (LPBYTE)sp, &sz); RegCloseKey(k); }
    if (!sp[0]) return libs;
    for (wchar_t *c = sp; *c; ++c) if (*c == L'/') *c = L'\\';
    libs.push_back(sp);
    const std::string vdf = ReadFile(Join(sp, L"steamapps\\libraryfolders.vdf"));
    size_t pos = 0;
    while (pos < vdf.size())
    {
        const size_t e = vdf.find('\n', pos); const std::string line = vdf.substr(pos, e == std::string::npos ? std::string::npos : e - pos); pos = e == std::string::npos ? vdf.size() : e + 1;
        if (line.find("\"path\"") == std::string::npos) continue;
        const wstring p = Widen(Unescape(Quoted(line, 1)));
        if (!p.empty() && std::find_if(libs.begin(), libs.end(), [&](const wstring &l) { return Lower(l) == Lower(p); }) == libs.end()) libs.push_back(p);
    }
    return libs;
}

// ---------------------------------------------------------------- game detection
struct ScanCtx { bool openvr = false, openxr = false; wstring vrPluginDir; int depth = 0; };
static void ScanDir(const wstring &dir, ScanCtx &c);
static void ScanFile(const wstring &f, void *ctx)
{
    ScanCtx &c = *(ScanCtx *)ctx; const wstring n = Lower(Leaf(f));
    if (n == L"openvr_api.dll") { if (!c.openvr || c.vrPluginDir.find(L"_Data") == wstring::npos) c.vrPluginDir = Parent(f); c.openvr = true; }
    else if (n == L"openxr_loader.dll" || n == L"unityopenxr.dll") c.openxr = true;
}
static void ScanSub(const wstring &d, void *ctx)
{
    ScanCtx &c = *(ScanCtx *)ctx; const wstring n = Lower(Leaf(d));
    static const wchar_t *skip[] = { L"streamingassets", L"content", L"paks", L"movies", L"videos", L"saved", L"logs", L"cache", L"mods", L"userdata", L"screenshots", L"il2cppassemblies", L"il2cpp_data", L"resources" };
    for (const wchar_t *s : skip) if (n == s) return;
    if (c.depth >= 5) return;
    ++c.depth; ScanDir(d, c); --c.depth;
}
static void ScanDir(const wstring &dir, ScanCtx &c) { ForEachEntry(dir, L"*", false, ScanFile, &c); ForEachEntry(dir, L"*", true, ScanSub, &c); }

struct StemCtx { wstring stem; ULONGLONG best = 0; wstring dir; bool unity = false; };
static void UnityData(const wstring &d, void *ctx) { StemCtx &s = *(StemCtx *)ctx; const wstring n = Leaf(d); if (EndsWithI(n, L"_Data")) { const wstring stem = n.substr(0, n.size() - 5); if (Exists(Join(s.dir, stem + L".exe"))) { s.stem = stem; s.unity = true; } } }
static void BiggestExe(const wstring &f, void *ctx) { StemCtx &s = *(StemCtx *)ctx; if (!EndsWithI(f, L".exe")) return; const wstring n = Lower(Leaf(f)); if (n.find(L"crashhandler") != wstring::npos || n.find(L"unins") != wstring::npos) return; WIN32_FILE_ATTRIBUTE_DATA a = {}; GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &a); const ULONGLONG sz = ((ULONGLONG)a.nFileSizeHigh << 32) | a.nFileSizeLow; if (sz > s.best) { s.best = sz; s.stem = Leaf(f).substr(0, Leaf(f).size() - 4); } }
static void UeShipping(const wstring &d, void *ctx) { StemCtx &s = *(StemCtx *)ctx; if (!s.stem.empty()) return; WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(Join(d, L"Binaries\\Win64\\*-Win64-Shipping.exe").c_str(), &fd); if (h != INVALID_HANDLE_VALUE) { s.stem = fd.cFileName; s.stem = s.stem.substr(0, s.stem.size() - 4); FindClose(h); } }
static wstring DetectStem(const wstring &dir, bool *unity)
{
    StemCtx s; s.dir = dir;
    ForEachEntry(dir, L"*", true, UnityData, &s);
    if (s.stem.empty()) ForEachEntry(dir, L"*", true, UeShipping, &s);
    if (s.stem.empty()) ForEachEntry(dir, L"*.exe", false, BiggestExe, &s);
    if (unity) *unity = s.unity;
    return s.stem;
}
static void Detect(Game &g)
{
    g.stem = DetectStem(g.dir, &g.unity);
    if (g.unity) { const std::string boot = ReadFile(Join(g.dir, g.stem + L"_Data\\boot.config")); if (boot.find("UnityOpenXR") != std::string::npos) g.openxr = true; }
    ScanCtx c; ScanDir(g.dir, c);
    g.openxr = g.openxr || c.openxr; g.openvr = c.openvr; g.pluginDir = c.vrPluginDir;
    // an OpenVR game's exe is the one whose *_Data folder holds the plugin (The Forest: TheForestVR.exe, not TheForest.exe)
    if (g.openvr && !g.openxr)
    {
        const wstring data = Parent(g.pluginDir);
        if (EndsWithI(Leaf(data), L"_Data")) { const wstring s = Leaf(data).substr(0, Leaf(data).size() - 5); if (Exists(Join(Parent(data), s + L".exe"))) { g.stem = s; g.unity = true; } }
    }
    if (g.stem.empty()) { g.openxr = g.openvr = false; return; }
    const wstring cfg = GameCfg(g);
    g.xrEnabled = Exists(cfg); g.cfgOff = Exists(cfg + L".off");
    g.vrInstalled = g.openvr && Exists(Join(g.pluginDir, L"openvr_api.orig.dll"));
}

static wstring FoldersFile() { return Join(g_home, L"folders.txt"); }
static std::vector<wstring> ManualFolders()
{
    std::vector<wstring> v; const std::string t = ReadFile(FoldersFile()); size_t p = 0;
    while (p < t.size()) { size_t e = t.find('\n', p); std::string l = t.substr(p, e == std::string::npos ? std::string::npos : e - p); p = e == std::string::npos ? t.size() : e + 1; while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back(); if (!l.empty()) v.push_back(Widen(l)); }
    return v;
}
// the proxy lives in each OpenVR game's plugin folder: after a package update, bring every installed copy up to date
static void RefreshProxies(std::vector<wstring> &notes)
{
    const wstring master = Join(g_home, L"openvr_api.dll");
    if (!Exists(master)) return;
    for (const Game &g : g_games)
    {
        if (!g.vrInstalled || g.openxr) continue;
        const wstring cur = Join(g.pluginDir, L"openvr_api.dll");
        if (SameFile(master, cur)) continue;
        if (CopyFileW(master.c_str(), cur.c_str(), FALSE)) notes.push_back(L"updated the OpenVR proxy in " + g.name);
        else notes.push_back(L"could not update the OpenVR proxy in " + g.name + L" (is the game running?)");
    }
}

static void ScanGames()
{
    g_games.clear();
    struct M { wstring name, dir; }; std::vector<M> found;
    for (const wstring &lib : SteamLibraries())
    {
        struct C { std::vector<M> *out; wstring lib; } c = { &found, lib };
        ForEachEntry(Join(lib, L"steamapps"), L"appmanifest_*.acf", false, [](const wstring &f, void *ctx) {
            C &c = *(C *)ctx; const std::string t = ReadFile(f); std::string name, inst; size_t p = 0;
            while (p < t.size()) { size_t e = t.find('\n', p); std::string l = t.substr(p, e == std::string::npos ? std::string::npos : e - p); p = e == std::string::npos ? t.size() : e + 1; if (l.find("\"name\"") != std::string::npos && name.empty()) name = Quoted(l, 1); else if (l.find("\"installdir\"") != std::string::npos && inst.empty()) inst = Quoted(l, 1); }
            if (!inst.empty()) c.out->push_back({ Widen(name.empty() ? inst : name), Join(Join(c.lib, L"steamapps\\common"), Widen(inst)) });
        }, &c);
    }
    for (const wstring &d : ManualFolders()) found.push_back({ Leaf(d), d });
    for (const M &m : found)
    {
        if (!IsDir(m.dir)) continue;
        Game g; g.name = m.name; g.dir = m.dir; Detect(g);
        if (!g.openxr && !g.openvr) continue;   // only VR games are listed
        g_games.push_back(g);
    }
    std::sort(g_games.begin(), g_games.end(), [](const Game &a, const Game &b) { return Lower(a.name) < Lower(b.name); });
}

// ---------------------------------------------------------------- central home: files, registration, migration
static const wchar_t *kImplicitKey = L"Software\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
static void DeleteFolderFiles(const wstring &dir) { ForEachEntry(dir, L"*", false, [](const wstring &f, void *) { DeleteFileW(f.c_str()); }, nullptr); RemoveDirectoryW(dir.c_str()); }

// an old per-game install (<game>\nr-bridge\) becomes games\<exe>.cfg; the folder and its registration go away
static void MigrateOld(const wstring &jsonPath, std::vector<wstring> &notes)
{
    const wstring folder = Parent(jsonPath), gameDir = Parent(folder);
    if (Lower(Leaf(folder)) != L"nr-bridge") return;
    if (Lower(folder) == Lower(g_home)) return;   // the home itself (its leaf is nr-bridge too): a stale manifest name there is not a per-game install
    const wstring stem = DetectStem(gameDir, nullptr);
    if (!stem.empty())
    {
        const wstring newCfg = Join(Join(g_home, L"games"), stem + L".cfg");
        for (const wchar_t *n : { L"nr-bridge.cfg", L"bonelab-nr-bridge.cfg" })   // the cfg's name after and before the rename
        {
            const wstring oldCfg = Join(folder, n);
            if (Exists(oldCfg) && !Exists(newCfg)) { CopyFileW(oldCfg.c_str(), newCfg.c_str(), FALSE); notes.push_back(L"migrated settings of " + stem + L" to games\\" + stem + L".cfg"); }
        }
    }
    for (const wchar_t *f : { L"nvngx_dlssnr.dll" }) { const wstring src = Join(folder, f), dst = Join(g_home, f); if (Exists(src) && !Exists(dst)) MoveFileW(src.c_str(), dst.c_str()); }
    DeleteFolderFiles(folder);
    notes.push_back(L"removed the old per-game install " + folder);
}

static void UpgradeMinimalGameCfgs(std::vector<wstring> &notes);
static bool SyncHome(std::vector<wstring> &notes)
{
    CreateDirectoryW(g_home.c_str(), nullptr); CreateDirectoryW(Join(g_home, L"games").c_str(), nullptr); CreateDirectoryW(Join(g_home, L"logs").c_str(), nullptr);
    bool ok = true;
    // rename from bonelab-nr-bridge.* to nr-bridge.*: an existing global cfg keeps its content under the new name
    { const wstring oldCfg = Join(g_home, L"bonelab-nr-bridge.cfg"), newCfg = Join(g_home, L"nr-bridge.cfg"); if (Exists(oldCfg) && !Exists(newCfg) && MoveFileW(oldCfg.c_str(), newCfg.c_str())) notes.push_back(L"renamed the global cfg to nr-bridge.cfg"); }
    const bool fromPackage = Exists(Join(g_setupDir, L"nr-bridge.dll"));
    if (fromPackage)
    {
        for (const wchar_t *f : { L"nr-bridge.dll", L"nr-bridge.json", L"nvngx.dll_nr_bridge.dll" })
            if (!CopyIfDifferent(Join(g_setupDir, f), Join(g_home, f))) { ok = false; notes.push_back(wstring(L"could not copy ") + f + L" (is a game running?)"); }
        CopyIfDifferent(Join(g_setupDir, L"openvr\\openvr_api.dll"), Join(g_home, L"openvr_api.dll"));
        if (!Exists(Join(g_home, L"nvngx_dlssnr.dll"))) CopyIfDifferent(Join(g_setupDir, L"nvngx_dlssnr.dll"), Join(g_home, L"nvngx_dlssnr.dll"));
        const wstring cfg = Join(g_home, L"nr-bridge.cfg");
        if (!Exists(cfg))
        {
            // the global cfg: the package defaults, minus the standalone name filter (the installer decides which games are on)
            std::string t = ReadFile(Join(g_setupDir, L"nr-bridge.cfg"));
            size_t p = t.find("\napp_match="); if (p != std::string::npos) { size_t e = t.find('\n', p + 1); t.replace(p, (e == std::string::npos ? t.size() : e) - p, "\napp_match="); }
            if (!t.empty()) WriteFile(cfg, t);
        }
    }
    // the old-named layer, manifest and forwarder go once the new ones are in place (a running game can hold the DLL)
    if (Exists(Join(g_home, L"nr-bridge.dll")))
        for (const wchar_t *f : { L"bonelab-nr-bridge.dll", L"bonelab-nr-bridge.json", L"nvngx.dll_bonelab_nr.dll" })
        {
            const wstring p = Join(g_home, f);
            if (!Exists(p)) continue;
            if (DeleteFileW(p.c_str())) notes.push_back(wstring(L"removed the old ") + f + L" (the package is now named nr-bridge)");
            else notes.push_back(wstring(L"could not remove the old ") + f + L" (is a game running?); start the installer again later");
        }
    UpgradeMinimalGameCfgs(notes);
    // registry: home pointer for the DLLs, and exactly one implicit-layer registration
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\LowPopLabs\\nr-bridge", 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS)
    { RegSetValueExW(k, L"Home", 0, REG_SZ, (const BYTE *)g_home.c_str(), (DWORD)((g_home.size() + 1) * sizeof(wchar_t))); RegCloseKey(k); }
    const wstring json = Join(g_home, L"nr-bridge.json");
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kImplicitKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS)
    {
        std::vector<wstring> stale;
        for (DWORD i = 0;; ++i) { wchar_t name[1024]; DWORD n = 1024; if (RegEnumValueW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break; if ((EndsWithI(name, L"\\nr-bridge.json") || EndsWithI(name, L"\\bonelab-nr-bridge.json")) && Lower(name) != Lower(json)) stale.push_back(name); }
        for (const wstring &s : stale) { RegDeleteValueW(k, s.c_str()); MigrateOld(s, notes); }
        if (Exists(json)) { const DWORD zero = 0; RegSetValueExW(k, json.c_str(), 0, REG_DWORD, (const BYTE *)&zero, sizeof(zero)); }
        RegCloseKey(k);
    }
    return ok;
}
static bool Registered()
{
    HKEY k = nullptr; bool r = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kImplicitKey, 0, KEY_READ, &k) == ERROR_SUCCESS) { DWORD v = 1, n = sizeof(v); r = RegQueryValueExW(k, Join(g_home, L"nr-bridge.json").c_str(), nullptr, nullptr, (LPBYTE)&v, &n) == ERROR_SUCCESS && v == 0; RegCloseKey(k); }
    return r && Exists(Join(g_home, L"nr-bridge.dll"));
}
static bool EnsureModel(wstring &note)
{
    const wstring dst = Join(g_home, L"nvngx_dlssnr.dll");
    if (Exists(dst)) return true;
    for (const Game &g : g_games) { const wstring c = Join(g.dir, L"nvngx_dlssnr.dll"); if (Exists(c)) { if (CopyFileW(c.c_str(), dst.c_str(), FALSE)) { note = L"model DLL copied from " + g.dir; return true; } } }
    note = L"nvngx_dlssnr.dll (the DLSS 5 model) is missing: copy it into " + g_home;
    return false;
}

// ---------------------------------------------------------------- per-game cfg: a notes block + a complete, independent copy of the settings
// Every game gets all keys, so tuning one game never touches another; the global cfg only seeds new games. The block
// of comments on top names the game, says where its files and log are, and carries what was learned tuning it (or
// generic guidance for a game nobody has tuned yet). The installer rewrites that block on every run (RefreshGameNotes)
// and never touches the settings below it, so notes can improve with each package without losing anyone's tuning.
static const char kNotesBegin[] = "# ==== nr-bridge notes for ";
static const char kNotesEnd[] = "# ==== end of notes: everything below is this game's own settings ====\n";
static std::string GlobalCfgText() { std::string t = ReadFile(Join(g_home, L"nr-bridge.cfg")); if (t.empty()) t = "enabled=1\nmode=direct\nscale=0.6\nfovea=0.6\nouter_scale=0.25\nouter_pack=1\npasses=1\nresidual=1\nmv=1\nngx=auto\n"; return t; }
static const Game *FindGame(const wstring &stem) { for (const Game &g : g_games) if (Lower(g.stem) == Lower(stem)) return &g; return nullptr; }

// what was learned while tuning the games that have actually been run with the bridge, by exe stem (lower case)
static std::string KnownGameNotes(const wstring &stemLower)
{
    if (stemLower == L"bonelab_steam_windows64") return
        "# Notes from tuning BONELAB (Quest 3 over Link, Meta OpenXR runtime, RTX 5060 Ti):\n"
        "# - Launch it the usual way (Meta app / Link). The log should open with \"negotiate ok\", \"xrCreateApiLayerInstance ...\n"
        "#   ACTIVE\" and \"xrCreateSession ... DIRECT ACTIVE\"; the bridge runs inside the game's own OpenXR session.\n"
        "# - The eye pair size is fixed for the session by the Meta runtime (seen 3648x1968 at 72 Hz, 3488x1920 at 80 Hz), so\n"
        "#   viewport_ref does nothing here.\n"
        "# - The Meta runtime snaps to HALF rate when a frame overruns: 72 Hz becomes 36, 80 Hz becomes 40, and the blur seen\n"
        "#   at half rate is reprojection, not the model. The game alone takes about 9.5 ms of the 13.9 ms budget at 72 Hz, so\n"
        "#   the model gets ~4 ms: scale 0.6 / fovea 0.6 / 1 pass measured 4.3 ms and held 72 Hz (the outer tier at 0.25 adds\n"
        "#   about 1.8 ms on top; check the 600-frame log line). scale 0.75 or 1.0 at fovea 0.6 fall to 36 Hz.\n"
        "# - In 80 Hz mode the model may take up to ~12.5 ms and still hold a steady 40 Hz. The settled quality config there:\n"
        "#   scale 0.65 / fovea 0.65 / outer 0.25 / packed / 2 passes / intensity, local_structure, local_tone 0.8 / style 1\n"
        "#   = 13.9 ms model, 40 Hz with dips to 34 on load spikes. fovea 0.7 / scale 0.5 / outer 0.3 / 2 passes = 12.1 ms, 40 Hz steady.\n"
        "# - One pass mostly changes lighting; face and skin detail need passes=2 (fine detail does not survive a 0.6-scale\n"
        "#   upsample, a second pass compounds it). Each pass costs about 3.9 ms per megapixel the model sees.\n"
        "# - preset 1 (what RenoDX used) costs the same as 0 and the presets were found to be inert; style 0/1/2 cost the same.\n"
        "# - If ReShade's dxgi.dll and renodx-dlss.addon64 are still in the game folder, park them (disable-reshade.cmd in the\n"
        "#   package): they spend a Neural Rendering pass on the desktop mirror every frame and cap the game around 60 Hz.\n"
        "# - Unity gives the bridge no depth buffer and no engine motion vectors: depth_value is a constant and mv=1 uses head\n"
        "#   rotation only, so very near objects can ghost slightly while the head translates.\n";
    if (stemLower == L"boneworks") return
        "# Notes from tuning BONEWORKS (SteamVR / OpenVR, Quest 3 over Link, RTX 5060 Ti):\n"
        "# - Turn the in-game ADAPTIVE RESOLUTION option OFF (graphics settings) before judging image quality. With it on the\n"
        "#   game shrinks its eye rect every frame to hold the frame rate; with the model's cost added it settles at a much\n"
        "#   lower resolution, which reads as blur, and an F10 A/B is unfair because bypassing lets the resolution climb back.\n"
        "#   The 600-frame log line reports \"viewport N% of max\": 100% means it is off. If you keep it on, set viewport_ref to\n"
        "#   N/100 so the model is not sized (and paid for) at a resolution the game never renders.\n"
        "# - The eye pair is up to 4272x2352 (two 2136x2352 textures), larger than BONELAB's, so the same settings cost more.\n"
        "#   Fixed resolution, 80 Hz mode, feather 600: scale 0.6 / fovea 0.6 / outer 0.25 / 1 pass = 9.6-11.9 ms model ->\n"
        "#   44-48 Hz; 0.7 / 0.7 / 0.25 / 1 pass = 11.6-14.8 ms -> 31-43 Hz; 0.7 / 0.7 / 0.3 / 2 passes = 19-26 ms -> 30-35 Hz.\n"
        "#   The model time swings about +-30% at the same settings because the game's own rendering shares the GPU.\n"
        "# - SteamVR lets the rate float (motion smoothing) instead of snapping to half rate like the Meta runtime, so the Hz\n"
        "#   in the log moves with the scene.\n"
        "# - Unity submits the eyes vertically flipped and with a viewport that changes size; both are handled automatically\n"
        "#   (\"texture bounds are vertically flipped\" and \"dynamic viewport\" in the log) and need no setting. Unity also gives\n"
        "#   no depth buffer and no engine motion vectors: depth_value is a constant and mv=1 uses head rotation only.\n"
        "# - Launch through Steam. The log should show \"hooked IVRCompositor_022 ... Submit slot 5\", \"direct engine ACTIVE\" and\n"
        "#   a \"pair L ... R ...\" line; a few \"VR_InitInternal ... err 108\" (HmdNotFound) lines before SteamVR is up are harmless.\n"
        "# - The bridge is the openvr_api.dll in BONEWORKS_Data\\Plugins, next to the game's own openvr_api.orig.dll. Steam's\n"
        "#   \"verify integrity of game files\" puts the game's DLL back; press Enable in the installer again afterwards.\n";
    return "";
}

// the comment block on top of a per-game cfg: what the file is, the game's facts, and the notes for it
static std::string NotesBlock(const wstring &stem)
{
    const Game *g = FindGame(stem);
    const std::string s = Narrow(stem);
    std::string t = kNotesBegin + (g ? Narrow(g->name) + " (" + s + ".exe)" : s + ".exe") + " ====\n"
        "# This file holds EVERY setting for this game. The installer wrote it when the game was enabled, as a copy of the\n"
        "# global nr-bridge.cfg (which only seeds new games), so tuning here never affects another game. Its presence\n"
        "# enables the bridge for this exe; the installer renames it to .off to disable. Saved changes apply in-game within a\n"
        "# second (or press F9); F10 toggles the effect for an A/B. The installer rewrites this notes block on every run and\n"
        "# never touches the settings below it.\n#\n";
    if (g)
    {
        t += "# Game: " + Narrow(g->name) + "  |  exe: " + s + ".exe  |  API: " + Narrow(g->api()) + (g->unity ? "  |  engine: Unity" : "") + "\n";
        t += "# Folder: " + Narrow(g->dir) + "\n";
        if (!g->openxr && g->openvr) t += "# Bridge: " + Narrow(Join(g->pluginDir, L"openvr_api.dll")) + " is the proxy; the game's own DLL is openvr_api.orig.dll next to it\n";
    }
    t += "# Log: " + Narrow(Join(Join(g_home, L"logs"), L"nr-bridge-" + stem + L".log")) + "\n#\n";
    const std::string known = KnownGameNotes(Lower(stem));
    if (!known.empty()) t += known;
    else
    {
        t += "# This game has not been tuned with the bridge yet. The settings below are the defaults; watch the log line written\n"
             "# every 600 frames (model ms and the frame interval in Hz) and trade scale, fovea, outer_scale and passes against it.\n"
             "# The model costs about 4 ms per megapixel per pass on an RTX 5060 Ti; F10 toggles it off and on for an A/B.\n";
        if (!g || g->openxr)
            t += "# OpenXR: the bridge runs inside the game's own OpenXR session; the log should show \"negotiate ok\",\n"
                 "#   \"xrCreateApiLayerInstance ... ACTIVE\" and \"xrCreateSession ... DIRECT ACTIVE\". The Meta runtime snaps to half\n"
                 "#   rate when a frame overruns (72 Hz -> 36), SteamVR lets it float.\n";
        if (!g || g->openvr)
            t += "# OpenVR: the proxy openvr_api.dll hooks IVRCompositor::Submit; the log should show \"hooked IVRCompositor_0xx\" and\n"
                 "#   \"direct engine ACTIVE\". MSAA or non-D3D11 eye textures are passed through untouched (the log says so). Steam's\n"
                 "#   \"verify integrity of game files\" puts the game's own DLL back: press Enable again afterwards. If the game has an\n"
                 "#   adaptive or dynamic resolution option, turn it off before judging image quality, or set viewport_ref to the\n"
                 "#   \"viewport N% of max\" the log reports.\n";
        if (!g || g->unity)
            t += "# Unity: the game gives the bridge no depth buffer and no engine motion vectors, so depth_value is a constant and\n"
                 "#   mv=1 uses head rotation only (slight ghosting on very near objects while the head translates).\n";
    }
    t += kNotesEnd;
    return t;
}
// skips the comment and blank lines at the top of a cfg text, so it starts at its first key
static size_t FirstKey(const std::string &t, size_t p = 0)
{
    while (p < t.size())
    {
        const size_t e = t.find('\n', p); const size_t next = e == std::string::npos ? t.size() : e + 1;
        size_t q = p; while (q < next && (t[q] == ' ' || t[q] == '\t')) ++q;
        if (q < next && t[q] != '#' && t[q] != '\r' && t[q] != '\n') return p;
        p = next;
    }
    return t.size();
}
// the global cfg without its own title block, which talks about seeding and would mislead inside a per-game file
static std::string SeedBody() { const std::string t = GlobalCfgText(); return t.substr(FirstKey(t)); }
static std::string FullGameCfg(const wstring &stem, const std::string &existing)
{
    std::string t = NotesBlock(stem) + "\n" + SeedBody();
    if (!existing.empty()) t += "\n# ---- lines carried over from this game's earlier cfg (later lines win) ----\n" + existing;
    return t;
}
// removes the installer-written comment block (this or an earlier build's) from the top of a per-game cfg, and the
// global cfg's title block that earlier builds copied in below it (a migrated standalone cfg starts with that title too)
static std::string StripNotes(const std::string &t)
{
    size_t cut = 0;
    if (t.compare(0, strlen(kNotesBegin), kNotesBegin) == 0)
    {
        const size_t e = t.find(kNotesEnd);
        if (e == std::string::npos) return t;            // a damaged block: leave the file alone
        cut = e + strlen(kNotesEnd);
    }
    else if (t.compare(0, 24, "# nr-bridge settings for") == 0)      // the three-line header of the first per-game build
    {
        const size_t e = t.find("\n\n");
        cut = e == std::string::npos ? t.size() : e + 2;
    }
    while (cut < t.size() && (t[cut] == '\n' || t[cut] == '\r')) ++cut;
    for (const char *title : { "# nr-bridge", "# bonelab-nr-bridge" })    // the seed's title block, any version
        if (t.compare(cut, strlen(title), title) == 0) { cut = FirstKey(t, cut); break; }
    return t.substr(cut);
}
// rewrites the notes block in every games\*.cfg and *.cfg.off; the settings below it are copied through unchanged
static void RefreshGameNotes(std::vector<wstring> &notes)
{
    struct C { std::vector<wstring> *notes; } c = { &notes };
    ForEachEntry(Join(g_home, L"games"), L"*.cfg*", false, [](const wstring &f, void *ctx) {
        const wstring leaf = Leaf(f);
        wstring stem;
        if (EndsWithI(leaf, L".cfg")) stem = leaf.substr(0, leaf.size() - 4);
        else if (EndsWithI(leaf, L".cfg.off")) stem = leaf.substr(0, leaf.size() - 8);
        else return;
        const std::string t = ReadFile(f);
        if (t.empty()) return;
        const std::string nt = NotesBlock(stem) + "\n" + StripNotes(t);
        if (nt == t) return;
        if (WriteFile(f, nt)) ((C *)ctx)->notes->push_back(L"refreshed the notes in games\\" + leaf);
    }, &c);
}
// a per-game cfg written by an earlier build held only enabled=1: expand it to a full copy so it can be tuned on its own
static void UpgradeMinimalGameCfgs(std::vector<wstring> &notes)
{
    struct C { std::vector<wstring> *notes; } c = { &notes };
    ForEachEntry(Join(g_home, L"games"), L"*.cfg", false, [](const wstring &f, void *ctx) {
        const std::string t = ReadFile(f);
        if (t.find("scale=") != std::string::npos) return;
        std::string keep; size_t p = 0;
        while (p < t.size()) { size_t e = t.find('\n', p); std::string l = t.substr(p, e == std::string::npos ? std::string::npos : e - p); p = e == std::string::npos ? t.size() : e + 1; if (!l.empty() && l[0] != '#' && l.find("enabled=") == std::string::npos) keep += l + "\n"; }
        const wstring stem = Leaf(f).substr(0, Leaf(f).size() - 4);
        if (WriteFile(f, FullGameCfg(stem, keep))) ((C *)ctx)->notes->push_back(L"expanded games\\" + Leaf(f) + L" to a full per-game settings file");
    }, &c);
}

// ---------------------------------------------------------------- enable / disable
static bool Enable(Game &g, wstring &msg)
{
    const wstring cfg = GameCfg(g), off = cfg + L".off";
    if (!g.openxr && g.openvr)
    {
        const wstring cur = Join(g.pluginDir, L"openvr_api.dll"), orig = Join(g.pluginDir, L"openvr_api.orig.dll"), master = Join(g_home, L"openvr_api.dll");
        if (!Exists(master)) { msg = L"the OpenVR proxy (openvr_api.dll) is missing from " + g_home; return false; }
        if (!Exists(orig) && !MoveFileW(cur.c_str(), orig.c_str())) { msg = L"could not rename the game's openvr_api.dll (is the game running?)"; return false; }
        if (!CopyFileW(master.c_str(), cur.c_str(), FALSE)) { msg = L"could not copy the proxy into " + g.pluginDir; return false; }
    }
    if (Exists(off) && !Exists(cfg)) MoveFileW(off.c_str(), cfg.c_str());
    if (!Exists(cfg)) WriteFile(cfg, FullGameCfg(g.stem, ""));
    wstring note; EnsureModel(note);
    msg = g.openxr ? L"enabled (OpenXR layer)" : L"proxy installed into " + g.pluginDir; if (!note.empty()) msg += L"; " + note;
    Detect(g);
    return true;
}
static bool Disable(Game &g, wstring &msg)
{
    const wstring cfg = GameCfg(g), off = cfg + L".off";
    if (!g.openxr && g.openvr)
    {
        const wstring cur = Join(g.pluginDir, L"openvr_api.dll"), orig = Join(g.pluginDir, L"openvr_api.orig.dll");
        if (Exists(orig)) { DeleteFileW(cur.c_str()); if (!MoveFileW(orig.c_str(), cur.c_str())) { msg = L"could not restore the game's openvr_api.dll (is the game running?)"; return false; } }
    }
    if (Exists(cfg)) { DeleteFileW(off.c_str()); MoveFileW(cfg.c_str(), off.c_str()); }
    msg = L"disabled (settings kept as " + Leaf(off) + L")";
    Detect(g);
    return true;
}

// ---------------------------------------------------------------- GUI
enum { ID_LIST = 100, ID_ENABLE, ID_DISABLE, ID_SETTINGS, ID_GLOBAL, ID_LOGS, ID_ADD, ID_RESCAN, ID_HEAD, ID_STATUS };
static HWND g_wnd, g_list, g_head, g_statusBar;
static void SetStatus(const wstring &s) { SetWindowTextW(g_statusBar, s.c_str()); }
static void FillList()
{
    ListView_DeleteAllItems(g_list);
    for (size_t i = 0; i < g_games.size(); ++i)
    {
        const Game &g = g_games[i];
        LVITEMW it = {}; it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = (int)i; it.pszText = (LPWSTR)g.name.c_str(); it.lParam = (LPARAM)i;
        ListView_InsertItem(g_list, &it);
        // the macro sends the message in a separate statement, so the strings must outlive the temporaries
        const wstring api = g.api(), st = g.status();
        ListView_SetItemText(g_list, (int)i, 1, (LPWSTR)api.c_str());
        ListView_SetItemText(g_list, (int)i, 2, (LPWSTR)st.c_str());
        ListView_SetItemText(g_list, (int)i, 3, (LPWSTR)g.stem.c_str());
        ListView_SetItemText(g_list, (int)i, 4, (LPWSTR)g.dir.c_str());
    }
    g_headline = L"Central install: " + g_home + L"   |   OpenXR layer " + (Registered() ? L"registered for " : L"NOT registered for ") + _wgetenv(L"USERNAME") +
                 (Exists(Join(g_home, L"nvngx_dlssnr.dll")) ? L"   |   model DLL present" : L"   |   model DLL MISSING (enable a game that has it, e.g. BONELAB)");
    SetWindowTextW(g_head, g_headline.c_str());
}
static int Selected() { return ListView_GetNextItem(g_list, -1, LVNI_SELECTED); }
static void Rescan() { SetStatus(L"scanning Steam libraries..."); ScanGames(); FillList(); SetStatus(std::to_wstring(g_games.size()) + L" VR games found. Select one and press Enable or Disable; double-click for its settings."); }
static void Layout(HWND w)
{
    RECT r; GetClientRect(w, &r); const int W = r.right, H = r.bottom;
    MoveWindow(g_head, 10, 8, W - 20, 36, TRUE);
    MoveWindow(g_list, 10, 48, W - 20, H - 48 - 44 - 30, TRUE);
    const int y = H - 30 - 36; int x = 10;
    for (int id : { ID_ENABLE, ID_DISABLE, ID_SETTINGS, ID_GLOBAL, ID_LOGS, ID_ADD, ID_RESCAN }) { MoveWindow(GetDlgItem(w, id), x, y, 108, 28, TRUE); x += 114; }
    MoveWindow(g_statusBar, 10, H - 26, W - 20, 22, TRUE);
}
static void OpenInNotepad(const wstring &p) { ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL); }
static LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m)
    {
    case WM_CREATE:
    {
        g_head = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, w, (HMENU)ID_HEAD, nullptr, nullptr);
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0, 0, 0, w, (HMENU)ID_LIST, nullptr, nullptr);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        const struct { const wchar_t *t; int wdt; } cols[] = { { L"Game", 230 }, { L"API", 120 }, { L"Bridge", 150 }, { L"Exe", 200 }, { L"Folder", 420 } };
        for (int i = 0; i < 5; ++i) { LVCOLUMNW c = {}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)cols[i].t; c.cx = cols[i].wdt; ListView_InsertColumn(g_list, i, &c); }
        const struct { int id; const wchar_t *t; } btns[] = { { ID_ENABLE, L"Enable" }, { ID_DISABLE, L"Disable" }, { ID_SETTINGS, L"Game settings" }, { ID_GLOBAL, L"Global settings" }, { ID_LOGS, L"Logs folder" }, { ID_ADD, L"Add folder..." }, { ID_RESCAN, L"Rescan" } };
        for (auto &b : btns) CreateWindowW(L"BUTTON", b.t, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, w, (HMENU)(INT_PTR)b.id, nullptr, nullptr);
        g_statusBar = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, w, (HMENU)ID_STATUS, nullptr, nullptr);
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        EnumChildWindows(w, [](HWND c, LPARAM f) -> BOOL { SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }, (LPARAM)f);
        return 0;
    }
    case WM_SIZE: Layout(w); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO *)lp)->ptMinTrackSize = { 900, 400 }; return 0;
    case WM_NOTIFY:
        if (((LPNMHDR)lp)->idFrom == ID_LIST && ((LPNMHDR)lp)->code == NM_DBLCLK) SendMessageW(w, WM_COMMAND, ID_SETTINGS, 0);
        return 0;
    case WM_COMMAND:
    {
        const int id = LOWORD(wp), sel = Selected();
        Game *g = sel >= 0 && sel < (int)g_games.size() ? &g_games[sel] : nullptr;
        wstring msg;
        switch (id)
        {
        case ID_ENABLE: if (!g) { SetStatus(L"select a game first"); break; } Enable(*g, msg); FillList(); ListView_SetItemState(g_list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); SetStatus(g->name + L": " + msg); break;
        case ID_DISABLE: if (!g) { SetStatus(L"select a game first"); break; } Disable(*g, msg); FillList(); ListView_SetItemState(g_list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); SetStatus(g->name + L": " + msg); break;
        case ID_SETTINGS: if (!g) { SetStatus(L"select a game first"); break; } if (!Exists(GameCfg(*g))) { SetStatus(g->name + L" is not enabled: enable it first (that creates its own complete settings file)"); break; } OpenInNotepad(GameCfg(*g)); SetStatus(L"editing " + GameCfg(*g) + L" (this game only; saved changes apply live in-game)"); break;
        case ID_GLOBAL: OpenInNotepad(Join(g_home, L"nr-bridge.cfg")); SetStatus(L"editing the defaults for NEWLY enabled games (each enabled game has its own complete cfg: use Game settings)"); break;
        case ID_LOGS: ShellExecuteW(nullptr, L"open", Join(g_home, L"logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_ADD:
        {
            BROWSEINFOW bi = {}; bi.hwndOwner = w; bi.lpszTitle = L"Pick a game folder (the one holding the game's exe)"; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            if (PIDLIST_ABSOLUTE p = SHBrowseForFolderW(&bi)) { wchar_t path[MAX_PATH] = {}; SHGetPathFromIDListW(p, path); CoTaskMemFree(p); if (path[0]) { std::string t = ReadFile(FoldersFile()); t += Narrow(path) + "\r\n"; WriteFile(FoldersFile(), t); Rescan(); } }
            break;
        }
        case ID_RESCAN: Rescan(); break;
        }
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

// ---------------------------------------------------------------- command line
// GUI subsystem: use the inherited stdout when it is redirected (scripts, tests), else attach to the parent console
static void OpenStdout()
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) { const int fd = _open_osfhandle((intptr_t)h, _O_TEXT); if (fd >= 0) { _dup2(fd, _fileno(stdout)); return; } }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) { FILE *fo = nullptr; freopen_s(&fo, "CONOUT$", "w", stdout); }
}
static void Print(const wstring &s) { const std::string n = Narrow(s); fwrite(n.data(), 1, n.size(), stdout); fflush(stdout); }
static wstring Pad(wstring s, size_t w) { if (s.size() > w) s = s.substr(0, w); while (s.size() < w) s += L' '; return s; }
static int Cli(int argc, wchar_t **argv)
{
    OpenStdout();
    const wstring cmd = argv[1];
    std::vector<wstring> notes;
    if (cmd == L"--home") { Print(g_home + L"\n"); return 0; }
    if (cmd == L"--sync") { const bool ok = SyncHome(notes); ScanGames(); RefreshProxies(notes); RefreshGameNotes(notes); for (auto &n : notes) Print(L"  " + n + L"\n"); Print(L"home " + g_home + L", layer " + (Registered() ? L"registered" : L"NOT registered") + L"\n"); return ok ? 0 : 1; }
    ScanGames();
    if (cmd == L"--notes") { RefreshGameNotes(notes); for (auto &n : notes) Print(L"  " + n + L"\n"); Print(notes.empty() ? L"every per-game cfg already carries the current notes\n" : L"done\n"); return 0; }
    if (cmd == L"--scan")
    {
        Print(L"home: " + g_home + L" (layer " + (Registered() ? L"registered" : L"not registered") + L")\n" + Pad(L"game", 34) + Pad(L"api", 18) + Pad(L"bridge", 22) + Pad(L"exe", 32) + L"folder\n");
        for (const Game &g : g_games) Print(Pad(g.name, 34) + Pad(g.api(), 18) + Pad(g.status(), 22) + Pad(g.stem, 32) + g.dir + L"\n");
        return 0;
    }
    if ((cmd == L"--enable" || cmd == L"--disable") && argc > 2)
    {
        for (Game &g : g_games) if (Lower(g.stem) == Lower(argv[2]) || Lower(g.name) == Lower(argv[2]))
        {
            wstring msg; const bool ok = cmd == L"--enable" ? Enable(g, msg) : Disable(g, msg);
            Print(g.name + L": " + msg + L"\n"); return ok ? 0 : 1;
        }
        Print(wstring(L"no VR game with exe or name '") + argv[2] + L"' (see --scan)\n"); return 2;
    }
    Print(L"nr-bridge-setup " SETUP_VERSION L": --scan | --sync | --enable <exe stem or name> | --disable <exe stem or name> | --notes | --home\n");
    return 2;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    wchar_t p[MAX_PATH] = {}; GetModuleFileNameW(nullptr, p, MAX_PATH); g_setupDir = Parent(p);
    wchar_t env[MAX_PATH] = {}; const DWORD n = GetEnvironmentVariableW(L"NR_BRIDGE_HOME", env, MAX_PATH);
    g_home = (n > 0 && n < MAX_PATH) ? wstring(env) : HomeDir();
    int argc = 0; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) return Cli(argc, argv);

    CoInitialize(nullptr);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES }; InitCommonControlsEx(&icc);
    std::vector<wstring> notes; SyncHome(notes);
    WNDCLASSW wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"NRBridgeSetup"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    g_wnd = CreateWindowW(wc.lpszClassName, L"nr-bridge setup " SETUP_VERSION L"  -  DLSS 5 Neural Rendering for VR games (LowPopLabs)", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 560, nullptr, nullptr, inst, nullptr);
    ShowWindow(g_wnd, show);
    Rescan();
    RefreshProxies(notes); RefreshGameNotes(notes); if (!notes.empty()) FillList();
    if (!notes.empty()) { wstring s; for (auto &x : notes) s += x + L"\n"; MessageBoxW(g_wnd, s.c_str(), L"nr-bridge setup: first run", MB_OK | MB_ICONINFORMATION); }
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0)) { if (!IsDialogMessageW(g_wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    CoUninitialize();
    return 0;
}

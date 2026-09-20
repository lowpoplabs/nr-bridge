# nr-bridge

An OpenXR API layer (plus an OpenVR front end) that puts DLSS 5 Neural Rendering on the **headset** image of VR games:
BONELAB and BONEWORKS so far. Author: **LowPopLabs**. Layer name `XR_APILAYER_LOWPOPLABS_nr_bridge`.

Renamed from `bonelab-nr-bridge` on 2026-09-12: the layer, manifest, cfg and forwarder are now `nr-bridge.dll`,
`nr-bridge.json`, `nr-bridge.cfg` and `nvngx.dll_nr_bridge.dll`. Starting the installer once upgrades an existing home
(cfg renamed with its content kept, old files removed, old layer registration replaced); `install-bridge.cmd` does the
same for a standalone folder, and the DLLs themselves read a leftover `bonelab-nr-bridge.cfg` or old forwarder if they
find one, so nothing falls back to the built-in defaults during the switch.

Two engines, chosen by `mode=` in `nr-bridge.cfg`:

| mode | what happens at every `xrEndFrame` | needs |
|---|---|---|
| **direct** (default) | the layer copies the eye pair to its own D3D12 device, builds motion vectors from the head rotation since the previous frame, runs the Neural Rendering model itself (NGX feature 18) at a configurable work resolution, transfers the model's edit back onto the full-resolution frame, copies it into the eye images | `nvngx_dlssnr.dll` (see Prerequisites). **No ReShade, no RenoDX.** |
| reshade | the eye pair is presented through a hidden DXGI swap chain on a second D3D11 device so ReShade + the RenoDX add-on treat it like a game window | ReShade `dxgi.dll` + `renodx-dlss.addon64` in the game folder |

The reshade engine is what was measured at 22 Hz: the add-on evaluated the mirror *and* the eye pair, with dummy motion data. Direct mode exists to fix both: one evaluation, at a chosen size, with real rotation vectors, and the desktop mirror left alone.

<!-- lpl:links -->
**[Download v0.3.0](https://github.com/lowpoplabs/nr-bridge/releases/latest)** · **[Changelog](CHANGELOG.md)** · **[Ko-fi](https://ko-fi.com/lowpoplabs)**
<!-- /lpl:links -->

## How direct mode gets its speed and its sharpness

- `scale=0.5` runs the model on a half-size copy of the eye pair (about the pixel count of the 1080p mirror). With `residual=1` the layer keeps the original full-resolution frame and adds only the *difference* the model made, upsampled, so fine detail is not blurred by the downscale. Raise `scale` if frame time allows.
- `mv=1` feeds the model per-pixel motion vectors computed from each eye's pose and field of view, current versus previous frame. Head rotation is exact; head translation is approximated as zero (mild ghosting on very near objects). This is what the model needs to stop smearing under head movement. `mv=0` reproduces the old behaviour for comparison.
- `fovea` is elliptical by default with a wide smoothstep fade (`feather`, in pixels). `outer_scale` adds one cheap pass
  over the whole eye pair at a low scale so everything outside the fovea gets the same lighting and tone; the fovea edge
  then only marks where the fine detail stops, which the eye does not pick out. Two tiers, blended in the resolve.
  With `outer_pack=1` (default) the outer eye pair is packed to the right of the fovea crops in the same work
  texture, so the first pass is one model evaluate over the atlas instead of two (each evaluate was measured to carry
  about 1.5 ms of fixed cost on the 5060 Ti); the passes after it run on the fovea block alone at its own size, so the
  outer strip is processed exactly once as before. `outer_pack=0` keeps the separate outer instance for A/B.
- `passes=2` runs the model twice, the second pass on the first pass's output, with a separate model instance per pass
  (that is how the RenoDX add-on's multi-pass works). This is what deepens face and skin detail; the cost multiplies.
- **Live reload:** the cfg is watched while the game runs. Save it and the change applies within a second, or press F9.
  Everything except `mode`, `ngx`, `app_match` and `any_app` can be changed live; geometry changes rebuild the textures,
  tuning changes recreate the model instances, motion settings apply immediately.
- NGX path: the driver's own NGX core is asked first; if the driver refuses the feature (`OutOfDate`), the model DLL is driven directly through `nvngx.dll_nr_bridge.dll`, a tiny forwarder named to satisfy the model's caller check. Both attempts and their results are logged.

## Prerequisites

- **The DLSS 5 Neural Rendering model, `nvngx_dlssnr.dll`** (NVIDIA DLSS 310.8 / Streamline 2.13, about 158 MB). It is
  **not part of any nr-bridge release and must never be added to one**: the file is NVIDIA's, and the package is
  distributed without it. Get it from the RenoDX Discord (the same place the RenoDX DLSS add-on comes from). Where to put it:
  - **With the installer:** `%LOCALAPPDATA%\nr-bridge\nvngx_dlssnr.dll` (the central home; **Logs folder** in the
    installer opens next to it). Dropping it next to `nr-bridge-setup.exe` before the first run, or leaving it in a game
    folder that you then enable, also works: the installer copies it into the home and reports "model DLL MISSING" in its
    headline until one is there.
  - **Standalone `install-bridge.cmd`:** the game folder (next to the game exe).
  - **Standalone `install-openvr.cmd`:** the game's plugin folder next to the proxy, or the game folder.
  The engine looks in the game folder first, then in its own folder (the home, or the folder the DLL runs from).
  Without the model the log shows `direct: snippet ...\nvngx_dlssnr.dll load=0` and frames pass through untouched.
- **An NVIDIA GPU and driver that run DLSS 5 Neural Rendering.** Verified on an RTX 5060 Ti with driver 591.86; the
  model refuses a Turing RTX 2070 Super (`FeatureNotSupported`). The driver's own NGX core may still refuse feature 18
  (`OutOfDate`); the bridge then drives the model DLL directly, which is the path that works today.
- **The headset runtime the game uses:** an OpenXR runtime (Meta Link or SteamVR) for OpenXR games, SteamVR for
  OpenVR games. 64-bit games only.
- **No admin rights:** run the installer or the scripts as the account that plays; everything is per-user (HKCU).

## Files (dist\)

| File | Purpose |
|---|---|
| `nr-bridge.dll` | the layer (both engines) |
| `nvngx.dll_nr_bridge.dll` | forwarder for the model DLL (direct mode) |
| `nr-bridge.json` | OpenXR layer manifest |
| `nr-bridge.cfg` | settings; an existing cfg is not overwritten on reinstall |
| `install-bridge.cmd` | copies files to `<game>\nr-bridge\`, registers the layer for the current user (HKCU, no admin) |
| `uninstall-bridge.cmd` | removes registration and folder |
| `disable-reshade.cmd` / `enable-reshade.cmd` | park / restore the ReShade proxy (`dxgi.dll`, `d3d11.dll` or `d3d12.dll`) in the game folder; the same thing as the installer's Monitor off / Monitor on |
| `openvr\openvr_api.dll` + `install-openvr.cmd` / `uninstall-openvr.cmd` | the OpenVR front end for SteamVR-native games (see below) |

## Installer (nr-bridge-setup.exe) and the central layout

`dist\nr-bridge-setup.exe` is the ReShade-style front door. Run it as the account that plays (no admin). It:

- creates the central home `%LOCALAPPDATA%\nr-bridge\` with the layer DLL + manifest (registered ONCE for the user),
  the forwarder, the model DLL, the global `nr-bridge.cfg`, `games\<exe>.cfg` per enabled game and `logs\`;
- removes any old per-game `<game>\nr-bridge\` install it finds registered, carrying its cfg over as that game's
  `games\<exe>.cfg` (the first run on the gaming PC migrates the current BONELAB install this way);
- scans every Steam library (plus folders added by hand) and lists the VR games with their API: **OpenXR** (Unity
  boot.config names UnityOpenXR, or an `openxr_loader.dll` ships with the game) or **OpenVR** (`openvr_api.dll` in the
  plugin folder). Non-VR games that merely ship an OpenXR loader (some UE titles) can appear; enabling them is harmless.
- **Headset on** for an OpenXR game = create `games\<exe>.cfg` (its presence activates the layer for that exe). For an
  OpenVR game = rename its `openvr_api.dll` to `openvr_api.orig.dll`, drop the proxy in, create the cfg. **Headset off**
  reverses both and keeps the settings as `<exe>.cfg.off`. Every per-game cfg is a **complete, independent copy of all
  settings**, so tuning one game never touches another; the global cfg only seeds newly enabled games. **Game settings**
  opens that file (live), **Global settings** the seed, **Logs folder** the central logs (`nr-bridge-<exe>.log`). A
  per-game cfg written by an earlier build (only `enabled=1`) is expanded to a full copy the next time the installer runs.
- **Monitor on / Monitor off** is the second, independent switch per game (see [Headset or monitor](#headset-or-monitor-the-two-switches-per-game)):
  it restores or parks the ReShade proxy DLL in the game folder (`dxgi.dll`, `d3d11.dll` or `d3d12.dll`, parked as
  `<name>.reshade-off`), which with the RenoDX add-on is what runs DLSS 5 on the **desktop window** (DirectX 11/12).
  The **Monitor (ReShade)** column shows `on (dxgi.dll)`, `off (parked)` or `no ReShade` (the installer never installs
  ReShade itself; only a DLL that really is ReShade is recognised, so DXVK or Special K proxies are left alone).
- Every per-game cfg starts with a **notes block**: what the file is, the game's exe, API, folder and log path, and what
  was learned tuning that game (BONELAB and BONEWORKS so far, with their measured settings and frame rates; generic
  OpenXR / OpenVR / Unity guidance for a game nobody has tuned yet). The installer rewrites that block on every run
  (or with `--notes`) and never touches the settings under it, so a new package brings new notes without losing tuning.
  The global cfg's own comments are game-neutral; it is the seed, not the place to tune.
- Every run of the installer (or `--sync`) also copies an updated OpenVR proxy into every game that has it installed,
  so a new package only needs the installer started once.
- **BONEWORKS note:** its in-game *Adaptive Resolution* option shrinks the eye rect every frame to hold the frame rate;
  with the model's cost added it settles at a much lower game resolution, which reads as blur, and an F10 A/B is then
  unfair (bypassing lets the game climb back to full resolution). Turn Adaptive Resolution off in the game's graphics
  options before judging image quality, or set `viewport_ref` to the "viewport N% of max" the log reports.
- Command line for scripts: `--scan`, `--sync`, `--enable <exe or name>`, `--disable <exe or name>` (headset),
  `--monitor-on <exe or name>`, `--monitor-off <exe or name>`, `--notes`, `--home`.
  `NR_BRIDGE_HOME` overrides the home folder (tests). Both DLLs find the home through
  `HKCU\Software\LowPopLabs\nr-bridge\Home`; without it they run standalone as before (cfg and log next to the DLL).

The `.cmd` scripts below still work for a standalone install without the installer.

## Headset or monitor: the two switches per game

DLSS 5 can run in two places in a VR game, and they are separate installs with separate costs:

| switch | what runs | where the picture is processed |
|---|---|---|
| **Headset** (this bridge) | the OpenXR layer or the OpenVR proxy | the eye pair the headset shows |
| **Monitor** (ReShade + the RenoDX DLSS add-on in the game folder) | ReShade's `dxgi.dll` / `d3d11.dll` / `d3d12.dll` proxy | the desktop mirror window (DirectX 11/12), which is what a recording captures |

The installer switches each one per game without touching the other, so a game can be set up for playing or for
recording in two clicks:

- **Playing in VR:** Headset on, Monitor off. With ReShade left on as well, the add-on spends a Neural Rendering pass
  on the mirror every frame on top of the bridge's, which capped BONELAB around 60 Hz.
- **Recording from the monitor:** Headset off, Monitor on. The headset shows the untouched game and the desktop window
  gets the full RenoDX treatment for the capture.
- Both on works, at the combined GPU cost; both off leaves the game as shipped.

Monitor off only renames the proxy DLL to `<name>.reshade-off` in the game folder; Monitor on renames it back. ReShade's
ini, presets and the add-on file stay where they are, so nothing has to be reconfigured. Switch while the game is
closed: a running game holds the DLL and the installer reports "is the game running?".

## Install on the gaming PC (as the account that plays, no admin)

1. Copy the `dist` folder over and run `install-bridge.cmd`.
2. Run `disable-reshade.cmd` (or press **Monitor off** in the installer) so ReShade and the RenoDX add-on stay out of the process while playing in VR (otherwise they still spend a Neural Rendering pass on the mirror window and cap the game around 60 Hz). `enable-reshade.cmd` / **Monitor on** brings them back for recording from the monitor.
3. Launch BONELAB. `nr-bridge\nr-bridge-BONELAB_Steam_Windows64.log` (every log is named after the game exe, so logs
   from different games never overwrite each other) should show, in order: `negotiate ok`, `xrCreateApiLayerInstance ... ACTIVE`, `direct: core Init_Ext ... Success`, `parameter block slots ... confirmed`, `xrCreateSession ... DIRECT ACTIVE`, `direct: eye pair WxH, NR work wxh`, then either `core CreateFeature(18) ... Success` or `core ... OutOfDate` followed by `snippet CreateFeature(18) ... Success`, then per-frame lines.
4. **F10** toggles the effect for A/B. Tune in the cfg and restart the game.

## OpenVR games (BONEWORKS and anything else launched through SteamVR)

BONEWORKS and most older Unity VR titles talk to SteamVR through OpenVR, not OpenXR, so the layer above never sees
them. `dist\openvr\` is a second front end for those: a drop-in `openvr_api.dll` that forwards every export to the
game's own DLL (renamed `openvr_api.orig.dll` next to it) and hooks `IVRCompositor::Submit`. The left eye's Submit is
held back; when the right eye arrives both eyes go through the same direct engine in one pass, in place, and are then
submitted. Same cfg keys, same log, same F9/F10 keys, same `nvngx_dlssnr.dll` requirement.

1. Copy `dist\openvr` to the gaming PC and run `install-openvr.cmd "E:\SteamLibrary\steamapps\common\BONEWORKS"`
   (any game folder works; it finds `*_Data\Plugins\openvr_api.dll`, renames it, copies the proxy, the forwarder, the
   cfg if none is there, and the model DLL from the BONELAB folder if it is not next to the script). 64-bit games only.
2. Launch the game through Steam. `<game>_Data\Plugins\nr-bridge-<exe name>.log` (`nr-bridge-BONEWORKS.log` for BONEWORKS) should show `VR_InitInternal2 ... err 0`,
   `hooked IVRCompositor_0xx vtable ... Submit slot 5`, `direct engine ACTIVE`, `IVRSystem_022 for poses/FOV -> <ptr>`,
   a `pair L ... R ...` line with the eye textures, and then the usual `direct:` lines.
3. `uninstall-openvr.cmd "<game folder>"` puts the game's own DLL back.

First BONEWORKS run (2026-09-11): hooked, engine active, model 5.3 ms at 1 pass. It showed two things the engine now
handles: the game uses a dynamic viewport (the submitted eye rect changes size almost every frame), so the engine sizes
its textures and the model from the texture's full size (`maxW`/`maxH` per view) and only the rect moves; and Unity
submits vertically flipped bounds, so the eyes are sampled and written back mirrored (`flipY`) and the model sees them
upright with correct vertical motion vectors. Both are covered by `test_direct.exe` (arguments 9 and 10) and the
`ngx=tophalf` debug model.

Notes: Submit slot numbers come from the interface version the game requests (5 up to IVRCompositor_028, 6 from 029
where `GetSubmitTexture` was inserted; `GetLastPoses` is 3 in every version). Texture arrays use slice = eye. Games
that submit MSAA textures or non-D3D11 textures are passed through with a log line. Head-rotation motion vectors come
from `GetLastPoses` and `IVRSystem_022`; if a runtime ever drops that interface version the model gets zero vectors.
Verified on the dev PC against a fake runtime (`test_openvr.exe`, 7 variants: vtable/FnTable, 022/029, array/sbs/two
textures, passthrough bit-exact and the half model) and for export forwarding against the real SteamVR stub; not yet
run against a headset.

## What the log tells you if it does not work

- `snippet CreateFeature(18) -> FeatureNotSupported`: the model refused the GPU or driver (this is what the dev PC's RTX 2070 Super gets). On the 5060 Ti with driver 591.86 this is not expected.
- `... -> PlatformError`: the model rejected the calling module; the forwarder DLL is missing or renamed.
- `EvaluateFeature -> ...`: a per-frame failure; the layer disables itself for the session and forwards frames untouched.
- `D3D12 device removed`: a GPU fault; send the log.

## Verified on the dev PC

Direct engine, passthrough (`ngx=none`): bit-exact round trip at scale 1.0 and 0.5 through the D3D12 pipeline, shared textures and cross-API fences. NGX: core init, capability block, slot discovery, forwarder load and model init all succeed; only feature creation is refused (Turing GPU). Reshade engine: bit-exact round trip, and ReShade + RenoDX adopted the hidden swap chain end to end.

## Build

`build.cmd` (VS 2022 Build Tools). Two things the repo does not carry:

- **NVIDIA NGX SDK headers** — `nvsdk_ngx_defs.h` and `nvsdk_ngx_params.h` from the
  [DLSS SDK](https://github.com/NVIDIA/DLSS) (`include/`) go into `src/ngx/`. They are NVIDIA's and
  are not redistributed here.
- **The OpenXR headers** in `src/openxr/` are the Khronos Group's, Apache-2.0 OR MIT, included as-is.

`package.cmd` builds and assembles `dist\`, then zips it to `releases
r-bridge-<version>.zip`. Tests: `build\test_direct.exe [scale] [ngx 0-4] [frames] [fovea] [passes] [reconfigure] [outer_scale] [outer_pack]`, `build\test_openvr.exe build\openvr-test [ngx 3|4] [array|sbs|two] [022|029|fn022]`, `build\test_openvr.exe real build\openvr-real`, `build\test_core.exe`, `build\test_layer.exe [runtime.json]`.

## Support

Provided as-is. Bug reports welcome via GitHub Issues. No Discord, no custom work, no promises on turnaround. If it saved you time or you enjoy it:

[![Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/lowpoplabs)

## License

[MIT](LICENSE). The OpenXR headers are © The Khronos Group, Apache-2.0 OR MIT. The DLSS 5 model and the NGX SDK headers are NVIDIA's and are not part of this repo or any release.

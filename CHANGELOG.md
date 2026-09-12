# Changelog

All notable changes to nr-bridge (DLSS 5 Neural Rendering for VR games, by LowPopLabs). Versions cover the
installer, the OpenXR layer and the OpenVR proxy together; they are released as one package.

## 0.2.1 - 2026-09-12

First public release on GitHub — no functional changes.

### Added
- The layer's first log line credits the author and tip jar (`negotiate ok: nr-bridge 0.2.1 by LowPopLabs - ko-fi.com/lowpoplabs ...`).
- MIT license, README Support section, and build notes on the two header sets that are not in the repo
  (NVIDIA NGX SDK headers, which carry a proprietary notice, and the Unity URP reference sources).

## 0.2.0 - 2026-09-12

### Changed
- **Renamed from `bonelab-nr-bridge` to `nr-bridge`.** The layer, manifest, cfg and forwarder are now `nr-bridge.dll`,
  `nr-bridge.json`, `nr-bridge.cfg` and `nvngx.dll_nr_bridge.dll`; the OpenXR layer name is
  `XR_APILAYER_LOWPOPLABS_nr_bridge` (implementation version 2) and the disable variable is `DISABLE_NR_BRIDGE`.
- **The global cfg is game-neutral.** It is the seed for newly enabled games only; its comments no longer quote
  BONELAB measurements or use BONEWORKS as an example, and the standalone-only `app_match` key is explained as such.
- **Every per-game cfg starts with a notes block** written by the installer: what the file is, the game's name, exe,
  API, engine, folder and log path, and what was learned tuning that game. BONELAB and BONEWORKS carry their measured
  settings and frame rates (Meta half-rate locks, the 72 Hz and 80 Hz budgets, the settled 2-pass config, Adaptive
  Resolution, the larger BONEWORKS eye pair); any other game gets generic OpenXR, OpenVR and Unity guidance. The
  installer rewrites the block on every run and never touches the settings under it. The seed's own title block is no
  longer copied into per-game files.
- The installer's first-run message now lists what it migrated, renamed or refreshed.

### Added
- `--notes` command line option: rewrite the notes block in every per-game cfg (also `.cfg.off`) without changing
  anything else.
- **Prerequisites** section in the README: where `nvngx_dlssnr.dll` must go (`%LOCALAPPDATA%\nr-bridge\` with the
  installer, the game folder for the standalone scripts), that it comes from the RenoDX Discord and is never part of a
  release, the GPU and driver requirement, and the runtime each game type needs.

### Migration (automatic)
- Starting the 0.2.0 installer once upgrades an existing home: the global cfg is renamed with its content kept, the old
  layer, manifest and forwarder are removed once the new ones are in place (a running game can delay that to the next
  start), the old layer registration is replaced, and every per-game cfg gets the notes block while keeping its values.
  Per-game cfgs are named after the game exe and are otherwise untouched.
- `install-bridge.cmd` does the same for a standalone folder; both uninstall scripts remove either name.
- The DLLs themselves read a leftover `bonelab-nr-bridge.cfg` (renaming it when they can) and load the old forwarder
  name if the new one is missing, so no install falls back to the built-in defaults during the switch.

### Fixed
- A stale manifest registration pointing into the home folder could have been mistaken for an old per-game install
  (the home's folder name is `nr-bridge` too) and deleted the home; the installer now refuses that folder.
- Per-game cfgs written by 0.1.0 carried the seed's "read once at game start" title, which was wrong since live reload;
  it is stripped when the notes block is refreshed.

## 0.1.0 - 2026-09-10 to 2026-09-11

First working package, released as `bonelab-nr-bridge`.

### Added
- OpenXR API layer with two engines: **reshade** (eye pair presented through a hidden swap chain so ReShade + the
  RenoDX add-on process it; measured at 22 Hz) and **direct** (the layer runs the Neural Rendering model itself on its
  own D3D12 device through NGX feature 18, driver core first, then the model DLL through a forwarder).
- Direct mode: work resolution (`scale`) with residual transfer onto the full-resolution frame, rotation-only motion
  vectors from the view poses, elliptical feathered `fovea`, an outer low-resolution tier (`outer_scale`) packed into
  one atlas evaluate (`outer_pack`), multi-pass (`passes`), model tuning keys matching the RenoDX panel, GPU timestamps
  and frame-interval reporting every 600 frames, F10 toggle for A/B.
- Live reload: the cfg is watched while the game runs; F9 forces a reload. Geometry changes rebuild textures, tuning
  changes recreate the model instances.
- **OpenVR front end** (`openvr_api.dll` proxy) for SteamVR-native games: hooks `IVRCompositor::Submit`, pairs the eyes,
  handles dynamic viewports (`viewport_ref`) and vertically flipped bounds. First run on BONEWORKS.
- **Installer** `nr-bridge-setup.exe` with the central layout in `%LOCALAPPDATA%\nr-bridge`: one layer registration per
  user, per-game cfgs (`games\<exe>.cfg`, presence enables the game, `.off` keeps settings), per-exe logs, Steam library
  scan with OpenXR/OpenVR detection, enable/disable per game, migration of the old per-game folder install, command
  line `--scan --sync --enable --disable --home`.
- Every per-game cfg is a complete, independent copy of all settings, so tuning one game never affects another.
- Standalone scripts: `install-bridge.cmd`, `uninstall-bridge.cmd`, `install-openvr.cmd`, `uninstall-openvr.cmd`,
  `disable-reshade.cmd`, `enable-reshade.cmd`.
- Dev-PC tests: `test_core`, `test_direct`, `test_openvr` (fake runtime, 7 variants), `test_layer`.

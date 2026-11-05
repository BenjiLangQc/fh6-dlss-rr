# FH6 DLSS Ray Reconstruction

A ReShade add-on that makes Forza Horizon 6 use **DLSS Ray Reconstruction** instead of DLSS Super Resolution, and
feeds it the game's **raw ray-traced GI** so RR does the denoising instead of the game's own GI denoiser.

Personal, offline-tested mod. FH6 is an online game: injecting code into it could be flagged. Play offline.

## Requirements
- NVIDIA RTX GPU, FH6 with **DLSS** on and **ray-traced GI** set to Medium or High.
- ReShade 6.x **with full add-on support**, installed for `forzahorizon6.exe` (DirectX 12).
- The game's own Streamline files (shipped with FH6), including `sl.dlss_d.dll` / `nvngx_dlssd.dll`.

## Install
1. Copy `build\mod\fh6rr.addon64` into the game folder (next to `forzahorizon6.exe`).
2. In the game folder's `ReShade.ini`, add:
   ```ini
   [ADDON]
   LoadFromDllMain=fh6rr.addon64
   ```
   This makes ReShade load the add-on at startup, before the game initializes Streamline. Without it the
   add-on cannot enable Ray Reconstruction. (The value is a file list, not `1`.)
3. Do **not** also install `fh6rr_recon.addon64` (the analysis tool) - only one of them at a time.

On first launch the add-on writes `fh6rr.ini` and `fh6rr.log` next to it.

## Use
| Key | Action |
|---|---|
| **F8** | **Whole mod on/off** (off = the game's own DLSS and GI denoiser). Key set by `toggle_key` |
| F7 | DLSS Ray Reconstruction on/off (off = the game's normal DLSS) |
| F6 | GI denoiser bypass on/off (off = the game's own GI denoiser, still upscaled by RR) |

Both start on. The GI bypass only acts while RR is actually running, so raw noisy GI never reaches the screen
without RR denoising it. If anything RR needs is missing, the add-on logs it and leaves the game's DLSS in place.

`fh6rr.ini`:
```ini
[fh6rr]
rr=1          ; start with RR on
gi_bypass=1   ; start with the GI denoiser bypass on
gi_scale=0    ; raw GI brightness: 0 = x1 (matches the game), 1 = xPI, 2 = x2, 3 = x0.5
hotkeys=1     ; F8/F7/F6 toggles
toggle_key=119 ; master on/off key as a Windows virtual-key code: 119 = F8, 120 = F9, 121 = F10
```

## How it works
1. Hooks Streamline's `slInit` and adds DLSS Ray Reconstruction (`kFeatureDLSS_RR`) to the game's feature list.
2. Each frame, instead of the game's DLSS-SR evaluate, it runs a compute pass that decodes FH6's G-buffer into RR
   guides (diffuse albedo, specular albedo, world normal + roughness), tags them, sets RR options (game's DLSS mode
   and output size, camera matrices from `slSetConstants`) and evaluates RR through Streamline.
3. GI bypass: skips the game's GI temporal pass and first spatial pass, and runs its own shader in place of the
   second spatial pass (same root signature and bindings, only the pipeline state is swapped). That shader writes the
   raw 1-ray GI into the format the game's GI resolve pass reads, so lighting receives noisy GI and RR cleans it.

Details of every decoded format: `notes/phase1_findings.md`, `notes/phase2_findings.md`.

## Build
Run `fetch_deps.bat` once (clones ReShade v6.7.3 headers and MinHook at the pinned commit into `external/`), then
run `build.bat` (VS 2022 Build Tools, Windows SDK 10.0.26100 for `dxc.exe`). Outputs:
- `build\mod\fh6rr.addon64` - the mod
- `build\recon\fh6rr_recon.addon64` - recon tool (F9 frame capture, F8 G-buffer debug views)

Third-party code in `external/`: ReShade headers (BSD-3), NVIDIA Streamline headers (MIT), MinHook (BSD-2).

## Known limits
- Tied to the current FH6 build: shader hashes (`ea12dd2b624b6962`, `6e9ffce615b836da`) and G-buffer layout can
  change with a game patch. If the hashes change, the GI bypass simply stays inactive (RR still works).
- Scalar specular (flagged materials) uses F0 = 0.04; RT6 colored F0 is used where the game uses it.

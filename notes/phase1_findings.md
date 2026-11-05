# Phase 1 findings (FH6, 4K output, DLSS Performance, RTGI on)

Source: `captures/run4/day_outdoor.txt` (bindings) + `captures/run1` (day/tunnel structure).
R-ids are per-capture; formats below are what matters.

## Stability notes
- ReShade `descriptor_tracking` (copy/update_descriptor_tables events) crashes FH6 ~20 s into gameplay. Keep `tracking=0`.
- A vectored exception handler makes FH6 crash at startup (guard page 0x80000001). Never register one.
- Never MinHook Streamline plugin functions by pointer: `slDLSSGetOptimalSettings` and `slDLSSGetState` share an address.
- Native `SetComputeRoot*` vtable hooks never fire; ReShade's `bind_descriptor_tables` event works and is cheap.

## Streamline / DLSS
- Game loads DLSS SR, DLSS-G, Reflex, NIS, PCL. **DLSS-RR (dlss_d) is NOT loaded** (`slIsFeatureLoaded(1001)` -> error 31).
- Input color: 3840x2160 R11G11B10F texture, extent 1920x1080 (same texture as G-buffer RT0).
- Output: 3840x2160 R11G11B10F. Depth: 1920x1080 R32G8X24 (reverse Z). MV: 1920x1080 RG16F, 2D, camera motion included, not jittered, mvecScale (0.5,-0.5).
- Exposure: not tagged (null). preExposure 1, HDR true.
- Constants: near 0.1, far 50000, FOV 0.8727 rad (50 deg), jitter present, matrices present.

## G-buffer (single MRT pass inside RenderOpaque)
| RT | format | role (guess) |
|----|--------|--------------|
| 0 | R11G11B10F 4K-tex @1080p | HDR lit color (DLSS input) |
| 1 | RGB10A2UN | normals (encoding TBD) |
| 2 | RGBA8U | material info (raygen calls it `g_screenMaterialInfo`) |
| 3 | R32U | packed |
| 4 | RGBA8_SRGB | albedo / base color |
| 5 | RGBA8UN | material params (roughness/metal?) |
| 6 | R32U | packed |
| 7 | R32U | packed |
| DS | R32G8X24 | depth/stencil (DLSS depth) |

## RT GI chain (`CPresentationView::RenderRTBufferEffects`)
Raygen `ae9ed9e5b2123cca` (names intact): writes `g_GIOutput` (u1,space20), irradiance cache
`g_IrradianceCacheKeys/Values` (4 MB / 16 MB buffers). Reads `g_screenDepth`, `g_screenMaterialInfo`,
`g_inputReflection0`, `g_BlueNoise`, `g_StencilBuffer`, sky/track spec cubes, F+ light data.

Order (all 240x135 groups = 1920x1080 / 8x8):
1. `f764c84509f70e17` -> RG32F `g_screenDepth` for raygen
2. `f8fedc319fb55af7` -> RGBA16F `g_inputReflection0`
3. **RAYS** `ae9ed9e5b2123cca` [1920x1080] -> RGBA16F noisy GI (`g_GIOutput`)
4. `ea12dd2b624b6962` temporal: noisy GI + history A (t26) + history B (t27) + R8U history length (t28) -> A', B', new R8U length
5. `6e9ffce615b836da` spatial #1 (ping)
6. `6e9ffce615b836da` spatial #2 (pong) -> becomes next frame's history A
7. `30f774e7ce6a460c` resolve -> RGBA16F **denoised GI** used by lighting/composite

The denoiser carries two RGBA16F signals (likely an SH / luma-chroma split), so skipping it needs the
encoding written by pass 4 to be understood before pass 7 can be fed raw GI.

## RR guide availability
- Albedo: G-buffer RT4 (sRGB) - likely usable directly.
- Normals: RT1 RGB10A2 - need to decode (octahedral?) into world/view normals.
- Roughness: probably RT5 - confirm from G-buffer pixel shader.
- Specular albedo: not stored; derive from metalness/F0 + roughness (env-BRDF approximation).
- Depth, MV: already tagged for DLSS.

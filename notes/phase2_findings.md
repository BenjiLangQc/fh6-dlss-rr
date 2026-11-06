# Phase 2 findings (shader analysis)

Disassembly: `captures/disasm/*.txt` (dxc -dumpbin of `captures/shaders/*.bin`).
Key shaders: lighting PS `3065277b859c6bbb_G` (FPlusPlusComposite variant), GI resolve `30f774e7ce6a460c_C`,
GI temporal `ea12dd2b624b6962_C`, SS-GI trace `f8fedc319fb55af7_C`, raygen `ae9ed9e5b2123cca_R`.

## G-buffer decoding (all confirmed in code)
The lighting PS reads the G-buffer through unbounded tables: `space38` = uint textures, `space37` = float textures.

| Lighting PS slot | Format | Content | Decode |
|---|---|---|---|
| uint[1] | R32U (MRT RT3) | **normal + smoothness** | bits 8..19 / 20..31 = oct x/y (12 bit, `v*2/4095-1`, oct decode, normalize); bits 0..7 = smoothness `s = v/255`, **roughness = 1 - s** |
| uint[2] | RGBA8U (MRT RT2, `g_screenMaterialInfo`) | material params + flags | x&127 /127, z&127 /127, z bit7, **w = flags**: bit0 => F0 = 0.04, bit5 => F0 = float[3].a, bit6 => use float[8] |
| float[3] | RGBA (MRT RT4, sRGB) | **diffuse albedo** rgb (clamped to 0.998); a = scalar F0 when flag bit5 | read through sRGB view = linear |
| float[8] | RGBA | extra color (emissive/sheen) when flag bit6 | |
| uint[9] | R32U (MRT RT6 or RT7) | **specular F0 rgb**, packed 11/11/10 floats | `f16((v<<4)&0x7FF0)`, `f16((v>>7)&0x7FF0)`, `f16((v>>17)&0x7FE0)`; used when flag bit0 == 0 |
| uint[10] | R32U (MRT RT7 or RT6) | 2nd normal (oct, 11/11 float-packed) + blend weight, or color when flags&65 | |
| uint[15] | ? | material id in .y (`(y & ~128) != 66` test) | |

Verified in game with the F8 debug view (phase 3): albedo (RT4), normals (RT3 oct), roughness
(1 - RT3 low byte) and material flags (RT2.w) all look right. **RT6 = specular F0 (uint[9])**,
**RT7 = second normal / blend (uint[10])**, zero on most surfaces (game then uses RT3 normal only).
RT6 holds F0 **only where flag bit0 == 0**; elsewhere it carries unrelated packed data.
Scalar-F0 path (flag bit5): RT4 alpha (car paint ~0.75, foliage ~0.4 dithered) and RT5 alpha
(~1 everywhere, dark in creases = occlusion) are both implausible as F0.
RT5 rgb = packed material params (G ~1 on most surfaces).

**Decision for RR guides:** F0 = (flags & 1) == 0 ? unpack(RT6) : 0.04 (flag-bit5 materials are
foliage/buildings/trim = dielectrics, 0.04 is physically right). Normal = RT3 oct, blended with RT7
second normal exactly like the game when (flags & 65) == 0. Roughness = 1 - RT3 low byte.
Diffuse albedo = RT4 rgb (sRGB view). Specular albedo = EnvBRDF approx(F0, roughness, NdotV).

Depth for GI shaders: Hi-Z `R32_TL mips=11`; view Z = `cb0[0].w / (d - cb0[0].z)`, d <= 0 = sky.

## GI signal chain
1. `f8fe` = **screen-space GI trace** (Hi-Z march, radiance from previous frame color pyramid 960x540 mips=10).
   Writes RGBA16F (rgb radiance, a hit distance); rgb = -1 where it misses.
2. RAYS raygen `RTBuffer_RayGeneration`: traces misses with DXR, writes `g_GIOutput` RGBA16F =
   **1 ray radiance (rgb) + hit distance (a)**; inserts into a world irradiance cache (hash grid).
   Ray direction is not stored.
3. `ea12` temporal + ray reuse over a 10x10 tile (groupshared `sh_RayDir`, `sh_Radiance_hitT`,
   `sh_EncodedNormalDepth`), projects into SH, history blend `w = L/(L+1)`, `L = min(maxHist+1, len)`;
   for young history it blends toward the wave mean (32 px) => clearing history is NOT raw.
   Per ray it also accumulates a **visibility term from hit distance** (0 when hitT < radius) -> A.w.
4. `6e9f` x2: spatial filter ping-pong.
5. `30f7` resolve: `Y = dot(n, B.xyz) + B.w`, `RGB = (Y+Co-Cg, Y+Cg, Y-Co-Cg)`, out.a = A.w.
   - **B = luma SH L1 (L1x, L1y, L1z, L0)**, **A = (Co, Cg, unused, AO)**.

## RR bypass plan (chosen)
Replace passes 3-4 with one compute pass of ours that writes, per pixel, into the textures pass 5 reads:
- `B = (0, 0, 0, k*Y)`, `A = (k*Co, k*Cg, 0, ao)` from the raw radiance of pass 2 (YCoCg of rgb),
  `ao = hitT > 0 && hitT < radius ? 0 : 1` (same as the game; radius from its cbuffer or a constant).
- Pass 5 evaluates at the pixel's own normal, so no ray direction is needed.
- `k` (radiance -> irradiance scale, 1 or pi) is calibrated in game with an A/B toggle against the game's
  own denoiser (ini value).
- Result: lighting receives **noisy 1 spp GI**; final color is noisy; DLSS-RR denoises + upscales.

## RR evaluation plan
- Game never loads the dlss_d Streamline plugin -> call **NGX directly** (`nvngx_dlssd.dll`, DLSS SDK
  `nvsdk_ngx_helpers_dlssd.h`). Intercept `slEvaluateFeature(kFeatureDLSS)` and skip it, evaluate RR on the
  same command list instead.
- Inputs: color = ScalingInputColor, output = ScalingOutputColor, depth, MV (+ jitter, mvec scale from
  `slSetConstants`), and guides written by our guide pass:
  - diffuse albedo = float[3].rgb
  - specular albedo = EnvBRDF(F0, roughness, NdotV) (Karis analytic approx), F0 from uint[9] / flags
  - normals (world, from oct) + roughness (1 - smoothness), packed NormalRoughness
- Exposure: not provided by game -> use auto-exposure flag or 1.0.

## Phase 3 status
- Step 1 (debug views, F8): done, all guides verified.
- Step 2 (RR on the game's image, F7): **working** (2026-11-05). Path: ReShade `[ADDON] LoadFromDllMain=fh6rr_recon.addon64`
  (a file list, not a bool) -> addon hooks `slInit` and appends kFeatureDLSS_RR (game passes features 0 4 3 2 1000,
  flags 0xc9) -> on F7 the DLSS-SR evaluate is replaced by guide pass (native list) + slDLSSDSetOptions +
  slSetTagForFrame(Albedo, SpecularAlbedo, NormalRoughness) + slEvaluateFeature(kFeatureDLSS_RR). 1920x1080 -> 3840x2160.
- Step 3 (GI denoiser bypass, F6): **working** (2026-11-05). ea12 + 6e9f #1 skipped (ReShade dispatch event
  returns true), 6e9f #2 runs `gi_raw.hlsl` via SetPipelineState with the game's root signature/tables; resolve
  30f7 untouched. Scale **x1** matches the game's brightness (so stored GI = raw radiance, cosine-sampled rays).
  F6 + F7 together gives a clean RR-denoised image.
- Gotcha: with LoadFromDllMain the addon outlives FH6's probe device; always follow the newest device.
- RR path runs inside a scoped SEH guard (no VEH!) that logs the step and disables RR on a fault.

## Standalone build (no ReShade) - working (2026-11-05)
Lessons, each one cost a broken build:
- Identify shaders by FNV over the **DXIL part only** (`gi::identity_hash`): FH6 embeds a different debug name
  (ILDN part + container digest) with and without ReShade; the program is identical.
  Temporal = 0x2065ab87ddffd305, spatial = 0x0f647000c451f382.
- Intercepting D3D12CreateDevice (inline or import table) or hooking the native device's functions breaks RTSS's
  overlay. Hook the device Streamline hands over (its wrapper, in sl.interposer.dll) instead.
- Streamline does not wrap command lists in FH6. Native D3D12Core Dispatch/SetPipelineState hooks only see the
  loading screen; gameplay lists use per-object heap vtables (calls end up in nvwgf2umx.dll). D3D12Core's
  OMSetRenderTargets code is shared with unrelated methods - never treat its `this` as a list for discovery.
- So the GI bypass swaps shaders **at pipeline creation**: temporal pass -> gi_raw.hlsl (RAW_REG=t20), spatial
  pass -> gi_copy.hlsl (copies only signal A; must never write u3, which is the raygen output R309 in run 1).
- The list the game hands Streamline is usually not unwrappable (heap vtable); record the RR guide pass on it
  directly. Before that fix RR silently fell back to DLSS-SR on almost every frame.

## Phase 3 build order
1. Debug views: a hotkey that blits each decoded guide (albedo, F0, roughness, normal) to screen to verify decoding
   and the RT6/RT7 question.
2. RR via NGX on top of the game's normal (denoised) image - proves the integration before touching GI.
3. GI bypass pass + A/B toggle + scale calibration.
4. Polish: resolution changes, alt-tab, menus/photo mode, disabling cleanly when RT GI is off.

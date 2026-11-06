// Replacement for FH6's GI spatial denoise pass #2 (6e9ffce615b836da), run with the GAME's root signature
// and descriptor tables (only the pipeline state is swapped), so registers must match that pass exactly:
//   t23, space36 : raw 1-ray GI (raygen g_GIOutput: rgb radiance, a = hit distance) - still raw because
//                  the temporal pass and spatial pass #1 are skipped
//   u2,  space36 : signal A (Co, Cg, unused, ao)   -> read by the resolve pass 30f774e7ce6a460c as t22
//   u3,  space36 : signal B (L1x, L1y, L1z, L0)    -> read by the resolve pass as t23
// Writing L1 = 0 and L0 = Y makes the resolve output exactly the raw per-pixel irradiance (no direction needed).

// Variants are compiled by build.bat: GI_SCALE_IDX 0..3 = x1, xPI, x2, x0.5
//                                  GI_AO_IDX    0..3 = off, soft 0.25 m, soft 1 m (matches the game in tests), hard 1.5 m
// The game's own temporal pass reads its AO radius and soft/hard mode from a constant buffer we cannot see
// from this pass, so the mode is user-selectable (F5 / fh6rr.ini gi_ao).
#ifndef GI_SCALE_IDX
#define GI_SCALE_IDX 0
#endif
#ifndef GI_AO_IDX
#define GI_AO_IDX 2
#endif
static const float k_scales[4] = { 1.0, 3.14159265, 2.0, 0.5 };
#define GI_SCALE k_scales[GI_SCALE_IDX]

// RAW_REG: t23 when replacing spatial pass #2 at dispatch time (ReShade build), t20 when this shader replaces the
// temporal pass at pipeline creation (standalone build: the temporal pass reads the raw GI from t20).
#ifndef RAW_REG
#define RAW_REG t23
#endif
Texture2D<float4>   g_raw  : register(RAW_REG, space36);
RWTexture2D<float4> g_outA : register(u2, space36);
RWTexture2D<float4> g_outB : register(u3, space36);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
	uint w, h;
	g_outA.GetDimensions(w, h);
	if (id.x >= w || id.y >= h)
		return;

	const float4 raw = g_raw[id];
	const float3 L = max(raw.rgb, 0.0) * GI_SCALE;
	// Visibility from the ray hit distance (hitT <= 0 means the ray missed / no hit)
#if GI_AO_IDX == 0
	const float ao = 1.0;
#elif GI_AO_IDX == 1
	const float ao = raw.a > 0.0 ? saturate(raw.a / 0.25) : 1.0;
#elif GI_AO_IDX == 2
	const float ao = raw.a > 0.0 ? saturate(raw.a / 1.0) : 1.0;
#else
	const float ao = (raw.a > 0.0 && raw.a < 1.5) ? 0.0 : 1.0;
#endif

	const float Y  = 0.25 * L.r + 0.5 * L.g + 0.25 * L.b;
	const float Co = 0.5 * (L.r - L.b);
	const float Cg = -0.25 * L.r + 0.5 * L.g - 0.25 * L.b;

	g_outA[id] = float4(Co, Cg, 0.0, ao);
	g_outB[id] = float4(0.0, 0.0, 0.0, Y);
}

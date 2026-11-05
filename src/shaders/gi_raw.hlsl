// Replacement for FH6's GI spatial denoise pass #2 (6e9ffce615b836da), run with the GAME's root signature
// and descriptor tables (only the pipeline state is swapped), so registers must match that pass exactly:
//   t23, space36 : raw 1-ray GI (raygen g_GIOutput: rgb radiance, a = hit distance) - still raw because
//                  the temporal pass and spatial pass #1 are skipped
//   u2,  space36 : signal A (Co, Cg, unused, ao)   -> read by the resolve pass 30f774e7ce6a460c as t22
//   u3,  space36 : signal B (L1x, L1y, L1z, L0)    -> read by the resolve pass as t23
// Writing L1 = 0 and L0 = Y makes the resolve output exactly the raw per-pixel irradiance (no direction needed).

#ifndef GI_SCALE
#define GI_SCALE 1.0
#endif
#ifndef AO_RADIUS
#define AO_RADIUS 1.5
#endif

Texture2D<float4>   g_raw  : register(t23, space36);
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
	// Same visibility rule the game's temporal pass applies per ray: a short hit occludes
	const float ao = (raw.a > 0.0 && raw.a < AO_RADIUS) ? 0.0 : 1.0;

	const float Y  = 0.25 * L.r + 0.5 * L.g + 0.25 * L.b;
	const float Co = 0.5 * (L.r - L.b);
	const float Cg = -0.25 * L.r + 0.5 * L.g - 0.25 * L.b;

	g_outA[id] = float4(Co, Cg, 0.0, ao);
	g_outB[id] = float4(0.0, 0.0, 0.0, Y);
}

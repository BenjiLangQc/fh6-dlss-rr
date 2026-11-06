// Standalone build: replaces FH6's GI spatial denoise pass (6e9ffce615b836da program) at pipeline creation.
// The game runs that pipeline twice with different bindings:
//   run 1: t22 = A (written by gi_raw.hlsl in the temporal slot) -> u2 = intermediate, u3 = R309 (raw ray output!)
//   run 2: t22 = intermediate                                    -> u2 = A for the resolve, u3 = B for the resolve
// Only signal A is passed along. u3 is never written: in run 1 that keeps the ray tracer's own output buffer
// untouched (writing it fed last frame's data back into the raw GI and caused lighting trails), and in run 2 that
// buffer already holds B, written by gi_raw.hlsl in the temporal slot.

Texture2D<float4>   g_inA  : register(t22, space36);
RWTexture2D<float4> g_outA : register(u2, space36);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
	uint w, h;
	g_outA.GetDimensions(w, h);
	if (id.x >= w || id.y >= h)
		return;
	g_outA[id] = g_inA[id];
}

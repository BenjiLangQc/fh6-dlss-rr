// Writes the DLSS Ray Reconstruction guide buffers from FH6's G-buffer.
// Decoding is the game's own (verified with the F8 debug view, see notes/phase2_findings.md).

cbuffer Params : register(b0)
{
	uint   g_width;
	uint   g_height;
	float  g_tan_x;   // 1 / proj[0][0]
	float  g_tan_y;   // 1 / proj[1][1]
	float3 g_right;   // camera basis in world space (from slSetConstants)
	uint   g_options; // rr.h k_guide_*: bits 0-1 F0 mode (0 = 0.04, 1 = game's rule, 2 = old RT6), 4 = diffuse *= RT5.a, 8 = no (1 - F0) on diffuse
	float3 g_up;
	float  g_pad1;
	float3 g_fwd;
	float  g_pad2;
};

Texture2D<uint>   g_normalSmooth : register(t0); // RT3: oct normal (bits 8..31) + smoothness (bits 0..7)
Texture2D<float4> g_albedo       : register(t1); // RT4 through sRGB view -> linear
Texture2D<uint>   g_rt6          : register(t2); // RT6: specular F0 (packed 11/11/10) when flag bit0 == 0
Texture2D<uint>   g_rt7          : register(t3); // RT7: second normal + weight (packed 11/11/10)
Texture2D<uint4>  g_material     : register(t4); // RT2: w = flags
Texture2D<float4> g_rt5          : register(t5); // RT5: rgb = packed material params, a = texture occlusion (dark in creases)

RWTexture2D<float4> g_out_diffuse  : register(u0); // diffuse albedo
RWTexture2D<float4> g_out_specular : register(u1); // specular albedo (pre-integrated)
RWTexture2D<float4> g_out_normal   : register(u2); // world normal xyz + roughness w (packed mode)

float3 oct_decode(float2 o)
{
	float3 n = float3(o, 1.0 - abs(o.x) - abs(o.y));
	float t = saturate(-n.z);
	n.x += n.x >= 0.0 ? -t : t;
	n.y += n.y >= 0.0 ? -t : t;
	return normalize(n);
}

float3 unpack_111110(uint v)
{
	return float3(f16tof32((v << 4) & 0x7FF0), f16tof32((v >> 7) & 0x7FF0), f16tof32((v >> 17) & 0x7FE0));
}

// Karis' analytic approximation of the pre-integrated split-sum environment BRDF
float3 env_brdf_approx(float3 f0, float roughness, float n_dot_v)
{
	const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
	const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
	const float4 r = roughness * c0 + c1;
	const float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
	const float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
	return f0 * ab.x + ab.y;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
	if (id.x >= g_width || id.y >= g_height)
		return;

	const uint ns = g_normalSmooth[id];
	const uint4 material = g_material[id];
	const uint flags = material.w;

	// Normal, blended with the second normal exactly like the game's lighting shader
	float3 n = oct_decode(float2((ns >> 8) & 4095, (ns >> 20) & 4095) * (2.0 / 4095.0) - 1.0);
	if ((flags & 65) == 0)
	{
		const float3 v7 = unpack_111110(g_rt7[id]);
		const float3 n2 = oct_decode(v7.xy * 2.0 - 1.0);
		n = normalize(lerp(n, n2, v7.z));
	}
	const float roughness = 1.0 - (ns & 255) / 255.0;

	// Specular F0. Default (mode 0): dielectric 0.04 everywhere. Tested in game, this gives RR the cleanest result:
	// any per-material F0 jump in the guides shows up after RR as a seam along material borders (roads, the
	// cockpit's carbon panel), even when it matches the game's own F0.
	// Mode 1 = the game's own rule (lighting shader 3065277b859c6bbb):
	//   flag bit5 set -> scalar F0 = RT4 alpha; bit5 clear, bit0 clear -> RT6 (packed 11/11/10); else 0.04.
	// Mode 2 = the original decode (RT6 wherever bit0 is clear, ignoring bit5).
	// A high per-material F0 also makes the diffuse guide (base * (1 - F0)) very dark; RR divides by it, so noise is
	// amplified into dark blotches (seen on the black rear panels). Another reason for constant 0.04.
	const float4 albedo = g_albedo[id];
	const uint f0_mode = g_options & 3;
	float3 f0 = 0.04;
	if (f0_mode == 1)
	{
		if (flags & 32)
			f0 = saturate(albedo.a);
		else if ((flags & 1) == 0)
			f0 = saturate(unpack_111110(g_rt6[id]));
	}
	else if (f0_mode == 2 && (flags & 1) == 0)
		f0 = saturate(unpack_111110(g_rt6[id]));
	const float3 base = min(albedo.rgb, 0.998);
	float3 diffuse = (g_options & 8) ? base : base * (1.0 - f0); // the game also uses base * (1 - F0)
	if (g_options & 4)
		diffuse *= saturate(g_rt5[id].a);

	// View vector for N.V (camera looks down +z in view space)
	const float2 ndc = float2((id.x + 0.5) / g_width * 2.0 - 1.0, 1.0 - (id.y + 0.5) / g_height * 2.0);
	const float3 v_view = normalize(float3(ndc.x * g_tan_x, ndc.y * g_tan_y, 1.0));
	const float3 n_view = float3(dot(n, g_right), dot(n, g_up), dot(n, g_fwd));
	const float n_dot_v = saturate(dot(n_view, -v_view));

	// The game scales the whole specular term of bit5 materials by RT2.x / 127 (lighting PS: %134 / %649), so their
	// RT4-alpha F0 never shows at full strength. Mode 1 copies that; without it the guide claims a much stronger
	// reflection than the game draws, and RR rebuilds one (a reflection edge that moves with view and light).
	float spec_scale = 1.0;
	if (f0_mode == 1 && (flags & 32))
		spec_scale = (material.x & 127) / 127.0;

	g_out_diffuse[id] = float4(diffuse, 1.0);
	g_out_specular[id] = float4(env_brdf_approx(f0, roughness, n_dot_v) * spec_scale, 1.0);
	g_out_normal[id] = float4(n, roughness);
}

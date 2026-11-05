// Debug view: decodes FH6 G-buffer channels and writes them into the final LDR image (after
// tonemapping, before the HUD), so exposure/bloom do not distort them.
// Decoding mirrors the game's own lighting shader (see notes/phase2_findings.md).

cbuffer Params : register(b0)
{
	uint g_mode;
	uint g_src_w;   // G-buffer render size (e.g. 1920x1080)
	uint g_src_h;
	uint g_dst_w;   // output size (e.g. 3840x2160)
	uint g_dst_h;
	uint3 g_pad;
};

Texture2D<uint>   g_normalSmooth : register(t0); // MRT RT3: oct normal (bits 8..31) + smoothness (bits 0..7)
Texture2D<float4> g_albedo       : register(t1); // MRT RT4 through an sRGB view -> linear
Texture2D<uint>   g_rt6          : register(t2); // MRT RT6: packed 11/11/10 floats (F0 candidate)
Texture2D<uint>   g_rt7          : register(t3); // MRT RT7: packed 11/11/10 floats (F0 candidate)
Texture2D<uint4>  g_material     : register(t4); // MRT RT2: material params, w = flags
Texture2D<float4> g_rt5          : register(t5); // MRT RT5: RGBA8 UNORM (role being verified)
RWTexture2D<float4> g_out        : register(u0); // final LDR image (RGBA8, gamma encoded)

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

float3 to_display(float3 linear_color)
{
	return pow(saturate(linear_color), 1.0 / 2.2);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
	if (id.x >= g_dst_w || id.y >= g_dst_h)
		return;
	const uint2 src = min(uint2((id.x * g_src_w) / g_dst_w, (id.y * g_src_h) / g_dst_h), uint2(g_src_w - 1, g_src_h - 1));

	const uint ns = g_normalSmooth[src];
	float3 c = 0;
	switch (g_mode)
	{
	case 1: // diffuse albedo
		c = to_display(g_albedo[src].rgb);
		break;
	case 2: // normal remapped to 0..1 (data, no gamma)
	{
		const float2 o = float2((ns >> 8) & 4095, (ns >> 20) & 4095) * (2.0 / 4095.0) - 1.0;
		c = oct_decode(o) * 0.5 + 0.5;
		break;
	}
	case 3: // roughness = 1 - smoothness (data)
		c = 1.0 - (ns & 255) / 255.0;
		break;
	case 4: // RT6 as packed 11/11/10 color (values > 1 show as white)
		c = to_display(unpack_111110(g_rt6[src]));
		break;
	case 5: // RT7 as packed 11/11/10 color
		c = to_display(unpack_111110(g_rt7[src]));
		break;
	case 6: // material flags: r = bit0 (F0 = 0.04), g = bit5 (F0 from albedo.a), b = bit6 (extra color)
	{
		const uint f = g_material[src].w;
		c = float3((f & 1) ? 1 : 0, (f & 32) ? 1 : 0, (f & 64) ? 1 : 0);
		break;
	}
	case 7: // resolved specular F0, scalar path from RT4 alpha (previous guess)
	{
		const uint f = g_material[src].w;
		c = to_display((f & 32) ? g_albedo[src].aaa : ((f & 1) ? 0.04 : unpack_111110(g_rt6[src])));
		break;
	}
	case 8: // RT4 (albedo) alpha
		c = g_albedo[src].aaa;
		break;
	case 9: // RT5 rgb
		c = g_rt5[src].rgb;
		break;
	case 10: // RT5 alpha
		c = g_rt5[src].aaa;
		break;
	case 11: // resolved specular F0, scalar path from RT5 alpha (new guess)
	{
		const uint f = g_material[src].w;
		c = to_display((f & 32) ? g_rt5[src].aaa : ((f & 1) ? 0.04 : unpack_111110(g_rt6[src])));
		break;
	}
	}
	g_out[id] = float4(c, 1.0);
}

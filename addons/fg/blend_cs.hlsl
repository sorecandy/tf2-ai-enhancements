// blend_cs.hlsl - tf2-fg M1b compute shaders (cs_5_0, FL 11_0: 2 UAVs).
//   diff_main  : cut guard. Samples every 8th pixel of prev/cur, counts samples whose perceptual luma changed by more
//                than g_cut_delta and adds the count to g_stat[0] (one atomic per group).
//   blend_main : gen = lerp(prev, cur, t) in LINEAR light. When the diff pass counted more than g_cut_frac of the
//                g_total samples as changed (a camera cut: death, kill cam, respawn, cinematic, spectator switch), the
//                frames are unrelated and a blend would be a double exposure, so gen = cur instead.
// prev/cur are _SRGB SRVs on our R8G8B8A8_TYPELESS copies of the back buffer, so Load() returns linear values;
// the output UAV is R8G8B8A8_UNORM (typed UAVs cannot be _SRGB), so the result is sRGB-encoded here by hand.
Texture2D<float4> g_prev : register(t0);
Texture2D<float4> g_cur  : register(t1);
RWTexture2D<unorm float4> g_out : register(u0);
RWByteAddressBuffer g_stat : register(u1);   // uint [0] = changed samples (cleared to 0 before diff_main)

cbuffer Params : register(b0)
{
	uint2 g_size;
	float g_t;          // 0 = prev, 1 = cur
	float g_cut_frac;   // > 0: cut guard on (fraction of g_total samples); 0 = off (diff_main is not dispatched)
	float g_cut_delta;  // |sqrt(luma prev) - sqrt(luma cur)| above this counts as changed
	float g_total;      // number of samples diff_main takes (computed on the CPU)
	float2 g_pad;
};

static const uint kStride = 8;

float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float3 linear_to_srgb(float3 c)
{
	c = saturate(c);
	return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

groupshared uint gs_changed;

[numthreads(8, 8, 1)]
void diff_main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
	if (gi == 0)
		gs_changed = 0;
	GroupMemoryBarrierWithGroupSync();
	const uint2 p = id.xy * kStride + kStride / 2;
	if (p.x < g_size.x && p.y < g_size.y)
	{
		const float a = sqrt(max(luma(g_prev.Load(int3(p, 0)).rgb), 0.0));
		const float b = sqrt(max(luma(g_cur.Load(int3(p, 0)).rgb), 0.0));
		if (abs(a - b) > g_cut_delta)
		{
			uint unused;
			InterlockedAdd(gs_changed, 1u, unused);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (gi == 0 && gs_changed != 0)
	{
		uint unused;
		g_stat.InterlockedAdd(0, gs_changed, unused);
	}
}

// select_main (Mode 3, FSR): runs after FSR wrote g_out. When the diff pass counted a camera cut, the FSR frame of two
// unrelated images is replaced by cur (same rule as blend_main); otherwise g_out is left exactly as FSR wrote it.
[numthreads(8, 8, 1)]
void select_main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= g_size.x || id.y >= g_size.y)
		return;
	if (!(g_cut_frac > 0.0 && g_total > 0.0 && float(g_stat.Load(0)) > g_cut_frac * g_total))
		return;
	const float4 b = g_cur.Load(int3(id.xy, 0));
	g_out[id.xy] = float4(linear_to_srgb(b.rgb), b.a);
}

[numthreads(8, 8, 1)]
void blend_main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= g_size.x || id.y >= g_size.y)
		return;
	float t = g_t;
	if (g_cut_frac > 0.0 && g_total > 0.0 && float(g_stat.Load(0)) > g_cut_frac * g_total)
		t = 1.0;
	const float4 a = g_prev.Load(int3(id.xy, 0));
	const float4 b = g_cur.Load(int3(id.xy, 0));
	const float4 m = lerp(a, b, t);
	g_out[id.xy] = float4(linear_to_srgb(m.rgb), m.a);
}

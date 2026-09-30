// prep_cs.hlsl -- cs_5_0. Turns TF2's TSAA inputs (full-size typeless RTs, DVS sub-rect at the
// top-left) into three exact-render-size, typed DLSS inputs.
//   t0 CurFrame   : TF2 HDR scene, SRV R16G16B16A16_FLOAT on the R16G16B16A16_TYPELESS RT
//   t1 Velocity   : _rt_MRT1, SRV R16G16_FLOAT on R16G16_TYPELESS; raw = 1024*(curUV_jittered - prevUV)
//   t2 ZBuffer    : main DS, SRV R24_UNORM_X8_TYPELESS on R24G8_TYPELESS (standard Z: 0 near, 1 far)
//   u0 color out  : R16G16B16A16_FLOAT, render size  (DLSS pInColor, linear HDR, pre-tonemap)
//   u1 mv out     : R16G16_FLOAT, render size        (DLSS pInMotionVectors, render pixels, cur + mv = prev)
//   u2 depth out  : R32_FLOAT, render size           (DLSS pInDepth, device Z, DepthInverted = 0)
Texture2D<float4>   g_scene : register(t0);
Texture2D<float2>   g_vel   : register(t1);
Texture2D<float>    g_depth : register(t2);
RWTexture2D<float4> o_color : register(u0);
RWTexture2D<float2> o_mv    : register(u1);
RWTexture2D<float>  o_depth : register(u2);

cbuffer Params : register(b0)
{
    uint2  g_render_size;   // main-pass viewport (DVS sub-rect), e.g. 1280x800
    float2 g_jitter_uv;     // CBufTSAA c_jitterX/Y (viewport UV, y down) captured for THIS frame
    float  g_vel_scale;     // 1/1024
    float  g_sub_jitter;    // 1 = the velocity contains the jitter delta: remove it (tf2-dlss-sr MVJittered=1)
    float2 g_pad;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_render_size))
        return;

    o_color[id.xy] = g_scene[id.xy];

    // raw/1024 = curUV_jittered - prevUV_unjittered; content in the jittered frame sits at uv - c_jitter,
    // so the jitter-free "prev - cur" delta is -(raw/1024 + c_jitter)  (same formula TSAA uses, sign flipped).
    const float2 raw    = g_vel[id.xy];
    const float2 mv_uv  = -(raw * g_vel_scale + g_sub_jitter * g_jitter_uv);
    o_mv[id.xy]         = mv_uv * float2(g_render_size);   // DLSS: render-resolution pixels, MVLowRes, MVJittered = 0

    o_depth[id.xy] = g_depth[id.xy];
}

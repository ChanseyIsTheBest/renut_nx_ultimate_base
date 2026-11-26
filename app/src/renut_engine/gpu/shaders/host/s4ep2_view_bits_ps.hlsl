// S4ep2-XBLA-nx (github.com/FurryLilManChild/S4ep2-XBLA-nx, GPL-3.0, 0.5.1; its 4d08d3da and 0ed3e6e0, view_bits_ps modes 3
// and 4): one EDRAM view read through another where one side is a 32_FLOAT target, carried over bit for bit as the
// console holds the pixel (render/draw.cpp, SyncColorView32bppLocked with s4ep2_view_sync_32float). On the console both
// views are the same 32 bits of EDRAM; here each is its own host target. Episode II reads its 7e3 scene through a 32_FLOAT
// view and that through an 8_8_8_8 one (its "24<-3F" and "06<-24" pairs, 70-600 a 10 s window).
// Layouts (as Xenos stores them, R in the low bits): 8_8_8_8 R 0-7, G 8-15, B 16-23, A 24-31; 2_10_10_10_FLOAT R 0-9,
// G 10-19, B 20-29, A 30-31; 32_FLOAT one IEEE float.
// g_Push.param0 is the mode: 3 = 32_FLOAT drawn, read as 8_8_8_8; 4 = 7e3 drawn, read as 32_FLOAT.
// g_Push.param1 is 2^bias of the drawn target when the host target holds its colour without the bias, else 1
// (blur_renut_target_bias_rgb on: the target holds the biased colour, as the EDRAM does).
// The pixel plus g_Push.translate is the source texel; present_vs draws the fullscreen triangle.
#include "utility.hlsli"

// The 10 bits of a 7e3 float, as view_fp10_decode_ps.hlsl's Encode7e3U (port 297's exact encode).
uint Encode7e3U(float value)
{
    const float v = clamp(value, 0.0, 31.875);
    if (!(v >= 0.25))
        return uint(floor(v * 512.0 + 0.5)) & 0x3FFu;
    const uint u = asuint(v);
    return min(((u + 0x8000u) - (124u << 23u)) >> 16u, 1023u);
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    const int2 texel = int2(position.xy) + int2(g_Push.translate);
    const float4 c = g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].Load(int3(texel, 0));
    const int mode = int(round(g_Push.param0));
    if (mode == 4)
    {
        const uint3 f10 = uint3(Encode7e3U(c.r * g_Push.param1), Encode7e3U(c.g * g_Push.param1),
                                Encode7e3U(c.b * g_Push.param1));
        const uint a2 = uint(round(clamp(c.a, 0.0, 1.0) * 3.0)) & 3u;
        const uint w = f10.r | (f10.g << 10u) | (f10.b << 20u) | (a2 << 30u);
        return float4(asfloat(w), 0.0, 0.0, 1.0);
    }
    // mode 3: the float's bits as four bytes (no arithmetic on it when the scale is 1: a denormal or NaN keeps its bits).
    const uint w = g_Push.param1 == 1.0 ? asuint(c.r) : asuint(c.r * g_Push.param1);
    return float4(float(w & 0xFFu), float((w >> 8u) & 0xFFu), float((w >> 16u) & 0xFFu), float((w >> 24u) & 0xFFu)) /
           255.0;
}

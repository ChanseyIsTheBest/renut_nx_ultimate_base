// S4ep2-XBLA-nx (github.com/FurryLilManChild/S4ep2-XBLA-nx, GPL-3.0, 0.5.1; its d8761055 "resolve 7e3 targets as the
// console stores them", view_bits_ps mode 5): a colour resolve of a 2_10_10_10_FLOAT (7e3) target, drawn as the console's
// EDRAM holds it (render/draw.cpp, S4ep2Resolve7e3Locked with s4ep2_resolve_clamp_7e3). The host target is RGBA16F,
// which keeps values far past 7e3's range, negatives and NaN; the console stores colour 0 to 31.875 (after the target's
// exponent bias), no NaN, and its resolve copies that. A blend past the range, or a biased target the draw-time clamp
// (dishonored_renut_clamp_7e3, constant 990) leaves alone, reads back clamped here.
// g_Push.param0: 2^bias when the host target holds the colour without the target's exponent bias, else 1
// (blur_renut_target_bias_rgb on: it holds the biased colour, as the EDRAM does). Alpha: 0 to 1, NaN 0.
// As resolve_copy_ps otherwise: the destination pixel plus g_Push.translate is the source texel.
#include "utility.hlsli"

float4 main(float4 position : SV_Position) : SV_Target0
{
    const int2 texel = int2(position.xy) + int2(g_Push.translate);
    const float4 c = g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].Load(int3(texel, 0));
    const float3 rgb = select(isnan(c.rgb), float3(0.0, 0.0, 0.0), c.rgb * g_Push.param0);
    return float4(clamp(rgb, 0.0, 31.875), select(isnan(c.a), 0.0, saturate(c.a)));
}

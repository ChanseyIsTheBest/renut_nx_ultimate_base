// asura-nx (asura_renut_fixed16_bias): resolve_clamp_ps with the lower bound and a scale. A colour resolve of a 16-bit
// fixed-point target (Xenos k_16_16 / k_16_16_16_16, held as FP16: render/format.cpp), drawn with the console's range:
// signed targets store -32..32, unsigned targets (gfmt 16_16 / 16_16_16_16) store 0..32, parameterized via
// g_Push.param0 (render/draw.cpp, ResolveFixed16Locked), then scaled by g_Push.scale.x (the resolve's exponent bias). As
// resolve_copy_ps otherwise: the destination pixel plus g_Push.translate is the source texel.
#include "utility.hlsli"

float4 main(float4 position : SV_Position) : SV_Target0
{
    const int2 texel = int2(position.xy) + int2(g_Push.translate);
    return clamp(g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].Load(int3(texel, 0)), g_Push.param0, 32.0) * g_Push.scale.x;
}

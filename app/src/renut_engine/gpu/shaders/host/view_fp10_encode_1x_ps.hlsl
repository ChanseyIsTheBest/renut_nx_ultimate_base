// Specialized 1x (bias_scale == 1.0) 0x3F (7e3 RGBA16F) -> 0x36 (R10G10B10A2_UNORM) for SyncColorViewLocked.
#include "utility.hlsli"

float Encode7e3(float value)
{
    const float v = clamp(value, 0.0, 31.875);
    if (!(v >= 0.25))
        return floor(v * 512.0 + 0.5);
    const uint u = asuint(v);
    const uint code = min(((u + 0x8000u) - (124u << 23u)) >> 16u, 1023u);
    return float(code);
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    const int2 texel = int2(position.xy) + int2(g_Push.translate);
    const float4 c = g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].Load(int3(texel, 0));
    return float4(Encode7e3(c.r), Encode7e3(c.g), Encode7e3(c.b), 0.0) / 1023.0 +
           float4(0.0, 0.0, 0.0, c.a);
}

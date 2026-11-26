// Specialized branchless mode 0 (0x36 R10G10B10A2_UNORM -> 0x3F 7e3 RGBA16F) for SyncColorViewLocked.
#include "utility.hlsli"

float Decode7e3(uint f10)
{
    const uint e = f10 >> 7u;
    const uint m = f10 & 0x7Fu;
    if (e == 0u)
        return float(m) * (1.0 / 512.0);
    return asfloat(((e + 124u) << 23u) | (m << 16u));
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    const int2 texel = int2(position.xy) + int2(g_Push.translate);
    const float4 c = g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].Load(int3(texel, 0));
    const uint3 f10 = uint3(round(clamp(c.rgb, 0.0, 1.0) * 1023.0));
    return float4(Decode7e3(f10.r), Decode7e3(f10.g), Decode7e3(f10.b), c.a);
}

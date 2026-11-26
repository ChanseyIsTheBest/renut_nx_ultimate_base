// EDRAM view format conversion between 8_8_8_8 (EDRAM 0), 2_10_10_10 (EDRAM 2), and
// 2_10_10_10_FLOAT 7e3 (EDRAM 3) in draw.cpp SyncColorViewLocked.
// Mode (in g_Push.param0, rounded):
//   0: 0x36 (R10G10B10A2_UNORM) -> 0x3F (7e3 RGBA16F)
//   1: 0x06 (R8G8B8A8_UNORM)    -> 0x3F (7e3 RGBA16F)
//   2: 0x06 (R8G8B8A8_UNORM)    -> 0x36 (R10G10B10A2_UNORM)
//   3: 0x36 (R10G10B10A2_UNORM) -> 0x06 (R8G8B8A8_UNORM)
//   4: 0x3F (7e3 RGBA16F)       -> 0x06 (R8G8B8A8_UNORM), scaled by g_Push.param1
#include "utility.hlsli"

float Decode7e3(uint f10)
{
    const uint e = f10 >> 7u;
    const uint m = f10 & 0x7Fu;
    if (e == 0u)
        return float(m) * (1.0 / 512.0);
    return asfloat(((e + 124u) << 23u) | (m << 16u));
}

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

    if (mode == 0)
    {
        const uint3 f10 = uint3(round(clamp(c.rgb, 0.0, 1.0) * 1023.0));
        return float4(Decode7e3(f10.r), Decode7e3(f10.g), Decode7e3(f10.b), c.a);
    }
    if (mode == 1)
    {
        const uint4 b = uint4(round(clamp(c, 0.0, 1.0) * 255.0));
        const uint w = b.r | (b.g << 8u) | (b.b << 16u) | (b.a << 24u);
        const uint3 f10 = uint3(w & 0x3FFu, (w >> 10u) & 0x3FFu, (w >> 20u) & 0x3FFu);
        return float4(Decode7e3(f10.r), Decode7e3(f10.g), Decode7e3(f10.b), float((w >> 30u) & 3u) / 3.0);
    }
    if (mode == 2)
    {
        const uint4 b = uint4(round(clamp(c, 0.0, 1.0) * 255.0));
        const uint w = b.r | (b.g << 8u) | (b.b << 16u) | (b.a << 24u);
        return float4(float(w & 0x3FFu) / 1023.0, float((w >> 10u) & 0x3FFu) / 1023.0,
                      float((w >> 20u) & 0x3FFu) / 1023.0, float((w >> 30u) & 3u) / 3.0);
    }
    if (mode == 3)
    {
        const uint3 f10 = uint3(round(clamp(c.rgb, 0.0, 1.0) * 1023.0));
        const uint a2 = uint(round(clamp(c.a, 0.0, 1.0) * 3.0)) & 3u;
        const uint w = f10.r | (f10.g << 10u) | (f10.b << 20u) | (a2 << 30u);
        return float4(float(w & 0xFFu), float((w >> 8u) & 0xFFu), float((w >> 16u) & 0xFFu),
                      float((w >> 24u) & 0xFFu)) /
               255.0;
    }

    // mode == 4: 0x3F (7e3 RGBA16F) -> 0x06 (R8G8B8A8_UNORM)
    const uint3 f10 = uint3(Encode7e3U(c.r * g_Push.param1), Encode7e3U(c.g * g_Push.param1),
                            Encode7e3U(c.b * g_Push.param1));
    const uint a2 = uint(round(clamp(c.a, 0.0, 1.0) * 3.0)) & 3u;
    const uint w = f10.r | (f10.g << 10u) | (f10.b << 20u) | (a2 << 30u);
    return float4(float(w & 0xFFu), float((w >> 8u) & 0xFFu), float((w >> 16u) & 0xFFu),
                  float((w >> 24u) & 0xFFu)) /
           255.0;
}

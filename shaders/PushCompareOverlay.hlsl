// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS comparison clip labels (engine/RecordingCompare.h): a straight-alpha sRGB RGBA badge
// blended onto the recorder's NV12/P010 picture. The picture under the badge is read from a copy
// (bgY, bgUv) so no typed UAV loads are needed. SDR: BT.709 limited range from sRGB. HDR: the
// badge's white sits at 203 nits (BT.2408 reference white), coded as BT.2020 PQ 10-bit.
#include "HdrColor.hlsli"
Texture2D<float4> overlay : register(t0);
Texture2D<float> bgY : register(t1);
Texture2D<float2> bgUv : register(t2);
RWTexture2D<float> outY : register(u0);
RWTexture2D<float2> outUv : register(u1);
cbuffer Constants : register(b0) {
    uint x0, y0, w, h; // badge place in the picture (even) and size
    uint hdr; float fade; uint sx, pad2; // fade: 1 - opacity (scene transitions; 0 = opaque)
    // sx: the first badge column drawn (0 for a whole badge). The Reveal comparison layout draws
    // part of a badge, cut at its sweeping line, and the line itself as it slides in and out.
};
float3 coded(float3 srgb) {
    if (hdr) {
        const float3 lin = lerp(pow((srgb + 0.055) / 1.055, 2.4), srgb / 12.92, step(srgb, 0.04045));
        const float3 e = HdrEncodePq(lin * (203.0 / 80.0));
        const float y = dot(e, float3(.2627, .6780, .0593));
        return float3(64 + 876 * y, 512 + 896 * (e.b - y) / 1.8814, 512 + 896 * (e.r - y) / 1.4746) * 64.0 / 65535.0;
    }
    const float y = dot(srgb, float3(0.2126, 0.7152, 0.0722));
    return float3((16 + 219 * y) / 255.0, (128 + 224 * (srgb.b - y) / 1.8556) / 255.0, (128 + 224 * (srgb.r - y) / 1.5748) / 255.0);
}
float codes(float v) { return hdr ? round(v * 65535.0 / 64.0) * 64.0 / 65535.0 : v; }

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    const uint2 p = uint2(x0, y0) + id.xy;
    const float4 o = overlay[id.xy + uint2(sx, 0)];
    float luma = bgY[id.xy];
    const float keep = saturate(1.0 - fade);
    if (o.a > 0) luma = lerp(luma, coded(saturate(o.rgb)).x, o.a * keep);
    outY[p] = codes(luma);
    if ((id.x & 1) != 0 || (id.y & 1) != 0) return;
    // Chroma: the 2x2 badge pixels it covers, colour weighted by alpha.
    float3 sum = 0;
    float alpha = 0;
    [unroll] for (uint j = 0; j < 2; ++j)
        [unroll] for (uint i = 0; i < 2; ++i) {
            const float4 t = overlay[min(id.xy + uint2(i, j), uint2(w - 1, h - 1)) + uint2(sx, 0)];
            sum += t.rgb * t.a;
            alpha += t.a;
        }
    float2 chroma = bgUv[id.xy / 2];
    if (alpha > 0) chroma = lerp(chroma, coded(saturate(sum / alpha)).yz, alpha * 0.25 * keep);
    outUv[p / 2] = float2(codes(chroma.x), codes(chroma.y));
}

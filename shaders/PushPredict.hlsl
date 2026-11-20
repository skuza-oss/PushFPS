// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFlow 2's Predict (src/pushflow/PushFlowBackend.cpp `forecast`; docs/PUSHFLOW2.md). The rule for one
// pixel, its numbers and its reasons are in include/veyra/pushflow/PushFlowPredict.h, which the graphics
// test compares this shader against pixel by pixel: change both together.
//
// A frame for a time `ahead` of an interval AFTER the newest real frame, from the half-way grids of the
// pair that has arrived. Every pixel is a pixel of the newest real frame, moved along the motion found
// or, where that motion is not trusted, left where it is.
//
// Two stages (the constant `stage`): 0 clears the totals, 1 makes the picture and counts every 16th pixel.
Texture2D<float4> previous : register(t0);
Texture2D<float4> current : register(t1);
RWStructuredBuffer<float> grid0 : register(u0);
RWStructuredBuffer<float> grid1 : register(u1);
RWStructuredBuffer<uint> totals : register(u2);    // counted, would move, held, serial
RWTexture2D<float4> output : register(u3);
cbuffer Constants : register(b0) {
    uint srcW, srcH, padW, padH;
    uint modelW, modelH, encoding, alignCorners;
    uint stage, serial;
    float ahead, unused0;
};
static const float2 kSettle = float2(0.5, 1.5);
static const float2 kOpposite = float2(0.5, 1.5);
static const float2 kAgree = float2(0.08, 0.16);
static const float2 kStill = float2(0.25, 0.5);
static const uint kSampleStep = 16;

float3 encodeColor(float3 c) {
    if (encoding == 1) {
        c = saturate(c);
        return lerp(c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055, step(0.0031308, c));
    }
    if (encoding == 2) {
        const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
        const float3 y = pow(saturate(max(c, 0.0) * (80.0 / 10000.0)), m1);
        return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
    }
    return saturate(c);
}
float fade(float x, float2 range) {
    const float t = saturate((x - range.x) / (range.y - range.x));
    return 1.0 - t * t * (3.0 - 2.0 * t);
}
// Bilinear tap of a full-resolution frame at a continuous pixel position (pixel centres at integers).
float3 tapFrame(Texture2D<float4> t, float2 q) {
    q = clamp(q, 0.0, float2(srcW - 1, srcH - 1));
    const uint2 a = uint2(q);
    const uint2 b = min(a + 1, uint2(srcW - 1, srcH - 1));
    const float2 f = q - float2(a);
    return lerp(lerp(t[a].rgb, t[uint2(b.x, a.y)].rgb, f.x), lerp(t[uint2(a.x, b.y)].rgb, t[b].rgb, f.x), f.y);
}
float3 tapEncoded(Texture2D<float4> t, float2 q) {
    q = clamp(q, 0.0, float2(srcW - 1, srcH - 1));
    const uint2 a = uint2(q);
    const uint2 b = min(a + 1, uint2(srcW - 1, srcH - 1));
    const float2 f = q - float2(a);
    return lerp(lerp(encodeColor(t[a].rgb), encodeColor(t[uint2(b.x, a.y)].rgb), f.x), lerp(encodeColor(t[uint2(a.x, b.y)].rgb), encodeColor(t[b].rgb), f.x), f.y);
}
float2 modelToSource(float2 q) { return (q + 0.5) * float2(srcW, srcH) / float2(modelW, modelH) - 0.5; }
float2 gridToModel(float2 g) {
    const float2 pad = float2(padW, padH);
    return alignCorners != 0 ? (g + 1.0) * 0.5 * (pad - 1.0) : ((g + 1.0) * pad - 1.0) * 0.5;
}
// Where the half-way pixel at s reads a frame, as a step from s (the grid is read inside the picture).
float2 stepAt(RWStructuredBuffer<float> grid, float2 s) {
    const float2 c = clamp(s, 0.0, float2(modelW - 1, modelH - 1));
    const uint2 a = uint2(c);
    const uint2 b = min(a + 1, uint2(modelW - 1, modelH - 1));
    const float2 f = c - float2(a);
    const uint i00 = (a.y * padW + a.x) * 2, i10 = (a.y * padW + b.x) * 2, i01 = (b.y * padW + a.x) * 2, i11 = (b.y * padW + b.x) * 2;
    const float2 g00 = float2(grid[i00], grid[i00 + 1]), g10 = float2(grid[i10], grid[i10 + 1]);
    const float2 g01 = float2(grid[i01], grid[i01 + 1]), g11 = float2(grid[i11], grid[i11 + 1]);
    const float2 top = g00 + (g10 - g00) * f.x, bottom = g01 + (g11 - g01) * f.x;
    return gridToModel(top + (bottom - top) * f.y) - c;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (stage == 0) {
        if (id.x == 0 && id.y == 0) { totals[0] = 0; totals[1] = 0; totals[2] = 0; totals[3] = serial; }
        return;
    }
    if (id.x >= srcW || id.y >= srcH) return;
    const float2 at = (float2(id.xy) + 0.5) * float2(modelW, modelH) / float2(srcW, srcH) - 0.5;
    const float k = 1.0 + 2.0 * ahead;
    float2 s = at;
    for (int i = 0; i < 2; ++i) s = at - k * stepAt(grid1, s);
    const float2 d = stepAt(grid1, s), back = stepAt(grid0, s);
    const float2 place = s + d;
    float trust = fade(length(s + k * d - at), kSettle) * fade(length(back + d), kOpposite);
    if (place.x < -0.5 || place.y < -0.5 || place.x > float(modelW) - 0.5 || place.y > float(modelH) - 0.5) trust = 0.0;
    const float3 apart = abs(tapEncoded(previous, modelToSource(s + back)) - tapEncoded(current, modelToSource(place)));
    trust *= fade(max(apart.r, max(apart.g, apart.b)), kAgree);
    const float moved = length(place - at);
    const bool held = moved >= kStill.y && trust < 0.5;
    trust *= 1.0 - fade(moved, kStill);
    const float3 own = current[id.xy].rgb;
    float3 shown = own;
    if (trust > 0.0) {
        const float2 q = float2(id.xy) + (place - at) * float2(srcW, srcH) / float2(modelW, modelH);
        shown = own + (tapFrame(current, q) - own) * trust;
    }
    output[id.xy] = float4(shown, 1.0);
    if (id.x % kSampleStep == 0 && id.y % kSampleStep == 0) {
        InterlockedAdd(totals[0], 1u);
        if (moved >= kStill.y) InterlockedAdd(totals[1], 1u);
        if (held) InterlockedAdd(totals[2], 1u);
    }
}

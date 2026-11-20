// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFlow 2's Guard (src/pushflow/PushFlowBackend.cpp; docs/PUSHFLOW2.md; the rule is guard_tiles() in
// scripts/pushflow/prepare_runtime.py, which measured it on the shipped models, and
// include/veyra/pushflow/PushFlowGuard.h, which holds its numbers and its decision for the unit test).
//
// An object that moves further between two real frames than the network follows is left out of the
// generated frame: shown where neither frame has it, which is nowhere. Guard finds those places and shows
// the nearer real frame there, so the object steps at the game's own rate instead of blinking out.
//
// The picture is cut into tiles of 16 x 16 motion pixels. A tile is RISKY when all three hold:
//   CHANGED  what it shows differs between the two real frames once its own change of brightness is
//            taken out (a fade is not a change);
//   UNUSED   what one of the two frames shows there is shown nowhere in the generated frame. A place in
//            a frame counts as shown by a generated pixel that reads it when that pixel takes it (the
//            mask's weight), or when the pixel's other sample is the same picture: the network often
//            takes one frame only, and what it shows of the other is then the same thing;
//   INSIDE   it is further from the picture's edge than the picture's motion reaches (a pan pushes the
//            edge out of the picture: unused, and right).
// A risky tile takes its changed neighbours with it (an object is not cut in half), and a frame with
// more than a third of its tiles risky is the nearer real frame whole.
//
// Four stages, one dispatch each (the constant `stage`):
//   0  per tile: how much it changed; its counts set to zero
//   1  per motion pixel: what it shows of each frame, added to the tiles it reads (whole numbers, so
//      the order the graphics card adds them in cannot change the sum)
//   2  one thread: the decision for every tile, and the frame's totals
//   3  per picture pixel: the generated picture, or the nearer real frame where a tile is risky
Texture2D<float4> previous : register(t0);
Texture2D<float4> current : register(t1);
Texture2D<float4> generated : register(t2);
RWStructuredBuffer<float> grid0 : register(u0);
RWStructuredBuffer<float> grid1 : register(u1);
RWStructuredBuffer<float> mask : register(u2);
RWStructuredBuffer<float> tiles : register(u3);    // per tile: change, risky before growing, risky
RWStructuredBuffer<uint> counts : register(u4);    // per tile: shown of the first frame, of the second, motion; then 4 totals
RWTexture2D<float4> output : register(u5);
cbuffer Constants : register(b0) {
    uint srcW, srcH, padW, padH;
    uint modelW, modelH, encoding, alignCorners;
    uint maskForFirst, stage, tilesW, tilesH;
    uint nearest, serial, unused0, unused1;
};
static const uint kTile = 16;
static const float kChange = 0.05;      // a tile changed: mean difference of its brightness pattern, 0..1
static const float kAgree = 0.08;       // two samples are the same picture: largest channel difference below this
static const float kUnused = 0.25;      // a tile of a frame is unused: less than this share of it is shown
static const float kEdge = 1.5;         // the edge band: this many times the largest motion of a tile, plus one tile
static const uint kWholePercent = 35;   // more risky tiles than this: the nearer real frame, whole

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
// Bilinear tap of a full-resolution frame at a continuous pixel position (pixel centres at integers).
float3 tapFrame(Texture2D<float4> t, float2 q) {
    q = clamp(q, 0.0, float2(srcW - 1, srcH - 1));
    const uint2 a = uint2(q);
    const uint2 b = min(a + 1, uint2(srcW - 1, srcH - 1));
    const float2 f = q - float2(a);
    return lerp(lerp(t[a].rgb, t[uint2(b.x, a.y)].rgb, f.x), lerp(t[uint2(a.x, b.y)].rgb, t[b].rgb, f.x), f.y);
}
// Motion-pixel position -> full-resolution pixel position, and a grid value -> motion-pixel position.
float2 modelToSource(float2 q) { return (q + 0.5) * float2(srcW, srcH) / float2(modelW, modelH) - 0.5; }
float2 gridToModel(float2 g) {
    const float2 pad = float2(padW, padH);
    return alignCorners != 0 ? (g + 1.0) * 0.5 * (pad - 1.0) : ((g + 1.0) * pad - 1.0) * 0.5;
}
float brightness(Texture2D<float4> t, uint2 at) {
    const float3 c = encodeColor(tapFrame(t, modelToSource(float2(at))));
    return (c.r + c.g + c.b) / 3.0;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const uint tileCount = tilesW * tilesH;
    if (stage == 0) {
        if (id.x >= tilesW || id.y >= tilesH) return;
        const uint t = id.y * tilesW + id.x;
        float a[256], b[256];
        float meanA = 0.0, meanB = 0.0;
        uint k;
        for (k = 0; k < 256; ++k) {
            const uint2 at = id.xy * kTile + uint2(k % kTile, k / kTile);
            a[k] = brightness(previous, at); b[k] = brightness(current, at);
            meanA += a[k]; meanB += b[k];
        }
        meanA /= 256.0; meanB /= 256.0;
        float change = 0.0;
        for (k = 0; k < 256; ++k) change += abs((a[k] - meanA) - (b[k] - meanB));
        tiles[t * 3 + 0] = change / 256.0;
        tiles[t * 3 + 1] = 0.0;
        tiles[t * 3 + 2] = 0.0;
        counts[t * 3 + 0] = 0; counts[t * 3 + 1] = 0; counts[t * 3 + 2] = 0;
        return;
    }
    if (stage == 1) {
        if (id.x >= tilesW * kTile || id.y >= tilesH * kTile) return;
        const uint i = id.y * padW + id.x;
        const float2 readA = gridToModel(float2(grid0[i * 2], grid0[i * 2 + 1]));
        const float2 readB = gridToModel(float2(grid1[i * 2], grid1[i * 2 + 1]));
        const float m = saturate(mask[i]);
        const float w0 = maskForFirst != 0 ? m : 1.0 - m;
        const float3 a = encodeColor(tapFrame(previous, modelToSource(readA)));
        const float3 b = encodeColor(tapFrame(current, modelToSource(readB)));
        const float3 apart = abs(a - b);
        const float same = 1.0 - saturate(max(apart.r, max(apart.g, apart.b)) / kAgree);
        const int2 tileA = int2(floor(readA / float(kTile))), tileB = int2(floor(readB / float(kTile)));
        if (tileA.x >= 0 && tileA.y >= 0 && tileA.x < int(tilesW) && tileA.y < int(tilesH))
            InterlockedAdd(counts[(uint(tileA.y) * tilesW + uint(tileA.x)) * 3 + 0], uint(max(w0, same) * 255.0 + 0.5));
        if (tileB.x >= 0 && tileB.y >= 0 && tileB.x < int(tilesW) && tileB.y < int(tilesH))
            InterlockedAdd(counts[(uint(tileB.y) * tilesW + uint(tileB.x)) * 3 + 1], uint(max(1.0 - w0, same) * 255.0 + 0.5));
        const float moved = min(max(length(readA - float2(id.xy)), length(readB - float2(id.xy))), 255.0);
        InterlockedAdd(counts[((id.y / kTile) * tilesW + id.x / kTile) * 3 + 2], uint(moved * 16.0 + 0.5));
        return;
    }
    if (stage == 2) {
        if (id.x != 0 || id.y != 0) return;
        uint t;
        float furthest = 0.0;   // the largest motion any tile has on average, in motion pixels
        for (t = 0; t < tileCount; ++t) furthest = max(furthest, float(counts[t * 3 + 2]) / (256.0 * 16.0));
        const int reach = int(ceil(kEdge * furthest / float(kTile))) + 1;
        for (t = 0; t < tileCount; ++t) {
            const int tx = int(t % tilesW), ty = int(t / tilesW);
            const bool changed = tiles[t * 3 + 0] > kChange;
            const bool unused = float(min(counts[t * 3 + 0], counts[t * 3 + 1])) < kUnused * 256.0 * 255.0;
            const bool inside = tx >= reach && ty >= reach && tx < int(tilesW) - reach && ty < int(tilesH) - reach;
            tiles[t * 3 + 1] = changed && unused && inside ? 1.0 : 0.0;
        }
        uint risky = 0;
        for (t = 0; t < tileCount; ++t) {
            const int tx = int(t % tilesW), ty = int(t / tilesW);
            bool on = tiles[t * 3 + 1] > 0.5;
            if (!on && tiles[t * 3 + 0] > kChange) {
                for (int dy = -1; dy <= 1 && !on; ++dy)
                    for (int dx = -1; dx <= 1 && !on; ++dx) {
                        const int nx = tx + dx, ny = ty + dy;
                        if (nx < 0 || ny < 0 || nx >= int(tilesW) || ny >= int(tilesH)) continue;
                        on = tiles[(uint(ny) * tilesW + uint(nx)) * 3 + 1] > 0.5;
                    }
            }
            tiles[t * 3 + 2] = on ? 1.0 : 0.0;
            risky += on ? 1u : 0u;
        }
        counts[tileCount * 3 + 0] = risky;
        counts[tileCount * 3 + 1] = risky * 100 > kWholePercent * tileCount ? 1u : 0u;
        counts[tileCount * 3 + 2] = tileCount;
        counts[tileCount * 3 + 3] = serial;
        return;
    }
    // stage 3
    if (id.x >= srcW || id.y >= srcH) return;
    const float3 made = generated[id.xy].rgb;
    float share = 0.0;
    if (counts[tileCount * 3 + 1] != 0) {
        share = 1.0;
    } else if (counts[tileCount * 3 + 0] != 0) {
        // The tile's own area is replaced whole; half a tile around it fades out.
        const float2 q = (float2(id.xy) + 0.5) * float2(modelW, modelH) / float2(srcW, srcH) - 0.5;
        const float2 p = clamp((q + 0.5) / float(kTile) - 0.5, 0.0, float2(tilesW - 1, tilesH - 1));
        const uint2 lo = uint2(p);
        const uint2 hi = min(lo + 1, uint2(tilesW - 1, tilesH - 1));
        const float2 f = p - float2(lo);
        const float r00 = tiles[(lo.y * tilesW + lo.x) * 3 + 2], r10 = tiles[(lo.y * tilesW + hi.x) * 3 + 2];
        const float r01 = tiles[(hi.y * tilesW + lo.x) * 3 + 2], r11 = tiles[(hi.y * tilesW + hi.x) * 3 + 2];
        share = saturate(2.0 * lerp(lerp(r00, r10, f.x), lerp(r01, r11, f.x), f.y));
    }
    float3 shown = made;
    if (share > 0.0) {
        const float3 real = nearest != 0 ? current[id.xy].rgb : previous[id.xy].rgb;
        shown = lerp(made, real, share);
    }
    output[id.xy] = float4(shown, 1.0);
}

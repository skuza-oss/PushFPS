// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFlow 2's Predict: a frame for a time AFTER the newest real frame, made from the two real frames
// that have already arrived. Nothing from the future is used, so the newest real frame is shown as soon
// as it is ready instead of one frame interval later; the frames after it are forecasts.
// Pure (builds with g++): the numbers, the rule for one pixel, and where the frames stand in time.
// shaders/PushPredict.hlsl does the same on the graphics card (change both together; testPushFlow
// compares them pixel by pixel), and predict_report() in scripts/pushflow/prepare_runtime.py is the rule
// as it was measured on the shipped models (docs/PUSHFLOW2.md).
//
// How a forecast is made. The network is run once for the pair, for the half-way time. Its second grid
// says, for a pixel x of the half-way frame, where that pixel is in the newest frame: x + d(x). So the
// pixel moves 2 d a frame, and at a time `ahead` of an interval after the newest frame it stands at
// x + (1 + 2 ahead) d(x). A forecast pixel y therefore left the half-way frame at the x that solves
// y = x + (1 + 2 ahead) d(x) (found by stepping twice), and shows what the newest frame has at x + d(x).
//
// How far a forecast pixel is trusted (0..1; the rest is the newest real frame, a hold):
//   SETTLE    the search for x must come to rest;
//   OPPOSITE  the motion to the older frame must be the opposite of the motion to the newer one (it is
//             not where something is covered, uncovered or lost);
//   AGREE     the two real frames must show the same thing at the two places the motion joins (they do
//             not across a cut);
//   INSIDE    the place read must be inside the picture;
//   STILL     less than a quarter of a pixel is not moved at all, so a still picture and a HUD stay exact.
// It never invents a picture: every pixel is a pixel of the newest real frame, moved or not.
#pragma once
#include "veyra/pushflow/PushFlowMotion.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace veyra::pushflow::predict {

inline constexpr float kSettle[2] = {0.5f, 1.5f};      // motion pixels the search may be off: trusted below, not above
inline constexpr float kOpposite[2] = {0.5f, 1.5f};    // motion pixels the two motions may differ from opposite
inline constexpr float kAgree[2] = {0.08f, 0.16f};     // largest channel difference of the two samples, encoded 0..1
inline constexpr float kStill[2] = {0.25f, 0.5f};      // motion pixels moved: not moved below, moved above
inline constexpr uint32_t kSampleStep = 16;            // every 16th pixel each way is counted for the totals

// 1 at or below `lo`, 0 at or above `hi`, smooth between.
inline float fade(float x, float lo, float hi) {
    const float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

// Where a forecast stands: `sub` of `multiplier` steps after the newest real frame (1 .. multiplier-1).
inline float ahead(uint32_t sub, uint32_t multiplier) { return multiplier >= 2 && sub >= 1 && sub < multiplier ? float(sub) / float(multiplier) : 0.0f; }
// Its presentation time: the newest frame's (b) plus that share of the last interval (b - a). 0 when the
// interval is not one a forecast is made across (not positive, or longer than a second).
inline int64_t pts(int64_t a100ns, int64_t b100ns, uint32_t sub, uint32_t multiplier) {
    const int64_t d = b100ns - a100ns;
    if (multiplier < 2 || sub < 1 || sub >= multiplier || d <= 0 || d > 10000000) return 0;
    return b100ns + (d / multiplier) * sub + ((d % multiplier) * sub + multiplier / 2) / multiplier;
}

struct Pixel {
    motion::reference::Rgb colour;   // in the frames' own colour space
    float trust = 0;                 // how much of it is the moved pixel
    float moved = 0;                 // how far the motion found here would move it, in motion pixels
    bool held = false;               // it would have moved, and is not trusted to
};

// One forecast pixel. current(x, y): the newest real frame in its own colour space; previousEncoded /
// currentEncoded(x, y): the two real frames encoded 0..1 (for AGREE); grid0 / grid1(x, y): the half-way
// grids at a whole padded-model position.
template <typename Frame, typename EncodedA, typename EncodedB, typename Grid0, typename Grid1>
Pixel forecast(const motion::Geometry& g, uint32_t x, uint32_t y, float aheadOf, Frame&& current, EncodedA&& previousEncoded, EncodedB&& currentEncoded,
               Grid0&& grid0, Grid1&& grid1) {
    using motion::reference::Rgb; using motion::reference::Vec2; using motion::reference::bilinear;
    const Vec2 at{motion::modelCoord(float(x), g.srcW, g.modelW), motion::modelCoord(float(y), g.srcH, g.modelH)};
    // Where the half-way pixel nearest `s` reads a frame, as a step from s (the grid is read inside the picture).
    const auto step = [&](auto&& grid, Vec2 s) {
        const Vec2 c{std::clamp(s.x, 0.0f, float(g.modelW - 1)), std::clamp(s.y, 0.0f, float(g.modelH - 1))};
        const Vec2 read = bilinear<Vec2>(c.x, c.y, g.modelW, g.modelH, grid);
        return Vec2{motion::gridToModel(read.x, g.padW, g.alignCorners) - c.x, motion::gridToModel(read.y, g.padH, g.alignCorners) - c.y};
    };
    const float k = 1.0f + 2.0f * aheadOf;
    Vec2 s = at;
    for (int i = 0; i < 2; ++i) { const Vec2 d = step(grid1, s); s = Vec2{at.x - k * d.x, at.y - k * d.y}; }
    const Vec2 d = step(grid1, s), back = step(grid0, s);
    const Vec2 place{s.x + d.x, s.y + d.y};                                  // in the newest frame, motion pixels
    const auto length = [](float a, float b) { return std::sqrt(a * a + b * b); };
    float trust = fade(length(s.x + k * d.x - at.x, s.y + k * d.y - at.y), kSettle[0], kSettle[1]) *
                  fade(length(back.x + d.x, back.y + d.y), kOpposite[0], kOpposite[1]);
    if (place.x < -0.5f || place.y < -0.5f || place.x > float(g.modelW) - 0.5f || place.y > float(g.modelH) - 0.5f) trust = 0.0f;
    const auto encoded = [&](auto&& frame, Vec2 m) {
        return bilinear<Rgb>(motion::modelToSource(m.x, g.srcW, g.modelW), motion::modelToSource(m.y, g.srcH, g.modelH), g.srcW, g.srcH, frame);
    };
    const Rgb first = encoded(previousEncoded, Vec2{s.x + back.x, s.y + back.y}), second = encoded(currentEncoded, place);
    trust *= fade(std::max({std::abs(first.r - second.r), std::abs(first.g - second.g), std::abs(first.b - second.b)}), kAgree[0], kAgree[1]);
    Pixel out;
    out.moved = length(place.x - at.x, place.y - at.y);
    out.held = out.moved >= kStill[1] && trust < 0.5f;
    trust *= 1.0f - fade(out.moved, kStill[0], kStill[1]);
    out.trust = trust;
    const Rgb own = current(x, y);
    if (trust <= 0.0f) { out.colour = own; return out; }
    // The moved pixel: the picture's own pixel position plus the step, so that no step is no change.
    const float qx = float(x) + (place.x - at.x) * float(g.srcW) / float(g.modelW), qy = float(y) + (place.y - at.y) * float(g.srcH) / float(g.modelH);
    const Rgb shifted = bilinear<Rgb>(qx, qy, g.srcW, g.srcH, current);
    out.colour = own + (shifted - own) * trust;
    return out;
}

} // namespace veyra::pushflow::predict

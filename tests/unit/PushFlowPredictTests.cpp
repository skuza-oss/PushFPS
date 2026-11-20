// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFlow 2's Predict, the rule for one pixel and where the frames stand in time
// (pushflow/PushFlowPredict.h). The grids here are written by hand for a known motion, so the right
// answer is known exactly. Pure: also builds with g++.
#include "veyra/pushflow/PushFlowPredict.h"
#include <cstdio>
#include <vector>

using namespace veyra::pushflow;
using motion::reference::Rgb;
using motion::reference::Vec2;

namespace {
// A picture whose value is a smooth function of position, so a shift can be checked to a fraction of a pixel.
float shade(float x, float y) { return 0.5f + 0.25f * std::sin(x * 0.21f) * std::cos(y * 0.17f) + 0.2f * std::sin((x + y) * 0.05f); }
}

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); failures += !ok; };

    // --- where the frames stand in time ---------------------------------------------------------------
    check(predict::ahead(1, 2) == 0.5f && predict::ahead(1, 4) == 0.25f && predict::ahead(3, 4) == 0.75f, "a forecast stands sub / multiplier of an interval after the real frame");
    check(predict::ahead(0, 2) == 0.0f && predict::ahead(2, 2) == 0.0f && predict::ahead(1, 1) == 0.0f, "there is none at the real frame itself, at the next one, or without a multiplier");
    check(predict::pts(1000, 334333, 1, 2) == 334333 + 166667 && predict::pts(0, 333333, 1, 3) == 333333 + 111111 && predict::pts(0, 333333, 2, 3) == 333333 + 222222,
          "its time is the real frame's plus that share of the last interval");
    {
        bool rising = true;
        for (uint32_t m = 2; m <= 8; ++m) { int64_t last = 500000; for (uint32_t s = 1; s < m; ++s) { const auto t = predict::pts(166667, 500000, s, m); rising = rising && t > last && t < 500000 + 333333; last = t; } }
        check(rising, "the forecasts of a pair come after the real frame, in order, and before the next real frame is expected");
    }
    check(predict::pts(500000, 500000, 1, 2) == 0 && predict::pts(600000, 500000, 1, 2) == 0 && predict::pts(0, 10000001, 1, 2) == 0 && predict::pts(0, 333333, 2, 2) == 0,
          "no forecast across an interval that is not positive or is longer than a second");
    check(predict::fade(0.0f, 0.5f, 1.5f) == 1.0f && predict::fade(0.5f, 0.5f, 1.5f) == 1.0f && predict::fade(1.5f, 0.5f, 1.5f) == 0.0f && predict::fade(9.0f, 0.5f, 1.5f) == 0.0f &&
          std::abs(predict::fade(1.0f, 0.5f, 1.5f) - 0.5f) < 1e-6f, "trust fades smoothly between its two limits");

    // --- one pixel ----------------------------------------------------------------------------------------
    // The motion size is half the picture, to exercise both coordinate systems; the tensors are padded.
    motion::Geometry g; g.srcW = 128; g.srcH = 72; g.modelW = 64; g.modelH = 36; g.padW = 64; g.padH = 64; g.alignCorners = true;
    // Everything moves `vx`, `vy` motion pixels a frame: a half-way pixel reads the newer frame half of
    // that further on and the older frame half of that back. `breakOpposite` makes the older motion wrong.
    struct Scene { float vx = 0, vy = 0; float breakOpposite = 0; bool cut = false; };
    const auto run = [&](const Scene& sc, uint32_t x, uint32_t y, float aheadOf) {
        const auto grid = [&](float sign, float extra) {
            return [&, sign, extra](uint32_t gx, uint32_t gy) {
                return Vec2{motion::modelToGrid(float(gx) + sign * 0.5f * sc.vx + extra, g.padW, g.alignCorners), motion::modelToGrid(float(gy) + sign * 0.5f * sc.vy, g.padH, g.alignCorners)};
            };
        };
        // The newer frame is the picture moved by one frame's motion (in picture pixels, twice the motion pixels).
        const auto current = [&](uint32_t px, uint32_t py) { const float v = shade(float(px) - 2.0f * sc.vx, float(py) - 2.0f * sc.vy); return Rgb{v, v, v}; };
        const auto previous = [&](uint32_t px, uint32_t py) { const float v = sc.cut ? 1.0f - shade(float(py), float(px)) : shade(float(px), float(py)); return Rgb{v, v, v}; };
        return predict::forecast(g, x, y, aheadOf, current, previous, current, grid(-1.0f, sc.breakOpposite), grid(1.0f, 0.0f));
    };
    const auto truth = [&](const Scene& sc, uint32_t x, uint32_t y, float aheadOf) { return shade(float(x) - 2.0f * sc.vx * (1.0f + aheadOf), float(y) - 2.0f * sc.vy * (1.0f + aheadOf)); };
    {
        const Scene still;
        bool exact = true;
        for (uint32_t y = 0; y < g.srcH; y += 7) for (uint32_t x = 0; x < g.srcW; x += 5) { const auto p = run(still, x, y, 0.5f); exact = exact && p.colour.r == shade(float(x), float(y)) && p.trust == 0.0f && !p.held; }
        check(exact, "nothing moves: the forecast is the newest frame exactly, pixel for pixel");
        const Scene barely{0.1f, 0.0f};   // 0.1 motion pixels a frame: 0.05 ahead at the half-way time
        const auto p = run(barely, 60, 30, 0.5f);
        check(p.colour.r == shade(60.0f - 0.2f, 30.0f) && p.trust == 0.0f && !p.held, "less than a quarter of a pixel is not moved: a still picture and a HUD stay exact");
    }
    for (const float aheadOf : {0.25f, 0.5f, 0.75f}) {
        const Scene pan{3.0f, -1.0f};   // 3 motion pixels right and 1 up a frame = 6 and 2 picture pixels
        double worst = 0, hold = 0; bool trusted = true, never = true;
        for (uint32_t y = 16; y < g.srcH - 16; y += 3) for (uint32_t x = 24; x < g.srcW - 24; x += 3) {
            const auto p = run(pan, x, y, aheadOf);
            worst = std::max(worst, double(std::abs(p.colour.r - truth(pan, x, y, aheadOf))));
            hold = std::max(hold, double(std::abs(shade(float(x) - 6.0f, float(y) + 2.0f) - truth(pan, x, y, aheadOf))));
            trusted = trusted && p.trust > 0.99f; never = never && !p.held;
        }
        std::printf("INFO ahead %.2f: a pan's forecast is off by at most %.4f where holding the newest frame is off by %.4f\n", aheadOf, worst, hold);
        check(trusted && never && worst < 0.01 && hold > 10 * worst, "a pan is forecast to where it will be: far closer to the true later frame than holding");
    }
    {
        const Scene pan{3.0f, 0.0f};
        const auto inside = run(pan, 64, 36, 0.5f);
        Scene lost = pan; lost.breakOpposite = 4.0f;      // the older frame's motion is not the opposite of the newer one's
        const auto occluded = run(lost, 64, 36, 0.5f);
        check(inside.trust > 0.99f && occluded.trust == 0.0f && occluded.held && occluded.colour.r == shade(64.0f - 6.0f, 36.0f),
              "where the two motions are not opposite the pixel is held: it shows the newest frame, unmoved");
        Scene slightly = pan; slightly.breakOpposite = 1.0f;
        const auto between = run(slightly, 64, 36, 0.5f);
        check(between.trust > 0.0f && between.trust < 1.0f, "in between it is trusted in part");
        Scene cut = pan; cut.cut = true;                   // the older frame is another picture
        int held = 0, all = 0;
        for (uint32_t y = 16; y < g.srcH - 16; y += 3) for (uint32_t x = 24; x < g.srcW - 24; x += 3) { const auto p = run(cut, x, y, 0.5f); ++all; held += p.held; }
        std::printf("INFO a cut: %d of %d pixels held\n", held, all);
        check(held * 10 > all * 6, "across a cut the two frames do not agree where the motion joins them: most of the picture is held");
        const auto edge = run(pan, 2, 36, 0.5f);           // what a forecast pixel at the left edge needs lies outside the picture
        check(edge.trust == 0.0f && edge.colour.r == shade(2.0f - 6.0f, 36.0f), "a pixel whose source lies outside the picture is held");
    }
    {
        // Motion that grows across the picture (a zoom): the search for the source must still settle.
        const auto grid1 = [&](uint32_t gx, uint32_t gy) { return Vec2{motion::modelToGrid(float(gx) + 0.02f * (float(gx) - 32.0f), g.padW, true), motion::modelToGrid(float(gy) + 0.02f * (float(gy) - 18.0f), g.padH, true)}; };
        const auto grid0 = [&](uint32_t gx, uint32_t gy) { return Vec2{motion::modelToGrid(float(gx) - 0.02f * (float(gx) - 32.0f), g.padW, true), motion::modelToGrid(float(gy) - 0.02f * (float(gy) - 18.0f), g.padH, true)}; };
        const auto flat = [&](uint32_t, uint32_t) { return Rgb{0.5f, 0.5f, 0.5f}; };
        const auto p = predict::forecast(g, 124, 68, 0.5f, flat, flat, flat, grid0, grid1);
        check(p.trust > 0.9f && p.moved > 0.5f && !p.held, "a slow zoom settles and is trusted");
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

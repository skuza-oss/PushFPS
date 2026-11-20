// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFlow's motion contract (pushflow/PushFlowMotion.h, pure):
//  - the size the motion is worked out at: every tier's own size is what it was, and a chosen size
//    is used whatever the tier;
//  - grid values, model positions and full-size positions convert both ways, for both corner
//    conventions, odd sizes, padding and a network smaller than the picture;
//  - the reference rebuild reads the real frames where the grids point (still, shifted by whole and
//    by part pixels, off the edge) and blends them with the mask, either way round, and never leaves
//    the range between its two samples.
#include "veyra/pushflow/PushFlowMotion.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace veyra::pushflow::motion;
using reference::Rgb;
using reference::Vec2;
static int failures = 0;
static void check(bool ok, const std::string& label) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str()); if (!ok) ++failures; }
static bool about(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }

// A small scene: frames, grids and mask as the backend holds them.
struct Scene {
    Geometry g;
    std::vector<Rgb> previous, current;
    std::vector<Vec2> grid0, grid1;   // padW x padH
    std::vector<float> mask;          // padW x padH
    explicit Scene(Geometry geometry) : g(geometry) {
        previous.resize(size_t(g.srcW) * g.srcH); current.resize(previous.size());
        grid0.resize(size_t(g.padW) * g.padH); grid1.resize(grid0.size()); mask.assign(grid0.size(), 0.5f);
        shift(grid0, 0, 0); shift(grid1, 0, 0);
    }
    // Grids that read the frame `dx`, `dy` full-size pixels away from each pixel.
    void shift(std::vector<Vec2>& grid, float dx, float dy) const {
        for (uint32_t y = 0; y < g.padH; ++y)
            for (uint32_t x = 0; x < g.padW; ++x) {
                // The full-size position this model pixel stands for, moved.
                const float sx = modelToSource(float(x), g.srcW, g.modelW) + dx, sy = modelToSource(float(y), g.srcH, g.modelH) + dy;
                grid[size_t(y) * g.padW + x] = {sourceToGrid(sx, g.srcW, g.modelW, g.padW, g.alignCorners), sourceToGrid(sy, g.srcH, g.modelH, g.padH, g.alignCorners)};
            }
    }
    Rgb at(uint32_t x, uint32_t y) const {
        return reference::rebuild(g, x, y,
            [&](uint32_t px, uint32_t py) { return previous[size_t(py) * g.srcW + px]; }, [&](uint32_t px, uint32_t py) { return current[size_t(py) * g.srcW + px]; },
            [&](uint32_t px, uint32_t py) { return grid0[size_t(py) * g.padW + px]; }, [&](uint32_t px, uint32_t py) { return grid1[size_t(py) * g.padW + px]; },
            [&](uint32_t px, uint32_t py) { return mask[size_t(py) * g.padW + px]; });
    }
};
// A picture whose value is a smooth function of position, so part-pixel reads can be checked exactly.
static Rgb ramp(float x, float y, uint32_t w, uint32_t h) { return {x / float(w - 1), y / float(h - 1), 0.25f}; }

int main() {
    // --- coordinates -----------------------------------------------------------------------------
    {
        bool roundTrip = true, centres = true;
        for (bool align : {true, false})
            for (uint32_t pad : {1u, 2u, 31u, 64u, 257u, 1920u})
                for (float x : {0.0f, 0.5f, 1.0f, 7.25f, float(pad) - 1.0f}) {
                    if (pad == 1 && align) continue;   // one sample: every grid value reads it
                    roundTrip = roundTrip && about(gridToModel(modelToGrid(x, pad, align), pad, align), x, 1e-3f);
                }
        check(roundTrip, "grid value <-> padded model position converts both ways (both corner conventions, odd sizes)");
        // GridSample: with align_corners -1 and +1 are the centres of the first and last sample; without,
        // they are the outer edges of the first and last sample.
        centres = about(gridToModel(-1, 64, true), 0) && about(gridToModel(1, 64, true), 63) && about(gridToModel(-1, 64, false), -0.5f) && about(gridToModel(1, 64, false), 63.5f);
        check(centres, "-1 and +1 mean the outer sample centres with align_corners and the outer edges without");
        check(about(modelCoord(0, 3840, 960), -0.375f) && about(modelCoord(3839, 3840, 960), 959.375f), "a 4K pixel reads a 960-wide motion field at its own centre");
        check(about(modelToSource(modelCoord(1234, 3840, 960), 3840, 960), 1234, 1e-2f), "picture -> motion field -> picture comes back to the same pixel");
        bool identity = true;
        for (bool align : {true, false})
            for (uint32_t src : {256u, 1920u, 3840u})
                for (uint32_t model : {128u, 960u})
                    for (uint32_t pad : {model, model + 32})
                        for (float x : {0.0f, 3.0f, 100.5f, float(src) - 1.0f})
                            identity = identity && about(gridToSource(sourceToGrid(x, src, model, pad, align), src, model, pad, align), x, 2e-2f);
        check(identity, "the grid value made for a full-size position reads that position (padding and a smaller network included)");
        check(!Geometry{}.valid() && Geometry{256, 160, 128, 80, 128, 96}.valid() && !Geometry{256, 160, 128, 80, 64, 96}.valid(), "a geometry needs sizes, and padding never smaller than the model");
    }
    // --- the size the motion is worked out at ----------------------------------------------------
    {
        const auto same = [](double a, double b) { return std::fabs(a - b) < 1e-6; };
        // The tiers' own sizes, as they were before the motion size could be chosen (copied from the
        // backend's header at 3.2.6).
        const auto before = [](uint32_t tier, uint32_t width, uint32_t height) {
            static constexpr double kMaxPixels[3] = {960.0 * 540.0, 1280.0 * 720.0, 1920.0 * 1080.0};
            const double area = std::max(1.0, double(width) * double(height));
            const double cap = std::sqrt(kMaxPixels[std::min<uint32_t>(tier, 2)] / area);
            return std::min(tier == 0 ? 0.5 : 1.0, cap);
        };
        bool unchanged = true;
        const uint32_t sizes[][2] = {{640, 360}, {1280, 720}, {1920, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}, {256, 160}, {1, 1}, {7680, 4320}};
        for (uint32_t tier = 0; tier < 6; ++tier)
            for (const auto& sz : sizes)
                unchanged = unchanged && networkScale(tier, sz[0], sz[1]) == before(tier, sz[0], sz[1]) && networkScale(tier, sz[0], sz[1], 0) == before(tier, sz[0], sz[1]) &&
                            networkScale(tier, sz[0], sz[1], 333) == before(tier, sz[0], sz[1]) && networkScale(tier, sz[0], sz[1], -720) == before(tier, sz[0], sz[1]);
        check(unchanged, "without a chosen motion size (or with one that does not exist) every tier's size is exactly what it was");
        check(same(networkScale(2, 3840, 2160, 540), 0.25) && same(networkScale(2, 3840, 2160, 720), 1.0 / 3.0) && same(networkScale(0, 3840, 2160, 1080), 0.5),
              "a chosen size is used whatever the tier: the full model at 960 x 540 of a 4K picture, the lite one at 1920 x 1080");
        check(same(networkScale(0, 1920, 1080, 540), 0.5) && same(networkScale(0, 1920, 1080, 1080), 1.0) && same(networkScale(2, 1920, 1080, 720), 2.0 / 3.0),
              "at 1080p: 540 lines is half, 1080 lines the whole picture (Fast is no longer held to half), 720 two thirds");
        check(same(networkScale(1, 640, 360, 540), 1.0) && same(networkScale(2, 256, 160, 1080), 1.0), "a picture smaller than the chosen size is never enlarged");
        check(same(networkScale(1, 3440, 1440, 720) * 3440 * networkScale(1, 3440, 1440, 720) * 1440, 1280.0 * 720.0), "an ultrawide picture gets the same number of picture elements, in its own shape");
        bool valid = motionHeightValid(0) && motionHeightValid(540) && motionHeightValid(720) && motionHeightValid(1080) && !motionHeightValid(-1) && !motionHeightValid(1) && !motionHeightValid(2160);
        for (int lines : kMotionHeights) valid = valid && motionHeightValid(lines);
        check(valid, "the motion sizes that exist are 0 (the tier's own), 540, 720 and 1080");
        setMotionHeight(720); const bool kept = motionHeight() == 720;
        setMotionHeight(900); const bool refused = motionHeight() == 0;
        setMotionHeight(1080); setMotionHeight(0); const bool off = motionHeight() == 0;
        check(kept && refused && off, "the process-wide choice keeps a size that exists and falls back to the tier's own for one that does not");
    }
    // --- what the too-slow watch does ------------------------------------------------------------
    {
        // The work of one generated frame, as a number to compare: picture elements times the model's
        // weight (the full model costs more than the lite one at the same size).
        const auto work = [](uint32_t tier, int lines, uint32_t width, uint32_t height) {
            const double scale = networkScale(tier, width, height, lines);
            return scale * scale * double(width) * double(height) * (tier >= 2 ? 2.0 : 1.0);
        };
        bool before = slowStep(2, 0) == SlowStep::TierDown && slowStep(1, 0) == SlowStep::TierDown && slowStep(0, 0) == SlowStep::Nothing &&
                      slowStep(2, 333) == SlowStep::TierDown && slowStep(7, 0) == SlowStep::TierDown;
        check(before, "without a chosen motion size the watch lowers the tier as it always did, and does nothing at Fast");
        bool cheaper = true, ends = true;
        const uint32_t sizes[][2] = {{1920, 1080}, {2560, 1440}, {3840, 2160}, {3440, 1440}};
        for (const auto& sz : sizes)
            for (uint32_t tier = 0; tier < 3; ++tier)
                for (int lines : kMotionHeights) {
                    uint32_t t = tier; int m = lines; int steps = 0;
                    for (;; ++steps) {
                        const auto step = slowStep(t, m);
                        if (step == SlowStep::Nothing) break;
                        const double was = work(t, m, sz[0], sz[1]);
                        if (step == SlowStep::TierDown || step == SlowStep::TierDownAndMotionToAuto) --t;
                        if (step == SlowStep::MotionToAuto || step == SlowStep::TierDownAndMotionToAuto) m = 0;
                        // Never more work; and less, except for the one case that is already the floor
                        // (Fast with 540 lines chosen on a picture of 1080 lines or more is Fast's own size).
                        const double now = work(t, m, sz[0], sz[1]);
                        cheaper = cheaper && now <= was + 1e-6 && (now < was - 1e-6 || (t == 0 && m == 0));
                        if (steps > 4) { ends = false; break; }
                    }
                    ends = ends && t == 0 && m == 0 && steps <= 3;
                }
        check(cheaper, "every step of the watch makes PushFlow cheaper, whatever tier and motion size it starts from");
        check(ends, "the watch ends at Fast with its own size in at most three steps");
        check(slowStep(0, 1080) == SlowStep::MotionToAuto && slowStep(1, 1080) == SlowStep::MotionToAuto && slowStep(2, 1080) == SlowStep::TierDown &&
              slowStep(2, 540) == SlowStep::TierDown && slowStep(1, 720) == SlowStep::TierDownAndMotionToAuto && slowStep(1, 540) == SlowStep::TierDownAndMotionToAuto &&
              slowStep(0, 540) == SlowStep::MotionToAuto,
              "a size larger than the tier's own goes first; Quality keeps a smaller size and changes model; Balanced goes to Fast's own");
    }
    // --- the rebuild -----------------------------------------------------------------------------
    for (bool align : {true, false})
        for (bool half : {false, true}) {
            // 96 x 64 picture; the network at full or half size, padded to a multiple of 32.
            Geometry g{96, 64, half ? 48u : 96u, half ? 32u : 64u, half ? 64u : 96u, half ? 32u : 64u, align, true};
            const std::string name = std::string(align ? "align_corners" : "edges") + (half ? ", network at half size" : ", network at full size");
            Scene s(g);
            for (uint32_t y = 0; y < g.srcH; ++y)
                for (uint32_t x = 0; x < g.srcW; ++x) { s.previous[size_t(y) * g.srcW + x] = ramp(float(x), float(y), g.srcW, g.srcH); s.current[size_t(y) * g.srcW + x] = s.previous[size_t(y) * g.srcW + x]; }
            // Exact everywhere inside. A smaller motion field ends half a model pixel before the
            // picture does, and its last value is repeated there: the outermost picture pixel of a
            // still pair reads up to half a pixel inwards. That is how the rebuild has always worked
            // (rebuild_scaled in scripts/pushflow/prepare_runtime.py); recorded here, not changed.
            bool still = true, border = true;
            for (uint32_t y = 0; y < g.srcH; ++y)
                for (uint32_t x = 0; x < g.srcW; ++x) {
                    const auto p = s.at(x, y); const Rgb want = ramp(float(x), float(y), g.srcW, g.srcH);
                    const bool edge = x == 0 || y == 0 || x == g.srcW - 1 || y == g.srcH - 1;
                    if (!edge) still = still && about(p.r, want.r, 2e-3f) && about(p.g, want.g, 2e-3f);
                    else border = border && about(p.r, want.r, 0.6f / float(g.srcW - 1)) && about(p.g, want.g, 0.6f / float(g.srcH - 1));
                }
            check(still, "a still pair comes back as it is (" + name + ")");
            check(border, "at the picture's outermost pixel a still pair is at most half a pixel off (" + name + ")");
            // The previous frame read 3.5 pixels to the right, the current one 2 down: on a ramp the
            // value moves by exactly that much, away from the edges.
            s.shift(s.grid0, 3.5f, 0); s.shift(s.grid1, 0, 2.0f);
            std::fill(s.mask.begin(), s.mask.end(), 1.0f);
            const auto fromPrevious = s.at(40, 30);
            std::fill(s.mask.begin(), s.mask.end(), 0.0f);
            const auto fromCurrent = s.at(40, 30);
            check(about(fromPrevious.r, ramp(43.5f, 30, g.srcW, g.srcH).r, 2e-3f) && about(fromPrevious.g, ramp(40, 30, g.srcW, g.srcH).g, 2e-3f),
                  "mask 1 shows the previous frame where its grid points, part pixels included (" + name + ")");
            check(about(fromCurrent.g, ramp(40, 32, g.srcW, g.srcH).g, 2e-3f) && about(fromCurrent.r, ramp(40, 30, g.srcW, g.srcH).r, 2e-3f),
                  "mask 0 shows the current frame where its grid points (" + name + ")");
            // The other way round for models whose mask is the current frame's share.
            Scene r(g); r.g.maskForFirst = false; r.previous = s.previous; r.current = s.current; r.grid0 = s.grid0; r.grid1 = s.grid1;
            std::fill(r.mask.begin(), r.mask.end(), 0.0f);
            check(about(r.at(40, 30).r, fromPrevious.r, 1e-5f), "a mask that is the current frame's share is read the other way round (" + name + ")");
            // Off the edge: the frame's edge repeats, nothing wraps and nothing is black.
            s.shift(s.grid0, 500.0f, 0); std::fill(s.mask.begin(), s.mask.end(), 1.0f);
            check(about(s.at(40, 30).r, 1.0f, 2e-3f), "a read off the right edge gives the edge (" + name + ")");
            s.shift(s.grid0, -500.0f, -500.0f);
            check(about(s.at(40, 30).r, 0.0f, 2e-3f) && about(s.at(40, 30).g, 0.0f, 2e-3f), "a read off the top left corner gives the corner (" + name + ")");
        }
    // --- a blend never leaves the range between its two samples ---------------------------------
    {
        Geometry g{128, 64, 128, 64, 128, 64, true, true};
        Scene s(g);
        for (uint32_t y = 0; y < g.srcH; ++y)
            for (uint32_t x = 0; x < g.srcW; ++x) {
                const bool in = x >= 30 && x < 50 && y >= 20 && y < 44;
                s.previous[size_t(y) * g.srcW + x] = in ? Rgb{0.9f, 0.85f, 0.2f} : Rgb{0.1f, 0.12f, 0.15f};
                s.current[size_t(y) * g.srcW + x] = Rgb{0.1f, 0.12f, 0.15f};
            }
        bool between = true, ends = true;
        for (float m = 0; m <= 1.0001f; m += 0.05f) {
            std::fill(s.mask.begin(), s.mask.end(), m);
            const Rgb p = s.at(35, 30);
            between = between && p.r >= 0.1f - 1e-4f && p.r <= 0.9f + 1e-4f && p.g >= 0.12f - 1e-4f && p.g <= 0.85f + 1e-4f;
        }
        std::fill(s.mask.begin(), s.mask.end(), 1.0f); ends = ends && about(s.at(35, 30).r, 0.9f);
        std::fill(s.mask.begin(), s.mask.end(), 0.0f); ends = ends && about(s.at(35, 30).r, 0.1f);
        std::fill(s.mask.begin(), s.mask.end(), 0.5f); ends = ends && about(s.at(35, 30).r, 0.5f);
        std::fill(s.mask.begin(), s.mask.end(), 7.0f); ends = ends && about(s.at(35, 30).r, 0.9f);    // a mask outside 0..1 is clamped
        std::fill(s.mask.begin(), s.mask.end(), -3.0f); ends = ends && about(s.at(35, 30).r, 0.1f);
        check(between, "a blended pixel lies between its two samples, whatever the mask");
        check(ends, "mask 1, 0 and 0.5 give the previous sample, the current one and their even mix; a mask outside 0..1 is clamped");
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

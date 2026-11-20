// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Comparison clips, the part without graphics (engine/RecordingComparePlan.h): which layouts take a
// chosen quality and what that does to the size, the bitrate and the encoder preset; when the Reveal
// layout's line is where, that it moves evenly, and what the line looks like.
#include "veyra/engine/RecordingComparePlan.h"
#include <cstdio>
#include <string>
using namespace veyra::engine;
static int failures = 0;
static void check(bool ok, const std::string& label) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str()); if (!ok) ++failures; }
static bool about(double a, double b, double eps = 1e-9) { return std::abs(a - b) <= eps; }

int main() {
    // ---- Clip quality ---------------------------------------------------------------------------
    // Automatic (0) is what every clip was before: 1440 at most, in every layout.
    for (const auto layout : {CompareLayout::Split, CompareLayout::SideBySide, CompareLayout::Vertical, CompareLayout::Reveal}) {
        check(compareClipHeightCap(layout, 0) == 1440, "automatic: at most 1440 high, layout " + std::to_string(int(layout)));
        check(!compareClipHeightChosen(layout, 0), "automatic is not a chosen quality, layout " + std::to_string(int(layout)));
    }
    // A chosen height: Split and Reveal only. Side by side and Vertical stay exactly as they are.
    for (const uint32_t h : {1080u, 1440u, 2160u}) {
        check(compareClipHeightCap(CompareLayout::Split, h) == h && compareClipHeightChosen(CompareLayout::Split, h), "split takes " + std::to_string(h));
        check(compareClipHeightCap(CompareLayout::Reveal, h) == h && compareClipHeightChosen(CompareLayout::Reveal, h), "reveal takes " + std::to_string(h));
        check(compareClipHeightCap(CompareLayout::SideBySide, h) == 1440 && !compareClipHeightChosen(CompareLayout::SideBySide, h), "side by side ignores " + std::to_string(h));
        check(compareClipHeightCap(CompareLayout::Vertical, h) == 1440 && !compareClipHeightChosen(CompareLayout::Vertical, h), "vertical ignores " + std::to_string(h));
    }
    check(compareClipHeightCap(CompareLayout::Reveal, 4320) == 2160, "never above 2160");
    check(compareClipHeightCap(CompareLayout::Reveal, 500) == 1440 && !compareClipHeightChosen(CompareLayout::Reveal, 500), "a height that is no choice counts as automatic");
    check(compareClipHeightCap(CompareLayout::Split, 1081) == 1080, "an odd height is made even");
    // A third more bits, never past the recorder's 160 Mbps, and nothing from nothing.
    check(compareClipBitrateMbps(37) == 48 && compareClipBitrateMbps(56) == 73 && compareClipBitrateMbps(100) == 130, "a third more bits");
    check(compareClipBitrateMbps(149) == 160 && compareClipBitrateMbps(160) == 160, "up to 160 Mbps");
    check(compareClipBitrateMbps(0) == 0, "no bitrate stays none");
    for (uint32_t mbps = 1; mbps <= 160; ++mbps) if (compareClipBitrateMbps(mbps) < mbps) { check(false, "a chosen quality never lowers the bitrate"); break; }
    // The preset: untouched up to 1440 high, whatever the rate (that is how clips were encoded so far).
    for (const uint32_t preset : {1u, 4u, 7u})
        for (const uint32_t fps : {60u, 120u, 180u, 240u})
            if (compareClipPreset(preset, 2560, 1440, fps) != preset || compareClipPreset(preset, 1920, 1080, fps) != preset) check(false, "up to 1440p the configured preset is kept");
    check(compareClipPreset(4, 2560, 1440, 240) == 4, "1440p at 240 FPS keeps the configured preset");
    // 4K: as configured up to 60 FPS, faster beyond.
    check(compareClipPreset(4, 3840, 2160, 60) == 4 && compareClipPreset(7, 3840, 2160, 60) == 7, "4K60 as configured");
    check(compareClipPreset(4, 3840, 2160, 90) == 2 && compareClipPreset(4, 3840, 2160, 120) == 2, "4K at 90 and 120 FPS: a fast preset");
    check(compareClipPreset(4, 3840, 2160, 144) == 1 && compareClipPreset(4, 3840, 2160, 180) == 1 && compareClipPreset(4, 3840, 2160, 240) == 1, "4K above 126 FPS: the fastest");
    check(compareClipPreset(1, 3840, 2160, 60) == 1 && compareClipPreset(2, 3840, 2160, 120) == 2, "a faster configured preset stays");
    check(compareClipPreset(0, 3840, 2160, 60) == 1 && compareClipPreset(99, 1920, 1080, 60) == 7, "out-of-range presets are brought in range");
    // When to say that frames were lost.
    check(!compareClipFellBehind(1800, 0) && !compareClipFellBehind(1800, 29), "a few lost frames are not worth a notice");
    check(!compareClipFellBehind(7200, 100), "nor is 1 in 72");
    check(compareClipFellBehind(1800, 60) && compareClipFellBehind(1200, 600), "30 or more and over 2 in 100 is");

    // ---- The Reveal layout's timing --------------------------------------------------------------
    check(compareRevealInterval(3) == 3 && compareRevealInterval(0) == 1 && compareRevealInterval(-5) == 1 && compareRevealInterval(100) == 30, "the interval is 1 to 30 seconds");
    check(compareRevealInterval(std::nan("")) == 3, "and 3 when it is no number");
    check(about(compareRevealSweepSeconds(1), 0.5) && about(compareRevealSweepSeconds(2), 0.6) && about(compareRevealSweepSeconds(3), 0.9) &&
          about(compareRevealSweepSeconds(5), 1.2) && about(compareRevealSweepSeconds(8), 1.2), "the sweep takes 0.5 to 1.2 seconds");
    for (const double every : {1.0, 2.0, 3.0, 5.0, 8.0}) {
        const std::string name = "every " + std::to_string(int(every)) + " s: ";
        const double sweep = compareRevealSweepSeconds(every), hold = every - sweep;
        check(hold >= 0.5, name + "each version holds for at least half a second");
        check(compareRevealPosition(0, every) == 0 && compareRevealPosition(hold * 0.5, every) == 0 && compareRevealPosition(hold, every) == 0, name + "opens on the console side");
        check(about(compareRevealPosition(hold + sweep / 2, every), 0.5), name + "half way at the middle of the sweep");
        check(about(compareRevealPosition(every, every), 1.0) && compareRevealPosition(every + hold * 0.5, every) == 1.0, name + "then all PushFPS, and holds");
        check(about(compareRevealPosition(every + hold + sweep / 2, every), 0.5), name + "sweeps back");
        check(about(compareRevealPosition(2 * every, every), 0.0, 1e-9) && compareRevealPosition(2 * every + hold * 0.5, every) == 0, name + "to the console side again");
        check(about(compareRevealPosition(6 * every + hold + sweep / 4, every), compareRevealPosition(hold + sweep / 4, every), 1e-9), name + "and repeats");
        // A sweep starts every `every` seconds: the first movement of each lies that far apart.
        check(compareRevealPosition(hold + 0.05, every) > 0 && compareRevealPosition(every + hold + 0.05, every) < 1 && compareRevealPosition(2 * every + hold + 0.05, every) > 0,
              name + "a sweep starts every interval");
        // Smooth: sampled as a 120 FPS clip, the line never jumps back, never leaps (at most 1.9 times
        // its average speed, the ease's peak) and gathers and loses speed gradually.
        const double dt = 1.0 / 120.0;
        bool monotone = true, gentle = true, eased = true;
        double before = 0, stepBefore = 0;
        const double average = dt / sweep;
        for (int i = 1; i * dt <= every; ++i) {
            const double p = compareRevealPosition(i * dt, every), step = p - before;
            if (step < -1e-12) monotone = false;
            if (step > average * 1.9) gentle = false;
            if (std::abs(step - stepBefore) > average * 0.2) eased = false;   // no frame changes the speed by more than a fifth of the average
            before = p; stepBefore = step;
        }
        check(monotone, name + "the line only moves forward during a sweep");
        check(gentle, name + "no leap");
        check(eased, name + "speed builds up and dies down gradually");
    }
    check(compareRevealPosition(-1, 3) == 0 && compareRevealPosition(std::nan(""), 3) == 0 && compareRevealPosition(INFINITY, 3) == 0, "odd times count as the start");
    {   // An hour in, the place is as exact as in the first seconds (a clip can run until it is stopped).
        const double every = 3, hold = every - compareRevealSweepSeconds(every);
        check(about(compareRevealPosition(3600 + hold + 0.3, every), compareRevealPosition(hold + 0.3, every), 1e-9), "an hour into the clip the timing is the same");
    }

    // ---- Where the line is ------------------------------------------------------------------------
    for (const uint32_t width : {1920u, 2560u, 3840u}) {
        const uint32_t lineW = compareRevealLine(width * 9 / 16).width;
        const std::string name = std::to_string(width) + " wide: ";
        check(lineW >= 16 && lineW % 4 == 0, name + "the strip is a multiple of four wide");
        check(compareRevealCentre(0, width, lineW) == -int32_t(lineW / 2), name + "at 0 the whole strip is left of the picture");
        check(compareRevealCentre(1, width, lineW) == int32_t(width + lineW / 2), name + "at 1 the whole strip is right of it");
        check(compareRevealCentre(0.5, width, lineW) == int32_t(width / 2), name + "half way it is in the middle");
        bool even = true, ordered = true;
        int32_t last = compareRevealCentre(0, width, lineW);
        for (int i = 0; i <= 1000; ++i) {
            const int32_t x = compareRevealCentre(i / 1000.0, width, lineW);
            if (x & 1) even = false;
            if (x < last) ordered = false;
            last = x;
        }
        check(even, name + "always an even x (the colour planes are half size)");
        check(ordered, name + "further right as the share grows");
        check(compareRevealCentre(-3, width, lineW) == compareRevealCentre(0, width, lineW) && compareRevealCentre(7, width, lineW) == compareRevealCentre(1, width, lineW),
              name + "shares outside 0..1 are clamped");
    }

    // ---- What the line looks like -----------------------------------------------------------------
    check(compareRevealLine(0).rgba.empty() && compareRevealLine(30).rgba.empty(), "no line for a picture too low to draw one");
    for (const uint32_t height : {720u, 1080u, 1440u, 2160u, 1081u}) {
        const RecordingLabel line = compareRevealLine(height);
        const std::string name = std::to_string(height) + " high: ";
        const uint32_t W = line.width, H = line.height;
        check(H == (height & ~1u) && W % 4 == 0 && line.rgba.size() == size_t(W) * H * 4, name + "a strip as high as the picture");
        check(W <= height / 8, name + "narrow (" + std::to_string(W) + " px)");
        const auto at = [&](uint32_t x, uint32_t y) { return &line.rgba[(size_t(y) * W + x) * 4]; };
        // The core: opaque white down the middle, away from the handle.
        const uint8_t* core = at(W / 2, H / 8);
        check(core[3] == 255 && core[0] >= 250 && core[1] >= 250 && core[2] >= 250, name + "a white core");
        // The glow: green, fading out towards the strip's edges, never a hard edge.
        const uint8_t* glow = at(W / 2 + std::max(3u, H / 240), H / 8);
        check(glow[1] > glow[0] && glow[1] > glow[2] && glow[3] > 40 && glow[3] < 255, name + "a green glow beside it");
        check(at(0, H / 8)[3] < at(W / 4, H / 8)[3] && at(W / 4, H / 8)[3] < glow[3], name + "that fades outwards");
        check(at(0, H / 8)[3] <= 32 && at(W - 1, H / 8)[3] <= 32, name + "to almost nothing at the strip's edges");
        // The handle: a dark disc with a green ring in the middle of the height.
        const uint32_t cx = W / 2, cy = H / 2;
        uint32_t radius = 0;
        while (cx + radius + 1 < W && at(cx + radius + 1, cy)[3] > 200) ++radius;
        check(radius >= 9 && radius * 2 + 2 <= W, name + "a round handle that fits the strip (radius " + std::to_string(radius) + ")");
        const uint8_t* ring = at(cx + radius - 1, cy);
        check(ring[1] > 200 && ring[1] > ring[0] + 60 && ring[1] > ring[2] + 60, name + "with a green ring");
        const uint8_t* disc = at(cx, cy - radius / 2 - radius / 4);
        check(disc[3] == 255 && disc[0] < 60 && disc[1] < 60 && disc[2] < 60, name + "and a dark face");
        // Two white chevrons, pointing outwards: bright somewhere on the handle's middle row, both sides.
        bool leftChevron = false, rightChevron = false;
        for (uint32_t x = cx - radius + 2; x < cx; ++x) if (at(x, cy)[0] > 220 && at(x, cy)[2] > 220) leftChevron = true;
        for (uint32_t x = cx + 1; x < cx + radius - 2; ++x) if (at(x, cy)[0] > 220 && at(x, cy)[2] > 220) rightChevron = true;
        check(leftChevron && rightChevron, name + "and a chevron on each side");
        // Mirrored left to right and top to bottom, so it looks the same moving either way.
        bool mirrored = true;
        for (uint32_t y = 0; y < H && mirrored; y += 7)
            for (uint32_t x = 0; x < W / 2; ++x) {
                const uint8_t* a = at(x, y); const uint8_t* b = at(W - 1 - x, y); const uint8_t* c = at(x, H - 1 - y);
                for (int k = 0; k < 4; ++k) if (std::abs(int(a[k]) - int(b[k])) > 1 || std::abs(int(a[k]) - int(c[k])) > 1) mirrored = false;
            }
        check(mirrored, name + "symmetrical");
        // Away from the handle every row is the same: the line is straight.
        bool straight = true;
        for (uint32_t x = 0; x < W; ++x) for (int k = 0; k < 4; ++k) if (at(x, 4)[k] != at(x, H / 4)[k]) straight = false;
        check(straight, name + "a straight line above and below the handle");
    }
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}

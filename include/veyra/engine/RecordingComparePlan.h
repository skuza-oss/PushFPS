#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFPS comparison clips (engine/RecordingCompare.h): the part without graphics. The label type and
// the layouts, the rules for a clip recorded at a chosen quality, and the Reveal layout's timing and
// line. Free of Windows headers, so tests/unit/PushComparePlanTests.cpp builds anywhere.
//
// The Reveal layout (CompareLayout::Reveal): one whole picture, no split. It opens on the console's
// own frames; a line then sweeps from the left edge to the right and uncovers what PushFPS shows; it
// holds, sweeps back, and so on for as long as the clip runs. A sweep starts every
// compareRevealInterval seconds. Left of the line is every presented frame, right of it the console
// side, both of the same moment. When the line is where: compareRevealPosition. Where that is in
// pixels: compareRevealCentre. What the line looks like: compareRevealLine.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace veyra::engine {

// A label. luma: full-range grey levels (255 white, 0 black), copied in as
// limited-range luma with neutral colour. rgba (preferred when present):
// straight-alpha sRGB RGBA8, blended onto the picture. Width and height are
// rounded down to even numbers when placed.
struct RecordingLabel {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> luma;
    std::vector<uint8_t> rgba;
};

// Split: one picture cut down the middle. SideBySide: both whole pictures at half size. Vertical:
// 1080 x 1920, the console above PushFPS. Reveal: the whole picture, uncovered by a sweeping line.
enum class CompareLayout : uint32_t { Split = 0, SideBySide = 1, Vertical = 2, Reveal = 3 };

// ---- Clip quality (RecordingSettings::compareMaxHeight): Split and Reveal, whose picture fills the
// frame, can be recorded at a height the user chooses. 0 is automatic, which is what every such clip
// was before 3.2.5: at most 1440 high, the standard bitrate, the configured encoder preset.
inline bool compareClipHeightChosen(CompareLayout layout, uint32_t chosen) {
    return (layout == CompareLayout::Split || layout == CompareLayout::Reveal) && chosen >= 720;
}
// The highest a comparison clip of this layout is recorded: the chosen height (720 to 2160) for Split
// and Reveal, else 1440. A larger picture is scaled down to it; a smaller one is never enlarged.
inline uint32_t compareClipHeightCap(CompareLayout layout, uint32_t chosen) {
    return compareClipHeightChosen(layout, chosen) ? (chosen > 2160 ? 2160u : chosen & ~1u) : 1440u;
}
// A clip at a chosen height gets a third more bits than a recording of that size, up to the 160 Mbps
// every recording stops at.
inline uint32_t compareClipBitrateMbps(uint32_t standardMbps) {
    return standardMbps ? std::min<uint32_t>(160, (standardMbps * 13 + 5) / 10) : 0u;
}
// And the encoder preset (1 fastest to 7 slowest) it is encoded with, for width x height at fps. Up
// to 1440 high: the configured one, as every clip was encoded so far. Above (4K): the configured one
// up to about 60 frames a second, a faster one beyond, because a comparison clip records every
// presented frame and an encoder that falls behind skips frames. A clip that stutters is worse than
// one encoded a little less thoroughly, and at these bitrates the presets look alike.
inline uint32_t compareClipPreset(uint32_t configured, uint32_t width, uint32_t height, uint32_t fps) {
    const uint32_t preset = std::clamp<uint32_t>(configured, 1, 7);
    if (height <= 1440) return preset;
    const uint64_t rate = uint64_t(width) * height * fps;            // pixels a second
    const uint32_t limit = rate <= 520000000ull ? 7u                 // 4K up to 62 FPS: as configured
                         : rate <= 1050000000ull ? 2u                // 4K up to 126 FPS
                         : 1u;                                       // beyond
    return std::min(preset, limit);
}
// Whether a clip lost enough frames to be worth saying: 30 or more, and over 2 in 100.
inline bool compareClipFellBehind(uint64_t written, uint64_t dropped) { return dropped >= 30 && dropped * 50 > written + dropped; }

// ---- The Reveal layout.
// How often the line sweeps: one sweep starts every so many seconds (the user's choice; 3 unless set).
inline double compareRevealInterval(double seconds) { return std::isfinite(seconds) ? std::clamp(seconds, 1.0, 30.0) : 3.0; }
// How long the line takes to cross, inside that interval: quick for a short interval, never rushed
// and never slow. The rest of the interval the picture holds still on one version.
inline double compareRevealSweepSeconds(double interval) { return std::clamp(0.3 * compareRevealInterval(interval), 0.5, 1.2); }
// The share of the picture's width that shows PushFPS, from the left edge, `seconds` into the clip:
// 0 (all console) for a hold, up to 1 over a sweep, then 1 (all PushFPS) for a hold and back to 0
// over a sweep, and so on. The sweep eases in and out (smootherstep: no speed and no acceleration
// at either end), so the line starts and stops without a jolt, and its place follows from the
// frame's time alone: every frame of the clip puts it exactly where it belongs at that moment.
inline double compareRevealPosition(double seconds, double interval) {
    if (!(seconds > 0) || !std::isfinite(seconds)) return 0.0;
    const double every = compareRevealInterval(interval), sweep = compareRevealSweepSeconds(interval), hold = every - sweep;
    double u = std::fmod(seconds, 2.0 * every);
    const bool back = u >= every;
    if (back) u -= every;
    double p = 0.0;
    if (u > hold) {
        const double x = std::min(1.0, (u - hold) / sweep);
        p = x * x * x * (x * (6.0 * x - 15.0) + 10.0);
    }
    return back ? 1.0 - p : p;
}
// The x of the line's centre in a picture `width` wide, for a line strip `lineWidth` wide (even
// numbers). At 0 and at 1 the whole strip is off the picture: it slides in over the left edge and
// out over the right one.
inline int32_t compareRevealCentre(double position, uint32_t width, uint32_t lineWidth) {
    const double p = std::clamp(position, 0.0, 1.0);
    const int32_t x = int32_t(std::lround(p * double(width + lineWidth))) - int32_t(lineWidth / 2);
    return x - (x & 1);
}
// The line: a strip `height` high (rounded down to even) and a multiple of four wide, straight-alpha
// sRGB. A white core in a soft green glow, and in the middle a round graphite handle with a green
// ring and two chevrons, like the handle of a before/after slider. Empty when the picture is too
// low to draw one.
RecordingLabel compareRevealLine(uint32_t height);

} // namespace veyra::engine

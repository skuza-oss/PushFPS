// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS comparison clips, the Reveal layout: the picture of the sweeping line (RecordingComparePlan.h).
#include "veyra/engine/RecordingComparePlan.h"

namespace veyra::engine {
namespace {
// Distance from (px, py) to the segment (ax, ay)-(bx, by).
double segmentDistance(double px, double py, double ax, double ay, double bx, double by) {
    const double vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    const double length2 = vx * vx + vy * vy;
    const double t = length2 > 0 ? std::clamp((wx * vx + wy * vy) / length2, 0.0, 1.0) : 0.0;
    return std::hypot(wx - t * vx, wy - t * vy);
}
// Coverage of a shape whose edge is `distance` away (negative inside), one pixel of anti-aliasing.
double coverage(double distance) { return std::clamp(0.5 - distance, 0.0, 1.0); }
} // namespace

RecordingLabel compareRevealLine(uint32_t height) {
    RecordingLabel out;
    const uint32_t H = height & ~1u;
    if (H < 32) return out;
    // Sizes follow the picture's height, so the line looks the same at 1080p, 1440p and 4K.
    const double radius = std::max(10.0, H / 40.0);          // the handle
    const double core = std::max(1.0, H / 720.0);            // half the white core's width
    const double glow = std::max(3.0, H / 160.0);            // how far the glow reaches (1/e)
    const double ring = std::max(1.5, radius / 7.0);
    const double stroke = std::max(1.5, radius / 6.5);       // the chevrons
    const uint32_t W = (uint32_t(std::ceil(radius)) * 2 + 12 + 3) & ~3u;
    out.width = W; out.height = H;
    out.rgba.assign(size_t(W) * H * 4, 0);
    const double cx = W / 2.0, cy = H / 2.0;
    // The brand's green, white, and the graphite of the badges.
    const double green[3] = {141, 242, 55}, white[3] = {255, 255, 255}, graphite[3] = {16, 18, 22};
    for (uint32_t y = 0; y < H; ++y) {
        const double dy = y + 0.5 - cy;
        const bool nearHandle = std::abs(dy) <= radius + 1.0;
        for (uint32_t x = 0; x < W; ++x) {
            const double sx = x + 0.5 - cx, dx = std::abs(sx);
            // Premultiplied colour, built from the bottom layer up.
            double r = 0, g = 0, b = 0, a = 0;
            const auto over = [&](const double colour[3], double alpha) {
                r = colour[0] * alpha + r * (1 - alpha); g = colour[1] * alpha + g * (1 - alpha);
                b = colour[2] * alpha + b * (1 - alpha); a = alpha + a * (1 - alpha);
            };
            over(green, 0.78 * std::exp(-std::max(0.0, dx - core) / glow));
            over(white, coverage(dx - core));
            if (nearHandle) {
                const double d = std::hypot(sx, dy);
                over(graphite, coverage(d - radius));
                over(green, coverage(std::abs(d - (radius - ring / 2)) - ring / 2));
                // Two chevrons pointing outwards, mirrored left and right, up and down.
                const double tip = radius * 0.56, arm = radius * 0.27;
                const double chevron = segmentDistance(dx, std::abs(dy), tip, 0, tip - arm, arm);
                over(white, coverage(chevron - stroke / 2));
            }
            uint8_t* p = &out.rgba[(size_t(y) * W + x) * 4];
            if (a > 0.002) {
                p[0] = uint8_t(std::clamp(r / a + 0.5, 0.0, 255.0)); p[1] = uint8_t(std::clamp(g / a + 0.5, 0.0, 255.0));
                p[2] = uint8_t(std::clamp(b / a + 0.5, 0.0, 255.0)); p[3] = uint8_t(std::clamp(a * 255.0 + 0.5, 0.0, 255.0));
            }
        }
    }
    return out;
}

} // namespace veyra::engine

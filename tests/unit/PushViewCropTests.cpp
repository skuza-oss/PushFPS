// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Crop of the picture in the view (engine/PreviewView.h): the kept part is what fits, fills or is
// stretched, the size returned stays the whole picture's so the mouse and the presenter share one
// transform, and without a crop every result is exactly what it was before the crop existed.
#include "veyra/engine/PreviewView.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace veyra::engine;
static int failures = 0;
static void check(bool ok, const std::string& label) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str()); if (!ok) ++failures; }
static bool about(float a, float b, float tolerance = 0.01f) { return std::fabs(a - b) <= tolerance; }

// The view's sizes as they were computed before the crop existed (copied from the header at 3.2.5).
static std::pair<float, float> before(const PreviewView& v, float cw, float ch, float iw, float ih) {
    if (cw <= 0 || ch <= 0 || iw <= 0 || ih <= 0) return {0, 0};
    const float orientedH = (v.quarterTurns() & 1) ? iw : ih;
    if (v.mode == 1) return {(v.quarterTurns() & 1 ? ih : iw) * v.zoom, orientedH * v.zoom};
    if (v.mode == 3) return {cw * v.zoom, ch * v.zoom};
    const float virtualW = float(v.aspect(iw, ih)) * orientedH;
    const float scale = (v.mode == 2 ? std::max(cw / virtualW, ch / orientedH) : std::min(cw / virtualW, ch / orientedH)) * v.zoom;
    return {virtualW * scale, orientedH * scale};
}

int main() {
    // --- without a crop nothing differs, bit for bit ------------------------------------------------
    {
        bool same = true;
        const float clients[][2] = {{1920, 1080}, {3440, 1440}, {2560, 1440}, {1280, 1024}, {1001, 777}, {3840, 2160}};
        const float images[][2] = {{1920, 1080}, {3840, 2160}, {1280, 720}, {720, 480}, {2560, 1440}};
        for (int mode = 0; mode <= 6; ++mode)
            for (float zoom : {1.0f, 0.5f, 1.37f, 2.0f})
                for (unsigned turns = 0; turns < 4; ++turns)
                    for (double dar : {0.0, 16.0 / 9, 4.0 / 3, 2.39})
                        for (auto& c : clients)
                            for (auto& im : images) {
                                PreviewView v; v.mode = mode; v.zoom = zoom; v.userQuarterTurns = turns; v.displayAspect = dar;
                                const auto a = v.renderedSize(c[0], c[1], im[0], im[1]), b = before(v, c[0], c[1], im[0], im[1]);
                                if (a.first != b.first || a.second != b.second) same = false;
                            }
        check(same, "no crop: every mode, zoom, rotation and shape gives exactly the size it gave before");
        PreviewView v;
        check(!v.cropped() && v.keptWidth() == 1.0f && v.keptHeight() == 1.0f && v.cropCenterX() == 0.5f && v.cropCenterY() == 0.5f,
              "no crop: the whole picture is kept and its middle is the middle");
        const auto k = v.keptRect(1920, 1080, 1920, 1080);
        check(k.left == 0 && k.top == 0 && k.right == 1920 && k.bottom == 1080, "no crop: the kept part is the whole drawn picture");
    }
    // --- fit: the kept part fits the window ------------------------------------------------------------
    {
        // A 16:9 picture with black bars baked in (a 21:9 game): top and bottom cut by 12.5 % each
        // leaves 16:6.75 = 2.37:1. On a 3440 x 1440 screen (2.389:1) it now fills the height.
        PreviewView v; v.cropTop = 0.125f; v.cropBottom = 0.125f; v.centerX = v.cropCenterX(); v.centerY = v.cropCenterY();
        const auto [rw, rh] = v.renderedSize(3440, 1440, 1920, 1080);
        check(about(rh * v.keptHeight(), 1440) && about(rw, 1440.0f / 0.75f * 16 / 9), "fit: the kept part is as high as the window; the picture is drawn larger than the window");
        const auto k = v.keptRect(3440, 1440, rw, rh);
        check(about(k.top, 0) && about(k.bottom, 1440) && about(k.right - k.left, rw) && about((k.left + k.right) / 2, 1720),
              "fit: the kept part sits in the middle, from the top of the window to the bottom");
        PreviewView whole;
        const auto [ww, wh] = whole.renderedSize(3440, 1440, 1920, 1080);
        check(about(wh, 1440) && rw > ww * 1.3f, "the same picture without the crop is a third smaller on that screen");
    }
    {
        // One side only: 10 % off the left. The kept part is 0.9 wide, centred on 0.55 of the picture.
        PreviewView v; v.cropLeft = 0.1f; v.centerX = v.cropCenterX(); v.centerY = v.cropCenterY();
        check(about(v.cropCenterX(), 0.55f, 1e-5f) && about(v.keptWidth(), 0.9f, 1e-5f), "one side: the kept part's middle moves by half of what is cut");
        const auto [rw, rh] = v.renderedSize(1920, 1080, 1920, 1080);
        // The kept part is 1728 x 1080 of the source: narrower than the window, so the height decides.
        check(about(rh, 1080) && about(rw, 1920), "one side cut on a screen of the picture's shape: the height still fits exactly");
        const auto k = v.keptRect(1920, 1080, rw, rh);
        check(about(k.left, 96) && about(k.right, 1824) && about(k.top, 0) && about(k.bottom, 1080), "the kept part is centred: bars of equal width left and right");
        // The mouse and the picture share the transform: the window's middle is the kept part's middle.
        const auto uv = v.sourcePoint(960, 540, 1920, 1080, 1920, 1080);
        check(about(uv.first, 0.55f, 1e-4f) && about(uv.second, 0.5f, 1e-4f), "the point under the middle of the window is the middle of the kept part");
        const auto edge = v.sourcePoint(k.left, 540, 1920, 1080, 1920, 1080);
        check(about(edge.first, 0.1f, 1e-4f), "the kept part's left edge on screen is the cut line in the picture");
    }
    // --- fill and stretch ------------------------------------------------------------------------------
    {
        PreviewView v; v.mode = 2; v.cropLeft = 0.2f; v.cropRight = 0.2f; v.centerX = v.cropCenterX(); v.centerY = v.cropCenterY();
        const auto [rw, rh] = v.renderedSize(1920, 1080, 1920, 1080);
        check(about(rw * v.keptWidth(), 1920) && rh * v.keptHeight() >= 1080 - 0.01f, "fill: the kept part covers the window");
        v.mode = 3;
        const auto [sw, sh] = v.renderedSize(1920, 1080, 1920, 1080);
        check(about(sw * v.keptWidth(), 1920) && about(sh * v.keptHeight(), 1080), "stretch: the kept part is exactly the window");
        v.mode = 1; v.zoom = 1;
        const auto [nw, nh] = v.renderedSize(1920, 1080, 1280, 720);
        check(nw == 1280 && nh == 720, "native pixels: a crop cuts, it does not scale");
    }
    // --- size on screen and limits ---------------------------------------------------------------------
    {
        PreviewView v; v.cropTop = 0.1f; v.cropBottom = 0.1f; v.centerX = v.cropCenterX(); v.centerY = v.cropCenterY();
        const auto [aw, ah] = v.renderedSize(1920, 1080, 1920, 1080);
        v.zoom = 0.5f;
        const auto [bw, bh] = v.renderedSize(1920, 1080, 1920, 1080);
        check(about(bw, aw / 2) && about(bh, ah / 2), "half the size is half the size, cropped or not");
        PreviewView wild; wild.cropLeft = 0.9f; wild.cropRight = 0.9f; wild.cropTop = -3.0f; wild.cropBottom = std::nanf("");
        check(about(wild.keptWidth(), 0.1f, 1e-5f) && wild.keptHeight() == 1.0f, "a side is at most 45 %, so a tenth always stays; a value that is no number cuts nothing");
        const auto [ww, wh] = wild.renderedSize(1920, 1080, 1920, 1080);
        check(std::isfinite(ww) && std::isfinite(wh) && ww > 0 && wh > 0, "the size stays a real number for any crop");
    }
    // --- a turned picture: the crop is of the picture as shown ------------------------------------------
    {
        PreviewView v; v.userQuarterTurns = 1; v.cropTop = 0.25f; v.centerX = v.cropCenterX(); v.centerY = v.cropCenterY();
        const auto [rw, rh] = v.renderedSize(1080, 1920, 1920, 1080);   // shown 1080 wide, 1920 high
        check(about(rw, 1080) || about(rh * v.keptHeight(), 1920), "a picture turned on its side is cropped as it is shown");
        const auto k = v.keptRect(1080, 1920, rw, rh);
        check(k.bottom - k.top < rh && k.bottom > k.top && k.right > k.left, "its kept part is a real rectangle, smaller than the picture");
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

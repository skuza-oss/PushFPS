// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFPS ground-truth frame-generation quality test, the pure part (no GPU; tests/unit/PushQualityTests.cpp).
// The test withholds every second real frame from frame generation, keeps a small luma copy of it,
// and compares the frame generation produced for that moment with it:
//   - PSNR (dB) of the generated frame against the withheld original,
//   - the mean pixel error and the share of pixels off by more than kBadPixel,
//   - PSNR inside the HUD boxes (exclusion regions or detected HUD), where artefacts show first,
//   - the same PSNR for a plain 50/50 blend of the two neighbouring real frames (the baseline any
//     interpolation should beat),
//   - PSNR of real frames through the same pipeline (the ceiling: enhancement and colour handling
//     change real frames too),
//   - the share of generated frames shown on time (within half a frame interval of their spacing),
//   - the worst frame of the test, and the worst place: the picture cut into kTilesX x kTilesY tiles and
//     the weakest tile of each frame (since 3.2.7). An average hides both: PushFlow leaves an object
//     that moves too fast out of the generated frame (docs/PUSHFLOW2.md), which costs the whole-picture
//     PSNR a fraction of a dB and the tile the object should be in nearly everything.
// Source pictures (YUV / RGB from the capture) and presented pictures (RGB from the graph) are
// compared as luma on a kGridW x kGridH grid; a straight-line fit from real frames maps the source's
// luma onto the presented one, so range and tone differences between the two don't count as errors.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace veyra::engine::quality {

constexpr uint32_t kGridW = 320, kGridH = 180;
constexpr float kBadPixel = 10.0f / 255.0f;

struct Luma {
    std::vector<float> y;        // kGridW * kGridH values, 0..1, row major
    bool valid() const { return y.size() == size_t(kGridW) * kGridH; }
};

// Source pictures (CPU memory, as the capture delivers them).
enum class SourceFormat { Nv12, P010, Yuy2, Yuv420p, Bgra8, Rgba8 };
struct SourcePlanes {
    SourceFormat format = SourceFormat::Nv12;
    const uint8_t* plane0 = nullptr;   // Y (or packed / RGB)
    size_t pitch0 = 0;
    uint32_t width = 0, height = 0;
    bool limitedRange = true;          // YUV only
};
bool lumaFromSource(const SourcePlanes& src, Luma& out);

// Presented pictures (a readback of the graph output).
enum class OutputLayout { Rgba8, Bgra8, Rgb10a2, Rgba16fLinear };
bool lumaFromOutput(const uint8_t* data, size_t pitch, uint32_t width, uint32_t height, OutputLayout layout, Luma& out);

// A HUD box in fractions of the picture.
struct Box { float left = 0, top = 0, right = 0, bottom = 0; };

double psnr(const Luma& a, const Luma& b, const std::vector<Box>* inside = nullptr);
// The weakest place: the lowest PSNR among the kTilesX x kTilesY tiles of the grid.
constexpr uint32_t kTilesX = 16, kTilesY = 9;
double worstTilePsnr(const Luma& a, const Luma& b);
double meanAbsError(const Luma& a, const Luma& b);
double badPixelShare(const Luma& a, const Luma& b, float threshold = kBadPixel);
Luma blend(const Luma& a, const Luma& b);

// y = gain * x + offset, least squares over pairs of real frames (source luma x, presented luma y).
struct Fit { double gain = 1, offset = 0; bool valid = false; };
Fit fitLinear(const std::vector<const Luma*>& source, const std::vector<const Luma*>& presented);
Luma apply(const Fit& fit, const Luma& source);

// One generated frame against its withheld original.
struct Sample {
    double psnr = 0, blendPsnr = 0, hudPsnr = 0, meanError = 0, badShare = 0;
    bool hud = false;
    double worstTile = 0;        // worstTilePsnr of this frame
};
Sample measure(const Luma& generated, const Luma& original, const Luma& previous, const Luma& next, const Fit& fit,
               const std::vector<Box>& hudBoxes);

struct Report {
    uint32_t samples = 0, realSamples = 0, hudSamples = 0;
    double psnr = 0, blendPsnr = 0, realPsnr = 0, hudPsnr = 0, meanError = 0, badShare = 0;
    uint64_t generatedShown = 0, generatedOnTime = 0;
    // The least accurate generated frame of the test, and the weakest place of a frame (each frame's
    // worst tile, averaged over the frames as the other values are). Shown, never part of the score.
    double worstFramePsnr = 0, worstPlacePsnr = 0;
    double onTimeShare() const { return generatedShown ? double(generatedOnTime) / double(generatedShown) : 0; }
    double gainOverBlend() const { return psnr - blendPsnr; }
    // 0..100 for ranking frame generation methods: PSNR, its gain over blending, HUD PSNR and timing.
    double score() const;
};
Report summarize(const std::vector<Sample>& samples, double realPsnr, uint32_t realSamples, uint64_t generatedShown, uint64_t generatedOnTime);

// Whether a presented frame came at its time: the spacing to the previous one differs from the spacing
// of their timestamps by at most half of it.
bool onTime(int64_t ptsDelta100ns, int64_t hostDelta100ns);

} // namespace veyra::engine::quality

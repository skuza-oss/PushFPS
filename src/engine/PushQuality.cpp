// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS ground-truth frame-generation quality test: luma grids, PSNR and the report (pure).
#include "veyra/engine/PushQuality.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace veyra::engine::quality {
namespace {
constexpr int kTaps = 4;   // 4 x 4 samples per grid cell

float half(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000) << 16, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) bits = sign;
        else { int e = -1; uint32_t m = mant; do { m <<= 1; ++e; } while (!(m & 0x400)); bits = sign | uint32_t(127 - 15 - e) << 23 | (m & 0x3FF) << 13; }
    } else if (exp == 31) bits = sign | 0x7F800000 | mant << 13;
    else bits = sign | (exp + 112) << 23 | mant << 13;
    float f; std::memcpy(&f, &bits, 4); return f;
}
float srgbEncode(float v) {
    if (!std::isfinite(v)) return 0.0f;
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}
float lumaRgb(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

// Samples kTaps x kTaps points in every grid cell of a width x height picture.
template <class F> void grid(uint32_t width, uint32_t height, Luma& out, F value) {
    out.y.assign(size_t(kGridW) * kGridH, 0.0f);
    for (uint32_t gy = 0; gy < kGridH; ++gy)
        for (uint32_t gx = 0; gx < kGridW; ++gx) {
            float sum = 0;
            for (int sy = 0; sy < kTaps; ++sy) {
                const uint32_t y = std::min(height - 1, uint32_t((gy + (sy + 0.5) / kTaps) * height / kGridH));
                for (int sx = 0; sx < kTaps; ++sx) {
                    const uint32_t x = std::min(width - 1, uint32_t((gx + (sx + 0.5) / kTaps) * width / kGridW));
                    sum += value(x, y);
                }
            }
            out.y[size_t(gy) * kGridW + gx] = sum / float(kTaps * kTaps);
        }
}
bool insideAny(const std::vector<Box>* boxes, uint32_t gx, uint32_t gy) {
    if (!boxes) return true;
    const float x = (gx + 0.5f) / kGridW, y = (gy + 0.5f) / kGridH;
    for (const auto& b : *boxes) if (x >= b.left && x < b.right && y >= b.top && y < b.bottom) return true;
    return false;
}
} // namespace

bool lumaFromSource(const SourcePlanes& s, Luma& out) {
    if (!s.plane0 || s.width < 16 || s.height < 16) return false;
    const auto yuv = [&](float code8, float maxCode) {
        float v = code8 / maxCode;
        if (s.limitedRange) v = (v - 16.0f / 255.0f) * (255.0f / 219.0f);
        return std::clamp(v, 0.0f, 1.0f);
    };
    switch (s.format) {
    case SourceFormat::Nv12: case SourceFormat::Yuv420p:
        if (s.pitch0 < s.width) return false;
        grid(s.width, s.height, out, [&](uint32_t x, uint32_t y) { return yuv(float(s.plane0[size_t(y) * s.pitch0 + x]), 255.0f); });
        return true;
    case SourceFormat::P010:
        if (s.pitch0 < size_t(s.width) * 2) return false;
        grid(s.width, s.height, out, [&](uint32_t x, uint32_t y) {
            uint16_t v; std::memcpy(&v, s.plane0 + size_t(y) * s.pitch0 + size_t(x) * 2, 2);
            return yuv(float(v >> 6), 1023.0f);
        });
        return true;
    case SourceFormat::Yuy2:
        if (s.pitch0 < size_t(s.width) * 2) return false;
        grid(s.width, s.height, out, [&](uint32_t x, uint32_t y) { return yuv(float(s.plane0[size_t(y) * s.pitch0 + size_t(x) * 2]), 255.0f); });
        return true;
    case SourceFormat::Bgra8: case SourceFormat::Rgba8: {
        if (s.pitch0 < size_t(s.width) * 4) return false;
        const bool bgr = s.format == SourceFormat::Bgra8;
        grid(s.width, s.height, out, [&](uint32_t x, uint32_t y) {
            const uint8_t* p = s.plane0 + size_t(y) * s.pitch0 + size_t(x) * 4;
            const float r = (bgr ? p[2] : p[0]) / 255.0f, g = p[1] / 255.0f, b = (bgr ? p[0] : p[2]) / 255.0f;
            return lumaRgb(r, g, b);
        });
        return true;
    }
    }
    return false;
}

bool lumaFromOutput(const uint8_t* data, size_t pitch, uint32_t width, uint32_t height, OutputLayout layout, Luma& out) {
    if (!data || width < 16 || height < 16) return false;
    const size_t bpp = layout == OutputLayout::Rgba16fLinear ? 8 : 4;
    if (pitch < size_t(width) * bpp) return false;
    grid(width, height, out, [&](uint32_t x, uint32_t y) {
        const uint8_t* p = data + size_t(y) * pitch + size_t(x) * bpp;
        switch (layout) {
        case OutputLayout::Rgba8: return lumaRgb(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f);
        case OutputLayout::Bgra8: return lumaRgb(p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f);
        case OutputLayout::Rgb10a2: {
            uint32_t v; std::memcpy(&v, p, 4);
            return lumaRgb((v & 1023) / 1023.0f, ((v >> 10) & 1023) / 1023.0f, ((v >> 20) & 1023) / 1023.0f);
        }
        case OutputLayout::Rgba16fLinear: {
            uint16_t h[3]; std::memcpy(h, p, 6);
            return lumaRgb(srgbEncode(half(h[0])), srgbEncode(half(h[1])), srgbEncode(half(h[2])));
        }
        }
        return 0.0f;
    });
    return true;
}

double psnr(const Luma& a, const Luma& b, const std::vector<Box>* inside) {
    if (!a.valid() || !b.valid()) return 0;
    double sum = 0; size_t n = 0;
    for (uint32_t gy = 0; gy < kGridH; ++gy)
        for (uint32_t gx = 0; gx < kGridW; ++gx) {
            if (!insideAny(inside, gx, gy)) continue;
            const size_t i = size_t(gy) * kGridW + gx;
            const double d = double(a.y[i]) - double(b.y[i]);
            sum += d * d; ++n;
        }
    if (!n) return 0;
    const double mse = sum / double(n);
    return mse <= 1e-10 ? 100.0 : std::min(100.0, 10.0 * std::log10(1.0 / mse));
}
double worstTilePsnr(const Luma& a, const Luma& b) {
    if (!a.valid() || !b.valid()) return 0;
    const uint32_t tw = kGridW / kTilesX, th = kGridH / kTilesY;
    double worst = 0;   // the largest mean squared error among the tiles
    for (uint32_t ty = 0; ty < kTilesY; ++ty)
        for (uint32_t tx = 0; tx < kTilesX; ++tx) {
            double sum = 0;
            for (uint32_t gy = ty * th; gy < (ty + 1) * th; ++gy)
                for (uint32_t gx = tx * tw; gx < (tx + 1) * tw; ++gx) {
                    const size_t i = size_t(gy) * kGridW + gx;
                    const double d = double(a.y[i]) - double(b.y[i]);
                    sum += d * d;
                }
            worst = std::max(worst, sum / double(tw * th));
        }
    return worst <= 1e-10 ? 100.0 : std::min(100.0, 10.0 * std::log10(1.0 / worst));
}
double meanAbsError(const Luma& a, const Luma& b) {
    if (!a.valid() || !b.valid()) return 0;
    double sum = 0;
    for (size_t i = 0; i < a.y.size(); ++i) sum += std::abs(double(a.y[i]) - double(b.y[i]));
    return sum / double(a.y.size());
}
double badPixelShare(const Luma& a, const Luma& b, float threshold) {
    if (!a.valid() || !b.valid()) return 0;
    size_t bad = 0;
    for (size_t i = 0; i < a.y.size(); ++i) if (std::abs(a.y[i] - b.y[i]) > threshold) ++bad;
    return double(bad) / double(a.y.size());
}
Luma blend(const Luma& a, const Luma& b) {
    Luma out;
    if (!a.valid() || !b.valid()) return out;
    out.y.resize(a.y.size());
    for (size_t i = 0; i < a.y.size(); ++i) out.y[i] = 0.5f * (a.y[i] + b.y[i]);
    return out;
}

Fit fitLinear(const std::vector<const Luma*>& source, const std::vector<const Luma*>& presented) {
    Fit fit;
    if (source.size() != presented.size() || source.empty()) return fit;
    double sx = 0, sy = 0, sxx = 0, sxy = 0, n = 0;
    for (size_t k = 0; k < source.size(); ++k) {
        if (!source[k] || !presented[k] || !source[k]->valid() || !presented[k]->valid()) continue;
        for (size_t i = 0; i < source[k]->y.size(); i += 7) {   // every 7th cell is plenty
            const double x = source[k]->y[i], y = presented[k]->y[i];
            sx += x; sy += y; sxx += x * x; sxy += x * y; n += 1;
        }
    }
    const double den = n * sxx - sx * sx;
    if (n < 100 || std::abs(den) < 1e-9) return fit;
    fit.gain = (n * sxy - sx * sy) / den;
    fit.offset = (sy - fit.gain * sx) / n;
    // A picture through the pipeline keeps its tones roughly; a wild fit means mismatched frames.
    fit.valid = fit.gain > 0.5 && fit.gain < 2.0 && std::abs(fit.offset) < 0.5;
    if (!fit.valid) fit = {};
    return fit;
}
Luma apply(const Fit& fit, const Luma& source) {
    Luma out = source;
    for (auto& v : out.y) v = float(std::clamp(fit.gain * v + fit.offset, 0.0, 1.0));
    return out;
}

Sample measure(const Luma& generated, const Luma& original, const Luma& previous, const Luma& next, const Fit& fit,
               const std::vector<Box>& hudBoxes) {
    Sample s;
    const Luma truth = apply(fit, original);
    const Luma baseline = apply(fit, blend(previous, next));
    s.psnr = psnr(generated, truth);
    s.blendPsnr = baseline.valid() ? psnr(baseline, truth) : 0;
    s.meanError = meanAbsError(generated, truth);
    s.badShare = badPixelShare(generated, truth);
    s.worstTile = worstTilePsnr(generated, truth);
    if (!hudBoxes.empty()) { s.hudPsnr = psnr(generated, truth, &hudBoxes); s.hud = s.hudPsnr > 0; }
    return s;
}

double Report::score() const {
    if (!samples) return 0;
    // PSNR 20..45 dB maps to 0..60 points, its gain over blending up to 20, HUD PSNR up to 10,
    // timing up to 10. A method that doesn't beat blending still scores on its own PSNR.
    const double base = std::clamp((psnr - 20.0) / 25.0, 0.0, 1.0) * 60.0;
    const double gain = std::clamp(gainOverBlend() / 6.0, 0.0, 1.0) * 20.0;
    const double hud = hudSamples ? std::clamp((hudPsnr - 20.0) / 25.0, 0.0, 1.0) * 10.0 : 5.0;
    const double timing = onTimeShare() * 10.0;
    return std::round((base + gain + hud + timing) * 10.0) / 10.0;
}

Report summarize(const std::vector<Sample>& samples, double realPsnr, uint32_t realSamples, uint64_t generatedShown, uint64_t generatedOnTime) {
    Report r;
    r.realPsnr = realPsnr; r.realSamples = realSamples;
    r.generatedShown = generatedShown; r.generatedOnTime = generatedOnTime;
    if (samples.empty()) return r;
    // Averaged as mean squared error, then back to dB (averaging dB values flatters bad frames).
    auto meanDb = [&](auto get, auto use) {
        double mse = 0; uint32_t n = 0;
        for (const auto& s : samples) { if (!use(s)) continue; mse += std::pow(10.0, -get(s) / 10.0); ++n; }
        return n ? std::min(100.0, -10.0 * std::log10(mse / n)) : 0.0;
    };
    r.samples = uint32_t(samples.size());
    r.psnr = meanDb([](const Sample& s) { return s.psnr; }, [](const Sample&) { return true; });
    r.blendPsnr = meanDb([](const Sample& s) { return s.blendPsnr; }, [](const Sample& s) { return s.blendPsnr > 0; });
    r.hudPsnr = meanDb([](const Sample& s) { return s.hudPsnr; }, [](const Sample& s) { return s.hud; });
    for (const auto& s : samples) { r.meanError += s.meanError; r.badShare += s.badShare; if (s.hud) ++r.hudSamples; }
    r.meanError /= double(samples.size()); r.badShare /= double(samples.size());
    r.worstFramePsnr = samples.front().psnr;
    for (const auto& s : samples) r.worstFramePsnr = std::min(r.worstFramePsnr, s.psnr);
    r.worstPlacePsnr = meanDb([](const Sample& s) { return s.worstTile; }, [](const Sample& s) { return s.worstTile > 0; });
    return r;
}

bool onTime(int64_t ptsDelta100ns, int64_t hostDelta100ns) {
    if (ptsDelta100ns <= 0) return false;
    return std::llabs(hostDelta100ns - ptsDelta100ns) * 2 <= ptsDelta100ns;
}

} // namespace veyra::engine::quality

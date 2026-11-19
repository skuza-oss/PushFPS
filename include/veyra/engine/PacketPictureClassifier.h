// SPDX-License-Identifier: GPL-3.0-or-later
// Part of PushFPS, a modified version of Veyra.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace veyra::engine {
// Picture numbers for hardware-decoded streams (PS5 Remote Play), where the decoded picture never reaches
// the CPU and ContentDedup would otherwise see no picture at all ("blind": every sample kept).
//
// A game at 30 FPS inside a 60 FPS stream hands the console's encoder the same picture twice. The encoder
// codes the repeat as a near-empty inter frame (skip blocks plus a little refinement), so its compressed
// size is a small fraction of a new picture's. The classifier tracks the typical size of new pictures and
// of repeats separately (slow exponential averages in the log domain) and puts the boundary between them
// (their geometric mean); before both are known it uses a fixed share of the new-picture size. A repeat
// keeps the previous picture number, a new picture takes the next one: exactly what ContentDedup expects
// from its CPU comparison.
//
// Tuning without a rebuild (lab use): PUSHFPS_PACKET_DEDUP=0 turns it off, PUSHFPS_PACKET_DEDUP_SHARE sets
// the start-up share (default 0.25), PUSHFPS_PACKET_DEDUP_BIAS moves the boundary (multiplier on the
// geometric mean, default 1.0).
class PacketPictureClassifier {
public:
    struct Result { uint64_t picture = 0; bool repeat = false; double threshold = 0; };

    PacketPictureClassifier() {
        if (const char* v = std::getenv("PUSHFPS_PACKET_DEDUP")) enabled_ = !(v[0] == '0' && v[1] == 0);
        if (const char* v = std::getenv("PUSHFPS_PACKET_DEDUP_SHARE")) { const double x = std::atof(v); if (x > 0.01 && x < 0.95) share_ = x; }
        if (const char* v = std::getenv("PUSHFPS_PACKET_DEDUP_BIAS")) { const double x = std::atof(v); if (x > 0.1 && x < 10) bias_ = x; }
    }
    bool enabled() const { return enabled_; }
    void reset() { haveNew_ = haveRepeat_ = false; last_ = 0; }

    // bytes: compressed size of the sample's picture (without parameter sets); boundary: a stream reset or
    // settings change, after which the first sample always counts as new.
    Result observe(uint32_t bytes, bool boundary) {
        Result r;
        if (boundary) reset();
        const double size = std::log(std::max<double>(bytes, 16));
        double threshold;
        if (haveNew_ && haveRepeat_) threshold = 0.5 * (logNew_ + logRepeat_) + std::log(bias_);
        else if (haveNew_) threshold = logNew_ + std::log(share_);
        else threshold = -1e9;   // the first sample is new
        r.threshold = std::exp(threshold);
        r.repeat = size < threshold && last_ != 0;
        if (r.repeat) {
            logRepeat_ = haveRepeat_ ? logRepeat_ + kRate * (size - logRepeat_) : size;
            haveRepeat_ = true;
        } else {
            ++last_;
            // An IDR or a scene cut is far larger than the usual new picture; clamp its pull on the average.
            const double sample = haveNew_ ? std::min(size, logNew_ + 1.5) : size;
            logNew_ = haveNew_ ? logNew_ + kRate * (sample - logNew_) : sample;
            haveNew_ = true;
        }
        r.picture = last_;
        return r;
    }

private:
    static constexpr double kRate = 0.05;
    bool enabled_ = true, haveNew_ = false, haveRepeat_ = false;
    double share_ = 0.25, bias_ = 1.0, logNew_ = 0, logRepeat_ = 0;
    uint64_t last_ = 0;
};
}

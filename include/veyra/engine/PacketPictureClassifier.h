// SPDX-License-Identifier: GPL-3.0-or-later
// Part of PushFPS, a modified version of Veyra.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace veyra::engine {
// Picture numbers for hardware-decoded streams (PS5 Remote Play), where the decoded picture never reaches
// the CPU and ContentDedup would otherwise see no picture at all ("blind": every sample kept).
//
// A game at 30 FPS inside a 60 FPS stream hands the console's encoder the same picture twice. The encoder
// codes the repeat as an inter frame with little to say, so it is smaller than the new picture before it.
// How much smaller depends on the scene: in GTA 6 gameplay a new picture is 45-90 KB and its repeat 2-6 KB
// (refinement bursts up to about 18 KB), in a near-static pause menu at 6 Mbit/s 15-21 KB against 8-11 KB.
// An absolute size boundary cannot cover both, so each sample is judged against the one before it:
//
//   a sample is a repeat when the previous sample was a new picture and this one is smaller by at least
//   `ratio` (default 1.3), and either by at least `bigRatio` (default 2.5; gameplay) or the small-large-small
//   rhythm of 30-in-60 is already running (the sample before the new picture was a repeat or was also
//   smaller by `ratio`; menus and quiet scenes).
//
// Two repeats in a row are never called (a 30-in-60 cadence has none), so a failure keeps pictures rather
// than dropping them. A repeat keeps the previous picture number, a new picture takes the next one: exactly
// what ContentDedup expects from its CPU comparison. Recorded GTA 6 streams: gameplay 100% / 99.6%, pause
// menu 99.8% of samples in strict alternation.
//
// Lab switches (no rebuild): PUSHFPS_PACKET_DEDUP=0 turns it off; PUSHFPS_PACKET_DEDUP_RATIO and
// PUSHFPS_PACKET_DEDUP_BIG set the two ratios.
class PacketPictureClassifier {
public:
    struct Result { uint64_t picture = 0; bool repeat = false; double threshold = 0; };

    PacketPictureClassifier() {
        char v[32];
        if (env("PUSHFPS_PACKET_DEDUP", v)) enabled_ = !(v[0] == '0' && v[1] == 0);
        if (env("PUSHFPS_PACKET_DEDUP_RATIO", v)) { const double x = std::atof(v); if (x > 1.05 && x < 5) ratio_ = x; }
        if (env("PUSHFPS_PACKET_DEDUP_BIG", v)) { const double x = std::atof(v); if (x > 1.05 && x < 20) bigRatio_ = x; }
        if (bigRatio_ < ratio_) bigRatio_ = ratio_;
    }
    bool enabled() const { return enabled_; }
    void reset() { last_ = 0; prev_ = prev2_ = 0; prevRepeat_ = prev2Repeat_ = false; count_ = 0; }

    // bytes: compressed size of the sample's picture; boundary: a stream reset or settings change, after
    // which the first sample always counts as new.
    Result observe(uint32_t bytes, bool boundary) {
        Result r;
        if (boundary) reset();
        const double size = std::max<double>(bytes, 16);
        if (count_ >= 1 && !prevRepeat_ && last_ != 0) {
            r.threshold = prev_ / ratio_;
            // A still screen (a paused map: 0.13 Mbit/s, about 270 bytes per sample) codes new pictures and
            // repeats alike as near-empty frames; at 1 KB or less there is nothing new to show either way.
            if (size <= kStill) r.repeat = true;
            else if (size < r.threshold) {
                const bool big = size < prev_ / bigRatio_;
                const bool rhythm = count_ >= 2 && (prev2Repeat_ || prev2_ < r.threshold);
                r.repeat = big || rhythm;
            }
        }
        if (!r.repeat) ++last_;
        prev2_ = prev_; prev_ = size;
        prev2Repeat_ = prevRepeat_; prevRepeat_ = r.repeat;
        if (count_ < 2) ++count_;
        r.picture = last_;
        return r;
    }

private:
    static bool env(const char* name, char (&out)[32]) {
#ifdef _MSC_VER
        char* value = nullptr; size_t length = 0;
        if (_dupenv_s(&value, &length, name) != 0 || !value) return false;
        const bool ok = value[0] != 0;
        size_t i = 0; for (; i + 1 < sizeof(out) && value[i]; ++i) out[i] = value[i];
        out[i] = 0; std::free(value);
        return ok;
#else
        const char* value = std::getenv(name);
        if (!value || !value[0]) return false;
        size_t i = 0; for (; i + 1 < sizeof(out) && value[i]; ++i) out[i] = value[i];
        out[i] = 0; return true;
#endif
    }
    static constexpr double kStill = 1024;
    bool enabled_ = true;
    double ratio_ = 1.3, bigRatio_ = 2.5;
    double prev_ = 0, prev2_ = 0;
    bool prevRepeat_ = false, prev2Repeat_ = false;
    int count_ = 0;
    uint64_t last_ = 0;
};
}

#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFPS before/after comparison clips. A comparison recording is a normal
// live recording (LiveRecorder.h) in one of four layouts.
// Split (CompareLayout::Split): the picture is split down the middle:
//  - the right half is every presented frame, generated ones included (what
//    PushFPS shows, with every enhancement);
//  - the left half is the console: the last real frame, held until the next
//    real frame arrives, so it moves at the console's own frame rate.
// Side by side (CompareLayout::SideBySide): the whole frame twice at half size,
// the console on the left and every presented frame on the right, centred
// vertically on black (shaders/PushCompareSideBySide.hlsl); the labels sit in
// the black bar above each picture.
// Vertical (CompareLayout::Vertical, for TikTok, YouTube Shorts and Reels): a
// 1080 x 1920 (9:16) file of its own size, the console on top and every
// presented frame below, each filling half the screen (the middle of the
// frame, cropped to 9:8 and scaled with a smooth filter,
// shaders/PushCompareVertical.hlsl); the labels sit next to the divider in
// the middle, the part of the screen those apps leave free.
// Reveal (CompareLayout::Reveal, RecordingComparePlan.h): the whole picture, no
// split. It opens on the console side; a glowing line sweeps left to right and
// uncovers every presented frame, holds, sweeps back, and repeats. Left of the
// line is PushFPS, right of it the console, so the PushFPS badge sits at the top
// left and the console's at the top right, each cut off at the line with its
// picture. The line and the badges are blended in (PushCompareOverlay.hlsl).
//
// The console side is the raw console picture when the presenter passes it
// (RecordingRawFrame: the graph's untouched copy of the input, kept while a
// comparison clip records): no AI enhancement, upscaling or colour work, only
// scaled to the recorded size (shaders/PushCompareRaw.hlsl). Without it (the
// first frames, or a source without references) the held enhanced real frame
// is used as before.
//
// Labels: a luma-only label (RecordingLabel::luma) is copied in; a colour label
// (RecordingLabel::rgba: rounded badges, the PushFPS logo) is blended onto the
// picture with its alpha (shaders/PushCompareOverlay.hlsl). In HDR (P010, PQ)
// white is written at 203 nits, the HDR reference white. A label too large for
// its place is scaled down to fit, never left out.
// tests/integration/PushGpuTests.cpp runs all of it on WARP.
#include "veyra/engine/RecordingComparePlan.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace veyra::pipeline { class StateTracker; }

namespace veyra::engine {

// (RecordingLabel and CompareLayout are in RecordingComparePlan.h, which has no Windows headers.)
// The label resampled (area average; colour weighted by alpha) to width x height.
RecordingLabel scaleRecordingLabel(const RecordingLabel& label, uint32_t width, uint32_t height);
// Called once per recording file on the engine thread with the recorded size.
using RecordingLabelRenderer = std::function<void(uint32_t width, uint32_t height, RecordingLabel& left, RecordingLabel& right)>;

// The raw console picture of a real frame: linear BT.709 in scRGB units
// (1 = 80 nits), a float RGBA texture of the source size, COMMON on entry and
// left in COMMON. sdrInHdr: an SDR source shown in HDR (RTX Video HDR), scaled
// to the 203-nit reference white as the presenter's comparison view does.
struct RecordingRawFrame {
    ID3D12Resource* texture = nullptr;
    bool sdrInHdr = false;
};

// The recorded (encoded) size of a comparison clip for a presented frame of frameW x frameH:
// the frame's own size, except Vertical: always 1080 x 1920, the size the short-video apps use.
inline void compareOutputSize(CompareLayout layout, uint32_t frameW, uint32_t frameH, uint32_t& width, uint32_t& height) {
    if (layout == CompareLayout::Vertical) { width = 1080; height = 1920; return; }
    width = frameW; height = frameH;
}
// Vertical: the part of a frameW x frameH picture each half shows (pictureW x pictureH, 9:8):
// the middle, as large as fits. Even numbers.
struct CompareCrop { uint32_t x = 0, y = 0, width = 0, height = 0; };
inline CompareCrop compareVerticalCrop(uint32_t frameW, uint32_t frameH, uint32_t pictureW, uint32_t pictureH) {
    CompareCrop c;
    if (!frameW || !frameH || !pictureW || !pictureH) return c;
    // Wider than the half: full height, the middle columns. Narrower: full width, the middle rows.
    if (uint64_t(frameW) * pictureH >= uint64_t(frameH) * pictureW) {
        c.height = frameH;
        c.width = std::min<uint32_t>(frameW, uint32_t(uint64_t(frameH) * pictureW / pictureH)) & ~1u;
    } else {
        c.width = frameW;
        c.height = std::min<uint32_t>(frameH, uint32_t(uint64_t(frameW) * pictureH / pictureW)) & ~1u;
    }
    c.x = ((frameW - c.width) / 2) & ~1u;
    c.y = ((frameH - c.height) / 2) & ~1u;
    return c;
}

// Comparison clips, when the recorder falls behind (all encoder slots busy, or the disk behind):
// whether to skip this frame. The console side moves only on real frames, so generated frames are
// skipped first and one slot stays free for the next real frame; a real frame is skipped only when
// no slot is free at all (the caller finds that out by trying to take one).
inline bool compareSkipsFrame(bool generated, unsigned freeSlots, bool diskBehind) {
    return generated && (diskBehind || freeSlots < 2);
}

class RecordingCompare {
public:
    RecordingCompare();
    ~RecordingCompare();
    RecordingCompare(const RecordingCompare&) = delete;
    RecordingCompare& operator=(const RecordingCompare&) = delete;
    // y: width x height (R8, or R16 for HDR); uv: half size (R8G8 or R16G16): the presented
    // frame's size. Vertical composes into planes of its own (outputY/outputUv, compareOutputSize).
    bool create(ID3D12Device* device, uint32_t width, uint32_t height, bool hdr, const RecordingLabel& left,
                const RecordingLabel& right, std::wstring& reason, CompareLayout layout = CompareLayout::Split);
    // After the conversion wrote y and uv: updates the console side (from `raw`
    // on a real frame when given, else from the real frame itself), composes the
    // layout, then the divider and labels. `states` tracks y, uv and this
    // object's textures. `slot` is the recorder's frame slot: a descriptor for
    // `raw` is written there, reused only after that slot's frame finished.
    // `seconds`: the frame's time in the clip. Only Reveal uses it: it decides where the line is.
    void compose(ID3D12GraphicsCommandList* list, pipeline::StateTracker& states, ID3D12Resource* y, ID3D12Resource* uv, bool generated,
                 const RecordingRawFrame* raw = nullptr, unsigned slot = 0, double seconds = 0.0);
    // Reveal: a sweep starts every so many seconds (compareRevealInterval; 3 unless set).
    void setRevealInterval(double seconds) { revealInterval_ = compareRevealInterval(seconds); }
    // Reveal, after a frame was composed: the x from which the picture is the console side (left of
    // it: every presented frame), and whether the line was on the picture.
    uint32_t revealX() const { return revealX_; }
    bool revealLineShown() const { return revealLineShown_; }
    uint32_t revealLineWidth() const { return lineW_; }
    uint64_t generatedFrames() const { return generated_; }
    uint64_t realFrames() const { return real_; }
    uint64_t rawFrames() const { return rawFrames_; }
    bool rawHold() const { return rawHold_; }     // the console side shows the raw picture now
    bool rawAvailable() const { return raw_ != nullptr; }
    bool labelled() const { return !patches_.empty() || !overlays_.empty(); }
    uint32_t splitX() const { return split_; }
    CompareLayout layout() const { return layout_; }
    // Side by side: where the two half-size pictures are.
    uint32_t pictureTop() const { return top_; }
    uint32_t pictureWidth() const { return pictureW_; }
    uint32_t pictureHeight() const { return pictureH_; }
    uint32_t rightX() const { return rightX_; }
    // Vertical: the composed picture the recorder encodes (null in the other layouts, which
    // compose into y and uv), its size, each half's height and the part of the frame shown.
    ID3D12Resource* outputY() const { return outY_.Get(); }
    ID3D12Resource* outputUv() const { return outUv_.Get(); }
    uint32_t outputWidth() const { return outW_; }
    uint32_t outputHeight() const { return outH_; }
    CompareCrop crop() const { return crop_; }

    struct Patch { uint32_t x = 0, y = 0, width = 0, height = 0; uint64_t yOffset = 0, uvOffset = 0; uint32_t yPitch = 0, uvPitch = 0; };
    // The divider and luma labels (copied in).
    const std::vector<Patch>& patches() const { return patches_; }
    struct Overlay { uint32_t x = 0, y = 0, width = 0, height = 0; uint64_t offset = 0; uint32_t pitch = 0; };
    // Colour labels (blended in).
    const std::vector<Overlay>& overlays() const { return overlays_; }

private:
    struct SideBySide; // the compute passes, kept out of this header
    struct Vertical;
    struct Raw;
    struct Blend;
    void rawPass(ID3D12GraphicsCommandList* list, pipeline::StateTracker& states, const RecordingRawFrame& raw, unsigned slot);
    // One badge (or part of one) to blend: overlay `index` from its column `sourceX` on, at (x, y).
    struct Draw { size_t index = 0; uint32_t x = 0, y = 0, width = 0, height = 0, sourceX = 0; };
    void blend(ID3D12GraphicsCommandList* list, pipeline::StateTracker& states, ID3D12Resource* y, ID3D12Resource* uv,
               const Draw* draws, size_t count);
    Microsoft::WRL::ComPtr<ID3D12Resource> holdY_, holdUv_, upload_, outY_, outUv_;
    std::unique_ptr<SideBySide> sideBySide_;
    std::unique_ptr<Vertical> vertical_;
    std::unique_ptr<Raw> raw_;
    std::unique_ptr<Blend> blend_;
    std::vector<Patch> patches_;
    std::vector<Overlay> overlays_;
    CompareLayout layout_ = CompareLayout::Split;
    uint32_t width_ = 0, height_ = 0, split_ = 0;
    uint32_t top_ = 0, pictureW_ = 0, pictureH_ = 0, rightX_ = 0;
    uint32_t outW_ = 0, outH_ = 0;
    CompareCrop crop_;
    bool hdr_ = false, holdValid_ = false, rawHold_ = false;
    // Reveal: which overlays are the PushFPS badge, the console badge and the line (-1: none).
    int revealPush_ = -1, revealConsole_ = -1, revealLine_ = -1;
    uint32_t lineW_ = 0, revealX_ = 0;
    bool revealLineShown_ = false;
    double revealInterval_ = 3.0;
    uint64_t generated_ = 0, real_ = 0, rawFrames_ = 0;
};

} // namespace veyra::engine

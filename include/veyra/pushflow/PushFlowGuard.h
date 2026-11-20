// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFlow 2's Guard: its numbers and its decision, in one place a test can run (pure; builds with g++).
// shaders/PushGuard.hlsl does the same on the graphics card (its stage 2 is `decide` below; change both
// together), and guard_tiles() in scripts/pushflow/prepare_runtime.py is the rule as it was measured on
// the shipped models (docs/PUSHFLOW2.md has the measurements, and the two rules that failed before it).
//
// What Guard is for: an object that moves further between two real frames than the network follows is
// left out of the generated frame. Guard shows the nearer real frame at such a place, so the object
// steps at the game's own rate instead of blinking out. It never invents a picture, and a frame it
// replaces whole is a held frame, counted as that.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace veyra::pushflow::guard {

inline constexpr uint32_t kTile = 16;          // motion pixels a side
inline constexpr float kChange = 0.05f;        // a tile changed: mean difference of its brightness pattern, 0..1
inline constexpr float kAgree = 0.08f;         // two samples are the same picture: largest channel difference below this
inline constexpr float kUnused = 0.25f;        // a tile of a frame is unused: less than this share of it is shown
inline constexpr float kEdge = 1.5f;           // the edge band: this many times the largest motion of a tile, plus one tile
inline constexpr uint32_t kWholePercent = 35;  // more risky tiles than this: the nearer real frame, whole

// What the first two stages hand to the decision, per tile.
struct Tile {
    float change = 0;       // how much what it shows differs between the two frames, its own brightness change taken out
    float shownFirst = 0;   // the share of the first frame's tile that is shown somewhere in the generated frame, 0..
    float shownSecond = 0;  // the same for the second frame
    float moved = 0;        // how far, on average, the generated pixels in this tile read their samples from (motion pixels)
};
struct Decision {
    std::vector<uint8_t> risky;   // per tile: the nearer real frame is shown here
    uint32_t count = 0;           // risky tiles
    bool whole = false;           // so many that the frame is the nearer real frame whole
    int reach = 0;                // tiles from the edge that are left alone
};

// How much a generated pixel shows of the place it reads in one frame: what it takes of it, or all of
// it when its other sample is the same picture (the network often takes one frame only).
inline float shown(float weight, float largestChannelDifference) {
    const float same = 1.0f - std::clamp(largestChannelDifference / kAgree, 0.0f, 1.0f);
    return std::max(std::clamp(weight, 0.0f, 1.0f), same);
}

inline Decision decide(const std::vector<Tile>& tiles, uint32_t across, uint32_t down) {
    Decision d;
    const size_t count = size_t(across) * down;
    if (!across || !down || tiles.size() != count) return d;
    float furthest = 0;
    for (const Tile& t : tiles) furthest = std::max(furthest, t.moved);
    d.reach = int(std::ceil(kEdge * furthest / float(kTile))) + 1;
    std::vector<uint8_t> first(count, 0);
    for (size_t i = 0; i < count; ++i) {
        const int x = int(i % across), y = int(i / across);
        const bool changed = tiles[i].change > kChange;
        const bool unused = std::min(tiles[i].shownFirst, tiles[i].shownSecond) < kUnused;
        const bool inside = x >= d.reach && y >= d.reach && x < int(across) - d.reach && y < int(down) - d.reach;
        first[i] = changed && unused && inside;
    }
    d.risky.assign(count, 0);
    for (size_t i = 0; i < count; ++i) {
        const int x = int(i % across), y = int(i / across);
        bool on = first[i] != 0;
        // A risky tile takes its changed neighbours with it, so an object is not cut in half.
        if (!on && tiles[i].change > kChange)
            for (int dy = -1; dy <= 1 && !on; ++dy)
                for (int dx = -1; dx <= 1 && !on; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= int(across) || ny >= int(down)) continue;
                    on = first[size_t(ny) * across + size_t(nx)] != 0;
                }
        d.risky[i] = on;
        d.count += on;
    }
    d.whole = uint64_t(d.count) * 100 > uint64_t(kWholePercent) * count;
    return d;
}

} // namespace veyra::pushflow::guard

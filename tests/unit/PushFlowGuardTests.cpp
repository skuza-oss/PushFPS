// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFlow 2's Guard, the decision (pushflow/PushFlowGuard.h): which tiles show the nearer real frame.
// The cases are the ones the rule was measured on with the shipped models (docs/PUSHFLOW2.md): it must
// act where an object was left out, and leave followed motion, slow motion, pans and fades alone.
// Pure: also builds with g++.
#include "veyra/pushflow/PushFlowGuard.h"
#include <cstdio>

using namespace veyra::pushflow::guard;

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); failures += !ok; };
    const uint32_t W = 32, H = 20;
    const auto still = [&] { return std::vector<Tile>(size_t(W) * H, Tile{0.0f, 1.0f, 1.0f, 0.0f}); };
    const auto at = [&](uint32_t x, uint32_t y) { return size_t(y) * W + x; };
    const auto riskyAt = [&](const Decision& d, uint32_t x, uint32_t y) { return d.risky.size() == size_t(W) * H && d.risky[at(x, y)] != 0; };

    // --- what a generated pixel shows of a place -------------------------------------------------
    check(shown(1.0f, 0.5f) == 1.0f && shown(0.0f, 0.0f) == 1.0f, "taken, or the same picture in the other sample: shown");
    check(shown(0.0f, 0.5f) == 0.0f && shown(0.0f, kAgree) == 0.0f, "not taken, and the other sample is another picture: not shown");
    check(shown(0.3f, 0.5f) == 0.3f && shown(0.3f, kAgree / 2) == 0.5f && shown(7.0f, 1.0f) == 1.0f && shown(-1.0f, 1.0f) == 0.0f, "in between, the larger of the two; never outside 0..1");

    // --- nothing happened, or nothing went wrong ---------------------------------------------------
    {
        const auto d = decide(still(), W, H);
        check(d.count == 0 && !d.whole && d.reach == 1, "a still picture: nothing is risky");
        auto fade = still();   // a fade: brightness changes, the pattern does not, so no tile changed
        for (auto& t : fade) { t.change = 0.0f; t.shownFirst = 0.0f; }
        check(decide(fade, W, H).count == 0, "a fade: unused by one frame's weights, but nothing changed, so nothing is risky");
        auto followed = still();   // an object followed: its old place changed, and the frame's object is shown where it now stands
        for (uint32_t x = 10; x < 13; ++x) for (uint32_t y = 8; y < 11; ++y) { followed[at(x, y)].change = 0.2f; followed[at(x, y)].moved = 3.0f; }
        check(decide(followed, W, H).count == 0, "an object that was followed: changed, shown, not risky");
        auto oneFrame = still();   // the network takes the second frame only, everything slides slowly: shown through the other sample
        for (auto& t : oneFrame) { t.change = 0.3f; t.shownFirst = shown(0.0f, 0.01f); t.shownSecond = 1.0f; t.moved = 2.0f; }
        check(decide(oneFrame, W, H).count == 0, "a slow pan taken from one frame only: the other frame shows the same picture, not risky");
    }
    // --- an object left out ----------------------------------------------------------------------------
    {
        auto lost = still();
        for (uint32_t x = 10; x < 12; ++x) for (uint32_t y = 8; y < 10; ++y) { lost[at(x, y)].change = 0.25f; lost[at(x, y)].shownFirst = 0.02f; }
        lost[at(12, 9)].change = 0.2f;    // a changed neighbour whose own counts look fine (the object's edge)
        lost[at(20, 9)].change = 0.2f;    // a changed tile far away that is shown: something else, followed
        const auto d = decide(lost, W, H);
        check(d.count == 5 && !d.whole && riskyAt(d, 10, 8) && riskyAt(d, 11, 9), "the tiles of an object shown nowhere are risky");
        check(riskyAt(d, 12, 9), "and take their changed neighbour with them, so the object is not cut in half");
        check(!riskyAt(d, 20, 9) && !riskyAt(d, 13, 9) && !riskyAt(d, 9, 8), "a changed tile elsewhere and unchanged neighbours are left alone");
        auto second = still();
        second[at(15, 10)].change = 0.3f; second[at(15, 10)].shownSecond = 0.1f;
        check(decide(second, W, H).count == 1, "the same for the second frame's object (where it arrives)");
        auto barely = still();
        barely[at(15, 10)].change = kChange; barely[at(15, 10)].shownFirst = 0.0f;
        check(decide(barely, W, H).count == 0, "a change at the threshold is not a change");
        barely[at(15, 10)].change = 0.3f; barely[at(15, 10)].shownFirst = kUnused;
        check(decide(barely, W, H).count == 0, "a quarter shown is shown");
    }
    // --- the edge: a pan pushes the picture's edge out --------------------------------------------------
    {
        auto pan = still();
        for (auto& t : pan) { t.change = 0.3f; t.moved = 24.0f; }                       // everything slides 24 motion pixels
        for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < 3; ++x) pan[at(x, y)].shownFirst = 0.0f;   // the strip that left the picture
        const auto d = decide(pan, W, H);
        check(d.reach == 4 && d.count == 0, "what a pan pushes out of the picture is unused and right: the edge band is left alone");
        auto inner = pan;
        inner[at(16, 10)].shownFirst = 0.0f;
        const auto e = decide(inner, W, H);
        check(riskyAt(e, 16, 10) && e.count == 9, "inside the band the rule still acts (and takes the changed tiles around it)");
        auto huge = still();
        for (auto& t : huge) { t.change = 0.3f; t.moved = 400.0f; t.shownFirst = 0.0f; }
        check(decide(huge, W, H).count == 0, "motion that reaches across the whole picture leaves no inside: nothing is replaced in pieces");
    }
    // --- too much of the frame: the nearer real frame, whole ------------------------------------------------
    {
        auto cut = still();
        for (auto& t : cut) { t.change = 0.4f; t.shownFirst = 0.0f; t.shownSecond = 0.0f; }
        const auto d = decide(cut, W, H);
        check(d.whole && d.count == W * H, "two unrelated pictures: the frame is held whole, and counted as held");
        auto third = still();
        uint32_t marked = 0;
        for (uint32_t y = 2; y < H - 2 && marked * 100 <= kWholePercent * W * H; y += 2)
            for (uint32_t x = 2; x < W - 2 && marked * 100 <= kWholePercent * W * H; x += 2) { third[at(x, y)].change = 0.3f; third[at(x, y)].shownFirst = 0.0f; ++marked; }
        check(marked * 100 <= kWholePercent * W * H || decide(third, W, H).whole, "above the share, whole");
        auto few = still();
        few[at(15, 10)].change = 0.3f; few[at(15, 10)].shownFirst = 0.0f;
        check(!decide(few, W, H).whole, "one object is mended in its place, the frame stays a generated frame");
    }
    // --- sizes that are not what was expected --------------------------------------------------------------
    check(decide({}, W, H).risky.empty() && decide(still(), 0, H).count == 0 && decide(still(), W, H + 1).risky.empty(), "a tile list of another size decides nothing");
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

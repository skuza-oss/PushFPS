// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Frame generation health (engine/PushFgHealth.h): frame generation that is on and shows nothing
// generated is reported with its cause after a few seconds, and nothing else is: not a menu, not a
// method that presents its own frames, not settings still being applied, not a probing trickle.
#include "veyra/engine/PushFgHealth.h"
#include <cstdio>
#include <string>
using namespace veyra::engine;
static int failures = 0;
static void check(bool ok, const std::string& label) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str()); if (!ok) ++failures; }

// A healthy 30 -> 120 session.
static PushFgSample healthy() {
    PushFgSample s;
    s.running = true; s.wanted = true; s.measurable = true; s.paused = false; s.settled = true; s.active = true;
    s.budgetLimited = false; s.contentFps = 30; s.realFps = 30; s.generatedFps = 90; s.multiplierNow = 4;
    return s;
}
static PushFgIdle after(PushFgHealth& h, const PushFgSample& s, int seconds) {
    PushFgIdle last = PushFgIdle::None;
    for (int i = 0; i < seconds; ++i) last = h.update(s);
    return last;
}

int main() {
    // ---- One second -------------------------------------------------------------------------------
    check(PushFgHealth::kHoldSeconds >= 8, "a method gets several seconds to start before anything is said");
    check(pushFgIdleNow(healthy()) == PushFgIdle::None, "a healthy session is no fault");
    { auto s = healthy(); s.generatedFps = 0; check(pushFgIdleNow(s) == PushFgIdle::NoFrames, "the method runs, real frames arrive, nothing generated: no frames"); }
    { auto s = healthy(); s.generatedFps = 0; s.active = false; check(pushFgIdleNow(s) == PushFgIdle::Inactive, "the method is not running: inactive"); }
    { auto s = healthy(); s.generatedFps = 0; s.budgetLimited = true; check(pushFgIdleNow(s) == PushFgIdle::TooSlow, "the scheduler skips every frame: too slow"); }
    { auto s = healthy(); s.generatedFps = 0; s.multiplierNow = 1; check(pushFgIdleNow(s) == PushFgIdle::SameRate, "a multiplier of 1 in use: the target is the game's own rate"); }
    { auto s = healthy(); s.generatedFps = 0; s.multiplierNow = 1; s.active = false; check(pushFgIdleNow(s) == PushFgIdle::Inactive, "a method that is not running comes first"); }
    { auto s = healthy(); s.generatedFps = 0; s.multiplierNow = 0; check(pushFgIdleNow(s) == PushFgIdle::NoFrames, "an unknown multiplier is no excuse"); }
    { auto s = healthy(); s.generatedFps = 0; s.displayLimited = true; check(pushFgIdleNow(s) == PushFgIdle::SameRate, "a screen that cannot show more than the game's frames: its limit, not a fault"); }
    { auto s = healthy(); s.generatedFps = 0; s.displayLimited = true; s.budgetLimited = true; check(pushFgIdleNow(s) == PushFgIdle::SameRate, "the screen's limit comes before the scheduler's skipping"); }
    { auto s = healthy(); s.generatedFps = 0; s.displayLimited = true; s.active = false; check(pushFgIdleNow(s) == PushFgIdle::Inactive, "but a method that is not running is still that"); }
    { auto s = healthy(); s.displayLimited = true; check(pushFgIdleNow(s) == PushFgIdle::None, "a screen limit with frames still shown is nothing"); }
    check(!pushFgIdleIsFault(PushFgIdle::None) && !pushFgIdleIsFault(PushFgIdle::SameRate), "generating and the same rate are no faults");
    check(pushFgIdleIsFault(PushFgIdle::Inactive) && pushFgIdleIsFault(PushFgIdle::TooSlow) && pushFgIdleIsFault(PushFgIdle::NoFrames), "the other three are");
    // Nothing to judge.
    { auto s = healthy(); s.generatedFps = 0; s.running = false; check(pushFgIdleNow(s) == PushFgIdle::None, "nothing playing"); }
    { auto s = healthy(); s.generatedFps = 0; s.wanted = false; check(pushFgIdleNow(s) == PushFgIdle::None, "frame generation off"); }
    { auto s = healthy(); s.generatedFps = 0; s.measurable = false; check(pushFgIdleNow(s) == PushFgIdle::None, "a method that presents its own frames is not judged"); }
    { auto s = healthy(); s.generatedFps = 0; s.paused = true; check(pushFgIdleNow(s) == PushFgIdle::None, "a comparison view or a test pauses the watch"); }
    { auto s = healthy(); s.generatedFps = 0; s.settled = false; check(pushFgIdleNow(s) == PushFgIdle::None, "settings still being applied"); }
    { auto s = healthy(); s.generatedFps = 0; s.contentFps = 2; check(pushFgIdleNow(s) == PushFgIdle::None, "a picture that hardly changes (a menu): no pairs, no fault"); }
    { auto s = healthy(); s.generatedFps = 0; s.realFps = 3; check(pushFgIdleNow(s) == PushFgIdle::None, "hardly any real frames shown: not judged"); }
    // A trickle is still nothing; a real share is something.
    { auto s = healthy(); s.generatedFps = 1.5; check(pushFgIdleNow(s) == PushFgIdle::NoFrames, "a pair let through now and then is still nothing (30 FPS game)"); }
    { auto s = healthy(); s.generatedFps = 2.9; check(pushFgIdleNow(s) == PushFgIdle::NoFrames, "under a tenth of the game's rate"); }
    { auto s = healthy(); s.generatedFps = 3.0; check(pushFgIdleNow(s) == PushFgIdle::None, "a tenth of the game's rate counts as generating"); }
    { auto s = healthy(); s.contentFps = 60; s.realFps = 60; s.generatedFps = 5; check(pushFgIdleNow(s) == PushFgIdle::NoFrames, "60 FPS game: 5 generated a second is nothing"); }
    { auto s = healthy(); s.contentFps = 60; s.realFps = 60; s.generatedFps = 60; check(pushFgIdleNow(s) == PushFgIdle::None, "60 -> 120 is healthy"); }
    { auto s = healthy(); s.contentFps = 12; s.realFps = 12; s.generatedFps = 1.9; check(pushFgIdleNow(s) == PushFgIdle::NoFrames, "the floor is 2 a second"); }
    // A reduced group (the scheduler generates fewer than asked) is not this fault.
    { auto s = healthy(); s.budgetLimited = true; s.generatedFps = 30; check(pushFgIdleNow(s) == PushFgIdle::None, "a reduced multiplier still generates"); }

    // ---- Over time --------------------------------------------------------------------------------
    {
        PushFgHealth h;
        auto bad = healthy(); bad.generatedFps = 0;
        check(after(h, bad, PushFgHealth::kHoldSeconds - 1) == PushFgIdle::None, "nothing is said in the first seconds");
        check(h.update(bad) == PushFgIdle::NoFrames, "and it is said once it has lasted");
        check(after(h, bad, 30) == PushFgIdle::NoFrames && h.reported() == PushFgIdle::NoFrames, "it stays said while it lasts");
        check(h.update(healthy()) == PushFgIdle::None, "and stops the moment frames are generated");
        check(after(h, bad, PushFgHealth::kHoldSeconds - 1) == PushFgIdle::None, "a new spell starts counting again");
    }
    {   // An interruption starts the count again: nearly long enough, one good second, nearly long enough again.
        PushFgHealth h;
        auto bad = healthy(); bad.generatedFps = 0;
        after(h, bad, PushFgHealth::kHoldSeconds - 1); h.update(healthy());
        check(after(h, bad, PushFgHealth::kHoldSeconds - 1) == PushFgIdle::None, "a good second in between starts the count again");
        check(h.update(bad) == PushFgIdle::NoFrames, "only an unbroken spell is reported");
    }
    {   // The cause first reported stays, also when the scheduler's flag comes and goes.
        PushFgHealth h;
        auto slow = healthy(); slow.generatedFps = 0; slow.budgetLimited = true;
        auto none = healthy(); none.generatedFps = 0;
        check(after(h, slow, PushFgHealth::kHoldSeconds) == PushFgIdle::TooSlow, "too slow is reported");
        check(h.update(none) == PushFgIdle::TooSlow && h.update(slow) == PushFgIdle::TooSlow, "and does not flicker to another cause");
        // The cause at the moment of reporting is the one given.
        PushFgHealth g;
        after(g, none, PushFgHealth::kHoldSeconds - 1);
        check(g.update(slow) == PushFgIdle::TooSlow, "the cause of the second it is reported in");
    }
    {   // Changing the settings (not settled) clears it: the new settings get their own count.
        PushFgHealth h;
        auto bad = healthy(); bad.generatedFps = 0; bad.active = false;
        check(after(h, bad, 10) == PushFgIdle::Inactive, "an inactive method is reported");
        auto applying = bad; applying.settled = false;
        check(h.update(applying) == PushFgIdle::None, "cleared while new settings are applied");
        check(after(h, bad, PushFgHealth::kHoldSeconds - 1) == PushFgIdle::None && h.update(bad) == PushFgIdle::Inactive, "and counted again after");
    }
    {   // A menu in the middle: no report during it, and none right after it either.
        PushFgHealth h;
        auto bad = healthy(); bad.generatedFps = 0;
        auto menu = bad; menu.contentFps = 1; menu.realFps = 60;
        after(h, bad, PushFgHealth::kHoldSeconds - 2);
        check(after(h, menu, 20) == PushFgIdle::None, "a still picture is never reported");
        check(after(h, bad, PushFgHealth::kHoldSeconds - 1) == PushFgIdle::None, "and the count starts again when the game moves");
    }
    {   // The same rate is reported as such, and never as a fault.
        PushFgHealth h;
        auto same = healthy(); same.generatedFps = 0; same.multiplierNow = 1;
        const auto got = after(h, same, PushFgHealth::kHoldSeconds);
        check(got == PushFgIdle::SameRate && !pushFgIdleIsFault(got), "a target equal to the game's rate is information, not a fault");
    }
    {   // A healthy session, an hour of it, never reports.
        PushFgHealth h;
        bool quiet = true;
        for (int i = 0; i < 3600; ++i) {
            auto s = healthy();
            s.generatedFps = 60 + (i * 37) % 31;          // 60 to 90 a second
            s.budgetLimited = i % 11 == 0;                // the scheduler trims a group now and then
            if (h.update(s) != PushFgIdle::None) quiet = false;
        }
        check(quiet, "an hour of healthy play is never reported");
        h.reset();
        check(h.reported() == PushFgIdle::None, "reset");
    }
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}

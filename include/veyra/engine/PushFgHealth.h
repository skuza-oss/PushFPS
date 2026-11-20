#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFPS frame generation health. Frame generation switched on while nothing generated is shown is a
// fault the user cannot see: the picture plays, at the game's own frame rate. (Until 3.2.2 PushFlow
// ran like that in the program, with no error anywhere.) This watches the frames that are really
// presented and says so, with the cause, once it has lasted a few seconds.
//
// Pure: the bridge feeds one sample a second (PushFps.cpp, pushFlowBudgetWatch) from the engine's own
// counters of presented frames (FrameFlowWindow.h). Tests: tests/unit/PushFgHealthTests.cpp.
#include <algorithm>

namespace veyra::engine {

enum class PushFgIdle : int {
    None = 0,      // generating, switched off, or nothing to judge
    SameRate = 1,  // nothing to add: the multiplier in use is 1 (the target is the game's own frame rate),
                   // or the screen cannot show more than the game's own frames (no fault)
    Inactive = 2,  // the chosen method is not running
    TooSlow = 3,   // the scheduler skips the generated frames: the graphics card does not keep up
    NoFrames = 4,  // the method runs and real frames arrive, yet nothing generated is shown
};
// A fault the user should hear about (SameRate is a setting, not a fault).
constexpr bool pushFgIdleIsFault(PushFgIdle idle) { return int(idle) >= int(PushFgIdle::Inactive); }

struct PushFgSample {
    bool running = false;        // a source is playing
    bool wanted = false;         // frame generation is on with a multiplier above 1 (the applied settings)
    bool measurable = true;      // false for a method that makes its frames while presenting: not counted here
    bool paused = false;         // a comparison view is on screen, or a test is running that holds frames back
    bool settled = false;        // the applied settings are the ones asked for and the rate window is full
    bool active = false;         // the engine reports the method as running
    bool budgetLimited = false;  // the scheduler is skipping generated frames
    bool displayLimited = false; // the engine's own hint: the output is more than the screen can show
    double contentFps = 0;       // distinct game pictures a second
    double realFps = 0;          // real frames presented a second
    double generatedFps = 0;     // generated frames presented a second
    double multiplierNow = 0;    // the multiplier the last pair used (0: not known)
};

// What this one second looks like. None also when there is nothing to judge: nothing playing,
// settings still being applied, or a picture that hardly changes (a menu, a paused game: with
// duplicate removal there are no pairs to work on, and that is as it should be).
inline PushFgIdle pushFgIdleNow(const PushFgSample& s) {
    if (!s.running || !s.wanted || !s.measurable || s.paused || !s.settled) return PushFgIdle::None;
    if (s.contentFps < 10.0 || s.realFps < 10.0) return PushFgIdle::None;
    // "Practically nothing": a scheduler that lets a probing pair through now and then still counts
    // as nothing (a tenth of the game's rate, at least 2 a second).
    if (s.generatedFps >= std::max(2.0, 0.1 * s.contentFps)) return PushFgIdle::None;
    if (!s.active) return PushFgIdle::Inactive;
    // Frames that are made but have no place on the screen are not shown, and that is the screen's
    // limit (the interface has its own hint for it), not a method that fails.
    if (s.displayLimited || (s.multiplierNow > 0 && s.multiplierNow < 1.5)) return PushFgIdle::SameRate;
    if (s.budgetLimited) return PushFgIdle::TooSlow;
    return PushFgIdle::NoFrames;
}

// One sample a second. A cause is reported once nothing has come out for kHoldSeconds in a row (ten: a method may take a few seconds to start), and
// then stays the same until frames are generated again (or there is nothing to judge), so the
// message does not flicker between two causes.
class PushFgHealth {
public:
    static constexpr int kHoldSeconds = 10;
    PushFgIdle update(const PushFgSample& s) {
        const PushFgIdle now = pushFgIdleNow(s);
        if (now == PushFgIdle::None) { seconds_ = 0; reported_ = PushFgIdle::None; return reported_; }
        if (seconds_ < kHoldSeconds) ++seconds_;
        if (seconds_ >= kHoldSeconds && reported_ == PushFgIdle::None) reported_ = now;
        return reported_;
    }
    PushFgIdle reported() const { return reported_; }
    void reset() { seconds_ = 0; reported_ = PushFgIdle::None; }
private:
    int seconds_ = 0;
    PushFgIdle reported_ = PushFgIdle::None;
};

} // namespace veyra::engine

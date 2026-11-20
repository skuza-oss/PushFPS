// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFlow 2's experiments: which exist in this build, which a user asked for, and which may run.
// Pure (no Windows, no Qt): the unit test builds it with g++ too. docs/PUSHFLOW2.md has the plan.
//
// The rules this file holds:
//  - Every experiment is OFF unless its switch was turned on by the user. A preference that is missing,
//    damaged, written by a newer version or of the wrong kind reads as "everything off".
//  - An experiment has four states a user can be shown: not in this build, not available here (with a
//    reason the part itself gives), off, on. "Asked for" and "running" are told apart: a part that was
//    asked for and could not start says so, and PushFlow runs as it does without it.
//  - One emergency stop for all of them, for this run of the program only: the environment variable
//    VEYRA_PUSHFLOW2_OFF, which `PushFPS.exe --safe-mode` sets. Saved choices are not touched by it.
//  - Nothing here is part of the settings a preset stores: the preference `pushFlow2` is the only
//    place, a small JSON object of yes/no values, so an older PushFPS reads its own settings as before.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace veyra::pushflow2 {

enum class Experiment : uint32_t { Fusion = 0, Guard, Predict, Dual, Response, Memory, Count };
inline constexpr uint32_t kCount = uint32_t(Experiment::Count);
// The names in the preference; never change one that has shipped.
inline constexpr const char* kKeys[kCount] = {"fusion", "guard", "predict", "dual", "response", "memory"};
// What this build contains. A part is listed here in the commit that adds its code and its tests.
inline constexpr bool kBuilt[kCount] = {true, true, true, false, false, false};

inline constexpr bool built(Experiment e) { return uint32_t(e) < kCount && kBuilt[uint32_t(e)]; }
inline constexpr const char* key(Experiment e) { return uint32_t(e) < kCount ? kKeys[uint32_t(e)] : ""; }

// What the user asked for: one bit per experiment.
struct Flags {
    uint32_t bits = 0;
    constexpr bool get(Experiment e) const { return uint32_t(e) < kCount && (bits >> uint32_t(e) & 1u) != 0; }
    constexpr Flags with(Experiment e, bool on) const {
        if (uint32_t(e) >= kCount) return *this;
        return Flags{on ? bits | (1u << uint32_t(e)) : bits & ~(1u << uint32_t(e))};
    }
    constexpr bool operator==(const Flags&) const = default;
};

// The preference's text: {"v":1,"fusion":true,...}. Only `true` turns something on. Read strictly: an
// object of names and true / false / small whole numbers, nothing nested. Anything else is not ours to
// guess at, and reads as everything off (`ok` false). Names this version does not know are skipped, so
// a preference written by a newer PushFPS still gives this one its own switches.
struct Parsed { Flags flags; bool ok = false; };
inline Parsed parse(std::string_view text) {
    Parsed out;
    size_t i = 0;
    const auto space = [&] { while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) ++i; };
    if (text.size() > 1024) return out;
    space();
    if (i >= text.size() || text[i] != '{') return out;
    ++i;
    Flags flags;
    space();
    if (i < text.size() && text[i] == '}') { ++i; space(); out.ok = i == text.size(); return out; }
    for (;;) {
        space();
        if (i >= text.size() || text[i] != '"') return out;
        const size_t start = ++i;
        while (i < text.size() && text[i] != '"') { if (text[i] == '\\' || static_cast<unsigned char>(text[i]) < 0x20) return out; ++i; }
        if (i >= text.size()) return out;
        const std::string_view name = text.substr(start, i - start);
        ++i;
        space();
        if (i >= text.size() || text[i] != ':') return out;
        ++i;
        space();
        bool value = false;
        if (text.substr(i, 4) == "true") { value = true; i += 4; }
        else if (text.substr(i, 5) == "false") { i += 5; }
        else {
            const size_t digits = i;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9' && i - digits < 9) ++i;   // "v": 1
            if (i == digits) return out;
        }
        for (uint32_t e = 0; e < kCount; ++e) if (name == kKeys[e]) flags = flags.with(Experiment(e), value);
        space();
        if (i < text.size() && text[i] == ',') { ++i; continue; }
        if (i < text.size() && text[i] == '}') { ++i; break; }
        return out;
    }
    space();
    if (i != text.size()) return out;
    out.flags = flags; out.ok = true;
    return out;
}
inline std::string serialize(Flags flags) {
    std::string s = "{\"v\":1";
    for (uint32_t e = 0; e < kCount; ++e) { s += ",\""; s += kKeys[e]; s += flags.get(Experiment(e)) ? "\":true" : "\":false"; }
    return s + "}";
}

// --- process-wide: what was asked for, and the emergency stop ---------------------------------------
namespace detail {
inline std::atomic<uint32_t>& requestedBits() { static std::atomic<uint32_t> bits{0}; return bits; }
inline std::atomic<bool>& stopped() { static std::atomic<bool> off{false}; return off; }
inline std::atomic<bool>& predictPausedFlag() { static std::atomic<bool> paused{false}; return paused; }
}
inline void setRequested(Flags flags) { detail::requestedBits().store(flags.bits, std::memory_order_relaxed); }
inline Flags requested() { return Flags{detail::requestedBits().load(std::memory_order_relaxed)}; }
inline void setEmergencyOff(bool off) { detail::stopped().store(off, std::memory_order_relaxed); }
inline bool emergencyOff() { return detail::stopped().load(std::memory_order_relaxed); }
// Whether a part should try to run: in this build, asked for, and not stopped for this run.
inline bool wanted(Experiment e) { return built(e) && !emergencyOff() && requested().get(e); }
// Predict steps aside, without anything being rebuilt, while something measures in-between frames (the
// optimiser; the engine adds the quality test and the benchmark window itself): read for every pair.
inline void setPredictPaused(bool paused) { detail::predictPausedFlag().store(paused, std::memory_order_relaxed); }
inline bool predictPaused() { return detail::predictPausedFlag().load(std::memory_order_relaxed); }

// What a user is shown for one experiment. `available` and `running` come from the part itself.
enum class State : uint32_t { NotBuilt = 0, Stopped, Off, Unavailable, Waiting, Running };
inline State state(Experiment e, bool asked, bool stoppedNow, bool available, bool running) {
    if (!built(e)) return State::NotBuilt;
    if (!asked) return State::Off;
    if (stoppedNow) return State::Stopped;
    if (running) return State::Running;
    return available ? State::Waiting : State::Unavailable;   // waiting: asked for, starts with the next pair
}

} // namespace veyra::pushflow2

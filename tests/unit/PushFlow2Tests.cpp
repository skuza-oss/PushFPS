// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFlow 2's experiment switches (pushflow/PushFlow2.h): everything is off unless asked for, a damaged
// or foreign preference reads as off, a newer version's preference still gives this one its own
// switches, the emergency stop wins for this run and changes nothing saved, and each state is told
// apart. Pure: also builds with g++.
#include "veyra/pushflow/PushFlow2.h"
#include <cstdio>
#include <string>

using namespace veyra::pushflow2;

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); failures += !ok; };

    // --- off unless asked -------------------------------------------------------------------------
    {
        bool allOff = true;
        for (uint32_t e = 0; e < kCount; ++e) allOff = allOff && !Flags{}.get(Experiment(e)) && !wanted(Experiment(e));
        check(allOff && requested() == Flags{} && !emergencyOff(), "nothing is on until a user turns it on");
        check(parse("{}").ok && parse("{}").flags == Flags{} && parse(" { } ").ok, "an empty preference is everything off");
    }
    // --- the preference, read strictly --------------------------------------------------------------
    {
        const auto p = parse("{\"v\":1,\"fusion\":true,\"guard\":false}");
        check(p.ok && p.flags.get(Experiment::Fusion) && !p.flags.get(Experiment::Guard) && !p.flags.get(Experiment::Predict), "true turns a part on, false and absent leave it off");
        const auto spaced = parse("\n{ \"fusion\" : true ,\t\"memory\":true }\r\n");
        check(spaced.ok && spaced.flags.get(Experiment::Fusion) && spaced.flags.get(Experiment::Memory), "spaces and line ends are allowed");
        const auto newer = parse("{\"v\":7,\"fusion\":true,\"somethingNew\":true,\"level\":3}");
        check(newer.ok && newer.flags == Flags{}.with(Experiment::Fusion, true), "a newer version's names are skipped, this version's switches still read");
        const auto twice = parse("{\"fusion\":true,\"fusion\":false}");
        check(twice.ok && !twice.flags.get(Experiment::Fusion), "a name given twice: the last one counts");
        bool round = true;
        for (uint32_t bits = 0; bits < (1u << kCount); ++bits) { const auto back = parse(serialize(Flags{bits})); round = round && back.ok && back.flags.bits == bits; }
        check(round, "every combination survives being written and read back");
    }
    // --- damaged, foreign and hostile text reads as everything off ---------------------------------
    {
        const char* bad[] = {"", " ", "null", "true", "[]", "[true]", "{", "}", "{\"fusion\":true", "{\"fusion\":tru}", "{\"fusion\":\"true\"}", "{\"fusion\":1.5}",
                             "{\"fusion\":{\"on\":true}}", "{\"fusion\":[true]}", "{fusion:true}", "{\"fusion\" true}", "{\"fusion\":true,}", "{,\"fusion\":true}",
                             "{\"fusion\":true}x", "{\"fusion\":true}{\"guard\":true}", "{\"fu\\u0073ion\":true}", "{\"fusion\":null}", "{\"fusion\":-1}", "{\"fusion\":TRUE}",
                             "{\"v\":1234567890123}", "{\"fusion\":true,\"guard\"}", "\xef\xbb\xbf{\"fusion\":true}"};
        bool off = true;
        for (const char* text : bad) { const auto p = parse(text); off = off && !p.ok && p.flags == Flags{}; }
        check(off, "damaged or foreign text is not guessed at: everything off");
        check(!parse(std::string(2000, ' ')).ok && !parse("{\"a\":true" + std::string(1100, ' ') + "}").ok, "a text far larger than the preference can be is refused");
        std::string cut = serialize(Flags{}.with(Experiment::Fusion, true).with(Experiment::Dual, true));
        bool truncated = true;
        for (size_t n = 0; n + 1 < cut.size(); ++n) truncated = truncated && !parse(std::string_view(cut).substr(0, n)).ok;
        check(truncated, "a preference cut off at any place (the program killed while saving) reads as everything off");
    }
    // --- what runs: in this build, asked for, not stopped -------------------------------------------
    {
        setRequested(Flags{0xFFFFFFFFu});
        bool onlyBuilt = true;
        for (uint32_t e = 0; e < kCount; ++e) onlyBuilt = onlyBuilt && wanted(Experiment(e)) == kBuilt[e];
        check(onlyBuilt, "asking for a part this build does not contain starts nothing");
        check(!wanted(Experiment::Count) && !built(Experiment::Count) && !Flags{0xFFFFFFFFu}.get(Experiment::Count), "a part that does not exist is never on");
        setEmergencyOff(true);
        bool stopped = true;
        for (uint32_t e = 0; e < kCount; ++e) stopped = stopped && !wanted(Experiment(e));
        check(stopped && requested().bits == 0xFFFFFFFFu, "the emergency stop turns every part off for this run and leaves what was asked for as it is");
        setEmergencyOff(false);
        setRequested(Flags{});
        check(!wanted(Experiment::Fusion), "turned off again, nothing runs");
    }
    // --- the state a user is shown ---------------------------------------------------------------------
    {
        const Experiment e = Experiment::Fusion;
        check(state(e, false, false, true, false) == State::Off && state(e, false, true, true, true) == State::Off, "not asked for: off, whatever else is true");
        check(state(e, true, true, true, true) == State::Stopped, "asked for and stopped for this run: said so");
        check(state(e, true, false, true, true) == State::Running && state(e, true, false, false, true) == State::Running, "running is running");
        check(state(e, true, false, true, false) == State::Waiting && state(e, true, false, false, false) == State::Unavailable, "asked for and not running: waiting to start, or not available here");
        bool notBuilt = true;
        for (uint32_t k = 0; k < kCount; ++k) if (!kBuilt[k]) notBuilt = notBuilt && state(Experiment(k), true, false, true, true) == State::NotBuilt;
        check(notBuilt, "a part not in this build is shown as that, never as running");
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

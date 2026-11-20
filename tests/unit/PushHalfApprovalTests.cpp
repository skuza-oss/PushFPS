// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFlow half precision (engine/PushHalfApproval.h): a passed test holds for the card, driver and
// PushFPS version it was measured with, and for nothing else.
#include "veyra/engine/PushHalfApproval.h"
#include <cstdio>
#include <string>
using namespace veyra::engine;
static int failures = 0;
static void check(bool ok, const std::string& label) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str()); if (!ok) ++failures; }

int main() {
    const std::string k = pushHalfApprovalKey(0x10DE, 0x2B85, "32.0.15.7652", "3.2.5");
    check(k == "10de:2b85|32.0.15.7652|3.2.5", "the key names the card, its driver and the PushFPS version: " + k);
    check(pushHalfApprovalHolds(k, k), "the same card, driver and version: the pass holds");
    check(!pushHalfApprovalHolds(k, pushHalfApprovalKey(0x10DE, 0x2684, "32.0.15.7652", "3.2.5")), "another card of the same maker: tested again");
    check(!pushHalfApprovalHolds(k, pushHalfApprovalKey(0x1002, 0x7550, "32.0.15.7652", "3.2.5")), "another maker's card: tested again");
    check(!pushHalfApprovalHolds(k, pushHalfApprovalKey(0x10DE, 0x2B85, "32.0.15.8100", "3.2.5")), "a driver update: tested again");
    check(!pushHalfApprovalHolds(k, pushHalfApprovalKey(0x10DE, 0x2B85, "32.0.15.7652", "3.2.6")), "a PushFPS update (it may ship other models or another runtime): tested again");
    check(!pushHalfApprovalHolds("", k), "a pass stored without a key (before 3.2.5) holds for nothing");
    check(!pushHalfApprovalHolds(k, ""), "an unknown card now: nothing holds");
    check(!pushHalfApprovalHolds("", ""), "nothing against nothing does not hold either");
    check(pushHalfApprovalKey(0, 0x2B85, "1.0", "3.2.5").empty() && pushHalfApprovalKey(0x10DE, 0, "1.0", "3.2.5").empty(), "no card: no key");
    check(pushHalfApprovalKey(0x10DE, 0x2B85, "1.0", "").empty(), "no version: no key");
    // A driver version that cannot be read still gives a key (card and version), and it is its own key.
    const std::string noDriver = pushHalfApprovalKey(0x10DE, 0x2B85, "", "3.2.5");
    check(noDriver == "10de:2b85||3.2.5" && pushHalfApprovalHolds(noDriver, noDriver) && !pushHalfApprovalHolds(noDriver, k), "an unreadable driver version: a key of its own");
    check(pushHalfApprovalKey(0x1002, 0x7550, "32.0.21025.1024", "2.1.5-pushfps.812") == "1002:7550|32.0.21025.1024|2.1.5-pushfps.812", "an AMD card and a test build");
    check(pushHalfApprovalKey(0x1'10DE, 0xF'2B85, "1", "3") == "10de:2b85|1|3", "only the 16-bit identifiers are used");
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}

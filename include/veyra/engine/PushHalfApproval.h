#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFlow half precision: what a passed test holds for. Whether the fp16 model works depends on
// the graphics card, its driver and the runtime and models a PushFPS version ships: the same switch
// that is fine on one card took the D3D12 device down on another. So a pass counts for exactly the
// card, driver and PushFPS version it was measured with. Until 3.2.5 it was kept for good, and a new
// card, a driver update or a PushFPS update started PushFlow in half precision untested.
//
// Pure. The bridge stores the key with the test's result (pref pushFlowHalfProbe, "key") and checks
// it at every start (QmlPlayerBridge.cpp); tests tests/unit/PushHalfApprovalTests.cpp.
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace veyra::engine {

// "10de:2b85|32.0.15.7652|3.2.5": the card (PCI vendor and device), its driver, the PushFPS version.
// Empty when the card or the version is not known: then nothing can be held to it.
inline std::string pushHalfApprovalKey(uint32_t vendorId, uint32_t deviceId, std::string_view driver, std::string_view build) {
    if (!vendorId || !deviceId || build.empty()) return {};
    char card[32];
    std::snprintf(card, sizeof card, "%04x:%04x", vendorId & 0xFFFFu, deviceId & 0xFFFFu);
    std::string key = card;
    key += '|'; key += driver;
    key += '|'; key += build;
    return key;
}
// A stored pass still holds when it was measured with exactly what is here now. A pass stored
// without a key (before 3.2.5) holds for nothing: it is tested again.
inline bool pushHalfApprovalHolds(std::string_view stored, std::string_view current) {
    return !stored.empty() && !current.empty() && stored == current;
}

} // namespace veyra::engine

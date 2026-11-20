// SPDX-License-Identifier: GPL-3.0-or-later
// Part of PushFPS, a modified version of Veyra.
#pragma once
// Keyboard and mouse as a PS5 controller for Remote Play: which key presses which button, and how
// mouse movement becomes right-stick deflection. Pure logic (no Win32 calls), so it can be tested
// on its own; Ps5KeyboardMouse feeds it key and mouse events and reads one ControllerState per poll.
//
// The settings live in a plain INI file (keyboard-mouse.ini) that is written with the defaults when
// missing and read again whenever it changes, so the mapping and the mouse feel can be tuned while
// playing. Keys are named as on the keyboard (W, Space, LShift, Mouse1, WheelUp, F5, Up, Num0 ...),
// actions as on the controller (Cross, R2, LeftUp, DpadLeft ...); several keys may share an action.
//
// Mouse to stick: a stick gives a speed, a mouse gives a distance. Each poll the movement since the
// previous one is turned into a speed (counts per second), lightly smoothed, and mapped to a
// deflection: nothing below a count of noise, then the game's own dead zone is jumped over
// (antiDeadzone) and the rest rises with the speed (sensitivity: counts per second for a full
// deflection; curve: 1 linear, below 1 more precise small movements, above 1 the reverse).
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "veyra/remoteplay/Types.h"

namespace veyra::remoteplay {

class KeyboardMouseMap {
public:
    enum class Action : uint8_t {
        None, Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3, Options, Share, Touchpad, PS,
        DpadUp, DpadDown, DpadLeft, DpadRight,
        LeftUp, LeftDown, LeftLeft, LeftRight,      // left stick, full deflection
        RightUp, RightDown, RightLeft, RightRight,  // right stick from keys (added to the mouse)
        Walk,                                       // held: the left stick at walkScale
        Count
    };
    // Mouse buttons and the wheel get key codes above the Windows virtual-key range.
    static constexpr uint32_t kMouse1 = 0x101, kMouse2 = 0x102, kMouse3 = 0x103, kMouse4 = 0x104, kMouse5 = 0x105,
                              kWheelUp = 0x110, kWheelDown = 0x111;

    struct Settings {
        double sensitivity = 2500;   // mouse counts per second for a full right-stick deflection
        double antiDeadzone = 0.20;  // smallest deflection while the mouse moves (the game ignores less)
        double curve = 1.0;
        double smoothing = 0.5;      // 0 = none; share of the previous speed kept per 8 ms poll
        double invertY = 0;          // 1: mouse forward looks down
        double yScale = 1.0;         // vertical speed relative to horizontal
        double walkScale = 0.5;
        double wheelPulseMs = 60;    // how long a wheel notch holds its button
    };

    KeyboardMouseMap() { load(defaultIni()); }

    // The file written when none exists; also documents every name.
    static std::string defaultIni() {
        return
            "; PushFPS keyboard and mouse for PS5 Remote Play.\n"
            "; F12 turns it on and off while a PS5 stream runs (the cursor is hidden and kept in the window);\n"
            "; Home also releases it and opens the PushFPS panel. Changes here apply within a second.\n"
            ";\n"
            "; Actions: Cross Circle Square Triangle L1 R1 L2 R2 L3 R3 Options Share Touchpad PS\n"
            ";          DpadUp DpadDown DpadLeft DpadRight LeftUp LeftDown LeftLeft LeftRight\n"
            ";          RightUp RightDown RightLeft RightRight Walk None\n"
            "; Keys: A-Z 0-9 F1-F24 Space Enter Esc Tab Backspace CapsLock LShift RShift LCtrl RCtrl LAlt RAlt\n"
            ";       Up Down Left Right Insert Delete Home End PageUp PageDown Num0-Num9 Grave Minus Equals\n"
            ";       LBracket RBracket Semicolon Quote Comma Period Slash Backslash\n"
            ";       Mouse1 (left) Mouse2 (right) Mouse3 (middle) Mouse4 Mouse5 WheelUp WheelDown\n"
            "; (F12 and Home are reserved.) The defaults follow GTA V's PlayStation layout.\n"
            "\n"
            "[Mouse]\n"
            "; counts per second for a full stick deflection: lower = faster camera\n"
            "sensitivity=2500\n"
            "; smallest deflection while moving, to get past the game's dead zone (0-0.6)\n"
            "antiDeadzone=0.20\n"
            "; 1 = linear; 0.7 = finer small movements; 1.3 = the reverse\n"
            "curve=1.0\n"
            "; 0 = raw, up to 0.9 = smoother but later\n"
            "smoothing=0.5\n"
            "invertY=0\n"
            "yScale=1.0\n"
            "\n"
            "[Movement]\n"
            "walkScale=0.5\n"
            "wheelPulseMs=60\n"
            "\n"
            "[Keys]\n"
            "W=LeftUp\nS=LeftDown\nA=LeftLeft\nD=LeftRight\n"
            "Mouse1=R2\nMouse2=L2\n"
            "LShift=Cross\nSpace=Square\nF=Triangle\nR=Circle\nBackspace=Circle\nEnter=Cross\n"
            "Q=R1\nTab=L1\nE=DpadRight\nX=DpadDown\nLCtrl=L3\nC=R3\nMouse3=R3\nV=Touchpad\n"
            "Esc=Options\nM=Options\nP=Share\nLAlt=Walk\n"
            "Up=DpadUp\nDown=DpadDown\nLeft=DpadLeft\nRight=DpadRight\n"
            "1=DpadUp\n2=DpadRight\n3=DpadDown\n4=DpadLeft\n"
            "WheelUp=DpadUp\nWheelDown=DpadDown\n";
    }

    // Replaces the settings and the key map from INI text; unknown lines are ignored. Returns the
    // number of key lines that were understood.
    int load(const std::string& ini) {
        Settings s; std::map<uint32_t, std::vector<Action>> keys;
        std::istringstream in(ini); std::string line, section; int understood = 0;
        while (std::getline(in, line)) {
            line = trim(line.substr(0, line.find(';')));
            if (line.empty()) continue;
            if (line.front() == '[') { section = lower(trim(line.substr(1, line.find(']') - 1))); continue; }
            const auto eq = line.find('='); if (eq == std::string::npos) continue;
            const std::string name = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
            if (section == "keys") {
                const uint32_t key = keyCode(name); const Action action = actionCode(value);
                if (key && (action != Action::None || lower(value) == "none")) {
                    if (action == Action::None) keys.erase(key); else keys[key].push_back(action);
                    ++understood;
                }
            } else {
                char* end = nullptr; const double v = std::strtod(value.c_str(), &end);
                if (end == value.c_str()) continue;
                const std::string n = lower(name);
                if (n == "sensitivity" && v >= 50 && v <= 100000) s.sensitivity = v;
                else if (n == "antideadzone" && v >= 0 && v <= 0.6) s.antiDeadzone = v;
                else if (n == "curve" && v >= 0.3 && v <= 3) s.curve = v;
                else if (n == "smoothing" && v >= 0 && v <= 0.9) s.smoothing = v;
                else if (n == "inverty") s.invertY = v != 0 ? 1 : 0;
                else if (n == "yscale" && v >= 0.1 && v <= 10) s.yScale = v;
                else if (n == "walkscale" && v >= 0.1 && v <= 1) s.walkScale = v;
                else if (n == "wheelpulsems" && v >= 10 && v <= 500) s.wheelPulseMs = v;
            }
        }
        settings_ = s; keys_ = std::move(keys);
        return understood;
    }
    const Settings& settings() const { return settings_; }

    // Events (key codes: Windows virtual keys with left/right modifiers told apart, or kMouse*/kWheel*).
    void key(uint32_t code, bool down) { if (code < down_.size()) down_[code] = down; }
    void mouseMove(int dx, int dy) { dx_ += dx; dy_ += dy; }
    void wheel(int notches, double nowMs) {
        if (notches > 0) wheelUpUntil_ = nowMs + settings_.wheelPulseMs;
        if (notches < 0) wheelDownUntil_ = nowMs + settings_.wheelPulseMs;
    }
    void releaseAll() { down_.fill(false); dx_ = dy_ = 0; vx_ = vy_ = 0; wheelUpUntil_ = wheelDownUntil_ = 0; lastPollMs_ = -1; }

    // One controller state from everything since the previous call.
    ControllerState poll(double nowMs) {
        ControllerState st; st.inputActive = true;
        std::array<bool, size_t(Action::Count)> act{};
        for (const auto& [code, actions] : keys_) {
            bool on = code < down_.size() && down_[code];
            if (code == kWheelUp) on = nowMs < wheelUpUntil_;
            if (code == kWheelDown) on = nowMs < wheelDownUntil_;
            if (on) for (Action a : actions) act[size_t(a)] = true;
        }
        using B = ControllerState::Button;
        const std::pair<Action, uint32_t> buttons[] = {
            {Action::Cross, B::Cross}, {Action::Circle, B::Circle}, {Action::Square, B::Square}, {Action::Triangle, B::Triangle},
            {Action::L1, B::L1}, {Action::R1, B::R1}, {Action::L3, B::L3}, {Action::R3, B::R3}, {Action::Options, B::Options},
            {Action::Share, B::Share}, {Action::Touchpad, B::Touchpad}, {Action::PS, B::PS},
            {Action::DpadUp, B::Up}, {Action::DpadDown, B::Down}, {Action::DpadLeft, B::Left}, {Action::DpadRight, B::Right}};
        for (const auto& [a, b] : buttons) if (act[size_t(a)]) st.buttons |= b;
        st.l2 = act[size_t(Action::L2)] ? 255 : 0;
        st.r2 = act[size_t(Action::R2)] ? 255 : 0;

        // Left stick from keys; a diagonal stays inside the circle.
        double lx = double(act[size_t(Action::LeftRight)]) - double(act[size_t(Action::LeftLeft)]);
        double ly = double(act[size_t(Action::LeftDown)]) - double(act[size_t(Action::LeftUp)]);
        normalise(lx, ly);
        if (act[size_t(Action::Walk)]) { lx *= settings_.walkScale; ly *= settings_.walkScale; }
        st.leftX = axis(lx); st.leftY = axis(ly);

        // Right stick: mouse speed, plus keys.
        const double dt = lastPollMs_ < 0 ? 8.0 : std::clamp(nowMs - lastPollMs_, 1.0, 50.0);
        lastPollMs_ = nowMs;
        // A stopped mouse stops the camera: two polls without a count (16 ms) end the movement at
        // once instead of letting the smoothing trail off. One empty poll is a slow hand.
        idlePolls_ = (dx_ == 0 && dy_ == 0) ? idlePolls_ + 1 : 0;
        const double k = settings_.smoothing;
        vx_ = k * vx_ + (1 - k) * (dx_ * 1000.0 / dt);
        vy_ = k * vy_ + (1 - k) * (dy_ * 1000.0 / dt * settings_.yScale * (settings_.invertY ? -1 : 1));
        dx_ = dy_ = 0;
        if (idlePolls_ >= 2) vx_ = vy_ = 0;
        double rx = 0, ry = 0;
        const double speed = std::hypot(vx_, vy_);
        if (speed > 1000.0 / dt * 0.5) {   // more than about half a count per poll: real movement
            const double m = std::pow(std::min(1.0, speed / settings_.sensitivity), settings_.curve);
            const double out = std::min(1.0, settings_.antiDeadzone + (1 - settings_.antiDeadzone) * m);
            rx = vx_ / speed * out; ry = vy_ / speed * out;
        } else { vx_ = vy_ = 0; }
        rx += double(act[size_t(Action::RightRight)]) - double(act[size_t(Action::RightLeft)]);
        ry += double(act[size_t(Action::RightDown)]) - double(act[size_t(Action::RightUp)]);
        normalise(rx, ry);
        st.rightX = axis(rx); st.rightY = axis(ry);
        return st;
    }

    // A pad and the keyboard together: buttons from either, each stick and trigger from whichever is
    // pushed further. Touch and motion stay the pad's.
    static ControllerState merge(ControllerState pad, const ControllerState& kbm) {
        if (!kbm.inputActive) return pad;
        if (!pad.inputActive) { ControllerState out = kbm; out.touches = pad.touches; return out; }
        pad.buttons |= kbm.buttons;
        pad.l2 = std::max(pad.l2, kbm.l2); pad.r2 = std::max(pad.r2, kbm.r2);
        auto further = [](int16_t& ax, int16_t& ay, int16_t bx, int16_t by) {
            if (double(bx) * bx + double(by) * by > double(ax) * ax + double(ay) * ay) { ax = bx; ay = by; }
        };
        further(pad.leftX, pad.leftY, kbm.leftX, kbm.leftY);
        further(pad.rightX, pad.rightY, kbm.rightX, kbm.rightY);
        return pad;
    }

    static uint32_t keyCode(std::string name) {
        name = lower(name);
        if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0]))) return uint32_t(std::toupper(static_cast<unsigned char>(name[0])));
        if (name.size() >= 2 && name[0] == 'f' && std::isdigit(static_cast<unsigned char>(name[1]))) {
            const int n = std::atoi(name.c_str() + 1); if (n >= 1 && n <= 24) return uint32_t(0x70 + n - 1);
        }
        if (name.size() == 4 && name.rfind("num", 0) == 0 && std::isdigit(static_cast<unsigned char>(name[3]))) return uint32_t(0x60 + (name[3] - '0'));
        static const std::map<std::string, uint32_t> named = {
            {"space", 0x20}, {"enter", 0x0D}, {"return", 0x0D}, {"esc", 0x1B}, {"escape", 0x1B}, {"tab", 0x09},
            {"backspace", 0x08}, {"capslock", 0x14}, {"lshift", 0xA0}, {"rshift", 0xA1}, {"shift", 0xA0},
            {"lctrl", 0xA2}, {"rctrl", 0xA3}, {"ctrl", 0xA2}, {"lalt", 0xA4}, {"ralt", 0xA5}, {"alt", 0xA4},
            {"up", 0x26}, {"down", 0x28}, {"left", 0x25}, {"right", 0x27}, {"insert", 0x2D}, {"delete", 0x2E},
            {"end", 0x23}, {"pageup", 0x21}, {"pagedown", 0x22}, {"grave", 0xC0}, {"minus", 0xBD}, {"equals", 0xBB},
            {"lbracket", 0xDB}, {"rbracket", 0xDD}, {"semicolon", 0xBA}, {"quote", 0xDE}, {"comma", 0xBC},
            {"period", 0xBE}, {"slash", 0xBF}, {"backslash", 0xDC},
            {"mouse1", kMouse1}, {"mouse2", kMouse2}, {"mouse3", kMouse3}, {"mouse4", kMouse4}, {"mouse5", kMouse5},
            {"wheelup", kWheelUp}, {"wheeldown", kWheelDown}};
        const auto it = named.find(name);
        return it == named.end() ? 0 : it->second;   // F12 and Home are not nameable: reserved
    }
    static Action actionCode(std::string name) {
        name = lower(name);
        static const std::map<std::string, Action> named = {
            {"cross", Action::Cross}, {"circle", Action::Circle}, {"square", Action::Square}, {"triangle", Action::Triangle},
            {"l1", Action::L1}, {"r1", Action::R1}, {"l2", Action::L2}, {"r2", Action::R2}, {"l3", Action::L3}, {"r3", Action::R3},
            {"options", Action::Options}, {"share", Action::Share}, {"create", Action::Share}, {"touchpad", Action::Touchpad},
            {"ps", Action::PS}, {"dpadup", Action::DpadUp}, {"dpaddown", Action::DpadDown}, {"dpadleft", Action::DpadLeft},
            {"dpadright", Action::DpadRight}, {"leftup", Action::LeftUp}, {"leftdown", Action::LeftDown},
            {"leftleft", Action::LeftLeft}, {"leftright", Action::LeftRight}, {"rightup", Action::RightUp},
            {"rightdown", Action::RightDown}, {"rightleft", Action::RightLeft}, {"rightright", Action::RightRight},
            {"walk", Action::Walk}};
        const auto it = named.find(name);
        return it == named.end() ? Action::None : it->second;
    }

private:
    static std::string trim(const std::string& s) {
        const auto b = s.find_first_not_of(" \t\r\n"); if (b == std::string::npos) return {};
        return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
    }
    static std::string lower(std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
    static void normalise(double& x, double& y) { const double m = std::hypot(x, y); if (m > 1) { x /= m; y /= m; } }
    static int16_t axis(double v) { return int16_t(std::lround(std::clamp(v, -1.0, 1.0) * 32767.0)); }

    Settings settings_;
    std::map<uint32_t, std::vector<Action>> keys_;
    std::array<bool, 0x120> down_{};
    int dx_ = 0, dy_ = 0, idlePolls_ = 0;
    double vx_ = 0, vy_ = 0, lastPollMs_ = -1, wheelUpUntil_ = 0, wheelDownUntil_ = 0;
};

} // namespace veyra::remoteplay

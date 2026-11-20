// SPDX-License-Identifier: GPL-3.0-or-later
// Part of PushFPS, a modified version of Veyra.
#pragma once
// Keyboard and mouse as a controller for a PS5 Remote Play stream (see KeyboardMouseMap for the
// mapping). F12 turns the capture on and off while a stream runs; Home releases it and lets the
// PushFPS panel open. While captured, the window's keyboard and mouse messages are taken before Qt
// sees them (so PushFPS shortcuts like F, V, Space and Tab do not fire), the mouse is read as raw
// movement, and the cursor is hidden and kept inside the window. Losing the focus releases it.
// Built like MoonlightInputCapture; the two never capture at the same time (one stream at a time).
#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QString>
#include <functional>
#include <windows.h>

#include "veyra/remoteplay/KeyboardMouseMap.h"

namespace veyra::ui {

class Ps5KeyboardMouse final : public QObject, public QAbstractNativeEventFilter {
public:
    explicit Ps5KeyboardMouse(QString settingsPath, QObject* parent = nullptr);
    ~Ps5KeyboardMouse() override;

    // While a PS5 stream with controller input runs: F12 works only then.
    void setActive(HWND topLevel, bool active);
    bool captured() const { return captured_; }
    bool capture(bool on);
    // The keyboard-and-mouse controller state for this poll; inputActive false when not captured.
    remoteplay::ControllerState poll();
    // Told when the capture turns on or off (for a notice).
    std::function<void(bool captured)> onCapturedChanged;

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void clipToWindow();
    void reloadIfChanged(bool force);
    static uint32_t keyCode(const MSG& msg);

    QString path_;
    FILETIME stamp_{};
    unsigned pollsSinceCheck_ = 0;
    remoteplay::KeyboardMouseMap map_;
    HWND window_ = nullptr;
    bool active_ = false, captured_ = false;
};

} // namespace veyra::ui

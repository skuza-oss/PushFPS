// SPDX-License-Identifier: GPL-3.0-or-later
// Part of PushFPS, a modified version of Veyra.
#include "veyra/ui/Ps5KeyboardMouse.h"

#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <format>
#include <windowsx.h>

#include "veyra/Log.h"

namespace veyra::ui {
namespace {
double nowMs() {
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return double(c.QuadPart) * 1000.0 / double(f.QuadPart);
}
}

Ps5KeyboardMouse::Ps5KeyboardMouse(QString settingsPath, QObject* parent) : QObject(parent), path_(std::move(settingsPath)) {
    reloadIfChanged(true);
    QCoreApplication::instance()->installNativeEventFilter(this);
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive && captured_) capture(false);
    });
}

Ps5KeyboardMouse::~Ps5KeyboardMouse() {
    capture(false);
    if (auto* app = QCoreApplication::instance()) app->removeNativeEventFilter(this);
}

void Ps5KeyboardMouse::setActive(HWND topLevel, bool active) {
    window_ = topLevel;
    if (active == active_) return;
    active_ = active;
    if (!active_) capture(false);
    else reloadIfChanged(false);
}

// The INI file: written with the defaults when missing, read again when its time stamp changes.
void Ps5KeyboardMouse::reloadIfChanged(bool force) {
    const std::wstring path = path_.toStdWString();
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        QFile out(path_);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const std::string text = remoteplay::KeyboardMouseMap::defaultIni();
            out.write(text.data(), qint64(text.size())); out.close();
            veyra::log::info("ps5-kbm", std::format("wrote default settings to {}", path_.toStdString()));
        }
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) { map_.load(remoteplay::KeyboardMouseMap::defaultIni()); return; }
        force = true;
    }
    if (!force && CompareFileTime(&info.ftLastWriteTime, &stamp_) == 0) return;
    stamp_ = info.ftLastWriteTime;
    QFile in(path_);
    if (!in.open(QIODevice::ReadOnly)) return;
    const QByteArray text = in.readAll();
    const int keys = map_.load(std::string(text.constData(), size_t(text.size())));
    const auto& s = map_.settings();
    veyra::log::info("ps5-kbm", std::format("settings loaded keys={} sensitivity={:.0f} antiDeadzone={:.2f} curve={:.2f} smoothing={:.2f}",
        keys, s.sensitivity, s.antiDeadzone, s.curve, s.smoothing));
}

bool Ps5KeyboardMouse::capture(bool on) {
    if (on == captured_) return true;
    if (on) {
        if (!active_ || !window_ || GetForegroundWindow() != window_) return false;
        RAWINPUTDEVICE device{0x01, 0x02, 0, window_};   // generic desktop / mouse
        if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
            veyra::log::warn("ps5-kbm", "RegisterRawInputDevices failed; keyboard and mouse not captured");
            return false;
        }
        reloadIfChanged(false);
        map_.releaseAll();
        captured_ = true;
        clipToWindow();
        SetCursor(nullptr);
    } else {
        captured_ = false;
        RAWINPUTDEVICE device{0x01, 0x02, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&device, 1, sizeof(device));
        ClipCursor(nullptr);
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        map_.releaseAll();   // nothing stays pressed on the console
    }
    veyra::log::info("ps5-kbm", captured_ ? "capture on" : "capture off");
    if (onCapturedChanged) onCapturedChanged(captured_);
    return true;
}

void Ps5KeyboardMouse::clipToWindow() {
    RECT client{};
    if (!window_ || !GetClientRect(window_, &client)) return;
    POINT topLeft{client.left, client.top}, bottomRight{client.right, client.bottom};
    ClientToScreen(window_, &topLeft); ClientToScreen(window_, &bottomRight);
    const RECT screen{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    ClipCursor(&screen);
}

remoteplay::ControllerState Ps5KeyboardMouse::poll() {
    if (++pollsSinceCheck_ >= 125) { pollsSinceCheck_ = 0; reloadIfChanged(false); }   // about once a second
    if (!captured_) return {};
    return map_.poll(nowMs());
}

// Windows virtual key with the left and right Shift, Ctrl and Alt told apart.
uint32_t Ps5KeyboardMouse::keyCode(const MSG& msg) {
    const uint32_t vk = uint32_t(msg.wParam);
    const bool extended = (msg.lParam >> 24) & 1;
    const UINT scan = UINT((msg.lParam >> 16) & 0xFF);
    switch (vk) {
    case VK_SHIFT: return MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? VK_RSHIFT : VK_LSHIFT;
    case VK_CONTROL: return extended ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU: return extended ? VK_RMENU : VK_LMENU;
    default: return vk;
    }
}

bool Ps5KeyboardMouse::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) {
    if (!active_ || eventType != "windows_generic_MSG") return false;
    const MSG* msg = static_cast<const MSG*>(message);
    if (!msg->hwnd || !window_ || GetAncestor(msg->hwnd, GA_ROOT) != window_) return false;
    const bool keyDown = msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN;
    const bool keyUp = msg->message == WM_KEYUP || msg->message == WM_SYSKEYUP;
    const bool repeat = keyDown && ((msg->lParam >> 30) & 1);

    if (!captured_) {
        // Not captured: only F12 is ours, to start.
        if (keyDown && !repeat && msg->wParam == VK_F12) { capture(true); if (result) *result = 0; return true; }
        return false;
    }

    auto consume = [&] { if (result) *result = 0; return true; };
    switch (msg->message) {
    case WM_INPUT: {
        alignas(8) BYTE buffer[128];
        UINT size = sizeof(buffer);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) != UINT(-1)) {
            const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer);
            if (raw->header.dwType == RIM_TYPEMOUSE && (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
                map_.mouseMove(raw->data.mouse.lLastX, raw->data.mouse.lLastY);
        }
        return false;   // DefWindowProc still cleans up the raw input buffer
    }
    case WM_SETCURSOR: SetCursor(nullptr); if (result) *result = TRUE; return true;
    case WM_SIZE: case WM_MOVE: clipToWindow(); return false;
    case WM_ACTIVATE: if (LOWORD(msg->wParam) == WA_INACTIVE) capture(false); return false;
    case WM_KILLFOCUS: capture(false); return false;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: map_.key(remoteplay::KeyboardMouseMap::kMouse1, true); return consume();
    case WM_LBUTTONUP: map_.key(remoteplay::KeyboardMouseMap::kMouse1, false); return consume();
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: map_.key(remoteplay::KeyboardMouseMap::kMouse2, true); return consume();
    case WM_RBUTTONUP: map_.key(remoteplay::KeyboardMouseMap::kMouse2, false); return consume();
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: map_.key(remoteplay::KeyboardMouseMap::kMouse3, true); return consume();
    case WM_MBUTTONUP: map_.key(remoteplay::KeyboardMouseMap::kMouse3, false); return consume();
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: case WM_XBUTTONUP: {
        const bool down = msg->message != WM_XBUTTONUP;
        map_.key(GET_XBUTTON_WPARAM(msg->wParam) == XBUTTON1 ? remoteplay::KeyboardMouseMap::kMouse4 : remoteplay::KeyboardMouseMap::kMouse5, down);
        if (result) *result = TRUE;
        return true;
    }
    case WM_MOUSEWHEEL: map_.wheel(GET_WHEEL_DELTA_WPARAM(msg->wParam) > 0 ? 1 : -1, nowMs()); return consume();
    case WM_MOUSEMOVE: case WM_MOUSEHWHEEL: return consume();
    default: break;
    }
    if (keyDown || keyUp) {
        if (msg->wParam == VK_F12) { if (keyDown && !repeat) capture(false); return consume(); }
        if (msg->wParam == VK_HOME) { if (keyDown) capture(false); return false; }   // PushFPS opens its panel
        if (msg->wParam == VK_LWIN || msg->wParam == VK_RWIN) return false;
        map_.key(keyCode(*msg), keyDown);
        return consume();
    }
    if (msg->message == WM_CHAR || msg->message == WM_SYSCHAR || msg->message == WM_DEADCHAR || msg->message == WM_SYSDEADCHAR) return consume();
    return false;
}

} // namespace veyra::ui

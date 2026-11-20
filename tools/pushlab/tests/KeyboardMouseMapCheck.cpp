#include "veyra/remoteplay/KeyboardMouseMap.h"
#include <cstdio>
using namespace veyra::remoteplay;
using B = ControllerState::Button;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main() {
    KeyboardMouseMap m;
    CHECK(m.load(KeyboardMouseMap::defaultIni()) > 30);
    double t = 0;
    auto st = m.poll(t);
    CHECK(st.inputActive && st.buttons == 0 && st.leftX == 0 && st.rightX == 0 && st.r2 == 0);
    // W + D: diagonal inside the circle
    m.key('W', true); m.key('D', true); st = m.poll(t += 8);
    CHECK(st.leftY < -20000 && st.leftX > 20000 && st.leftX < 23500);
    m.key(0xA4, true); st = m.poll(t += 8);   // LAlt = walk
    CHECK(st.leftX > 10000 && st.leftX < 12500);
    m.key(0xA4, false); m.key('W', false); m.key('D', false);
    // buttons and triggers
    m.key(KeyboardMouseMap::kMouse1, true); m.key(0x20, true); m.key(0xA0, true);
    st = m.poll(t += 8);
    CHECK(st.r2 == 255 && (st.buttons & B::Square) && (st.buttons & B::Cross));
    m.releaseAll(); st = m.poll(t += 8);
    CHECK(st.buttons == 0 && st.r2 == 0);
    // mouse: slow movement gets past the dead zone, fast saturates, stop returns to zero
    for (int i = 0; i < 10; ++i) { m.mouseMove(2, 0); st = m.poll(t += 8); }   // 250 counts/s
    std::printf("slow rightX=%d\n", st.rightX);
    CHECK(st.rightX > int(0.2 * 32767) && st.rightX < int(0.35 * 32767));
    for (int i = 0; i < 10; ++i) { m.mouseMove(40, 0); st = m.poll(t += 8); }  // 5000 counts/s
    CHECK(st.rightX == 32767);
    for (int i = 0; i < 4; ++i) st = m.poll(t += 8);
    std::printf("after stop rightX=%d\n", st.rightX);
    CHECK(st.rightX == 0);
    for (int i = 0; i < 10; ++i) { m.mouseMove(0, 10); st = m.poll(t += 8); }   // mouse back -> look down (+Y)
    CHECK(st.rightY > 0 && st.rightX == 0);
    for (int i = 0; i < 4; ++i) st = m.poll(t += 8);
    // wheel pulse
    m.wheel(1, t); st = m.poll(t += 8); CHECK(st.buttons & B::Up);
    st = m.poll(t += 100); CHECK(!(st.buttons & B::Up));
    // merge: pad stick wins when further, buttons combine
    ControllerState pad; pad.inputActive = true; pad.leftX = 30000; pad.buttons = B::Circle;
    ControllerState kb; kb.inputActive = true; kb.leftX = 10000; kb.buttons = B::Cross; kb.r2 = 255;
    auto mg = KeyboardMouseMap::merge(pad, kb);
    CHECK(mg.leftX == 30000 && (mg.buttons & B::Circle) && (mg.buttons & B::Cross) && mg.r2 == 255);
    ControllerState off; auto onlyPad = KeyboardMouseMap::merge(pad, off); CHECK(onlyPad == pad);
    // custom ini: remap and None, settings bounds
    CHECK(m.load("[Keys]\nW=Cross\nMouse1=None\n[Mouse]\nsensitivity=10\ncurve=0.7\n") == 2);
    CHECK(m.settings().sensitivity == 2500 && m.settings().curve == 0.7);
    m.key('W', true); m.key(KeyboardMouseMap::kMouse1, true); st = m.poll(t += 8);
    CHECK((st.buttons & B::Cross) && st.leftY == 0 && st.r2 == 0);
    CHECK(KeyboardMouseMap::keyCode("F12") == 0x7B);   // nameable, but the capture keeps it reserved
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails;
}

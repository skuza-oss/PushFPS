// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFlow 2's Predict through the whole engine, with a live source: the one thing no other test here
// reaches. A Frame Input channel (include/pushfps/FrameInput.h) is fed from this process and played by
// EngineController, the way a capture card's picture is, once without Predict and once with it.
//
// What it can say on the software renderer: the engine keeps playing with Predict on, real frames and
// forecasts reach the screen, nothing fails, and a real frame is shown sooner after it arrives than it is
// with in-between frames. What it cannot say: anything about a real graphics card or a real capture card.
// The source is slow on purpose (1.1 frames a second, small): the software renderer needs about a third of
// a second for one run of the network (measured 19 November 2026: at 2 frames a second no generated frame
// of either kind was ready in time), and a forecast must be ready half an interval after its frame. An
// interval may not pass one second, beyond which the engine sees a break and generates nothing.
//
// Usage: veyra_push_predict_engine_tests <output folder>   (PushFlow's runtime is found as the program finds it)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "veyra/engine/EngineController.h"
#include "veyra/pushflow/PushFlow2.h"
#include "veyra/Log.h"
#include "pushfps/FrameInput.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace veyra;
using namespace std::chrono_literals;

namespace {
constexpr uint32_t W = 256, H = 144;
constexpr int kRateNum = 11, kRateDen = 10;   // frames a second, as a fraction

// A grey gradient with a bright square that moves 4 pixels a frame.
std::vector<uint8_t> picture(unsigned index) {
    std::vector<uint8_t> p(size_t(W) * H * 4);
    const uint32_t left = 40 + (index * 4) % 120, top = 50;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const bool square = x >= left && x < left + 40 && y >= top && y < top + 40;
            const uint8_t v = square ? 235 : uint8_t(40 + (x * 60) / W + (y * 30) / H);
            uint8_t* q = p.data() + (size_t(y) * W + x) * 4;
            q[0] = v; q[1] = square ? 235 : v; q[2] = square ? 200 : v; q[3] = 255;
        }
    return p;
}

struct Played {
    bool started = false, failed = false;
    uint64_t real = 0, generated = 0, forecasts = 0, superseded = 0;
    double latencyMs = -1, realFps = 0, generatedFps = 0;
    std::string predict;
    std::wstring status;
};

Played play(bool predict, const std::filesystem::path& out, int seconds) {
    Played r;
    pushflow2::setRequested(pushflow2::Flags{}.with(pushflow2::Experiment::Predict, predict));
    const std::wstring channel = predict ? L"pushfps-predict-test-on" : L"pushfps-predict-test-off";
    pushfps::FrameInputSender sender;
    if (!sender.open(channel.c_str(), W, H, kRateNum, kRateDen, "PushFPS engine test")) { std::cout << "INFO the frame input channel could not be opened" << std::endl; return r; }
    std::atomic<bool> stop{false};
    std::thread feed([&] {
        unsigned index = 0;
        auto next = std::chrono::steady_clock::now();
        while (!stop.load()) {
            const auto p = picture(index++);
            sender.send(p.data(), W, H, W * 4, PUSHFPS_FORMAT_BGRA8);
            next += std::chrono::microseconds(1000000ll * kRateDen / kRateNum);
            std::this_thread::sleep_until(next);
        }
    });
    HWND window = CreateWindowExW(0, L"STATIC", L"PushFPS Predict engine test", WS_POPUP, 0, 0, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (window) {
        ShowWindow(window, SW_SHOWNOACTIVATE);
        engine::EngineController player; player.setVolume(0, true);
        engine::PlayerOptions options; options.nr = false; options.sr = false; options.fg = true; options.fgMultiplier = 2;
        options.settings.frameGenerationBackend = engine::FrameGenerationBackend::PushFlow;
        options.settings.vfgQuality = 0;   // the Fast tier
        player.open(window, L"frameinput:" + channel, options);
        r.started = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        // The engine's latency readout covers the last second (two frames here): one reading is kept for
        // every new real frame shown, and the middle one of those after the first few is what is compared.
        std::vector<double> latencies; uint64_t lastReal = 0;
        // Play until enough has been shown to judge, or the time is up: 34 real frames (the engine refreshes
        // what it reports of PushFlow every 32nd real frame, which is half a second for a game and half a
        // minute here), 6 generated ones and 6 latency readings. Where no frame is generated at all the
        // time runs out, which the caller reports.
        while (std::chrono::steady_clock::now() < deadline) {
            MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            const auto s = player.snapshot();
            r.failed = s.failed; r.status = s.status;
            const auto& flow = s.metrics.flow;
            r.real = flow.counters.realPresented; r.generated = flow.counters.generatedPresented;
            r.realFps = flow.realPresentFps; r.generatedFps = flow.generatedPresentFps;
            if (r.real != lastReal) { lastReal = r.real; if (flow.softwareLatencyMs && r.real > 6) latencies.push_back(*flow.softwareLatencyMs); }
            r.predict = s.pushFlowPredict; r.forecasts = s.pushFlowPredictFrames; r.superseded = s.pushFlowPredictSuperseded;
            if (s.failed) break;
            if (r.real >= 34 && r.generated >= 6 && latencies.size() >= 6) break;
            std::this_thread::sleep_for(20ms);
        }
        if (!latencies.empty()) { std::sort(latencies.begin(), latencies.end()); r.latencyMs = latencies[latencies.size() / 2]; }
        player.stop();
        const auto drained = std::chrono::steady_clock::now() + 15s;
        while (!player.idle() && std::chrono::steady_clock::now() < drained) {
            MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            std::this_thread::sleep_for(10ms);
        }
        if (!player.idle()) { std::cout << "INFO the engine did not come to rest after stop" << std::endl; r.failed = true; }
    }
    stop.store(true); feed.join(); sender.close();
    if (window) DestroyWindow(window);
    pushflow2::setRequested(pushflow2::Flags{});
    (void)out;
    return r;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { std::cout << "usage: veyra_push_predict_engine_tests <output folder>" << std::endl; return 2; }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::filesystem::path out = argv[1];
    std::filesystem::create_directories(out);
    Logger::instance().openFile((out / "engine.log").wstring());
    Logger::instance().setConsoleEnabled(false);
    int failures = 0;
    const auto check = [&](bool ok, const std::string& what) { std::cout << "PREDICT_ENGINE " << (ok ? "PASS " : "FAIL ") << what << std::endl; failures += !ok; return ok; };
    const auto narrow = [](const std::wstring& w) { std::string s; for (wchar_t c : w) s += c < 128 ? char(c) : '?'; return s; };
    const auto say = [&](const char* name, const Played& p) {
        std::cout << "PREDICT_ENGINE " << name << ": real frames shown " << p.real << ", generated frames shown " << p.generated << ", arrival to Present " << p.latencyMs << " ms, real "
                  << p.realFps << " / generated " << p.generatedFps << " a second, Predict '" << p.predict << "', forecasts made " << p.forecasts << ", not shown because the next real frame was due "
                  << p.superseded << (p.failed ? ", FAILED: " + narrow(p.status) : std::string()) << std::endl;
    };

    const Played plain = play(false, out, 75);
    say("in-between frames", plain);
    check(plain.started && !plain.failed, "the engine plays the live source with PushFlow and comes to rest");
    check(plain.real >= 34, "its real frames reach the screen (" + std::to_string(plain.real) + ")");
    check(plain.predict.empty() && plain.forecasts == 0, "Predict is not asked for and nothing of it runs ('" + plain.predict + "')");
    // On the software renderer the engine has shown no in-between frame from this source in any run so far
    // (they miss their time), so the two runs are compared only when it does; Predict is judged on its own.
    const bool generates = plain.generated >= 6;
    std::cout << "PREDICT_ENGINE INFO in-between frames reached the screen: " << (generates ? "yes" : "no") << std::endl;

    const Played ahead = play(true, out, 150);
    say("Predict", ahead);
    check(ahead.started && !ahead.failed, "with Predict on the engine plays the live source and comes to rest");
    check(ahead.real >= 34, "its real frames reach the screen (" + std::to_string(ahead.real) + ")");
    check(ahead.predict == "on", "Predict is in use ('" + ahead.predict + "')");
    check(ahead.forecasts > 0, "forecasts are made (" + std::to_string(ahead.forecasts) + ")");
    check(ahead.generated >= 6, "forecasts reach the screen (" + std::to_string(ahead.generated) + ")");
    // The real frame is not held back for them. In-between frames at 2X cannot be shown without holding
    // the real frame for at least half an interval (454 ms here); with forecasts on screen the real frame
    // must be shown within a quarter of an interval of arriving.
    const double interval = 1000.0 * kRateDen / kRateNum;
    check(ahead.latencyMs >= 0 && ahead.latencyMs < interval / 4.0, "while forecasts are shown, a real frame is on screen " + std::to_string(ahead.latencyMs) +
                                                                     " ms after it arrives (in-between frames would hold it " + std::to_string(interval / 2.0) + " ms or more)");
    if (generates && plain.latencyMs >= 0 && ahead.latencyMs >= 0)
        check(ahead.latencyMs + interval / 10.0 < plain.latencyMs, "and sooner than with in-between frames (" + std::to_string(ahead.latencyMs) + " ms against " + std::to_string(plain.latencyMs) + " ms)");
    std::cout << "PREDICT_ENGINE " << failures << " failure(s)" << std::endl;
    Logger::instance().closeFile();
    {
        // What the engine itself said about frames it dropped and pairs it refused: the last lines of each kind.
        std::ifstream log(out / "engine.log");
        std::vector<std::string> drops, holds, admits; std::string line;
        while (std::getline(log, line)) {
            if (line.find("[fg-drops]") != std::string::npos) drops.push_back(line);
            else if (line.find("[live-fg-hold]") != std::string::npos) holds.push_back(line);
            else if (line.find("admission") != std::string::npos) admits.push_back(line);
        }
        const auto tail = [](const char* name, const std::vector<std::string>& lines) {
            for (size_t i = lines.size() > 2 ? lines.size() - 2 : 0; i < lines.size(); ++i) std::cout << "PREDICT_ENGINE LOG " << name << ": " << lines[i].substr(0, 300) << std::endl;
        };
        tail("drops", drops); tail("hold", holds); tail("admission", admits);
    }
    CoUninitialize();
    return failures ? 1 : 0;
}

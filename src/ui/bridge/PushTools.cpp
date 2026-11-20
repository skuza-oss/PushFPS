// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFPS tools for finding out what went wrong and what a PC can do:
//  - the crash notice: after PushFPS stopped unexpectedly, the next start says so and offers a
//    summary to copy that holds nothing personal (engine/PushCrashNote.h);
//  - the graphics card test: PushFlowBench.exe (shipped beside PushFPS.exe) run in its own process,
//    with a result the user can copy;
//  - the performance report: what the session playing now really does (engine/PushPerfReport.h).
#include "../QmlPlayerBridge_p.h"
#include "veyra/engine/PushCrashNote.h"
#include "veyra/engine/PushPerfReport.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <format>

namespace veyra::ui {
using namespace bridge_detail;
namespace crashnote = veyra::engine::crashnote;

namespace {
// Beside the log and the crash handler's own note (lastFailurePath).
QString sessionNotePath() { return QFileInfo(lastFailurePath()).absolutePath() + QStringLiteral("/veyra-last-session.txt"); }
}

// --- crash notice ----------------------------------------------------------------
// At the very start, before anything of this run can overwrite them: the previous run's session
// note, and whether Optimise for my PC was in the middle of a test (its restore record is still
// there; the dialog clears it when the interface loads).
void QmlPlayerBridge::pushRememberPreviousRun() {
    impl_->pushOptimiseAtStart = impl_->prefs.value(QStringLiteral("pushOptimizeRestore")).toString();
    QFile note(sessionNotePath());
    if (note.open(QIODevice::ReadOnly)) {
        impl_->pushPreviousSession = QString::fromUtf8(note.read(512)).trimmed();
        note.close();
        note.remove();
    }
}

// Once a second (pushFlowBudgetWatch's tick): the note is rewritten when what is playing changes.
void QmlPlayerBridge::pushNoteSession() {
    const auto& s = impl_->snapshot;
    std::vector<crashnote::SessionPart> parts;
    if (hasSource()) {
        parts.push_back({"source", sourceKind().toStdString()});
        if (s.sourceWidth && s.sourceHeight) parts.push_back({"size", std::format("{}x{}", s.sourceWidth, s.sourceHeight)});
        std::string fg = "off";
        if (fgEnabled()) {
            const QString backend = fgBackendName();
            fg = std::format("{} {}X", backend.toStdString(), fgMultiplier());
            if (backend == QLatin1String("pushflow")) fg += vfgQuality() == 0 ? " fast" : vfgQuality() == 2 ? " quality" : " balanced";
        }
        parts.push_back({"fg", fg});
        parts.push_back({"nr", nrAnyEnabled() ? "1" : "0"});
        parts.push_back({"sr", srEnabled() ? "1" : "0"});
        parts.push_back({"hdr", s.sourceHdr ? "1" : "0"});
    }
    const QString note = QString::fromStdString(crashnote::sessionNote(parts));
    if (note == impl_->pushSessionNote) return;
    impl_->pushSessionNote = note;
    QFile file(sessionNotePath());
    if (note.isEmpty()) { file.remove(); return; }
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(note.toUtf8());
}

// markerText: veyra-last-failure.txt as the crash handler left it (read by the bridge at start).
void QmlPlayerBridge::pushLoadCrashReport(const QString& markerText) {
    impl_->pushCrashReport.clear();
    const auto marker = crashnote::parseMarker(markerText.toStdString());
    if (!marker.present || !marker.crash) return;
    crashnote::Facts facts;
    facts.version = version().toStdString();
    facts.windows = QSysInfo::kernelVersion().toStdString();
    if (const auto* adapter = engineAdapterDesc()) facts.gpu = QString::fromWCharArray(adapter->Description).toStdString();
    facts.session = impl_->pushPreviousSession.toStdString();
    const QJsonObject optimise = QJsonDocument::fromJson(impl_->pushOptimiseAtStart.toUtf8()).object();
    if (!optimise.isEmpty()) {
        const QString testing = optimise.value(QStringLiteral("testing")).toString();
        facts.optimise = testing.isEmpty() ? std::string("its first step") : testing.toStdString();
    }
    QVariantMap report;
    report.insert(QStringLiteral("present"), true);
    report.insert(QStringLiteral("module"), QString::fromStdString(marker.module));
    report.insert(QStringLiteral("dump"), QString::fromStdString(marker.dump));
    report.insert(QStringLiteral("optimising"), !optimise.isEmpty());
    report.insert(QStringLiteral("summary"), QString::fromStdString(crashnote::summary(marker, facts)));
    impl_->pushCrashReport = report;
    veyra::log::info("push-crash", std::format("previous run crashed in {} (optimiser running={}); notice queued",
        marker.module.empty() ? std::string("unknown") : marker.module, !optimise.isEmpty()));
    // After the intro has made way for the interface.
    QTimer::singleShot(3500, this, [this] {
        if (!impl_->pushCrashReport.isEmpty()) emit navigate(QStringLiteral("crash"));
    });
}

QVariantMap QmlPlayerBridge::pushCrashReport() const {
    if (!impl_->pushCrashReport.isEmpty()) return impl_->pushCrashReport;
    return QVariantMap{{QStringLiteral("present"), false}};
}
void QmlPlayerBridge::pushDismissCrashReport() { impl_->pushCrashReport.clear(); }

// --- performance report ------------------------------------------------------------
// The session playing now, as text to copy into a bug report or a hardware report: the card and its
// driver, the settings that matter, and the frames really presented a second (the game's own and the
// generated ones) with PushFPS's own software delay. Numbers and words from fixed lists only
// (engine/PushPerfReport.h): no device name, file, folder, address or account can be in it.
QString QmlPlayerBridge::pushPerformanceReport() const {
    const auto& s = impl_->snapshot;
    const auto& flow = s.metrics.flow;
    engine::perfreport::Facts f;
    f.version = version().toStdString();
    f.windows = QSysInfo::kernelVersion().toStdString();
    if (const auto* adapter = engineAdapterDesc()) {
        f.gpu = QString::fromWCharArray(adapter->Description).toStdString();
        f.gpuMemoryMb = uint64_t(adapter->DedicatedVideoMemory) >> 20;
        f.driver = engineAdapterDriverVersion().toStdString();
    } else f.softwareRenderer = true;   // no hardware adapter: Windows' software renderer
    f.displayHz = impl_->displayRefreshHz;
    f.vrrState = vrrState();
    f.playing = s.running && !s.image && hasSource();
    f.source = hasSource() ? sourceKind().toStdString() : std::string();
    f.sourceWidth = uint32_t(s.sourceWidth); f.sourceHeight = uint32_t(s.sourceHeight);
    f.outputWidth = uint32_t(std::max(0, int(s.metrics.resolution.output.width)));
    f.outputHeight = uint32_t(std::max(0, int(s.metrics.resolution.output.height)));
    f.hdr = s.sourceHdr;
    if (s.capture) { f.captureFps = s.captureFps; f.captureReceived = s.captureReceived; f.captureDropped = s.captureDropped; }
    f.contentFps = s.contentFps;
    f.fgOn = fgEnabled();
    if (f.fgOn) {
        const QString backend = fgBackendName();
        f.fgMethod = backend.toStdString();
        if (backend == QLatin1String("pushflow") || backend == QLatin1String("vfg")) f.fgTier = vfgQuality() == 0 ? "fast" : vfgQuality() == 2 ? "quality" : "balanced";
        f.multiplier = uint32_t(std::max(1, fgMultiplier()));
        f.multiplierNow = s.previewFgMultiplier;
    }
    f.adaptive = fgAdaptive(); f.pacingRepair = pacingRepair(); f.hudLock = hudLock();
    f.contentRate = contentRate();
    f.aiEnhancement = nrAnyEnabled(); f.upscaling = srEnabled();
    f.fgActive = s.fgActive; f.budgetLimited = s.fgBudgetLimited; f.fgHealth = impl_->fgIdle;
    f.ratesReady = flow.rateWindowReady;
    f.realFps = flow.realPresentFps; f.generatedFps = flow.generatedPresentFps;
    f.presentedFps = s.submissionFps; f.low1Fps = s.submissionLow1Fps;
    f.latencyMs = flow.softwareLatencyMs; f.latencyP95Ms = flow.softwareLatencyP95Ms; f.latencySamples = flow.latencySamples;
    if (gpuUtilizationKnown()) f.gpuPercent = gpuUtilization();
    f.pushFlowP50Ms = s.pushFlowP50Ms; f.pushFlowP95Ms = s.pushFlowP95Ms; f.pushFlowPrecision = s.pushFlowPrecision;
    f.pushFlowMotionWidth = s.pushFlowMotionWidth; f.pushFlowMotionHeight = s.pushFlowMotionHeight;
    f.pushFlowEncoderCache = s.pushFlowEncoderCache; f.pushFlowWholeMs = s.pushFlowWholeMs; f.pushFlowSplitMs = s.pushFlowSplitMs;
    f.pushFlowGuard = s.pushFlowGuard; f.pushFlowGuardFrames = s.pushFlowGuardFrames; f.pushFlowGuardMended = s.pushFlowGuardMended; f.pushFlowGuardHeld = s.pushFlowGuardHeld;
    f.pushFlowPredict = s.pushFlowPredict; f.pushFlowPredictFrames = s.pushFlowPredictFrames; f.pushFlowPredictSuperseded = s.pushFlowPredictSuperseded;
    f.pushFlowPredictCounted = s.pushFlowPredictCounted; f.pushFlowPredictMoving = s.pushFlowPredictMoving; f.pushFlowPredictHeld = s.pushFlowPredictHeld;
    return QString::fromStdString(engine::perfreport::json(f));
}

bool QmlPlayerBridge::pushCopyPerformanceReport() {
    const QString report = pushPerformanceReport();
    if (!pushCopyText(report)) { emit notice(tr("无法复制到剪贴板"), true); return false; }
    const bool playing = impl_->snapshot.running && !impl_->snapshot.image && hasSource();
    veyra::log::info("push-report", std::format("performance report copied playing={} bytes={}", playing, report.size()));
    emit notice(playing ? tr("性能报告已复制：可以直接粘贴到反馈或硬件报告里，里面没有任何个人信息")
                        : tr("性能报告已复制。现在没有画面在播放，所以只有显卡和设置；播放时再复制才有实际帧率"), false);
    return true;
}

// --- graphics card test ------------------------------------------------------------
// PushFlow's speed depends on the graphics card, and everything PushFPS knows about it so far comes
// from Windows' software renderer in the automated build. The test runs the same benchmark the build
// runs, on the card PushFPS renders on, at 1080p and 4K with every tier; the result has the card's
// name, its memory and timings: nothing about the user. In its own process, so a driver fault ends
// the test and not PushFPS.
bool QmlPlayerBridge::pushStartCardTest() {
    if (impl_->pushCardTest) return false;
    const auto fail = [this](const char* reason) {
        impl_->pushCardTestResult = QVariantMap{{QStringLiteral("done"), true}, {QStringLiteral("ok"), false}, {QStringLiteral("reason"), QString::fromLatin1(reason)}};
        return false;
    };
    if (impl_->snapshot.running) return fail("playing");
    const QString exe = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("PushFlowBench.exe"));
    if (!QFileInfo::exists(exe)) return fail("missing");
    const QString out = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("pushfps-card-test.json"));
    QFile::remove(out);
    QStringList args{QStringLiteral("--sizes"), QStringLiteral("1920x1080,3840x2160"), QStringLiteral("--tiers"), QStringLiteral("0,1,2"),
                     QStringLiteral("--pairs"), QStringLiteral("30"), QStringLiteral("--out"), out};
    if (const uint64_t luid = gfx::preferredAdapterLuid()) args << QStringLiteral("--adapter-luid") << QString::number(luid);
    auto* proc = new QProcess(this);
    proc->setProgram(exe);
    proc->setArguments(args);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    impl_->pushCardTest = proc;
    impl_->pushCardTestResult.clear();
    auto* timeout = new QTimer(proc);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, proc, [proc] { proc->kill(); });
    connect(proc, &QProcess::finished, this, [this, proc, out](int code, QProcess::ExitStatus status) {
        const QByteArray console = proc->readAll();
        const bool cancelled = proc->property("pushCancelled").toBool();
        impl_->pushCardTest = nullptr;
        proc->deleteLater();
        QFile file(out);
        QJsonObject r;
        if (file.open(QIODevice::ReadOnly)) r = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QFile::remove(out);
        const QJsonArray runs = r.value(QStringLiteral("runs")).toArray();
        veyra::log::info("push-card-test", std::format("exit={} status={} cancelled={} runs={} output={}", code, int(status), cancelled ? 1 : 0,
            runs.size(), console.right(300).simplified().toStdString()));
        QVariantMap result{{QStringLiteral("done"), true}};
        if (cancelled) { result.insert(QStringLiteral("ok"), false); result.insert(QStringLiteral("reason"), QStringLiteral("cancelled")); }
        else if (status != QProcess::NormalExit || r.isEmpty()) { result.insert(QStringLiteral("ok"), false); result.insert(QStringLiteral("reason"), QStringLiteral("stopped")); }
        else if (runs.isEmpty()) { result.insert(QStringLiteral("ok"), false); result.insert(QStringLiteral("reason"), QStringLiteral("no-pushflow")); }
        else {
            result.insert(QStringLiteral("ok"), true);
            result.insert(QStringLiteral("runs"), runs.toVariantList());
        }
        result.insert(QStringLiteral("adapter"), r.value(QStringLiteral("adapter")).toString());
        result.insert(QStringLiteral("vramMB"), r.value(QStringLiteral("vramMB")).toInt());
        // What to copy: the benchmark's own result with the PushFPS version and Windows build in front.
        if (!r.isEmpty()) {
            QJsonObject copy{{QStringLiteral("pushfps"), version()}, {QStringLiteral("windows"), QSysInfo::kernelVersion()}};
            for (auto it = r.begin(); it != r.end(); ++it) copy.insert(it.key(), it.value());
            result.insert(QStringLiteral("text"), QString::fromUtf8(QJsonDocument(copy).toJson(QJsonDocument::Indented)));
        }
        impl_->pushCardTestResult = result;
    });
    proc->start();
    if (!proc->waitForStarted(5000)) {
        impl_->pushCardTest = nullptr;
        proc->deleteLater();
        return fail("not-started");
    }
    timeout->start(300000);
    return true;
}

void QmlPlayerBridge::pushCancelCardTest() {
    if (!impl_->pushCardTest) return;
    impl_->pushCardTest->setProperty("pushCancelled", true);
    impl_->pushCardTest->kill();
}

QVariantMap QmlPlayerBridge::pushCardTestStatus() const {
    if (impl_->pushCardTest) return QVariantMap{{QStringLiteral("running"), true}, {QStringLiteral("done"), false}};
    QVariantMap status = impl_->pushCardTestResult;
    status.insert(QStringLiteral("running"), false);
    if (!status.contains(QStringLiteral("done"))) status.insert(QStringLiteral("done"), false);
    return status;
}

}  // namespace veyra::ui

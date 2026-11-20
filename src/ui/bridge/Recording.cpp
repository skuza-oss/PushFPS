// QML bridge: Recording responsibilities.
#include "../QmlPlayerBridge_p.h"
#include "veyra/Brand.h"
#include "veyra/engine/MicrophoneCapture.h"

namespace veyra::ui {
// --- live recording of the processed picture --------------------------------
// The engine owns the encoder and the file; this side only starts, stops and
// shows the state. The 3 second countdown is an interface animation and runs
// before the request is sent.
QString QmlPlayerBridge::recordingDirectory() const {
    const QString chosen = impl_->prefString("recordDir");
    if (!chosen.isEmpty()) return chosen;
    const QString videos = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    return QDir(videos.isEmpty() ? QDir::homePath() : videos).filePath(QString::fromLatin1(veyra::brand::kProductName));
}
// 0 = automatic from the picture size and frame rate (LiveRecorder).
int QmlPlayerBridge::recordingBitrateMbps() const { return std::clamp(impl_->prefInt("recordBitrateMbps", 0), 0, 400); }
int QmlPlayerBridge::recordingFpsLimit() const { return std::clamp(impl_->prefInt("recordFps", 0), 0, 240); }
bool QmlPlayerBridge::recordingHevc() const { return impl_->prefBool("recordHevc", false); }
bool QmlPlayerBridge::recordingAudio() const { return impl_->prefBool("recordAudio", true); }
bool QmlPlayerBridge::recording() const { return impl_->recordingState.value("active").toBool(); }
QVariantMap QmlPlayerBridge::recordingState() const { return impl_->recordingState; }

bool QmlPlayerBridge::recordingMicrophone() const { return impl_->prefBool("recordMic", false); }
QString QmlPlayerBridge::recordingMicrophoneId() const { return impl_->prefString("recordMicDevice"); }
bool QmlPlayerBridge::recordingMicrophoneTrack() const { return impl_->prefBool("recordMicTrack", false); }
QVariantList QmlPlayerBridge::microphoneDevices() const { return impl_->microphones; }
bool QmlPlayerBridge::replayEnabled() const { return impl_->prefBool("replayEnabled", false); }
int QmlPlayerBridge::replaySeconds() const { return std::clamp(impl_->prefInt("replaySeconds", 60), 15, 600); }
QVariantMap QmlPlayerBridge::replayState() const { return impl_->replayState; }

// 设置 → 录制: what a recording and the replay buffer both follow.
// PushFPS canvas (engine/PushCanvas.h) from preferences, with an output's own fit.
engine::PushCanvas QmlPlayerBridge::pushCanvas(int fit) const {
    engine::PushCanvas c;
    c.x = float(impl_->prefInt("canvasX", 0)) / 1000.0f;
    c.y = float(impl_->prefInt("canvasY", 0)) / 1000.0f;
    c.w = float(impl_->prefInt("canvasW", 1000)) / 1000.0f;
    c.h = float(impl_->prefInt("canvasH", 1000)) / 1000.0f;
    c.cropLeft = float(impl_->prefInt("canvasCropL", 0)) / 1000.0f;
    c.cropTop = float(impl_->prefInt("canvasCropT", 0)) / 1000.0f;
    c.cropRight = float(impl_->prefInt("canvasCropR", 0)) / 1000.0f;
    c.cropBottom = float(impl_->prefInt("canvasCropB", 0)) / 1000.0f;
    // canvasStretch: the layout editor stretched the game picture to its box (Ctrl resize).
    c.fit = impl_->prefBool("canvasStretch", false) ? engine::PushCanvasFit::Stretch : engine::PushCanvasFit(std::clamp(fit, 0, 2));
    c.background = uint32_t(impl_->prefInt("canvasBackground", 0)) & 0xFFFFFFu;
    // PushFPS crop and resize (View.cpp): with "also in recordings and streams" on, the outputs cut the
    // same sides as the screen. Off (the default) they keep the whole picture, as they always did.
    if (impl_->prefBool("pushCropOutputs", false)) {
        const auto crop = pushCropValues();
        if (crop[0] || crop[1] || crop[2] || crop[3]) {
            c.cropLeft = crop[0] / 1000.0f; c.cropTop = crop[1] / 1000.0f;
            c.cropRight = crop[2] / 1000.0f; c.cropBottom = crop[3] / 1000.0f;
        }
    }
    return c.sanitized();
}

engine::RecordingSettings QmlPlayerBridge::recordingOptions() const {
    engine::RecordingSettings options;
    options.directory = wideOf(recordingDirectory());
    options.bitrateMbps = uint32_t(recordingBitrateMbps());
    options.hevc = recordingHevc();
    options.fpsLimit = uint32_t(std::max(0, recordingFpsLimit()));
    options.audio = recordingAudio();
    options.microphone = recordingMicrophone();
    options.microphoneId = recordingMicrophoneId().toStdWString();
    options.microphoneTrack = recordingMicrophoneTrack();
    options.replaySeconds = uint32_t(replaySeconds());
    // PushFPS outputs and canvas: the recording's own size and the game picture's place on it.
    options.outputWidth = uint32_t(std::clamp(impl_->prefInt("recordWidth", 0), 0, 8192));
    options.outputHeight = uint32_t(std::clamp(impl_->prefInt("recordHeight", 0), 0, 8192));
    options.canvas = pushCanvas(impl_->prefInt("recordFit", 0));
    options.splitMinutes = uint32_t(std::clamp(impl_->prefInt("recordSplitMinutes", 0), 0, 240));
    if (const int kbps = impl_->prefInt("recordBitrateKbps", 0); kbps > 0) options.bitrateKbps = uint32_t(std::clamp(kbps, 300, 400000));
    // PushFPS advanced recording: only while 设置 → 录制 → 高级 is on; otherwise
    // the standard encoding (the RecordingSettings defaults).
    if (impl_->prefBool("recordAdvanced", false)) {
        options.rateControl = uint32_t(std::clamp(impl_->prefInt("recordRateControl", 1), 0, 2));
        options.quality = uint32_t(std::clamp(impl_->prefInt("recordQuality", 22), 1, 51));
        options.preset = uint32_t(std::clamp(impl_->prefInt("recordPreset", 4), 1, 7));
        options.keyframeSeconds = uint32_t(std::clamp(impl_->prefInt("recordKeyframe", 2), 1, 10));
        options.audioKbps = uint32_t(std::clamp(impl_->prefInt("recordAudioKbps", 192), 64, 320));
        options.matroska = impl_->prefInt("recordContainer", 0) == 1;
        // Encoder tuning (NVENC): two-pass and psycho-visual AQ.
        options.multipass = uint32_t(std::clamp(impl_->prefInt("recordMultipass", 0), 0, 2));
        options.psychoAq = impl_->prefBool("recordPsychoAq", false);
    }
    // Tests only: a small memory cap shows what the cap does.
    if (const int mb = qEnvironmentVariableIntValue("VEYRA_TEST_REPLAY_MEMORY_MB"); mb > 0) options.replayMemoryBytes = uint64_t(mb) << 20;
    return options;
}

// Every start and stop names its trigger, and a shortcut also the key the
// shortcut map was offered just before (field report 2026-11-08: recording
// toggled on and off around clicks and window activation, and the log could not
// tell a key from the 专业 button).
// (A template so the private Impl's key record is passed without naming it.)
template <class Key>
static void logRecordingRequest(const char* what, const QString& source, const QString& binding, const Key& k, const char* outcome) {
    std::string detail;
    if (source.startsWith(QStringLiteral("shortcut"))) {
        const qint64 age = k.atMs ? QDateTime::currentMSecsSinceEpoch() - k.atMs : -1;
        const QKeySequence pressed(QKeyCombination(Qt::KeyboardModifiers(k.modifiers & ~Qt::KeypadModifier), Qt::Key(k.key)));
        detail = std::format(" binding={}",binding.toStdString());
        if(age>=0&&age<=1000&&pressed==QKeySequence(binding,QKeySequence::PortableText)){
            detail += std::format(" key={} vk=0x{:X} scan=0x{:X} repeat={} receiver={} ageMs={}",
                                  binding.toStdString(),k.virtualKey,k.scanCode,k.autoRepeat,k.window.toStdString(),age);
        }else detail += " keyEvent=unobserved";
    }
    veyra::log::info("ui-record", std::format("{} source={}{} {}", what, source.isEmpty() ? std::string("unnamed") : source.toStdString(), detail, outcome));
}
void QmlPlayerBridge::startRecording(const QString& source) {
    const QString binding=source==QLatin1String("shortcut")?shortcuts().value("record").toString():QString();
    if (recording()) { logRecordingRequest("start", source, binding, impl_->lastShortcutKey, "refused=already-recording"); emit notice(tr("录制已在进行中"), true); return; }
    if (!hasSource()) { logRecordingRequest("start", source, binding, impl_->lastShortcutKey, "refused=no-source"); emit notice(tr("没有正在播放的画面，无法录制"), true); return; }
    logRecordingRequest("start", source, binding, impl_->lastShortcutKey, "requested");
    std::wstring error;
    const auto options = recordingOptions();
    if (!impl_->engine.requestRecording(options, error)) { emit notice(uiText(error), true); return; }
    pushRecordingNotices(options.audio, options.microphone, false);   // a card scene or a muted mixer: said now (PushStudio.cpp)
    // Shown at once; the poll fills in the file and the counters.
    impl_->recordingState["active"] = true;
    impl_->recordingState["failed"] = false;
    emit recordingChanged();
}
void QmlPlayerBridge::stopRecording(const QString& source) {
    const QString binding=source==QLatin1String("shortcut")?shortcuts().value("record").toString():QString();
    if (!recording()) { logRecordingRequest("stop", source, binding, impl_->lastShortcutKey, "refused=not-recording"); return; }
    logRecordingRequest("stop", source, binding, impl_->lastShortcutKey, "requested");
    impl_->engine.stopRecording();
    impl_->recordingState["active"] = false;
    emit recordingChanged();
}
void QmlPlayerBridge::toggleRecording(const QString& source) { if (recording()) stopRecording(source); else startRecording(source); }
// --- instant replay (#103) -----------------------------------------------------
// The buffer follows 设置 → 即时回放 and the recording settings; armed from
// startup, it takes frames only while something is presented.
void QmlPlayerBridge::syncReplay() {
    std::wstring error;
    const bool enabled = replayEnabled();
    if (!impl_->engine.setReplay(enabled, recordingOptions(), error)) emit notice(uiText(error), true);
    veyra::log::info("ui-replay", std::format("replay {} seconds={} microphone={}", enabled ? "on" : "off", replaySeconds(),
                                              recordingMicrophone() ? (recordingMicrophoneTrack() ? "track" : "mixed") : "off"));
    pollReplay();
}
void QmlPlayerBridge::saveReplay(const QString& source) {
    const QString binding = source.startsWith(QStringLiteral("shortcut")) ? shortcuts().value("replay").toString() : QString();
    if (!replayEnabled()) {
        logRecordingRequest("replay-save", source, binding, impl_->lastShortcutKey, "refused=off");
        emit notice(tr("即时回放未开启：设置 → 播放 → 即时回放"), true);
        return;
    }
    std::wstring error;
    if (!impl_->engine.saveReplay(wideOf(recordingDirectory()), error)) {
        logRecordingRequest("replay-save", source, binding, impl_->lastShortcutKey, "refused");
        emit notice(uiText(error), true);
        return;
    }
    logRecordingRequest("replay-save", source, binding, impl_->lastShortcutKey, "requested");
    pollReplay();
}
// Whole seconds and 10 MB steps: the state changes about once a second, not
// at the poll rate.
void QmlPlayerBridge::pollReplay() {
    const auto live = impl_->engine.replaySnapshot();
    QVariantMap state;
    state["active"] = live.active;
    state["seconds"] = int(std::floor(live.bufferedSeconds));
    state["megabytes"] = int(live.bufferedBytes / (uint64_t(10) << 20) * 10);
    state["memoryLimited"] = live.memoryLimited;
    state["saving"] = live.saving;
    state["failed"] = live.failed;
    state["error"] = live.error.empty() ? QString() : uiText(live.error);
    state["path"] = QString::fromStdWString(live.path);
    state["encoder"] = QString::fromStdWString(live.encoder);
    state["audio"] = live.audio;
    state["microphone"] = live.microphone;
    state["microphoneError"] = live.microphoneError.empty() ? QString() : uiText(live.microphoneError);
    state["saved"] = QVariant::fromValue(qulonglong(live.saved));
    // How much memory the buffer takes when the chosen length is full (10 MB steps, 0 = unknown): from
    // what the buffer holds now once it has a few seconds, before that from the recording settings and
    // the picture being played. A saved clip takes about the same on disk.
    {
        const double wanted = double(std::max(1, replaySeconds()));
        const double capMb = double(engine::replayMemoryCapBytes()) / 1048576.0;
        double fullMb = 0;
        if (live.active && live.bufferedSeconds >= 3.0) fullMb = double(live.bufferedBytes) / 1048576.0 / live.bufferedSeconds * wanted;
        else {
            const auto& snap = impl_->snapshot;
            const auto out = snap.metrics.resolution.output;
            double videoKbps = double(impl_->prefInt("recordBitrateKbps", 0));
            if (videoKbps <= 0 && recordingBitrateMbps() > 0) videoKbps = recordingBitrateMbps() * 1000.0;
            if (videoKbps <= 0 && snap.running && out.width > 0 && out.height > 0) {
                const int limit = recordingFpsLimit();
                const double shown = snap.submissionFps.value_or(snap.fps);
                const uint32_t fps = uint32_t(std::clamp(limit > 0 ? std::min<double>(limit, shown > 1 ? shown : limit) : shown, 1.0, 480.0));
                videoKbps = engine::recordingAutoBitrateMbps(out.width, out.height, fps, recordingHevc()) * 1000.0;
            }
            const double audioKbps = recordingAudio() ? (impl_->prefBool("recordAdvanced", false) ? double(std::clamp(impl_->prefInt("recordAudioKbps", 192), 64, 320)) : 192.0) : 0.0;
            if (videoKbps > 0) fullMb = (videoKbps + audioKbps) * wanted / 8.0 / 1000.0;
        }
        fullMb = std::min(fullMb, capMb);
        state["fullMegabytes"] = fullMb > 0 ? std::max(10, int(std::lround(fullMb / 10.0)) * 10) : 0;
        state["capMegabytes"] = int(std::lround(capMb / 10.0)) * 10;
    }
    if (state == impl_->replayState) return;
    const auto previous = impl_->replayState;
    impl_->replayState = state;
    emit replayChanged();
    if (live.saved != impl_->replaySaved) {
        impl_->replaySaved = live.saved;
        emit notice(tr("即时回放已保存：%1").arg(state.value("path").toString()), false);
    }
    const QString error = state.value("error").toString();
    if (live.failed && !error.isEmpty() && error != previous.value("error").toString()) emit notice(error, true);
}
void QmlPlayerBridge::refreshMicrophones() {
    QVariantList list;
    for (const auto& device : engine::microphoneDevices())
        list.push_back(QVariantMap{{"id", QString::fromStdWString(device.id)}, {"label", QString::fromStdWString(device.name)}});
    impl_->microphones = list;
    veyra::log::info("ui-record", std::format("microphones listed={}", list.size()));
    emit microphoneDevicesChanged();
}

void QmlPlayerBridge::chooseRecordingDirectory() {
    const QString dir = QFileDialog::getExistingDirectory(nullptr, tr("选择录制保存位置"), recordingDirectory());
    if (!dir.isEmpty()) setPreference(QStringLiteral("recordDir"), dir);
}
void QmlPlayerBridge::openRecordingDirectory() {
    const QString dir = recordingDirectory();
    QDir().mkpath(dir);
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) emit notice(tr("无法打开目录：%1").arg(dir), true);
}

void QmlPlayerBridge::chooseExportPath() {
    if (impl_->chain.mode == engine::ChainMode::Node) {
        emit notice(tr("节点链尚未接入导出执行器，请切换列表模式后导出"), true);
        return;
    }
    const QString path = QFileDialog::getExistingDirectory(nullptr,tr("选择导出目录"),exportTarget());
    setExportPath(path);
}

void QmlPlayerBridge::setExportPath(const QString& path) {
    if (path.isEmpty()) return;
    impl_->exportOutput = wideOf(path);
    impl_->exportStatus = QFileInfo(path).fileName();
    emit exportChanged();
}

void QmlPlayerBridge::startExport() {
    QString error;auto settings=impl_->exportSettings(error);
    if(!settings){emit notice(error,true);return;}
    if(impl_->exportQueue.count()==0){
        if(!QFileInfo(utf8Of(impl_->sourceLabel)).isFile()){emit notice(tr("请先添加要导出的文件"),true);return;}
        const QString target=exportTarget();
        const bool fileTarget=!target.isEmpty()&&!QFileInfo(target).isDir();
        const auto id=impl_->exportQueue.addFile(utf8Of(impl_->sourceLabel),fileTarget?target:QString());
        impl_->exportQueue.queue().setTrim(id,impl_->exportTrimStartSeconds,impl_->exportTrimEndSeconds);
        impl_->exportQueue.setSelectedId(id);
    }
    if(impl_->exportOutput.empty())chooseExportPath();
    if(impl_->exportOutput.empty())return;
    const QFileInfo target(exportTarget());
    const QString folder=target.isDir()?target.absoluteFilePath():target.absolutePath();
    std::wstring reason;
    if(!impl_->exportQueue.queue().start(wideOf(folder),*settings,impl_->exportHevc,impl_->exportRateControl,reason)){
        emit notice(uiText(reason),true);emit exportChanged();return;
    }
    veyra::log::info("qml-export",std::format("export frozen bitrateMbps={} rateControl={} hevc={} srTarget={} srPrescale={} srOutput={}",
        settings->exportBitrateMbps,int(impl_->exportRateControl),impl_->exportHevc,int(settings->srTarget),uint32_t(settings->srPrescale),uint32_t(settings->srOutput)));
    impl_->exportQueue.refresh();pollExport();emit navigate(QStringLiteral("exp"));
    // Leave the GPU to the export (field request 2026-10-02): whatever plays is closed,
    // unless 导出页 → 导出时关闭正在播放的内容 is off.
    if(impl_->prefBool("exportStopsPlayback",true)&&(hasSource()||impl_->openingSource)){
        veyra::log::info("qml-export","export started: closing the playing source");
        stopPlayback();emit notice(tr("已关闭正在播放的内容，显卡全部留给导出"),false);
    }
}
void QmlPlayerBridge::cancelExport() {
    impl_->exportQueue.queue().cancel();impl_->exportQueue.refresh();pollExport();emit exportChanged();
}

void QmlPlayerBridge::restartApplication() {
    requestApplicationRestart(false);
}
void QmlPlayerBridge::restartForOverlayCompatibility() {
    requestApplicationRestart(true);
}
void QmlPlayerBridge::requestApplicationRestart(bool overlayCompatibility) {
    if (exportRunning()) {
        emit notice(tr("设置已保存，请等待导出结束后重启软件"), true);
        return;
    }
    rememberPosition(true);
    qApp->setProperty("veyraOverlayCompatRestart", overlayCompatibility);
    // main launches the replacement only after UI/engine owners unwind.
    QCoreApplication::exit(42);
}

void QmlPlayerBridge::quit() {
    rememberPosition(true);
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) app->quit();
}

void QmlPlayerBridge::openCaptureDialog() { emit navigate(QStringLiteral("capture")); }
void QmlPlayerBridge::openPs5Dialog() { emit navigate(QStringLiteral("ps5")); }
void QmlPlayerBridge::openScreenCaptureDialog() { emit navigate(QStringLiteral("screen")); }
void QmlPlayerBridge::openAudioDialog() { emit navigate(QStringLiteral("audio")); }
void QmlPlayerBridge::openSubtitleDialog() { emit navigate(QStringLiteral("subtitle")); }


} // namespace veyra::ui

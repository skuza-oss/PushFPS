// SPDX-License-Identifier: GPL-3.0-or-later
// PushFPS lab: unattended measurement runs for tuning latency and picture quality.
//
// Inactive unless a lab file exists next to PushFPS.exe (pushlab.json) and the process was started with
// QML_XHR_ALLOW_FILE_READ=1 (that is what lets QML read the file). A normal launch never has either, so
// this component does nothing in normal use.
//
// pushlab.json (every field optional):
//   { "run": "id", "connectDelayMs": 1500, "warmupMs": 10000, "statsMs": 15000,
//     "latencyTries": 12, "qualitySeconds": 8, "disconnect": true, "quit": true }
//
// Everything is written to the log as "[ui] push-lab ..." lines (veyra.logUi), so a script can cut the
// engine's own per-second statistics between the phase markers:
//   push-lab begin {...}  /  phase=<name>  /  latency {...}  /  quality {...}  /  done {...}
import QtQuick

Item {
    id: lab
    // Main.qml passes a function that shows the playback page (what the PS5 dialog's Connect does).
    property var showPlayback: null

    property var config: null
    property string phase: "idle"
    property double phaseAt: 0
    property var report: ({})
    readonly property bool active: config !== null

    function log(text) { veyra.logUi("push-lab", text) }
    function enter(p) { phase = p; phaseAt = Date.now(); log("phase=" + p) }
    function elapsed() { return Date.now() - phaseAt }
    function num(key, fallback) { const v = config ? Number(config[key]) : NaN; return isFinite(v) ? v : fallback }
    function finish(ok, why) {
        report.ok = ok; report.reason = why || ""
        log("done " + JSON.stringify(report))
        enter("finished")
        if (config.disconnect !== false && veyra.hasSource) veyra.ps5Cancel()
        if (config.quit !== false) quitTimer.start()
    }

    Component.onCompleted: {
        // Only lab runs pass --pushlab; without it no file request is made (Qt logs local reads as errors).
        if (Qt.application.arguments.indexOf("--pushlab") < 0) return
        const xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            const text = String(xhr.responseText || "")
            if (text.length === 0) return            // no lab file (or file reads not allowed): normal launch
            try { lab.config = JSON.parse(text) } catch (e) { veyra.logUi("push-lab", "bad pushlab.json: " + e); return }
            lab.report = { run: String(lab.config.run || "") }
            lab.log("begin " + JSON.stringify(lab.config))
            lab.enter("load")
            veyra.ps5Load()
            tick.start()
        }
        try {
            xhr.open("GET", Qt.resolvedUrl("../../pushlab.json"))
            xhr.send()
        } catch (e) { }
    }

    Timer { id: quitTimer; interval: 3000; onTriggered: Qt.quit() }

    Timer {
        id: tick
        interval: 100
        repeat: true
        onTriggered: {
            if (!lab.active) { stop(); return }
            switch (lab.phase) {
            case "load":
                if (lab.elapsed() < lab.num("connectDelayMs", 1500)) return
                if (!veyra.ps5Connect()) { lab.finish(false, "connect refused: " + String(veyra.ps5.status || "")); stop(); return }
                if (lab.showPlayback) lab.showPlayback()
                lab.enter("connecting")
                return
            case "connecting":
                if (veyra.hasSource && veyra.ps5.active === true && veyra.openingSource !== true) { lab.enter("warmup"); return }
                if (lab.elapsed() > lab.num("connectTimeoutMs", 60000)) { lab.finish(false, "no picture after connecting"); stop() }
                return
            case "warmup":
                if (!veyra.hasSource) { lab.finish(false, "stream ended during warmup"); stop(); return }
                if (lab.elapsed() >= lab.num("warmupMs", 10000)) lab.enter("stats")
                return
            case "stats":
                if (!veyra.hasSource) { lab.finish(false, "stream ended during stats"); stop(); return }
                if (lab.elapsed() < lab.num("statsMs", 15000)) return
                if (lab.num("latencyTries", 12) > 0) {
                    if (!veyra.pushLatencyTestBegin(lab.num("latencyTries", 12))) { lab.report.latency = { error: "not started" }; lab.enter("quality-start") }
                    else lab.enter("latency")
                } else lab.enter("quality-start")
                return
            case "latency": {
                const st = veyra.pushLatencyTestStatus()
                if (st.state === "running") return
                lab.report.latency = st
                lab.log("latency " + JSON.stringify(st))
                lab.enter("quality-start")
                return
            }
            case "quality-start":
                if (lab.elapsed() < 1500) return          // let the control centre close and the picture settle
                if (lab.num("qualitySeconds", 8) > 0 && veyra.pushQualityBegin(lab.num("qualitySeconds", 8))) lab.enter("quality")
                else { lab.finish(true, "no quality test"); stop() }
                return
            case "quality": {
                const st = veyra.pushQualityStatus()
                if (st.running && lab.elapsed() < 120000) return
                lab.report.quality = st
                lab.log("quality " + JSON.stringify(st))
                lab.finish(true, "")
                stop()
                return
            }
            }
        }
    }
}

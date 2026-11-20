// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFlow 2, the parts a user sees: the frame quality test says its worst frame and its worst place
// (PushQualityTestDialog), PushFlow's motion size can be chosen apart from its tier (PushFlowMotionRow,
// preference pushFlowMotion), and each experiment has a switch that starts off and says whether it
// really runs (PushFlow2Row, preference pushFlow2), against the fixture bridge.
import QtQuick
import QtTest
import Veyra
Item {
    id: root
    width: 900; height: 900
    Item { id: host; anchors.fill: parent; property string dialog: ""; function close() { dialog = "" } }
    PushQualityTestDialog { id: quality; dialogHost: host }
    PushFlowMotionRow { id: motion; width: 620; y: 20 }
    PushFlow2Row {
        id: fusion; width: 620; y: 140; part: "fusion"; title: "Encode each real frame once"; explanation: "What it does"
        stateKey: "pushFlowEncoderCache"
        describe: function(st) { return "in use " + Number(st.pushFlowWholeMs || 0).toFixed(1) + " " + Number(st.pushFlowSplitMs || 0).toFixed(1) + " " + Math.round(Number(st.pushFlowFeaturesKept || 0)) }
    }
    PushFlow2Row {
        id: guard; width: 620; y: 260; part: "guard"; title: "Guard"; explanation: "What it does"
        stateKey: "pushFlowGuard"
        describe: function(st) { return "judged " + Math.round(Number(st.pushFlowGuardFrames || 0)) + " mended " + Math.round(Number(st.pushFlowGuardMended || 0)) + " held " + Math.round(Number(st.pushFlowGuardHeld || 0)) }
    }
    PushFlow2Row {
        id: predict; width: 620; y: 380; part: "predict"; title: "Predict"; explanation: "What it does"
        stateKey: "pushFlowPredict"
        describe: function(st) { return st.pushFlowPredictPaused === true ? "stepped aside" : "forecasts " + Math.round(Number(st.pushFlowPredictFrames || 0)) + " held " + Number(st.pushFlowPredictHeldPercent || 0).toFixed(0) }
    }

    TestCase {
        name: "PushFlow2"
        when: windowShown
        function init() {
            host.dialog = ""
            veyra.preferences = ({})
            veyra.hasSource = false
            veyra.fgEnabled = true
            veyra.fgBackendName = "pushflow"
            veyra.liveStats = ({ running: false })
            veyra.qualityModel = null
            veyra.qualityState = ({ running: false, done: false, failed: false })
            quality.st = ({})
            motion.readLive()
            fusion.readLive()
            guard.readLive()
            predict.readLive()
        }
        function cleanup() { veyra.qualityModel = null; veyra.fgBackendName = "dlss"; veyra.hasSource = true }
        function control(item, name) { const c = findChild(item, name); verify(c !== null, name); return c }

        // --- the frame quality test's worst frame and worst place -------------------------------
        function test_quality_test_says_its_worst_frame_and_place() {
            veyra.qualityModel = function(backend) {
                return { score: 82, psnr: 31.4, realPsnr: 48.0, blendPsnr: 27.0, gainOverBlend: 4.4, worstFramePsnr: 24.6, worstPlacePsnr: 11.3 }
            }
            host.dialog = "qualitytest"
            verify(quality.shown)
            quality.begin()
            tryCompare(quality, "running", false)
            const frame = control(quality, "quality-test-worst-frame"), place = control(quality, "quality-test-worst-place")
            tryCompare(frame, "visible", true, 3000)
            compare(frame.value, "24.6 dB")
            verify(place.visible)
            compare(place.value, "11.3 dB")
            // The score row is the one it always was: the two new rows add to it and change nothing in it.
            compare(control(quality, "quality-test-score").value, "82 / 100")
        }
        function test_a_result_without_them_shows_no_empty_rows() {
            // What an optimiser record or an older bridge gives: no worst frame, no worst place.
            veyra.qualityModel = function(backend) { return { score: 70, psnr: 30, realPsnr: 47, blendPsnr: 26, gainOverBlend: 4 } }
            host.dialog = "qualitytest"
            quality.begin()
            tryCompare(control(quality, "quality-test-score"), "visible", true, 3000)
            verify(!control(quality, "quality-test-worst-frame").visible)
            verify(!control(quality, "quality-test-worst-place").visible)
        }

        // --- PushFlow 2's experiment switches ----------------------------------------------------------
        function flags() { return JSON.parse(veyra.preferences.pushFlow2 || "{}") }
        function test_an_experiment_is_off_until_switched_on() {
            const sw = control(fusion, "list-pushflow2-fusion")
            verify(!fusion.asked && !sw.checked)
            verify(veyra.preferences.pushFlow2 === undefined, "showing the row writes nothing")
            const offHint = fusion.hint
            verify(offHint.length > 0)
            sw.toggled(true)
            compare(flags().fusion, true)
            compare(flags().v, 1)
            verify(fusion.asked && sw.checked)
            verify(fusion.hint !== offHint, "the hint says it is on")
            sw.toggled(false)
            compare(flags().fusion, false)
            verify(!fusion.asked)
            compare(fusion.hint, offHint)
        }
        function test_switching_one_experiment_keeps_what_else_is_stored() {
            // Another part's switch, and a name a newer version wrote, stay as they are.
            veyra.setPreference("pushFlow2", JSON.stringify({ v: 1, guard: true, somethingNew: true }))
            control(fusion, "list-pushflow2-fusion").toggled(true)
            const f = flags()
            verify(f.fusion === true && f.guard === true && f.somethingNew === true)
        }
        function test_a_damaged_preference_reads_as_off() {
            for (const text of ["", "not json", "[true]", "{\"fusion\":", "null", "7"]) {
                veyra.setPreference("pushFlow2", text)
                verify(!fusion.asked, "off for: " + text)
            }
            veyra.setPreference("pushFlow2", "{\"fusion\":\"yes\"}")
            verify(!fusion.asked, "only true turns it on")
            // Switching it on from a damaged preference writes a clean one.
            veyra.setPreference("pushFlow2", "not json")
            control(fusion, "list-pushflow2-fusion").toggled(true)
            compare(flags().fusion, true)
        }
        function test_the_row_says_whether_the_experiment_really_runs() {
            control(fusion, "list-pushflow2-fusion").toggled(true)
            const waiting = fusion.hint
            // PushFlow plays with the cache in use.
            veyra.liveStats = ({ running: true, pushFlowMotionWidth: 960, pushFlowMotionHeight: 540, pushFlowEncoderCache: "on",
                                 pushFlowWholeMs: 9.5, pushFlowSplitMs: 8.2, pushFlowFeaturesKept: 41, pushFlowFeaturesMade: 1 })
            veyra.hasSource = true
            tryCompare(fusion, "cacheState", "on", 3000)
            verify(fusion.hint.indexOf("9.5") >= 0 && fusion.hint.indexOf("8.2") >= 0 && fusion.hint.indexOf("41") >= 0)
            // Asked for and not used: the reason is said, in words.
            veyra.liveStats = ({ running: true, pushFlowMotionWidth: 960, pushFlowMotionHeight: 540, pushFlowEncoderCache: "mismatch" })
            tryCompare(fusion, "cacheState", "mismatch", 3000)
            verify(fusion.hint.indexOf(fusion.why("mismatch")) >= 0 && fusion.why("mismatch") !== "mismatch")
            for (const word of ["no-files", "half-precision", "dynamic-shapes", "session", "failed"]) verify(fusion.why(word) !== word, word)
            // Started in safe mode: on, and said not to run.
            veyra.liveStats = ({ running: true, pushFlowMotionWidth: 960, pushFlowMotionHeight: 540, pushFlowEncoderCache: "", pushFlow2Stopped: true })
            tryCompare(fusion, "stopped", true, 3000)
            verify(fusion.hint !== waiting)
            // Nothing playing: back to what is known without the engine.
            veyra.hasSource = false
            tryCompare(fusion, "live", false, 3000)
            compare(fusion.cacheState, "")
        }

        function test_each_experiment_has_its_own_switch_and_its_own_state() {
            control(guard, "list-pushflow2-guard").toggled(true)
            verify(guard.asked && !fusion.asked, "one switch does not turn the other on")
            compare(flags().guard, true)
            control(fusion, "list-pushflow2-fusion").toggled(true)
            verify(flags().guard === true && flags().fusion === true)
            veyra.liveStats = ({ running: true, pushFlowMotionWidth: 960, pushFlowMotionHeight: 540, pushFlowEncoderCache: "mismatch",
                                 pushFlowGuard: "on", pushFlowGuardFrames: 600, pushFlowGuardMended: 12, pushFlowGuardHeld: 3 })
            veyra.hasSource = true
            tryCompare(guard, "cacheState", "on", 3000)
            compare(guard.hint, "judged 600 mended 12 held 3")
            tryCompare(fusion, "cacheState", "mismatch", 3000)
            for (const word of ["no-grids", "too-small", "shader"]) verify(guard.why(word) !== word, word)
            control(guard, "list-pushflow2-guard").toggled(false)
            verify(!guard.asked && fusion.asked)
            veyra.hasSource = false
        }

        function test_predict_says_what_it_does_and_when_it_steps_aside() {
            control(predict, "list-pushflow2-predict").toggled(true)
            verify(predict.asked && flags().predict === true)
            veyra.liveStats = ({ running: true, pushFlowMotionWidth: 960, pushFlowMotionHeight: 540, pushFlowPredict: "on", pushFlowPredictFrames: 1200,
                                 pushFlowPredictHeldPercent: 7.4, pushFlowPredictSuperseded: 2, pushFlowPredictPaused: false })
            veyra.hasSource = true
            tryCompare(predict, "cacheState", "on", 3000)
            compare(predict.hint, "forecasts 1200 held 7")
            veyra.liveStats = ({ running: true, pushFlowMotionWidth: 960, pushFlowMotionHeight: 540, pushFlowPredict: "on", pushFlowPredictPaused: true })
            predict.readLive()
            compare(predict.hint, "stepped aside")
            // A file: asked for and not used, with the reason in words.
            veyra.liveStats = ({ running: true, pushFlowMotionWidth: 960, pushFlowMotionHeight: 540, pushFlowPredict: "not-live" })
            predict.readLive()
            compare(predict.cacheState, "not-live")
            verify(predict.why("not-live") !== "not-live" && predict.hint.indexOf(predict.why("not-live")) >= 0)
            control(predict, "list-pushflow2-predict").toggled(false)
            verify(!predict.asked)
            veyra.hasSource = false
        }

        // --- PushFlow's motion size ----------------------------------------------------------------
        function test_motion_size_is_automatic_until_chosen() {
            const seg = control(motion, "list-pushflow-motion")
            compare(motion.chosen, 0)
            compare(seg.current, "0")
            compare(seg.options.length, 4)
            verify(veyra.preferences.pushFlowMotion === undefined, "showing the row writes nothing")
            verify(motion.hint.length > 0 && motion.help.length > 0)
        }
        function test_choosing_a_motion_size_writes_the_preference() {
            const seg = control(motion, "list-pushflow-motion")
            seg.picked("720")
            compare(veyra.preferences.pushFlowMotion, 720)
            compare(motion.chosen, 720)
            compare(seg.current, "720")
            seg.picked("1080")
            compare(veyra.preferences.pushFlowMotion, 1080)
            seg.picked("0")
            compare(veyra.preferences.pushFlowMotion, 0)
            compare(motion.chosen, 0)
        }
        function test_a_size_that_does_not_exist_reads_as_automatic() {
            veyra.setPreference("pushFlowMotion", 900)
            compare(motion.chosen, 0)
            compare(control(motion, "list-pushflow-motion").current, "0")
            veyra.setPreference("pushFlowMotion", "abc")
            compare(motion.chosen, 0)
        }
        function test_the_row_says_the_size_really_in_use() {
            veyra.setPreference("pushFlowMotion", 720)
            const chosenHint = motion.hint
            verify(chosenHint.indexOf("720") >= 0)
            // PushFlow runs: the hint is what the network works at, here a 3440 x 1440 picture at 720 lines.
            veyra.liveStats = ({ running: true, pushFlowMotionWidth: 1488, pushFlowMotionHeight: 624 })
            veyra.hasSource = true
            tryCompare(motion, "liveWidth", 1488, 3000)
            compare(motion.liveHeight, 624)
            verify(motion.hint.indexOf("1488") >= 0 && motion.hint.indexOf("624") >= 0)
            // Another method, or nothing playing: the size is gone and the hint is the choice again.
            veyra.liveStats = ({ running: true, pushFlowMotionWidth: 0, pushFlowMotionHeight: 0 })
            tryCompare(motion, "liveWidth", 0, 3000)
            compare(motion.hint, chosenHint)
            veyra.liveStats = ({ running: true, pushFlowMotionWidth: 960, pushFlowMotionHeight: 540 })
            tryCompare(motion, "liveWidth", 960, 3000)
            veyra.hasSource = false
            tryCompare(motion, "liveWidth", 0, 3000)
        }
    }
}

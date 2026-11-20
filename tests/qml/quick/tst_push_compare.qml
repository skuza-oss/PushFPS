// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Comparison clip dialog (PushCompareDialog) against the fixture bridge: the layout and the
// length are kept in preferences, the start button records with them, the preview puts the
// FPS labels where the clip does, and the quick setup tile shows the choice. The Reveal
// layout's preview follows the clip's own timing, and the layouts that fill the frame
// (split, reveal) have a recording quality.
import QtQuick
import QtTest
import Veyra
import "../qml/Veyra/PushCompareClip.js" as Clip
Item {
    id: root
    width: 900; height: 1400
    Item { id: host; anchors.fill: parent; property string dialog: ""; property real motionScale: 1; function close() { dialog = "" } }
    PushCompareDialog { id: cmp; dialogHost: host }
    PushQuickSetup { id: quick; width: 560; y: 900 }

    TestCase {
        name: "PushCompare"
        when: windowShown
        function init() {
            host.dialog = ""
            veyra.hasSource = true
            veyra.recording = false
            veyra.recordingState = ({ saved: 0, active: false, path: "" })
            veyra.compareStarts = 0; veyra.compareSeconds = 0; veyra.compareLayout = -1
            veyra.setPreference("pushCompareLayout", 0)
            veyra.setPreference("pushCompareSeconds", 15)
            veyra.setPreference("pushCompareUntilStop", false)
            veyra.setPreference("pushCompareQuality", 0)
            veyra.setPreference("pushCompareRevealSeconds", 3)
            veyra.setPreference("pushGameFps", 30)
            host.dialog = "compare"
            verify(cmp.shown)
        }
        function action(label) { cmp.actionTriggered(label) }

        function test_defaults_split_and_15_seconds() {
            compare(cmp.layoutId, 0)
            compare(cmp.seconds, 15)
            action(qsTr("开始录制"))
            compare(veyra.compareStarts, 1)
            compare(veyra.compareSeconds, 15)
            compare(veyra.compareLayout, 0)
            compare(host.dialog, "", "the dialog closes once recording starts")
        }
        function test_side_by_side_and_length_are_kept_and_used() {
            findChild(cmp, "pushfps-compare-layout").picked("1")
            findChild(cmp, "pushfps-compare-length").picked("30")
            compare(veyra.preferences.pushCompareLayout, 1)
            compare(veyra.preferences.pushCompareSeconds, 30)
            action(qsTr("开始录制"))
            compare(veyra.compareSeconds, 30)
            compare(veyra.compareLayout, 1)
        }
        function test_vertical_for_short_video_apps() {
            findChild(cmp, "pushfps-compare-layout").picked("2")
            compare(veyra.preferences.pushCompareLayout, 2)
            compare(cmp.layoutId, 2)
            verify(cmp.verticalPreview, "portrait preview shown")
            verify(findChild(cmp, "pushfps-compare-preview-vertical") !== null)
            // Labels next to the divider: the console's just above, PushFPS's just below (192-high preview).
            const top = cmp.verticalTagY(0, 12, 192), bottom = cmp.verticalTagY(1, 12, 192)
            verify(top + 12 <= 96 && top > 60, "console label just above the middle, y=" + top)
            verify(bottom >= 96 && bottom < 120, "PushFPS label just below the middle, y=" + bottom)
            verify(cmp.layoutHint(2).indexOf("TikTok") >= 0)
            action(qsTr("开始录制"))
            compare(veyra.compareLayout, 2)
            const tile = findChild(quick, "pushfps-compare-clip")
            compare(tile.sub, qsTr("%1 秒").arg(15) + " · " + qsTr("竖屏"))
        }
        function test_lengths_up_to_two_minutes() {
            const length = findChild(cmp, "pushfps-compare-length")
            compare(length.options.map(o => o.id).join(","), "10,15,30,60,90,120", "the lengths there were, then 90 and 120 seconds")
            for (const seconds of [90, 120, 10]) {
                length.picked(String(seconds))
                compare(veyra.preferences.pushCompareSeconds, seconds)
                compare(cmp.seconds, seconds)
                action(qsTr("开始录制"))
                compare(veyra.compareSeconds, seconds)
                host.dialog = "compare"
            }
            compare(veyra.compareStarts, 3)
            veyra.setPreference("pushCompareSeconds", 120)
            compare(findChild(quick, "pushfps-compare-clip").sub, qsTr("%1 秒").arg(120) + " · " + qsTr("分屏"))
        }
        function test_until_i_stop_in_every_layout() {
            const toggle = findChild(cmp, "pushfps-compare-until-stop")
            verify(toggle !== null, "the dialog has an until-I-stop switch")
            const length = findChild(cmp, "pushfps-compare-length")
            findChild(cmp, "pushfps-compare-length").picked("30")
            toggle.toggled(true)
            compare(veyra.preferences.pushCompareUntilStop, true)
            verify(cmp.untilStop)
            verify(!length.enabled, "the length waits while there is none")
            compare(veyra.preferences.pushCompareSeconds, 30, "and is kept for later")
            // Split, side by side, vertical and reveal all start without a length.
            for (const layout of [0, 1, 2, 3]) {
                findChild(cmp, "pushfps-compare-layout").picked(String(layout))
                action(qsTr("开始录制"))
                compare(veyra.compareLayout, layout)
                compare(veyra.compareSeconds, 0, "0 seconds asks for a clip that runs until it is stopped (layout " + layout + ")")
                host.dialog = "compare"
            }
            compare(veyra.compareStarts, 4)
            const tile = findChild(quick, "pushfps-compare-clip")
            compare(tile.sub, qsTr("手动停止") + " · " + qsTr("滑动揭示"))
            // Switched off again: the length from before.
            toggle.toggled(false)
            verify(length.enabled)
            action(qsTr("开始录制"))
            compare(veyra.compareSeconds, 30)
            compare(tile.sub, qsTr("%1 秒").arg(30) + " · " + qsTr("滑动揭示"))
        }
        function test_reveal_layout_its_preview_and_how_often_it_sweeps() {
            const layout = findChild(cmp, "pushfps-compare-layout")
            compare(layout.options.map(o => o.id).join(","), "0,1,2,3", "split, side by side, vertical, then reveal")
            verify(!cmp.reveal, "split has no sweep interval")
            layout.picked("3")
            compare(veyra.preferences.pushCompareLayout, 3)
            compare(cmp.layoutId, 3)
            verify(cmp.reveal && !cmp.verticalPreview, "a landscape preview")
            const preview = findChild(cmp, "pushfps-compare-preview-reveal")
            verify(preview !== null)
            // (The dialog fades in: wait for it before reading what is visible.)
            tryVerify(function() { return preview.visible }, 3000, "the reveal preview is drawn")
            tryVerify(function() { return findChild(cmp, "pushfps-compare-tag-left") === null }, 3000, "without the split layout's fixed labels")
            verify(cmp.layoutHint(3).indexOf("PushFPS") >= 0)
            // How often the line sweeps: every 3 seconds unless chosen.
            verify(findChild(cmp, "pushfps-compare-reveal-row").visible)
            const every = findChild(cmp, "pushfps-compare-reveal")
            compare(every.options.map(o => o.id).join(","), "1,2,3,5,8")
            compare(every.current, "3")
            for (const seconds of [1, 5, 8, 2]) {
                every.picked(String(seconds))
                compare(veyra.preferences.pushCompareRevealSeconds, seconds)
                compare(cmp.revealEvery, seconds)
            }
            veyra.setPreference("pushCompareRevealSeconds", 4)
            compare(cmp.revealEvery, 3, "an interval that is no choice counts as 3")
            // The length works as in every layout.
            findChild(cmp, "pushfps-compare-length").picked("60")
            action(qsTr("开始录制"))
            compare(veyra.compareLayout, 3)
            compare(veyra.compareSeconds, 60)
            compare(findChild(quick, "pushfps-compare-clip").sub, qsTr("%1 秒").arg(60) + " · " + qsTr("滑动揭示"))
        }
        function test_reveal_preview_moves_as_the_clip_does() {
            // The recorder's rule (RecordingComparePlan.h), repeated for the preview: a sweep starts
            // every interval, takes 0.5 to 1.2 s, and each version holds for the rest.
            compare(Clip.revealSweepSeconds(1), 0.5)
            fuzzyCompare(Clip.revealSweepSeconds(3), 0.9, 1e-9)
            compare(Clip.revealSweepSeconds(8), 1.2)
            for (const every of Clip.revealIntervals) {
                const sweep = Clip.revealSweepSeconds(every), hold = every - sweep
                compare(Clip.revealPosition(0, every), 0, "opens on the original")
                compare(Clip.revealPosition(hold * 0.9, every), 0, "and holds there")
                fuzzyCompare(Clip.revealPosition(hold + sweep / 2, every), 0.5, 1e-9)
                fuzzyCompare(Clip.revealPosition(every, every), 1, 1e-9)
                compare(Clip.revealPosition(every + hold * 0.5, every), 1, "then all PushFPS")
                fuzzyCompare(Clip.revealPosition(every + hold + sweep / 2, every), 0.5, 1e-9)
                fuzzyCompare(Clip.revealPosition(2 * every + hold * 0.5, every), 0, 1e-9)
                let last = 0, forward = true
                for (let i = 0; i <= 200; ++i) {
                    const p = Clip.revealPosition(hold + sweep * i / 200, every)
                    if (p < last - 1e-12) forward = false
                    last = p
                }
                verify(forward, "the line only moves forward during a sweep (every " + every + " s)")
            }
            // In the preview: the line's x, and the labels cut at it.
            findChild(cmp, "pushfps-compare-layout").picked("3")
            compare(cmp.revealX(0, 320), 0)
            compare(cmp.revealX(0.5, 320), 160)
            compare(cmp.revealX(1, 320), 320)
            compare(cmp.revealX(7, 320), 320)
            verify(findChild(cmp, "pushfps-compare-reveal-tag-push") !== null && findChild(cmp, "pushfps-compare-reveal-tag-original") !== null,
                   "both labels, each on its side of the line")
            verify(findChild(cmp, "pushfps-compare-reveal-line") !== null, "and the line")
        }
        function test_clip_quality_for_the_layouts_that_fill_the_frame() {
            const row = findChild(cmp, "pushfps-compare-quality-row")
            const quality = findChild(cmp, "pushfps-compare-quality")
            verify(row !== null && quality !== null)
            compare(quality.options.map(o => o.id).join(","), "0,1080,1440,2160")
            compare(quality.options.map(o => o.label).join(","), [qsTr("自动"), "1080p", "1440p", "4K"].join(","))
            // Split (the layout a new install has) and Reveal have it; side by side and vertical stay as they are.
            const layout = findChild(cmp, "pushfps-compare-layout")
            // (The dialog fades in: wait for it before reading what is visible.)
            tryVerify(function() { return row.visible }, 3000, "split has the quality row")
            const shown = [], taken = []
            for (const id of [0, 1, 2, 3]) { layout.picked(String(id)); shown.push(row.visible); taken.push(cmp.takesQuality) }
            compare(taken.join(","), "true,false,false,true")
            compare(shown.join(","), "true,false,false,true")
            compare(cmp.quality, 0, "automatic unless chosen")
            compare(quality.current, "0")
            for (const height of [1080, 1440, 2160]) {
                quality.picked(String(height))
                compare(veyra.preferences.pushCompareQuality, height)
                compare(cmp.quality, height)
            }
            verify(cmp.qualityHint(2160) !== cmp.qualityHint(1440) && cmp.qualityHint(1440) !== cmp.qualityHint(0), "4K says what it needs")
            veyra.setPreference("pushCompareQuality", 900)
            compare(cmp.quality, 0, "a height that is no choice counts as automatic")
            // The choice is kept while another layout is used, and comes back with split or reveal.
            veyra.setPreference("pushCompareQuality", 2160)
            layout.picked("1")
            verify(!row.visible)
            compare(veyra.preferences.pushCompareQuality, 2160)
            layout.picked("0")
            verify(row.visible)
            compare(quality.current, "2160")
        }
        function test_a_running_clip_shows_its_time_and_stops_from_the_tile() {
            veyra.setPreference("pushCompareUntilStop", true)
            const tile = findChild(quick, "pushfps-compare-clip")
            veyra.recording = true
            veyra.recordingState = ({ saved: 0, active: true, path: "", compare: true, seconds: 83.4 })
            compare(tile.text, qsTr("停止对比片段"))
            compare(tile.sub, qsTr("正在录制…") + " 01:23", "how long it has run: a clip without a length has no other clock")
            verify(tile.enabled)
            veyra.recordingState = ({ saved: 0, active: true, path: "", compare: true, seconds: 3725 })
            compare(tile.sub, qsTr("正在录制…") + " 1:02:05")
            const stops = veyra.recordingStops
            tile.clicked()
            compare(veyra.recordingStops, stops + 1, "the tile stops it")
            verify(!veyra.recording)
            veyra.recordingState = ({ saved: 0, active: false, path: "" })
        }
        function test_an_unknown_layout_falls_back_to_split() {
            veyra.setPreference("pushCompareLayout", 5)
            compare(cmp.layoutId, 0)
            veyra.setPreference("pushCompareLayout", -1)
            compare(cmp.layoutId, 0)
            verify(!cmp.reveal)
        }
        function test_cancel_records_nothing() {
            action(qsTr("取消"))
            compare(veyra.compareStarts, 0)
            compare(host.dialog, "")
        }
        function test_an_unknown_length_falls_back_to_15() {
            veyra.setPreference("pushCompareSeconds", 7)
            compare(cmp.seconds, 15)
            veyra.setPreference("pushCompareSeconds", 0)
            compare(cmp.seconds, 15, "no length is its own switch, never a length of 0")
            action(qsTr("开始录制"))
            compare(veyra.compareSeconds, 15)
        }
        function test_preview_labels_follow_the_layout() {
            // Split: the top corners of the one picture (320 x 180 preview, 90-wide labels).
            compare(cmp.tagX(0, 90, 320, false), 6, "split: left label at the top left")
            compare(cmp.tagX(1, 90, 320, false), 224, "split: right label at the top right")
            compare(cmp.tagY(16, 180, false), 6, "split: labels at the top of the picture")
            // Side by side: centred over each half, in the black bar (180 / 4 = 45 high) above the pictures.
            compare(cmp.tagX(0, 90, 320, true) + 45, 80, "side by side: centred over the left picture")
            compare(cmp.tagX(1, 90, 320, true) + 45, 240, "side by side: centred over the right picture")
            const y = cmp.tagY(16, 180, true)
            verify(y >= 0 && y + 16 <= 45, "side by side: labels sit in the black bar, y=" + y)
            verify(findChild(cmp, "pushfps-compare-preview") !== null, "the preview is drawn")
        }
        function test_preview_labels_name_both_rates() {
            veyra.fgMultiplier = 4
            compare(cmp.leftTag, qsTr("主机原画 · %1 FPS").arg(30))
            compare(cmp.rightTag, "PushFPS · 120 FPS")
            veyra.fgMultiplier = 1
        }
        function test_tile_opens_the_dialog_with_nothing_playing() {
            veyra.hasSource = false
            const tile = findChild(quick, "pushfps-compare-clip")
            verify(tile.enabled, "the layout and length can be chosen before the capture card runs")
            const opened = []
            const onOpen = p => opened.push(p)
            veyra.navigate.connect(onOpen)
            tile.clicked()
            veyra.navigate.disconnect(onOpen)
            compare(opened.join(","), "compare")
            compare(veyra.compareStarts, 0)
            veyra.hasSource = true
        }
        function test_tile_shows_the_choice() {
            veyra.setPreference("pushCompareLayout", 1)
            veyra.setPreference("pushCompareSeconds", 60)
            const tile = findChild(quick, "pushfps-compare-clip")
            compare(tile.sub, qsTr("%1 秒").arg(60) + " · " + qsTr("并排"))
        }
    }
}

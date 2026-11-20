// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Crop and resize of the console picture: the maths (PushCrop.js), the editor (PushCropDialog: drag an
// edge, a corner, the middle; numbers, shapes, size, position, reset), the quick setup row and the
// profiles, against the fixture bridge.
import QtQuick
import QtTest
import Veyra
import "../qml/Veyra/PushCrop.js" as Crop
Item {
    id: root
    width: 1000; height: 1100
    // The quick setup first, so the dialog (which lives in the host while it is open) lies over it as in
    // the program: with the panel on top, a press in the middle of the editor's picture went to
    // whichever of the panel's controls happened to be there.
    PushQuickSetup { id: quick; width: 560; y: 20 }
    Item { id: host; anchors.fill: parent; z: 10; property string dialog: ""; function close() { dialog = "" } }
    PushCropDialog { id: dlg; dialogHost: host; frameAspect: 16 / 9 }

    function pref(key) { return veyra.preferences[key] }
    function sides() { const c = Crop.read(veyra.preferences); return [c.l, c.t, c.r, c.b] }

    TestCase {
        name: "PushCrop"
        when: windowShown
        function init() {
            host.dialog = ""
            veyra.preferences = ({ pushGameFps: 30 })
            veyra.hasSource = true
            veyra.sourceKind = "capture"
            veyra.presentationOwned = false
            veyra.liveStats = ({ running: false })
        }
        function open() {
            host.dialog = "crop"
            verify(dlg.shown)
            tryCompare(dlg, "motionS", 1, 3000)
            tryCompare(dlg, "motionDy", 0, 3000)
            waitForRendering(dlg)
            // A dialog closed a moment ago is still running its closing motion beside the opening one.
            wait(60)
            tryCompare(dlg, "motionS", 1, 3000)
            tryCompare(dlg, "opacity", 1, 3000)
        }
        // The editor writes a moment after the last change. Waiting for that write, not for a length of
        // time: the timer runs with the frames, and a build machine without a graphics card draws slowly.
        function settle() { tryCompare(dlg, "pendingWrite", false, 5000) }
        // A drag by hand, one event at a time, that first checks the press took the handle it was aimed at.
        function dragHandle(stage, handle, x, y, dx, dy) {
            mouseMove(stage, x, y)
            mousePress(stage, x, y)
            compare(stage.held, handle, "the press at " + Math.round(x) + "," + Math.round(y) + " of " + Math.round(stage.width) + "x" + Math.round(stage.height))
            mouseMove(stage, x + dx / 3, y + dy / 3)
            mouseMove(stage, x + dx * 2 / 3, y + dy * 2 / 3)
            mouseMove(stage, x + dx, y + dy)
            mouseRelease(stage, x + dx, y + dy)
            compare(stage.held, "", "released")
        }

        // --- maths ---------------------------------------------------------------------------------
        function test_values_are_cleaned() {
            const c = Crop.read({ pushCropL: 999, pushCropT: -5, pushCropR: "x", pushCropZoom: 0, pushCropX: 5000, pushCropOn: true })
            compare([c.l, c.t, c.r, c.b], [450, 0, 0, 0], "a side is 0 to 45 %")
            compare(c.zoom, 100, "no size is the fitted size")
            compare(c.x, 1000)
            verify(c.on)
            verify(Crop.neutral(Crop.read({})), "nothing set changes nothing")
            verify(!Crop.neutral(Crop.read({ pushCropZoom: 120 })))
        }
        function test_shapes_cut_evenly_around_the_middle() {
            const whole = Crop.read({})
            const wide = Crop.shaped(whole, 21 / 9, 16 / 9)
            compare([wide.l, wide.t, wide.r, wide.b], [0, 119, 0, 119], "21:9 out of 16:9: top and bottom")
            fuzzyCompare(Crop.keptAspect(wide, 16 / 9), 21 / 9, 0.01)
            const four = Crop.shaped(whole, 4 / 3, 16 / 9)
            compare([four.l, four.t, four.r, four.b], [125, 0, 125, 0], "4:3 out of 16:9: the sides")
            compare(Crop.shapeIndex(wide, 16 / 9), 2)
            compare(Crop.shapeIndex(whole, 16 / 9), 0)
            compare(Crop.shapeIndex(Crop.copy(whole, { l: 37 }), 16 / 9), -1, "a crop made by hand is none of the shapes")
            verify(!Crop.cuts(Crop.shaped(wide, 16 / 9, 16 / 9)), "the picture's own shape is the whole picture")
        }
        function test_handles_and_drags() {
            const whole = Crop.read({})
            compare(Crop.hit(whole, 2, 300, 800, 450, 9), "l", "an edge at the picture's border can be taken")
            compare(Crop.hit(whole, 2, 2, 800, 450, 9), "tl", "corners come first")
            compare(Crop.hit(whole, 400, 225, 800, 450, 9), "move")
            compare(Crop.hit(Crop.copy(whole, { l: 100 }), 40, 225, 800, 450, 9), "", "nothing to hold in a cut part")
            const corner = Crop.drag(whole, "br", -50, -80)
            compare([corner.l, corner.t, corner.r, corner.b], [0, 0, 50, 80])
            compare(Crop.drag(whole, "r", 100, 0).r, 0, "an edge cannot go past the picture")
            compare(Crop.drag(whole, "l", 900, 0).l, 450, "nor past 45 %")
            const start = Crop.copy(whole, { l: 100, r: 200, t: 50, b: 50 })
            const moved = Crop.drag(start, "move", 150, -200)
            compare([moved.l, moved.r, moved.t, moved.b], [250, 50, 0, 100], "sliding the kept part keeps its size")
            const far = Crop.drag(start, "move", 900, 0)
            compare([far.l, far.r], [300, 0], "and stops at the picture's edge")
        }

        // --- the editor ----------------------------------------------------------------------------
        function test_dragging_an_edge_crops_and_switches_it_on() {
            open()
            const stage = findChild(dlg, "pushfps-crop-stage")
            verify(stage.width > 200 && stage.height > 100)
            // The left edge, in the middle of its height, a tenth of the picture to the right.
            dragHandle(stage, "l", 1, stage.height / 2, stage.width * 0.1, 0)
            settle()
            const c = Crop.read(veyra.preferences)
            verify(Math.abs(c.l - 100) <= 8, "left " + c.l)
            compare([c.t, c.r, c.b], [0, 0, 0])
            compare(pref("pushCropOn"), true, "a crop that cuts something is on")
            // The bottom right corner inwards: up as well as left, which a scroll view would take over.
            dragHandle(stage, "br", stage.width - 1, stage.height - 1, -stage.width * 0.05, -stage.height * 0.2)
            settle()
            const d = Crop.read(veyra.preferences)
            verify(Math.abs(d.r - 50) <= 8 && Math.abs(d.b - 200) <= 10, "right " + d.r + " bottom " + d.b)
            verify(Math.abs(d.l - c.l) <= 1, "the other sides stay")
        }
        function test_the_middle_slides_the_kept_part() {
            veyra.preferences = ({ pushCropOn: true, pushCropL: 200, pushCropR: 200 })
            open()
            const stage = findChild(dlg, "pushfps-crop-stage")
            verify(stage.width > 200 && stage.height > 100)
            compare([dlg.crop.l, dlg.crop.r], [200, 200], "the editor opens on what is set")
            dragHandle(stage, "move", stage.width / 2, stage.height / 2, stage.width * 0.1, 0)
            compare(dlg.crop.l + dlg.crop.r, 400, "the editor's own picture follows at once")
            verify(Math.abs(dlg.crop.l - 300) <= 8, "left in the editor " + dlg.crop.l)
            settle()
            const c = Crop.read(veyra.preferences)
            verify(Math.abs(c.l - 300) <= 8 && Math.abs(c.r - 100) <= 8, "left " + c.l + " right " + c.r)
            compare(c.l + c.r, 400, "the kept part keeps its width")
        }
        function test_numbers_shapes_size_and_position() {
            open()
            const left = findChild(dlg, "pushfps-crop-l")
            left.edited("12,5")
            settle()
            compare(pref("pushCropL"), 125, "a number is a percentage, with a comma or a point")
            compare(left.text, "12.5")
            findChild(dlg, "pushfps-crop-shape").picked("2")
            settle()
            compare(root.sides(), [0, 119, 0, 119], "21:9 replaces the crop made by hand")
            compare(findChild(dlg, "pushfps-crop-shape").current, "2")
            findChild(dlg, "pushfps-crop-zoom").moved(150)
            findChild(dlg, "pushfps-crop-x").moved(-250)
            settle()
            compare(pref("pushCropZoom"), 150)
            compare(pref("pushCropX"), -250)
            compare(findChild(dlg, "pushfps-crop-zoom-value").text, "150%")
            findChild(dlg, "pushfps-crop-centre").clicked()
            settle()
            compare(pref("pushCropX"), 0)
            compare(pref("pushCropZoom"), 150, "centring moves the picture, it does not resize it")
        }
        function test_reset_shows_the_whole_picture_and_switches_it_off() {
            veyra.preferences = ({ pushCropOn: true, pushCropL: 100, pushCropB: 60, pushCropZoom: 130, pushCropY: 40 })
            open()
            compare(dlg.crop.l, 100, "the editor opens on what is set")
            dlg.actionTriggered(qsTr("重置"))
            compare(root.sides(), [0, 0, 0, 0])
            compare(pref("pushCropZoom"), 100)
            compare(pref("pushCropY"), 0)
            compare(pref("pushCropOn"), false, "nothing left to do: off")
            verify(dlg.shown, "reset keeps the editor open")
            dlg.actionTriggered(qsTr("完成"))
            verify(!dlg.shown)
        }
        function test_closing_keeps_the_last_change() {
            open()
            dlg.set({ t: 80 })
            host.dialog = ""            // closed before the delayed write
            compare(pref("pushCropT"), 80)
            compare(pref("pushCropOn"), true)
        }
        function test_recordings_follow_only_when_asked() {
            open()
            const sw = findChild(dlg, "pushfps-crop-outputs")
            verify(!sw.checked, "recordings and streams keep the whole picture unless asked")
            sw.toggled(true)
            compare(pref("pushCropOutputs"), true)
        }
        function test_the_editor_says_when_the_crop_does_not_apply() {
            open()
            const note = findChild(dlg, "pushfps-crop-note")
            verify(!note.visible, "a capture card picture: nothing to say")
            veyra.presentationOwned = true
            verify(note.visible && note.text.indexOf("XeSS") >= 0)
            veyra.presentationOwned = false
            veyra.sourceKind = "file"
            verify(note.visible, "a film keeps its whole picture, and the editor says so")
            veyra.sourceKind = "ps5"
            verify(!note.visible)
        }
        function test_the_editor_takes_one_frame_of_the_picture() {
            const before = veyra.hudStarts
            open()
            compare(veyra.hudStarts, before + 1)
            compare(veyra.hudLastSamples, 1)
            host.dialog = ""
            verify(veyra.hudCancels > 0, "closing stops a sample still under way")
        }

        // --- quick setup and profiles ------------------------------------------------------------------
        function test_quick_setup_row() {
            const row = findChild(quick, "pushfps-crop-row")
            const sw = findChild(quick, "pushfps-crop")
            verify(row !== null && sw !== null)
            compare(row.hint, qsTr("显示完整画面"))
            verify(!sw.enabled, "nothing set: nothing to switch")
            veyra.preferences = ({ pushGameFps: 30, pushCropOn: true, pushCropT: 119, pushCropB: 119 })
            verify(sw.enabled && sw.checked)
            verify(row.hint.indexOf("11.9") >= 0, row.hint)
            sw.toggled(false)
            compare(pref("pushCropOn"), false)
            compare(pref("pushCropT"), 119, "switching off keeps the values")
            compare(row.hint, qsTr("已关闭，设置保留"))
            let opened = ""
            const handler = p => opened = p
            veyra.navigate.connect(handler)
            findChild(quick, "pushfps-crop-edit").clicked()
            veyra.navigate.disconnect(handler)
            compare(opened, "crop")
        }
        function test_profiles_keep_the_crop() {
            veyra.preferences = ({ pushGameFps: 30, pushCropOn: true, pushCropL: 50, pushCropZoom: 120 })
            const withCrop = quick.snapshot()
            compare(withCrop.crop.l, 50)
            compare(withCrop.crop.zoom, 120)
            veyra.preferences = ({ pushGameFps: 30 })
            const without = quick.snapshot()
            compare(without.crop, false)
            quick.applySnapshot(withCrop)
            compare(pref("pushCropL"), 50)
            compare(pref("pushCropZoom"), 120)
            compare(pref("pushCropOn"), true)
            quick.applySnapshot(without)
            compare(pref("pushCropOn"), false, "a profile saved without a crop shows the whole picture")
            compare(pref("pushCropL"), 50, "and keeps the values for later")
            veyra.setPreference("pushCropOn", true)
            quick.applySnapshot({ fps: 30, target: "off" })
            compare(pref("pushCropOn"), true, "a profile from before the crop existed leaves it alone")
        }
    }
}

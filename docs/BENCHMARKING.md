# Measuring PushFPS on your PC

How fast frame generation runs depends on the graphics card, and the automated builds have none: they run on
Windows' software renderer, which checks that the picture is right and says nothing about speed. Every real
number therefore comes from a real PC. This page says how to take one, and what it is worth.

## What a number is worth

| Level | What was measured | What it proves |
|---|---|---|
| 1 | The automated build, on the software renderer | The code is correct. Nothing about speed. |
| 2 | **Test my graphics card**: PushFlow's time per generated frame on your card | How fast the network runs on that card and driver. Not a whole session. |
| 3 | **Performance report**: a live session's frames and PushFPS's own timing | What PushFPS really delivered with your source, settings and screen. |
| 4 | A camera or light sensor pointed at the screen | The delay you feel, button to screen. PushFPS cannot measure this itself. |
| 5 | The same result from someone else, on other hardware | That it was not a one-off. |

A level never stands in for the one above it. In particular:

- A time from the software renderer is not a time on any graphics card.
- A fast card test does not mean a smooth session: the capture card, the screen and everything else PushFPS
  does share the same card.
- PushFPS's *software latency* is the time from a frame's arrival in PushFPS to its `Present` call. It leaves
  out the console, the capture card and the display, so it is always less than what you feel.
- Generated frames make motion smoother. They do not make the console read the controller more often.

## Test my graphics card (level 2, about a minute)

*Settings → Test my graphics card → Test.* Nothing may be playing. It runs PushFlow's network at 1080p and 4K
in all three tiers, in its own process, and says for 30 and 60 FPS games which tiers keep up. **Copy result**
gives the card's name, its memory and the timings: nothing personal.

The same benchmark from a command line, for more runs (it sits beside `PushFPS.exe`):

```
PushFlowBench.exe --sizes 1920x1080,2560x1440,3840x2160 --tiers 0,1,2 --pairs 200 --out result.json
```

Run it three times with nothing else using the card and keep all three results. `--help` lists the options.

## Performance report (level 3)

1. Play as you normally would for at least five minutes, so the card has warmed up and the rates have settled.
2. *Settings → Performance report → Copy* (the same button appears in the quick setup when frame generation
   shows no frames).
3. Paste it where you report it.

What is in it:

| Field | Meaning |
|---|---|
| `gpu`, `display` | The card, its driver and memory; the screen's refresh rate |
| `source` | Kind of source, its size, what the capture card delivers and has dropped, the game's own frame rate |
| `frameGeneration`, `enhancement` | The settings that decide the load |
| `realFps` | The game's own frames PushFPS showed in the last second |
| `generatedFps` | The generated frames it showed in the last second |
| `presentedFps`, `low1Fps` | Everything it showed, and the 1 % low |
| `softwareLatencyMs` | Arrival in PushFPS to `Present`, mean and 95th percentile, with the sample count |
| `frameGenerationHealth` | 0 fine; 1 nothing to add at this target or on this screen; 2, 3 or 4 mean frame generation is on and shows nothing (the notes in the report say which is which) |
| `evidence` | What the numbers are worth, in words |

What is never in it: your Windows user name, a file or folder, a device name, a network address, an account,
a stream key. The fields do not exist, and the one free text (the card's name) is reduced to plain characters.

A healthy 30 FPS game at a 120 FPS target reads about `realFps` 30, `generatedFps` 90, `presentedFps` 120.
If `generatedFps` is far below that, the card is skipping frames: lower the tier, the multiplier or the output
resolution.

## The delay you feel (level 4)

PushFPS cannot see your screen, so this takes a camera:

1. Film the controller and the screen together in slow motion, 240 frames a second or more.
2. Press a button that changes the picture at once (a menu move, a jump).
3. Count the camera frames from the button going down to the first change on screen. At 240 frames a second
   one frame is 4.2 ms.
4. Do it at least 30 times and give the middle value and the spread, not the best one.
5. Repeat with the console straight on the same screen (HDMI passthrough) for comparison, and say which
   screen and which mode.

## Sending it in

Open a [hardware report](https://github.com/DosukaSOL/PushFPS/issues/new?template=hardware-report.yml) and
paste the performance report (and the card test, if you ran it). A card goes on the
[tested list](../README.md#tested-graphics-cards) after at least one steady half-hour session.

When you compare two settings, or PushFPS with something else, keep everything else the same: the game and
scene, the source, the output size, the target frame rate and the screen. A comparison that changes two things
at once proves nothing about either.

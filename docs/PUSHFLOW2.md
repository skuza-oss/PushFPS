# PushFlow 2: the plan, and where it stands

On 19 November 2026 a reviewer sent an engineering guide for the next generation of PushFlow: six parts, each
with its own tests and gates, and one rule above all of them: nothing that exists today may break. This file
is how PushFPS takes that guide: what each part is, what has been done, what was measured, and what waits
for something that code alone cannot give. It follows the [roadmap](ROADMAP.md)'s rules and adds none that
contradict them. No dates.

## The six parts

| Part | What it is | Where it stands |
|---|---|---|
| Guard | Find the places in a generated frame that are probably wrong and show something safe there instead | **Built, as an experiment that starts off** (*Professional → PushFlow 2: Guard*): where an object was left out of a generated frame, the nearer real frame is shown (below). Four rules failed in measurement before the one it uses held. Checked on drawn pictures and the software renderer only; whether it looks better in a game is for real cards and eyes to say. Beside it, since 3.2.7, the frame quality test says its **worst frame** and its **worst place** |
| Fusion | Encode every real frame once and work a pair's motion out once, then make every in-between time from it | **Its first step is built, as an experiment that starts off** (*Professional → PushFlow 2: encode each real frame once*): the model cut into its per-frame encoder and the rest, each real frame encoded once (below). The second step, a pair's motion worked out once, does not exist in these models: the time enters at the first motion block |
| Predict | Make frames from what has already arrived, without waiting for the next real frame | **Built, as an experiment that starts off** (*Professional → PushFlow 2: Predict*), for live sources only: the newest real frame is shown after its own processing instead of a frame interval later, and the frames after it are forecasts made from the two real frames that have arrived (below). On drawn pictures a forecast is level with holding the real frame at worst, and much nearer for slow motion. Never the normal mode. How much delay it saves and how it looks have not been measured on real hardware: that takes a camera |
| Dual | A second, deliberately delayed timeline for recording and streaming, beside the one the player sees | **Designed, not built** (below: "Dual, the first step as designed"). With Predict the player sees forecasts, and a recording is "as played", so it holds them too; the first step gives recordings and streams the true in-between frame of the same pair, which Predict's one run of the network has already made |
| Response | Turn the picture a little from the stick's movement before the console's own frame arrives | Research only. Whether it can work from a video signal at all is unproven; it needs a PS5, a high-speed camera and people to judge it |
| Memory | Bring back detail that earlier real frames showed | Research only. It depends on motion that has been checked (Guard, Fusion) |

## The rules these parts are built under

1. **Off means unchanged.** A part that is switched off leaves the picture, the timing and the saved settings
   exactly as they were. That is tested, not assumed.
2. **Measured before believed.** A rule built on the network's own tensors is run on the shipped models first
   (`scripts/pushflow/prepare_runtime.py`, `motion_report`, a few minutes in the `pushflow-models` workflow).
   The first rule failed exactly there, before any user saw it.
3. **Blocked is a status.** A step that cannot be tested here (no card, no camera, no clips, no licence) stays
   off and is listed below with what it waits for. It is not shipped "experimental" to find out.
4. **No switch without something behind it.** Settings, flags and files are added with the feature that needs
   them, not ahead of it.
5. **Every guard stays plugged in.** `tests/protected-features.json` names what must keep working and the test
   that guards each part; `scripts/checks/protected_features.py` fails the `checks` workflow when such a test
   is deleted, no longer run, or loses the check that matters.
6. **A held frame is not a generated frame.** Whatever a part falls back to is counted as what it is.

## Done

- **The motion contract is code.** What PushFlow's models give besides their picture (two sampling grids and a
  blend mask), what those are *not* (not optical flow between the real frames; not a confidence), and how
  positions convert between the motion field and the picture: `include/veyra/pushflow/PushFlowMotion.h`, with
  its own unit test (`veyra_push_flow_motion_tests`). Until now this lived only in a shader and a Python check.
- **The rebuild is checked against it.** The shader that makes the full-size picture
  (`shaders/PushFlowReconstruct.hlsl`) is compared pixel by pixel with the contract's reference, from the
  network's own grids and mask, on five kinds of pair (`testPushFlow`, software renderer). Before, it was only
  judged by how close its picture came to a drawn truth.
- **A motion report for every model build**: how the grids and mask behave when the network can and cannot
  follow a motion. It decides nothing; it is the measurement the next rule starts from.
- **The protected-feature list**, checked on every push (rule 5).
- **The frame quality test says its worst frame and its worst place** (3.2.7). The test already withheld
  every second real frame, generated it and compared; it reported averages. An object left out of a generated
  frame costs the whole picture a fraction of a decibel and one place nearly everything, so the test now also
  gives the least accurate frame, and the least accurate of 144 tiles averaged over the frames
  (`worstTilePsnr`, `include/veyra/engine/PushQuality.h`). In the unit test a missing object leaves the
  picture at 29.6 dB and its place at 8.1 dB. The score is what it was.
- **PushFlow's motion size can be chosen apart from its tier** (3.2.7, *Professional → PushFlow motion
  size*): automatic (each tier's own, as before), or at most 540, 720 or 1080 lines whatever the tier's
  model. It is a choice and not a new automatic rule, because of what the measurement below says: the models
  do not behave the same at every size, and not in one direction. The row shows the size really in use; the
  too-slow watch puts a size that is too much for the card back to automatic before it lowers a tier, and
  every step it can take is cheaper than the one before (unit test). Without a choice, every tier's size is
  bit for bit what it was (unit test), and the graph test runs PushFlow both ways.
- **Experiments have switches, and every one starts off** (`include/veyra/pushflow/PushFlow2.h`, preference
  `pushFlow2`). A part is not in the build, not available here, off, on and waiting, or running, and the row
  in Professional mode says which: asked for is not the same as in use. The preference is read strictly:
  damaged text, text of another kind and a file cut off mid-save all read as everything off, and a newer
  version's names are skipped. `PushFPS.exe --safe-mode` keeps every experiment off for one run and changes
  nothing saved. If PushFPS ever stops inside PushFlow, the next start switches them all off.
- **The encoder cache** (Fusion's first step; the owner's decision of 19 November 2026 to build every part
  without waiting for hardware results, each behind a switch). `scripts/pushflow/rife_graph.py`
  `split_encoder` cuts each model into two small encoders (one for the input's first frame, one for its
  second) and the rest; nothing is retrained, the same nodes and weights. `prepare_runtime.py` ships them
  only when together they give the whole model's output on the CPU: every output, seven times between
  the frames, two sizes, features kept from the pair before and made anew. Measured: the largest
  difference is 0.0 for both models (run 44629175911). In PushFlow (`PushFlowBackend.cpp`, `Split`) the
  second frame is encoded when its pair arrives and its features stay for the pair after it, where it is
  the first frame; another time between the same two frames runs the rest alone. So a steady 2× costs one
  encoder run per pair instead of two, and 3× and 4× one instead of four and six. It is used only after it
  gave the whole model's picture on the user's own graphics card at start, for a pair with nothing kept
  and for the pair after it. The whole model stays loaded: a split run that fails hands the same frame to
  it, and the row says so. Frames are told apart by the graph's own count; a frame whose number is
  unknown, skipped or outdated is encoded again. Not available with half precision yet. **Unverified on
  real hardware, and the saving is unmeasured there**: the ceiling in arithmetic is 7 to 24 % (below); the
  start-up line in the log and the performance report (`pushFlowEncoderCacheStartMs`) give one frame each
  way on the user's card.
- **Three studies of the shipped models** run in the `pushflow-models` workflow (`--study`): what the
  tensors do under motion (`motion_report`), where the network's work goes (`fusion_report`), and whether a
  frame ahead can be made from it (`predict_report`). They print numbers and decide nothing.

## Measured: what the models do when motion is too fast

`pushflow-models` run 44595965906 (19 November 2026), on the CPU, a 512 × 320 picture at the network's own
size, the middle time. A 40-pixel square moves across a still background:

| The square moved | Fast model | Quality model | What the in-between frame shows |
|---|---|---|---|
| 16 px (3 % of the width) | 47.6 dB | 62.5 dB | the square, half-way |
| 48 px (9 %) | 37.6 dB | 40.8 dB | the square, half-way |
| 96 px (19 %) | 36.1 dB | 35.5 dB | the square, half-way, less exactly |
| 160 px (31 %) | 29.0 dB | 29.0 dB | **no square at all** |
| 256 px (50 %) | 29.1 dB | 29.1 dB | **no square at all** |

What the tensors say in the last two rows: at the square's old place the mask takes the current frame, at its
new place the previous one; each shows background there. Nowhere does the mask hesitate, and where the mask
does keep both samples (12 to 68 % of these pictures) the two samples agree. Three things follow.

1. **A failure is a missing object, not a see-through double.** An object that moves further than the network
   can follow is left out of the generated frame. At 2× that is a blink at the game's own rate.
2. **The mask says nothing about it.** It is as decided where the network is wrong as where it is right.
3. **A smaller motion size reaches further, sometimes.** The same pairs in a 512 × 384 picture, with the
   network at full, half and a quarter of the picture's size and the picture rebuilt at full size (run
   44600838736):

   | The square moved | Model | Full size | Half | Quarter |
   |---|---|---|---|---|
   | 96 px | Quality | 35.6 dB | 38.7 dB | 37.7 dB |
   | 160 px | Quality | 29.8 dB, left out | 32.7 dB, back | 33.7 dB, back |
   | 160 px | Fast | 29.8 dB, left out | 29.3 dB, left out | 31.7 dB, back |
   | 256 px | both | left out | left out | left out |

   So the reach is not a fixed share of the screen, and the largest motion size is not the best one for
   fast motion: on these pairs the Quality model did better at half size than at full size. It is not
   a law. A quarter-size network still loses the square at 256 px, the Fast model gains only at a
   quarter, and a fine texture sliding sideways (run 44605491581) goes every way at once:

   | The texture moved | Model | Full size | Half | Quarter |
   |---|---|---|---|---|
   | 8 px | Quality | 22.9 dB | 30.7 dB | 27.2 dB |
   | 16 px | Quality | 31.7 dB | 23.1 dB | 14.7 dB |
   | 32 px | Quality | 25.2 dB | 33.9 dB | 20.5 dB |
   | 64 px | Quality | 20.4 dB | 23.6 dB | 15.0 dB |
   | 8 px | Fast | 22.2 dB | 27.8 dB | 14.9 dB |
   | 16 px | Fast | 14.4 dB | 14.6 dB | 22.3 dB |
   | 32 px | Fast | 19.9 dB | 19.2 dB | 14.9 dB |

   and a slow square is always best at full size (16 px, Quality: 63.0, 50.9, 48.3 dB). No rule that
   picks the size from the motion follows from this, so none was built. What was built is the choice
   (3.2.7, above) and the number to judge it by on a real game: change the motion size, run the frame
   quality test, compare the worst place.

A fade (the whole picture 0.3 brighter, nothing moving) is the opposite case: the mask keeps both samples
almost everywhere and they differ everywhere, because a blend is the right picture there.

These are drawn pictures. They show what the tensors do; they do not rank the tiers and say nothing about
how a game looks.

## Built, measured, taken out: the ghost guard

The first Guard rule assumed that a failure looks like a mixture: two samples that differ, and a mask that
cannot choose. Where that happened, the pixel was given to one sample, the real frame nearer in time. It was
written whole (shader, reference, tests, a switch in Professional mode), and its report on the shipped models
(run 44595135167) said:

- on every square pair, followed or not, it changed 0.00 to 0.03 % of the picture: there was nothing for it
  to act on, because a failure is not a mixture (above);
- on the fade it changed 91 to 98 % of the picture at its two stronger levels and turned a correct blend into
  a held frame (27 to 29 dB down to 16 to 17 dB).

It acted where the network was right and not where it was wrong. It did not prove itself, so it is not in the
product (roadmap rule 5). The contract, the reference and the stricter rebuild check that were written for it
are what stayed.

## Measured and not built: holding a whole frame

A second idea was a rule for the whole frame instead of for a place: when the two samples disagree over
much of the picture, the pair has failed, so show a real frame instead. Its number (the mean difference
between the two samples, run 44605491581) does not separate failure from success:

| Pair | Difference between the samples | The generated frame |
|---|---|---|
| a square, followed | 0.0004 to 0.0023 | 36 to 62 dB |
| a square, left out | 0.007 to 0.009 | 29 dB |
| a fade of 0.07, generated correctly | 0.062 to 0.076 | 38 to 39 dB |
| a fine texture, followed (8 px) | 0.058 (Fast), 0.092 (Quality) | 22 dB, 18 dB |
| a fine texture, lost (64 px and more) | 0.135 to 0.149 | 14 to 15 dB |
| a fade of 0.3, generated correctly | 0.30 to 0.32 | as good as a blend can be |
| two unrelated pictures (a cut) | 0.18 to 0.19 | no right answer |

A left-out square scores ten times lower than a correct fade, and a correct strong fade higher than a cut.
And where the network has lost a texture, its picture (14 to 15 dB) is as close to the truth as a blend of
the two frames (15.2 dB) and closer than a held frame (13.9 dB): holding would not have been better by the
only measure there is here. No threshold on this number does more good than harm, so the rule was not built.

## Measured: what Fusion could save

`fusion_report` follows every tensor of the model from its input (previous frame, current frame, time) and
counts the multiply-adds of the convolutions (run 44608578177):

| | Lite model (Fast, Balanced) | Full model (Quality) |
|---|---|---|
| Work that does not depend on the time | 28.4 % | 13.6 % |
| Work that depends on the time (done for every generated frame, whatever is reused) | 71.6 % | 86.4 % |

The part that does not depend on the time is the feature encoder, which takes the two frames as a batch of
two. That it treats them separately was measured and not assumed: with one frame replaced, the other
frame's half of every such tensor stayed identical (difference 0.0), and so did all of it at another time.
So each real frame's features could be made once and used for the pair before, the pair after and every
in-between time. The most that saves, in convolution work:

| | 2× | 3× | 4× |
|---|---|---|---|
| Lite model | 14.2 % | 21.3 % | 23.6 % |
| Full model | 6.8 % | 10.2 % | 11.4 % |

This is a ceiling in arithmetic, not in milliseconds: the samplings and resizes between the convolutions are
not counted, and on a graphics card the cost of a second session and of handing the features across is not
either. Nothing else in the network can be reused between in-between times: the time enters at the first
motion block, before any motion is worked out. The guide's "work a pair's motion out once" does not exist in
these models. So Fusion here means one thing, the encoder split, and its ceiling is 7 to 24 %. It is not
built: closing PushFlow's one model session safely already needed its own fix after a crash in the field
(3.2.2), and a second session for a gain that may be a few per cent in practice has to be measured on real
cards first.

## Measured: a frame ahead, from what the network already gives

Predict needs motion that points forward in time. Two ways to get it from the shipped models were tried
(`predict_report`, runs 44605491581 and 44606025232; a 320 × 192 picture, the frame after the newest one
forecast from the two before it and compared with the real one):

- **Asking the network for time 1.** It gives back the newest frame (64 to 80 dB), but its sampling grid is
  not the motion between the two frames: for a square moving 8 to 48 px to the right it reads −5.7 to
  −20.6 px. A forecast built on it is no better than holding the newest frame. A dead end.
- **From the half-way grids.** The grids of the ordinary in-between frame say where each of its points lies
  in the newest frame; carried one more half-step along the same line, they give a frame half an interval
  ahead with no extra run of the network:

  | The pair | Hold the newest frame | Forecast, Fast model | Forecast, Quality model |
  |---|---|---|---|
  | a square, 8 px a frame | 27.9 dB | 44.7 dB | 51.7 dB |
  | a square, 16 px | 24.8 dB | 35.4 dB | 48.3 dB |
  | a square, 32 px | 30.1 dB | 31.3 dB | 30.3 dB |
  | a square, 48 px | 25.4 dB | 27.7 dB | 27.1 dB |
  | a fine texture, 4 px | 13.9 dB | 19.7 dB | 36.6 dB |
  | a fine texture, 8 px | 13.9 dB | 21.2 dB | 17.7 dB |
  | a fine texture, 16 px | 13.9 dB | 15.6 dB | 27.6 dB |

So a forecast is possible, cheaply, for slow and steady motion, and falls back to about a held frame when the
motion is fast, which is exactly when a wrong forecast would show most. That is a proof that the tensors
allow it, on drawn pictures moving in straight lines. It is not a feature: a game's motion changes between
frames, a forecast frame has to be checked against the real frame that follows (Guard), it needs its own way
onto the screen and its own pacing, and whether it lowers the delay a player feels can only be shown with a
camera. Not built.

## Predict as built

**What changes on screen.** Ordinary frame generation shows a pair's in-between frames and then its real
frame, so the real frame waits about one frame interval. With Predict a batch is the real frame first and
its forecasts after it (`pushflow::predict::pts`: the real frame's time plus a share of the last interval),
and the engine gives the pair no frame interval of delay, only its own processing time
(`EngineController.cpp`, `predictPair`). At 30 FPS that is up to 33 ms less between the console and the
screen on paper (the delay it replaces is one frame interval, or what the engine had learned was enough).
On paper only: nobody has filmed it.

**How a forecast is made** (`shaders/PushPredict.hlsl`; the rule for one pixel is `predict::forecast` in
`include/veyra/pushflow/PushFlowPredict.h`, and the graphics test compares the two pixel by pixel). The
network runs **once** for the pair, for the half-way time, whatever the multiplier; every forecast is the
newest real frame moved along the motion its grids give. A pixel is moved only as far as the motion there
is trusted: the search for where it came from must settle, the motion to the older frame must be the
opposite of the motion to the newer one, the two real frames must show the same thing where the motion
joins them, and what it reads must lie inside the picture. Where it is not trusted the newest real frame
is left as it is. Less than a quarter of a pixel is not moved at all, so a still picture and a HUD stay
exact. Nothing is invented: every pixel is a pixel of the newest real frame.

**Measured before it was built** (`predict_report`, runs 44645655282 and 44646229341, 512 x 320, the rule
as built; dB against the true later frame, higher is nearer):

| Half an interval ahead | Holding the real frame | Fast model | Quality model |
|---|---|---|---|
| A square moving 2 / 8 / 16 px a frame | 33.9 / 27.8 / 24.8 | 46.2 / 42.9 / 27.0 | 52.0 / 48.3 / 36.6 |
| A square moving 32 / 48 / 96 px a frame | 30.1 / 25.4 / 26.0 | 30.2 / 25.7 / 26.0 | 30.3 / 26.0 / 26.0 |
| Fine texture panning 4 / 8 / 16 / 32 px a frame | 13.9 | 18.4 / 20.1 / 15.2 / 17.3 | 29.1 / 17.1 / 26.0 / 21.2 |
| A still picture | exact | exact | exact (83 dB or better) |
| Two unrelated pictures (a cut), where holding is exact | exact | 43.9 | 38.9 |

So on these pictures a forecast is never worse than holding on a moving picture, clearly better for slow
motion, and no better for fast motion, where most of the object is held. On the software renderer's small
test picture (256 x 160) the Quality model's forecast of a slow square was less than half as far from the
truth as holding (0.48 against 1.05 of 255), and the Fast tier's model, which follows a small object on so
small a picture poorly, was level with holding (1.07 against 1.05, with a third of the moving picture
held); through the real graph at 640 x 360 the Fast tier's forecasts were 1.4 to 6 dB nearer than holding. Three things were measured and dropped: a limit
on speed (it only took gains away: a fast pan is forecast well), the same checks without the one that the
two frames agree (a cut then left a visibly warped frame, 26 to 36 dB), and asking the network for time 1
(its grid there is not the motion between the frames).

**What it cannot do.** The next frame is not known. When something changes direction, appears or
disappears between two real frames, the forecast shows what would have happened had it carried on, until
the next real frame corrects it: a moving object can overshoot and snap back. A cut is seen one frame
late, as it is by any frame generation. At 60 FPS and 2X a forecast must be ready 8 ms after its real
frame, so a card that needs longer for PushFlow shows no forecasts at all (the row counts them).

**The engine's side**, all of it behind the switch: a pair's delay; its admission against the first
forecast's time; a forecast is dropped, never shown, once the next real frame is due (counted); the
learned pair delay is not fed from Predict's pairs; it steps aside, without anything being rebuilt, while
the quality test or *Optimise for my PC* measures in-between frames; a file, an export and a still image
never use it.

**What is checked**: the rule on its own (`veyra_push_flow_predict_tests`); on the software renderer with
both models, the shader against the rule, a still picture exact, a slow square nearer to the truth, an
in-between frame still exactly PushFlow's own (`testPushFlow`); through the real graph at 3X, the batch's
order and times and that no forecast is worse than holding (`veyra_push_flow_graph_tests`); and through
the **whole engine with a live source** (`veyra_push_predict_engine_tests`: a Frame Input channel the test
feeds, played by the engine as a capture card's picture is). There, on the software renderer, 24 forecasts
reached the screen for 34 real frames, nothing failed or stalled, and a real frame was on screen 3.8 ms
after it arrived, where in-between frames at 2X must hold it at least half an interval. The source runs at
1.1 frames a second, because that renderer needs a third of a second for one run of the network. **What
is not checked anywhere: a real graphics card and a real capture card.** The delay saved in milliseconds,
the evenness of the frames and whether forecasts arrive in time at 60 FPS are unknown.

## Dual, the first step as designed

Written down so that it is built the same way whoever builds it; nothing of it is in the program yet.

**What it is for.** With Predict on, the screen shows `real B, forecast, real C, forecast …`, and the
recorder takes what is shown, so a recording or a stream contains the forecasts with their overshoots.
A viewer has no use for a lower delay; a viewer wants the true frames.

**What is already there.** For a pair A, B Predict runs the network once, for the half-way time. That run
also gives the true in-between frame of A and B (it is what PushFlow shows without Predict), for the price
of `resolve()`, with no second run. So at 2X the creator's timeline `in-between(A,B), B, in-between(B,C),
C …` costs almost nothing beside the player's.

**How, without a second recorder path.** The recorder takes exactly one frame per presented frame, from
the present's own command list (`PresentCapture::record`), and that contract stays. Only *which texture*
it is given changes, one interval behind the screen:

| The screen shows | The recorder is given |
|---|---|
| real B (pair A, B) | real A |
| forecast after B | the true in-between frame of A and B |
| real C (pair B, C) | real B |

Every texture named is still held by the graph at that moment (A is the pair's older frame; the in-between
frame has its own slot). A forecast that is dropped drops its in-between frame from the recording, as a
dropped generated frame is missing today. After a break in the source the recorder is given the real frame
itself.

**What it needs before it is built**: the presenter given the texture to hand on (`VideoPresenter::present`
builds `PresentCapture::Frame` from the texture it shows); a slot in the graph for the in-between frame and
its HUD and exclusion compose from the older frame; that the read is ordered against the graph's next write
when the presenter has its own queue (the lease's fence covers the shown texture only: the first version
runs with the shared queue only, and says so otherwise); the recorded sound, which then leads the picture
by one interval (33 ms at 30 FPS), delayed by as much; comparison clips, the quality test and the HUD
sampler keep taking what is shown. 3X and 4X need in-between frames for times the half-way run does not
give: as played until a rebuild for any time from the half-way grids is measured.

**What it is not**: the audit's full Dual (a second timeline with its own clock, queue, rate and method,
OBS output). That needs a second run of the method per pair and a say over the graphics card's time
between the two, which cannot be judged without real cards.

## Guard's rule, found by measuring: changed, and shown nowhere

Three more rules were run on the shipped models before any shader was written (`guard_report`, runs
44631423182, 44632325354 and 44633268004). The first two failed in ways worth keeping:

- **"The frames differ here and the network found no motion near."** It never touched a correct picture,
  and it never acted either: where the network loses an object it still invents a few pixels of motion
  around it (5 to 17 px), and that counts as motion near.
- **"Something one frame shows is used nowhere in the generated frame", counted by the mask's weights.**
  It restored the left-out square, and it replaced 61 to 97 % of a slowly sliding texture with the earlier
  frame (34 dB down to 16 dB). The network often takes one frame only; the other frame is then "unused"
  everywhere, and nothing is wrong.

What holds on every drawn pair: a place in a real frame counts as **shown** by a generated pixel that reads
it when the pixel takes it, *or when the pixel's other sample is the same picture*. A tile of a frame is
unused when less than a quarter of it is shown anywhere. Risky = the tile changed between the two frames
(its own change of brightness taken out, so a fade is not a change), it is unused in one of them, and it is
not as near the picture's edge as the picture's motion reaches (a pan pushes those out, unused and right).

| | Fast model | Quality model |
|---|---|---|
| Correct pictures touched (squares moved 2 to 96 px, fades, a still picture, a square over a texture) | 0 of 9 | 0 of 10 |
| A square left out at 160 and 256 px: how far the old place is from the earlier frame (0.32 = background, 0 = the square) | 0.32 → 0.14 | 0.32 → 0.07 |
| A square moved 200 px over a texture | 0.36 → 0.12 | 0.28 → 0.17 |
| A fine texture the network had already lost (14 to 26 dB): how much is replaced | 4 to 48 % | 0 to 55 % |
| Two unrelated pictures | 33 % | 70 % |

So the rule acts where an object was left out and leaves correct pictures alone, on drawn pictures. What it
does where the network is already lost (hold part of the picture) can look blocky, which is why a frame
with much of it risky is held whole. Whether any of this looks better in a game is not known: a held object
steps at the game's own rate, which the usual error measure scores *worse* than a missing one. It is being
built as an experiment that starts off, for people with real games and cards to judge.

## Guard as built

One compute pass after the generated picture is made (`shaders/PushGuard.hlsl`, four stages), only when the
switch is on; with it off nothing of it is created or run. The decision is `decide` in
`include/veyra/pushflow/PushFlowGuard.h`, the same rule in a form a test can run:

1. per tile of 16 x 16 motion pixels, how much its picture changed between the two real frames;
2. per motion pixel, how much of the place it reads in each real frame it shows (what it takes of it, or
   all of it when its other sample is the same picture), added up per tile of each real frame;
3. the decision: changed, less than a quarter shown in one of the frames, and not in the edge band. A risky
   tile takes its changed neighbours with it so an object is not cut in half. When more than 35 % of the
   tiles are risky the frame is the nearer real frame whole;
4. the picture: the generated frame, with the nearer real frame blended in over risky tiles.

What it reports, live and in the performance report: frames judged, frames mended in places, frames held
whole. A held frame is a repeated real frame and is counted as that, never as a generated one that
succeeded.

What is checked: the decision on its own (`veyra_push_flow_guard_tests`); on the software renderer with both
models, that a still picture and a followed object come out exactly as without Guard, and that it never
moves a place further from the real frame (`testPushFlow`); through the real graph at 3X, that every
generated frame is the plain run's and nothing is mended or held (`veyra_push_flow_graph_tests`).

The rule as built differs from the one first measured in two small ways (the edge band comes from the
largest motion a tile has on average, and risky tiles take changed neighbours), so it was measured again as
built (`guard_as_built`, run 44643980334, 512 x 320 pictures):

| | Fast model | Quality model |
|---|---|---|
| Right pictures (30 dB or better) it touches | none | none |
| A square left out (128 to 256 px): tiles replaced | 1.4 to 2.8 % | 1.4 to 2.8 % |
| A fine texture the model had already lost, sliding 4 to 64 px: tiles replaced | 19 to 38 % | 1 to 50 % |
| The same, 128 px, and two unrelated pictures: tiles replaced (above 35 % the frame is held whole) | 68 to 69 % | 83 to 84 % |

Two things in it must be said plainly. A held object scores **worse** by the usual error measure than a
missing one (29 dB down to 26 dB for the left-out square): the measure prefers an empty background at the
right time to the object at the wrong time, and only eyes can say which looks better. And on a fine texture
the model has lost, Guard holds a fifth to a half of the picture in pieces while the rest is the model's
blur (20.6 dB down to 17.8 dB, where a held frame is 13.9 dB): that is where it can look blocky, and it is
the first thing to look at in a real game.

What is not known: how it looks in a real game, what it costs on a real card (four small dispatches and one
full-size one per generated frame), and whether its thresholds, found on drawn pictures, are right for game
pictures. Those need:

- **clips**: short recordings of real games at 60 FPS (fast turns, small fast objects, HUDs, cuts, fades).
  Every second frame is withheld, generated, and compared with the frame that was really there, which is what
  the in-app frame quality test already does for one session;
- **eyes**: the same clips with and without the rule, unlabelled, judged by people. A held region scores
  worse than a blend by the usual error measure even when it looks better, so numbers alone cannot decide.

## What each blocked step waits for

| Waits for | Steps |
|---|---|
| Real 60 FPS game clips, and people to judge them | Whether Guard should ever be on by default, and its thresholds; any automatic choice of the motion size (the choice by hand exists since 3.2.7) |
| Frame quality test results from real games at each motion size (the worst place, with the performance report, which names the motion size) | Whether a tier's own motion size should change |
| Card tests and performance reports from real cards (*Settings → Test my graphics card*, *Performance report*) | Fusion (the encoder split must be faster at equal quality on two classes of card, for a ceiling of 7 to 24 %); half precision per card |
| A camera filming screen and controller (240 FPS or more) | Every statement about delay: what Predict really saves, Response, latency modes |
| A PS5 with Remote Play, and testers | Response |
| Real recording, streaming and OBS sessions on several cards | Dual |
| Licence terms for runtimes and for any new model's weights | An NVIDIA-native path; any learned refinement |

## How the guide's first steps map to this repository

| The guide's step | Here |
|---|---|
| Freeze the baseline and list the protected features | `tests/protected-features.json`, checked on every push; the baseline is the 3.2.6 release and its CI run |
| A settings namespace with hidden, default-off flags | `PushFlow2.h`, preference `pushFlow2`, safe mode (`--safe-mode`); a switch is shown only for a part that is built |
| Frame identity, clocks and reset rules | Already in the engine (`FramePacket.h`, `FrameBatch.h`, the reset coordinator). Nothing was added; new metadata comes with the part that reads it |
| Leases, queues and teardown | Unchanged. The roadmap's "fixed pool of buffers" step covers it, and it ships after it is measured |
| Instruments and a test corpus | The motion report (models), `PushFlowBench` and the card test (speed), the frame quality test (a live session). The corpus is the clips above |
| Read-only motion data with tested coordinates | `PushFlowMotion.h`, `PushFlowBackend::motionBuffer` (tests only), `veyra_push_flow_motion_tests` |
| Guard's risk map and safe fallback | Built as an experiment that starts off (above), after four rules that failed in measurement |
| Encode once, reuse | Built as an experiment that starts off: the encoder cache (above) |
| Frames from the past only | Built as an experiment that starts off: Predict (above) |

# PushFlow

PushFlow is PushFPS's own frame generation. It runs **RIFE** (Real-Time Intermediate Flow Estimation, an open
neural network by hzwer, MIT licence) through **ONNX Runtime on DirectML**, inside PushFPS's own Direct3D 12
graph, so it works on NVIDIA, AMD and Intel graphics cards.

## How it works

1. **Pack.** A compute shader writes the two real frames and the time of the new frame into the network's
   input buffer on the GPU. A pair is packed once; at 3X and 4X the later frames only change the time plane.
2. **Network.** ONNX Runtime runs the RIFE model on DirectML, on PushFPS's own GPU queue, reading and writing
   buffers PushFPS allocated once (no CPU copies of pictures).
3. **Rebuild.** The models also output RIFE's final **warp grids** and **blend mask**
   (`scripts/pushflow/rife_graph.py` adds them to the vs-mlrt export). In the Fast tier the network runs at half
   size and `shaders/PushFlowReconstruct.hlsl` enlarges the *motion*, not the picture: every output pixel is
   sampled from the **full-resolution real frames** where the motion points. At full size the network's own
   picture is used (the rebuild reproduces it exactly: 99 dB on the CPU check).
4. **Protect.** HUD Lock and the HUD boxes are composed on top, as for every method; scene cuts reset the pair
   so PushFlow never blends across a cut.

| Tier | Model | Motion estimated at | Picture |
|---|---|---|---|
| Fast | RIFE 4.25 lite | up to 960×540 (at most half size) | rebuilt at full size from the full-size frames |
| Balanced | RIFE 4.25 lite | up to 1280×720 | rebuilt at full size when smaller, else network output |
| Quality | RIFE 4.26 | up to 1920×1080 | rebuilt at full size when smaller, else network output |

Since 3.1.6 no tier runs the network above 1080p: at 4K every tier ran RIFE at 4K (or 1080p for Fast) and
was too slow for live play, so the admission control skipped every pair and PushFlow stayed at the game's
frame rate (field report, 17 November 2026). A 4K picture now uses 960×540 / 1280×720 / 1920×1080 motion and
the full-resolution rebuild.

### Choosing the motion size (3.2.7)

*Professional → PushFlow motion size*: **Auto** (the table above, and what every version before had), or
**540p**, **720p** or **1080p**, used whatever the tier. The tier still chooses the model; the picture is
rebuilt at full size from the real frames either way, so sharpness does not change. A picture is never
enlarged: on a 1080p picture, 1080p means the network runs at the picture's own size.

Why it is a choice and not a new rule: on drawn pictures the models do not behave the same at every size, and
not in one direction ([measured](PUSHFLOW2.md#measured-what-the-models-do-when-motion-is-too-fast)). A smaller
size costs the card less and sometimes follows a fast object further; a larger one is more exact for small
motion. Which suits a game can only be measured on that game: change the size, run *Frame generation quality
test*, and compare **Worst place**, which is where a left-out object shows.

- The row says the size the network really runs at while PushFlow plays, and the performance report carries
  it (`pushFlowMotionSize`).
- Changing it rebuilds PushFlow like changing the tier does (a short pause in generated frames).
- If the card cannot keep up, the watch that lowers the tier first puts a chosen size that is larger than the
  tier's own back to Auto, and says so. Every step it takes is a cheaper one.
- Code: `pushflow::motion::networkScale`, `setMotionHeight`, `slowStep` in
  `include/veyra/pushflow/PushFlowMotion.h`; preference `pushFlowMotion`; `PushFlowMotionRow.qml`. It is not
  part of the settings a preset stores. Unverified on real hardware: it was built and tested on the software
  renderer, where the Fast tier at a chosen size gives the same picture as the Balanced tier at that size.

### Precision

Every model ships in full precision (fp32, the reference) and, where it agrees on the CPU, in half precision
(fp16 convolutions; warps, resizes, sigmoid and blend kept in fp32). Until 3.1.6 PushFlow compared both on
every start, inside PushFPS. Since 3.1.7 fp32 is the default: on Windows' software renderer the fp16 model
removed the D3D12 device, and a crash report from an RTX 5090 ended inside ONNX Runtime right after the check.

Since 3.2.0 half precision is tested **in a separate process**: *Professional → PushFlow half precision →
Test* runs `PushFlowBench.exe --fp16-probe` on the graphics card PushFPS renders on (Balanced, 1280×720). It
times fp32, then the fp16 model with PushFlow's own comparison, and passes only when fp16 matches (38 dB or
better) and is at least 10% faster. If the driver or DirectML faults, that process ends and PushFPS keeps
running. A passed test turns the switch on (preference `pushFlowHalf`, result in `pushFlowHalfProbe`); PushFlow
then still compares both at start and stays on fp32 if they do not match. `VEYRA_PUSHFLOW_PRECISION=fp16`
forces the half model for developers.

### The motion contract

What the network gives besides its picture, in one place: `include/veyra/pushflow/PushFlowMotion.h`.

- **Two grids.** For every pixel of the new frame, where to read the previous real frame and where to read the
  current one. They are sampling positions for this one moment, not the motion from one real frame to the
  next, and dividing them by the time does not make them that.
- **A mask.** How much of each of the two readings goes into the pixel. It is a blend weight, not a measure of
  how sure the network is: it is just as decided where the network is wrong
  ([measured](PUSHFLOW2.md#measured-what-the-models-do-when-motion-is-too-fast)).
- The header also holds a reference of the rebuild; the shader is checked against it pixel by pixel on the
  software renderer, from the network's own grids and mask (`testPushFlow`).

**Motion that is too fast.** An object that moves further between two real frames than the network can follow
is left out of the generated frame (it is not shown twice). On drawn pictures that happens somewhere above
a fifth of the picture's width per frame, and working the motion out at a smaller size sometimes brings the
object back (the motion size can be chosen since 3.2.7, above). Since 3.2.8 an experiment that starts off,
**Guard** (*Professional → PushFlow 2: Guard*), shows the nearer real frame at such a place, so the object
steps at the game's rate instead of blinking out; it is checked on drawn pictures only. The frame quality
test shows the fault since 3.2.7 as its **worst place**; [PushFlow 2](PUSHFLOW2.md) has the measurements,
the rule Guard uses, and what is still not known.

## Why PushFlow could stay at the game's frame rate

**The cause none of the work below found (fixed in 3.2.2).** Until 3.2.2 PushFlow never generated a frame
inside PushFPS. The engine's graph asks `EnhanceGraph::fgCreated()` whether a frame generation method exists
before it offers a pair of frames to one, and that function listed every method except PushFlow. With
PushFlow selected the graph offered nothing (`fgCandidates` = 0), no error was raised, and the picture stayed
at the source's frame rate. PichotM found it on an RTX 4090 (PS5 Remote Play) and fixed it with one line;
with the fix he measured 60 → about 120 FPS.

Every PushFlow test at the time drove `PushFlowBackend` directly, so they all passed, and the causes in the
table below were found by reading code rather than by watching PushFlow fail in the program. They are real
and worth having, but none of them could matter while the graph never asked for a frame.
`tests/integration/PushFlowGraphTests.cpp` now runs PushFlow through the graph in every build (a moving
square at 2X and 3X: every pair offered, every generated frame back and between its neighbours).

PushFlow never refuses to run: the frame budget (`FgRecoveryBudget`) skips every pair whose generated frames
would arrive too late, so a PushFlow that is too slow, or only looks too slow, shows as "stuck at 30 FPS"
with no error. 3.2.0 removed every cause found in the code:

| Cause | Fix |
|---|---|
| Every inference waited on the CPU: Run synchronised the providers, then `SynchronizeBoundOutputs`. The engine thread stood still for the whole network once per generated frame, and the GPU queue ran dry between the pack, network and resolve lists; those gaps counted as frame generation cost | Queued inference: Run returns once DirectML has recorded the network onto PushFPS's own queue (`disable_synchronize_execution_providers`); the resolve list follows on the same queue. A full wait every 120 inferences lets DirectML release finished work. `VEYRA_PUSHFLOW_SYNC=1` restores the waits |
| Dynamic input shapes: DirectML cannot fuse or pre-plan a graph whose size is unknown | Free dimensions fixed per session (`AddFreeDimensionOverrideByName`, names written into the models by `prepare_runtime.py`). `VEYRA_PUSHFLOW_DYNAMIC=1` keeps them dynamic |
| DirectML prepares the network on its first runs; the first live inference carried that time into the budget | Two warm-up runs at start, while the graph is built |
| A skipped pair marked PushFlow's history lost: the next pair generated nothing, and the evaluation after it was a lone probe on cold clocks that measured slow and started the next skip | PushFlow keeps its previous frame across skips (it reads the graph's own real frames) and its first evaluations after a skip are settling samples, as for NVIDIA VFG (`FgCostSettling`) |
| A tier the card really cannot sustain | The too-slow watch: after eight seconds in which the budget skipped nearly every pair, PushFlow moves one tier down and says why; on Fast it says what else would help |
| The network ran at 4K for 4K pictures | 3.1.6: motion estimated at most at 1080p, picture rebuilt at full size |
| ONNX Runtime's spinning CPU thread pool | One intra-op thread, no spinning |

The network's time per frame is now its GPU span on the queue (timestamps after the pack and before the
resolve), shown live in the quick setup and in the log every 30 seconds.

What CI measured for these changes, on Windows' software renderer (WARP, 640×360, build run 44202258035,
18 November 2026; the times are a CPU pretending to be a graphics card, so only the comparisons mean anything):

| | Fast | Balanced | Quality |
|---|---|---|---|
| Time the engine thread spends in the inference call, waiting (3.1.7) | 1573 ms | 3706 ms | 4903 ms |
| The same, queued (3.2.0) | 2.4 ms | 2.5 ms | 3.1 ms |
| The same, queued, dynamic shapes | 4.5 ms | 4.6 ms | 5.9 ms |
| Network span, static shapes | 1572 ms | 3697 ms | 4894 ms |
| Network span, dynamic shapes | 1616 ms | 3798 ms | 5017 ms |

- With static shapes ONNX Runtime no longer prints "Some nodes were not assigned to the preferred execution
  providers": every node runs on DirectML. With dynamic shapes it prints it for every session.
- All three tiers now start one after the other in one process. With the automatic fp16 self-check (3.1.6)
  the second tier failed with a removed device.
- The half precision probe ran in its own process and ended with a result: on WARP fp16 does not match
  (27.7 dB), so it stays off there.
- Queued inference does not pile up memory: 160 inferences with no full wait at all left the process's
  committed memory within 1 MB of where it was (139.0 MB after 40 and 138.9 MB at the end in run
  44213229154; 138.4 MB and 139.1 MB in run 44218469467; `PushFlowBench --leak-check`). The full wait every
  120 inferences stays as a margin for drivers WARP cannot stand in for.
- Queued inference relies on ONNX Runtime's DirectML provider submitting its command list before the run
  returns, which the pinned version (1.20) does: in queued mode the test pictures are correct and the
  network's GPU span lies between the pack and the resolve. A newer ONNX Runtime has to pass the same
  tests before it is pinned.

**Still not measured:** any of this on a real graphics card. WARP shows the new paths are correct and how
much CPU time they give back; it cannot show the network's speed on an RTX, Radeon or Arc card.

### What PushFlow does not do (yet), and why

A comparison with SmoovPlay's RIFE path (November 2026) listed TensorRT, CUDA Graphs, engine caching and
feature reuse between pairs as its advantages. Where PushFlow stands:

- **TensorRT, CUDA Graphs, engine caching**: NVIDIA only, and TensorRT's runtime is over a gigabyte that
  PushFPS may not bundle. PushFlow's equivalents on every vendor are the fixed-size session (DirectML
  compiles the whole network once, when PushFlow starts) and queued inference. A TensorRT backend cannot be
  written responsibly without an NVIDIA card to test it on; it stays on the roadmap (PF-009/010).
- **Feature reuse between pairs**: RIFE's encoder runs on both frames of a pair, and the newer frame's
  features could be kept for the next pair. It needs the network split into two models and checked to
  reproduce the original exactly. Not done; the encoder is a small part of the network (an estimate from
  its layer sizes, not a measurement).
- **Asynchronous inference**: done in 3.2.0 (queued inference, above).

## What was measured, and where

On the CPU, when the release is built (`scripts/pushflow/prepare_runtime.py`, numbers from the 2026-11-17 run):

| Check | Fast (4.25 lite) | Quality (4.26) |
|---|---|---|
| Rebuild from grids + mask vs the model's own picture | 99 dB (exact) | 99 dB (exact) |
| Moving square at t = 0.5 (vs 20 dB for a blend) | 38.8 dB | 55.5 dB |
| Fine texture, network at half size: old enlarged picture | 17.2 dB | 17.3 dB |
| Fine texture, network at half size: full-size rebuild | **25.0 dB** | **30.7 dB** |
| fp16 copy vs fp32 (CPU) | 73 dB | 75 dB |

On WARP (Windows' software renderer, `tests/integration/PushGpuTests.cpp`): the whole path through DirectML,
identical frames, the moving square, t = 0.25, fine texture in motion and the Fast tier's rebuild.

One finding from WARP: on per-pixel noise panning 8 pixels at 640×360, the Fast tier's rebuild scores 22.1 dB
against 15.2 dB for a blend, but the Quality model run at full size does not track that pattern (14.6 dB). Real
pictures are not per-pixel noise, and the moving-square check passes at 51.6 dB, but it is on the list: tests
with real game and video footage, and whether the Quality tier should also estimate motion at a lower scale.

**Not measured yet:** speed on real NVIDIA, AMD and Intel cards. PushFlowBench (below) exists so these
numbers come from real hardware, not estimates.

## Shutting a session down (3.2.2)

A crash file from the field (17 November 2026, version 3.1.5, Xbox streaming) ends inside ONNX Runtime while
PushFlow is shut down for a live settings rebuild. Read against the pinned ONNX Runtime 1.20.1:

- When a session is released, the DirectML provider lets go of its pool of GPU buffers first and only then
  releases the objects still waiting in its queue of GPU work. A pooled buffer among them hands itself back to
  a pool that was destroyed a moment earlier. The crash file shows exactly that: the pool's memory already torn
  down when the buffer arrives.
- Whether such a buffer is still queued depends on when the session ends. Queued inference (3.2.0) does not
  make the provider release its queue after each run, so a session that ends between two full waits can still
  hold some.

What PushFlow does since 3.2.2, for every source:

- before a session is released it asks the provider to finish and release its queue
  (`SynchronizeBoundOutputs`), while the pool is still there;
- it holds the pool itself, through an allocator handle, until the session is gone (`Status::poolHeld`).

Since 3.1.7 the fault was already caught rather than ending PushFPS, at the price of leaving that session's
memory behind.

**What the tests show, and what they do not.** `PushFlowBench --teardown-check N` starts PushFlow, runs a few
queued pairs (or none) and shuts it down at once, N times in four session kinds. In CI on WARP (run of
2026-11-18, 6 cycles):

| Teardown | Sessions | Faults caught | Pool held |
|---|---|---|---|
| 3.2.2 | 24 | 0 | yes |
| As 3.2.0 (`VEYRA_PUSHFLOW_TEST_UNGUARDED=1`) | 24 | 0 | no |

So the check did **not** reproduce the crash: the old teardown ran clean on WARP too. It shows that the new
teardown works and that the pinned runtime hands out its pool the way PushFlow relies on (the build fails if
a later pin stops doing so). That the change removes the crash rests on the crash file and on reading the
runtime, not on a reproduction, and nothing was run on a real graphics card.

One reading that was wrong, kept here so it is not made again: the provider also has a mode in which one
execution context is stored on the D3D12 device and shared by every session. That mode is only for ONNX
Runtime's Python API. Through the C API PushFlow uses, every session has a context of its own (checked in the
runtime and in CI: the device carries no such context).

## Measure your card

`PushFlowBench.exe` is next to `PushFPS.exe`. Run it from a command prompt in the PushFPS folder:

```
PushFlowBench.exe --sizes 1920x1080,2560x1440 --tiers 0,1,2 --pairs 40
```

It writes `pushflow-bench.json`: per size and tier, the precision your card passed, GPU time of the pack, the
network and the rebuild (D3D12 timestamps), CPU time of the inference call, and the wall time of a whole pair
at 2X, 3X and 4X. Share it in a GitHub issue to help build the hardware table. `--compare-shapes` adds a run
with dynamic shapes and `--compare-sync` one that waits after every inference (the behaviour before 3.2.0).

## Roadmap status

The engineering roadmap (17 November 2026) lists 31 items. Where PushFlow stands after 3.2.2:

| Item | Status |
|---|---|
| PF-001 measurement | Done in part: inference p50/p95/max ring in the log and live stats, PushFlowBench with GPU timestamps. Per-frame epoch/deadline tagging and ETW export: open |
| PF-002 model contract | Done in part: models.json (source, hashes, opset, padding, align corners, mask order, precision, CPU checks), pinned archives and packages. Wider test corpus (HDR ramps, odd sizes, cuts): open |
| PF-003 hardware gates | Open: needs RTX 30/40/50, RDNA2/3/4 and Arc results (PushFlowBench) |
| PF-004 split encoder / feature cache | Open: needs a split export of the network |
| PF-005 combined warps | The vs-mlrt export already warps both frames in one batched GridSample (found while exposing the grids) |
| PF-006 memory traffic | Done in part: per-pair packing (time plane only for later frames), preallocated bound buffers |
| PF-007 mixed precision | Done: fp16 convolutions with fp32 warps/blend; tested per card in a separate process since 3.2.0, fp32 fallback. Hardware reports: open |
| PF-008 buffer pool | Buffers are preallocated at start-up; multi-slot rings for concurrency: open |
| PF-009/010 TensorRT, CUDA Graphs | Open (optional NVIDIA plugin; not bundled) |
| PF-011 asynchronous inference | Done (3.2.0): queued on the graph's own queue, ordered by the queue, a full wait every 120 inferences |
| PF-012 full-resolution Fast tier | Done: grids + mask rebuild at full size |
| PF-013 multiple frames per pair | Done in part: the pair is packed once; the network still runs once per frame |
| PF-014 automatic selection | The optimiser tests PushFlow with the other methods; the too-slow watch lowers the tier live (3.2.0); half precision per card through the isolated probe (3.2.0) |
| PF-017 cut handling | Scene cuts and cadence breaks reset the pair: no generated frame across a cut |
| Others (presenter thread, latency policies, GPU-only quality probe, NLE, …) | Open |

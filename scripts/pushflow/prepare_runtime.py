#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Prepares PushFlow's runtime for a release build (CI only; nothing it downloads is committed).

  1. ONNX Runtime (DirectML build) and DirectML from nuget.org, pinned versions; the SHA-256 of each
     package is printed and checked against PINS when a value is set there.
  2. RIFE models (MIT weights by hzwer, ONNX by vs-mlrt) from vs-mlrt's "external-models" release: the
     pinned archives in MODEL_ARCHIVES; the newest lite model becomes pushflow-fast.onnx and the newest
     full model (no ensemble) pushflow-quality.onnx.
  3. Each model is checked on the CPU with a moving test pattern (identical frames come back, a moving
     square lands in the middle, well ahead of a plain blend) and shipped in full precision.

Writes:
  <out>/ort, <out>/dml               the extracted packages (headers for CMake)
  <out>/runtime/pushflow/            onnxruntime.dll, DirectML.dll, models/*.onnx, models.json, licences
Usage: python -I scripts/pushflow/prepare_runtime.py --out D:\\deps\\pushflow
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ORT_ID, ORT_VERSION = "Microsoft.ML.OnnxRuntime.DirectML", "1.20.1"
DML_ID, DML_VERSION = "Microsoft.AI.DirectML", "1.15.4"
MODELS_REPO, MODELS_TAG = "AmusementClub/vs-mlrt", "external-models"
# The two RIFE archives PushFlow ships (chosen by the checks below on the first build, 2026-11-17):
# v4.25 lite for Fast/Balanced, v4.26 for Quality. Change both the list and the pins together.
MODEL_ARCHIVES = ["rife_v4.25_lite.7z", "rife_v4.26.7z"]
# SHA-256 of the packages and model archives; a missing entry is printed only.
PINS: dict[str, str | None] = {
    "Microsoft.ML.OnnxRuntime.DirectML.1.20.1.nupkg": "6763468507b7cfc777b1334b3e174c11a540ddacb7bd4354bc2e0ec89e56eec2",
    "Microsoft.AI.DirectML.1.15.4.nupkg": "4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9",
    "rife_v4.25_lite.7z": "7d53e29fff5e67345b19f4ce97dfd1e34b490eedc752df2236904fda1a13842c",
    "rife_v4.26.7z": "dfdabd84a2a3db773f87604b8cc255e94a6a72f13550d910ccd3b4ee2606cd4f",
}


MIT = """MIT License

Copyright (c) {year}{holder}

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def pinned(name: str, path: Path) -> None:
    digest = sha256(path)
    print(f"sha256 {digest}  {name}")
    want = PINS.get(name)
    if want and want != digest:
        sys.exit(f"::error::{name}: SHA-256 {digest} does not match the pinned {want}")


def nuget(package: str, version: str, work: Path, out: Path) -> Path:
    url = f"https://api.nuget.org/v3-flatcontainer/{package.lower()}/{version}/{package.lower()}.{version}.nupkg"
    target = work / f"{package}.{version}.nupkg"
    print(f"download {url}")
    urllib.request.urlretrieve(url, target)
    pinned(target.name, target)
    folder = out / ("ort" if package == ORT_ID else "dml")
    shutil.rmtree(folder, ignore_errors=True)
    with zipfile.ZipFile(target) as z:
        for name in z.namelist():
            if ".." in Path(name).parts or name.startswith("/"):
                sys.exit(f"::error::unsafe entry {name} in {target.name}")
        z.extractall(folder)
    return folder


def seven_zip() -> str:
    for candidate in (shutil.which("7z"), r"C:\Program Files\7-Zip\7z.exe"):
        if candidate and Path(candidate).exists():
            return candidate
    sys.exit("::error::7-Zip not found")


def model_archives(work: Path) -> list[Path]:
    listing = subprocess.run(["gh", "release", "view", MODELS_TAG, "-R", MODELS_REPO, "--json", "assets"],
                             check=True, capture_output=True, text=True).stdout
    assets = [a["name"] for a in json.loads(listing)["assets"]]
    rife = sorted(a for a in assets if re.search(r"rife", a, re.I) and a.endswith((".7z", ".zip")))
    print("RIFE archives in the release:", ", ".join(rife) or "(none)")
    missing = [a for a in MODEL_ARCHIVES if a not in assets]
    if missing:
        sys.exit(f"::error::vs-mlrt external-models no longer has {', '.join(missing)}")
    rife = MODEL_ARCHIVES
    out = []
    for name in rife:
        subprocess.run(["gh", "release", "download", MODELS_TAG, "-R", MODELS_REPO, "-p", name, "-D", str(work), "--clobber"], check=True)
        pinned(name, work / name)
        out.append(work / name)
    return out


def version_of(path: Path):
    m = re.search(r"rife_?v?(\d+)\.(\d+)", path.name, re.I)
    return (int(m.group(1)), int(m.group(2))) if m else None


def describe(model_path: Path):
    import onnx
    m = onnx.load(str(model_path), load_external_data=False)
    inputs = [i for i in m.graph.input if i.name not in {init.name for init in m.graph.initializer}]
    if len(inputs) != 1 or len(m.graph.output) < 1:
        return None
    dims = inputs[0].type.tensor_type.shape.dim
    out_dims = m.graph.output[0].type.tensor_type.shape.dim
    if len(dims) != 4 or len(out_dims) != 4:
        return None
    channels = dims[1].dim_value
    fixed = [d.dim_value for d in dims[2:]]
    if channels not in (7, 11) or out_dims[1].dim_value not in (0, 3) or any(fixed):
        return None
    return channels


def name_free_dims(model):
    """Gives every free input dimension a name, so PushFPS can fix it per session
    (AddFreeDimensionOverrideByName): DirectML only folds the model's shape arithmetic and fuses
    the graph when the input size is known at session creation. Returns the dimension names."""
    inputs = [i for i in model.graph.input if i.name not in {init.name for init in model.graph.initializer}]
    names = []
    for k, d in enumerate(inputs[0].type.tensor_type.shape.dim):
        if not d.dim_value and not d.dim_param:
            d.dim_param = ("pushflow_n", "pushflow_c", "pushflow_h", "pushflow_w")[k]
        names.append(d.dim_param or str(d.dim_value))
    return names


def frame(w, h, square_x):
    import numpy as np
    y, x = np.mgrid[0:h, 0:w]
    base = (60 + 40.0 * x / w + 30.0 * y / h) / 255.0
    img = np.stack([base, base + 10 / 255.0, base + 20 / 255.0])
    dx, dy = x - square_x, y - h * 0.35
    inside = (dx >= 0) & (dx < 40) & (dy >= 0) & (dy < 40)
    light = ((dx.astype(int) // 8 + dy.astype(int) // 8) % 2) == 0
    for c, (lo, hi) in enumerate(((40, 235), (30, 200), (20, 60))):
        img[c][inside] = np.where(light[inside], hi, lo) / 255.0
    return img.astype(np.float32)


def tensor(a, b, t, channels):
    import numpy as np
    _, h, w = a.shape
    planes = [a, b, np.full((1, h, w), t, np.float32)]
    if channels == 11:
        y, x = np.mgrid[0:h, 0:w].astype(np.float32)
        planes += [np.full((1, h, w), 2.0 / (w - 1), np.float32), np.full((1, h, w), 2.0 / (h - 1), np.float32),
                   (2.0 * x / (w - 1) - 1.0)[None], (2.0 * y / (h - 1) - 1.0)[None]]
    return np.concatenate(planes)[None]


def psnr(a, b):
    import numpy as np
    mse = float(np.mean((np.clip(a, 0, 1) - np.clip(b, 0, 1)) ** 2))
    return 99.0 if mse < 1e-12 else 10 * np.log10(1.0 / mse)


def check_model(path: Path, channels: int) -> dict:
    import onnxruntime as ort
    w, h = 256, 192
    session = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
    name = session.get_inputs()[0].name
    first, second, middle = frame(w, h, 100), frame(w, h, 116), frame(w, h, 108)
    same = session.run(None, {name: tensor(first, first, 0.5, channels)})[0][0]
    mid = session.run(None, {name: tensor(first, second, 0.5, channels)})[0][0]
    blend = (first + second) / 2
    return {"identical_db": round(psnr(same, first), 2), "moving_db": round(psnr(mid, middle), 2),
            "blend_db": round(psnr(blend, middle), 2), "output": mid}


def detail_frame(w, h, shift):
    """Fine, non-repeating texture (hashed noise, 1-pixel lines) panning sideways: what the Fast tier must keep.
    Integer shifts keep the ground truth exact; the pattern never repeats, so the motion is unambiguous."""
    import numpy as np
    y, x = np.mgrid[0:h, 0:w].astype(np.int64)
    u = x - int(shift)
    hsh = (u * 73856093) ^ (y * 19349663)
    noise = ((hsh ^ (hsh >> 13)) * 1274126177 & 0xFFFF) / 65535.0
    lines = ((u % 37) == 0) | ((y % 29) == 0)
    tex = 0.25 + 0.5 * noise
    tex = np.where(lines, 0.95, tex)
    img = np.stack([tex, tex * 0.9 + 0.05, tex * 0.8 + 0.1])
    return np.clip(img, 0, 1).astype(np.float32)


def resize_box(img, f):
    import numpy as np
    c, h, w = img.shape
    return img.reshape(c, h // f, f, w // f, f).mean(axis=(2, 4)).astype(np.float32)


def bilinear_at(field, qy, qx):
    """field [C, h, w] sampled at continuous pixel coordinates (pixel centres at integers), clamped."""
    import numpy as np
    c, h, w = field.shape
    qx = np.clip(qx, 0, w - 1); qy = np.clip(qy, 0, h - 1)
    x0 = np.floor(qx).astype(np.int64); y0 = np.floor(qy).astype(np.int64)
    x1 = np.minimum(x0 + 1, w - 1); y1 = np.minimum(y0 + 1, h - 1)
    fx = qx - x0; fy = qy - y0
    a = field[:, y0, x0] * (1 - fx) + field[:, y0, x1] * fx
    b = field[:, y1, x0] * (1 - fx) + field[:, y1, x1] * fx
    return a * (1 - fy) + b * fy


def rebuild_scaled(img0, img1, grid0, grid1, mask, align, mask_for_first, model_hw):
    """What shaders/PushFlowReconstruct.hlsl does: grids and mask from the model size, frames at full size."""
    import numpy as np
    import rife_graph
    _, H, W = img0.shape
    mh, mw = model_hw
    ph, pw = rife_graph.to_hw2(grid0).shape[:2]
    yo, xo = np.mgrid[0:H, 0:W].astype(np.float64)
    qy = (yo + 0.5) * mh / H - 0.5; qx = (xo + 0.5) * mw / W - 0.5
    out = []
    for img, grid in ((img0, grid0), (img1, grid1)):
        g = np.moveaxis(rife_graph.to_hw2(grid), -1, 0)
        gx, gy = bilinear_at(g, qy, qx)
        if align:
            xm = (gx + 1) * 0.5 * (pw - 1); ym = (gy + 1) * 0.5 * (ph - 1)
        else:
            xm = ((gx + 1) * pw - 1) * 0.5; ym = ((gy + 1) * ph - 1) * 0.5
        xs = (xm + 0.5) * W / mw - 0.5; ys = (ym + 0.5) * H / mh - 0.5
        out.append(bilinear_at(img, ys, xs))
    m = bilinear_at(np.asarray(mask)[0], qy, qx)[0]
    return out[0] * m + out[1] * (1 - m) if mask_for_first else out[0] * (1 - m) + out[1] * m


def motion_report(path: Path, channels: int, align: int, mask_for_first: bool) -> list[dict]:
    """How this model's grids and mask behave when it can and cannot follow motion (CPU, a report in the log).

    A square moves further and further between two frames; then two unrelated pictures and a fade. For
    each pair, at the middle time:
      model_db            the model's picture against the true in-between frame;
      undecided_mask      share of the picture where the mask keeps both samples (0.2..0.8);
      mixture             share where the two samples differ AND the mask keeps both: a see-through
                          double image (min(w, 1 - w) * difference above 0.03);
      at the square's old place, its true middle place and its new place: the mean error against the
      truth, the mean mask, how far the two samples were read from (pixels), and how far the two
      readings are from mirroring each other (linear motion reads the two frames in opposite
      directions; |d0 + d1| is 0 then).
    Nothing is decided by it: it is here so that a rule built on these tensors is measured on the
    shipped models before it is believed (docs/PUSHFLOW2.md has the first reading)."""
    import numpy as np
    import onnxruntime as ort
    import rife_graph
    session = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
    name = session.get_inputs()[0].name
    w, h = 512, 320
    yo, xo = np.mgrid[0:h, 0:w].astype(np.float64)
    rows = []

    def study(label, img0, img1, truth, places=None):
        _, g0, g1, m = session.run(None, {name: tensor(img0, img1, 0.5, channels)})[:4]
        ph, pw = rife_graph.to_hw2(g0).shape[:2]
        samples, reads = [], []
        for img, grid in ((img0, g0), (img1, g1)):
            g = np.moveaxis(rife_graph.to_hw2(grid), -1, 0)
            gx, gy = bilinear_at(g, yo, xo)
            if align:
                xm = (gx + 1) * 0.5 * (pw - 1); ym = (gy + 1) * 0.5 * (ph - 1)
            else:
                xm = ((gx + 1) * pw - 1) * 0.5; ym = ((gy + 1) * ph - 1) * 0.5
            samples.append(np.clip(bilinear_at(img, ym, xm), 0, 1))
            reads.append((xm - xo, ym - yo))
        a, b = samples
        mask = np.clip(bilinear_at(np.asarray(m)[0], yo, xo)[0], 0, 1)
        w0 = mask if mask_for_first else 1 - mask
        apart = np.abs(a - b).max(axis=0)
        picture = a * w0 + b * (1 - w0)
        far0 = np.hypot(*reads[0]); far1 = np.hypot(*reads[1])
        mirror = np.hypot(reads[0][0] + reads[1][0], reads[0][1] + reads[1][1])
        row = {"pair": label, "model_db": None if truth is None else round(psnr(picture, truth), 2),
               "hold_db": None if truth is None else round(psnr(img0, truth), 2), "blend_db": None if truth is None else round(psnr((img0 + img1) / 2, truth), 2),
               "apart_mean": round(float(apart.mean()), 4), "apart_over_10_percent": round(100.0 * float((apart > 0.10).mean()), 2),
               "undecided_mask_percent": round(100.0 * float(((w0 > 0.2) & (w0 < 0.8)).mean()), 2),
               "mixture_percent": round(100.0 * float((np.minimum(w0, 1 - w0) * apart > 0.03).mean()), 2),
               "read_from_px_p95": round(float(np.percentile(np.maximum(far0, far1), 95)), 1), "not_mirrored_px_p95": round(float(np.percentile(mirror, 95)), 1)}
        for place, left in (places or {}).items():
            box = (slice(int(h * 0.35), int(h * 0.35) + 40), slice(int(left), int(left) + 40))
            row[place] = {"error": None if truth is None else round(float(np.abs(picture - truth)[:, box[0], box[1]].mean()), 3), "mask": round(float(w0[box].mean()), 2),
                          "read_from_px": round(float(np.maximum(far0, far1)[box].mean()), 1), "not_mirrored_px": round(float(mirror[box].mean()), 1),
                          "samples_apart": round(float(apart[box].mean()), 3)}
        rows.append(row)

    for move in (16, 48, 96, 160, 256):
        study(f"square moved {move} px", frame(w, h, 100), frame(w, h, 100 + move), frame(w, h, 100 + move / 2),
              {"old_place": 100, "middle_place": 100 + move / 2, "new_place": 100 + move})
    for pan in (8, 32, 64, 128, 256):
        study(f"fine texture panned {pan} px", detail_frame(w, h, 0), detail_frame(w, h, pan), detail_frame(w, h, pan // 2))
    study("two unrelated pictures", frame(w, h, 100), detail_frame(w, h, 0), None)
    dim = (frame(w, h, 100) * 0.6).astype(np.float32)
    for step in (0.07, 0.3):
        bright = np.clip(dim + step, 0, 1).astype(np.float32)
        study(f"a fade ({step} brighter, nothing moves)", dim, bright, ((dim + bright) / 2).astype(np.float32))
    # The same too-fast motion with the network at half and at a quarter of the picture's size, the
    # picture rebuilt at full size from the full-size frames (what the tiers do): is the reach a number
    # of the network's own pixels? (512 x 384, so that every size is a multiple of the model's block.)
    W2, H2 = 512, 384
    sized = [(f"square moved {move} px, 512 x 384", frame(W2, H2, 100), frame(W2, H2, 100 + move), frame(W2, H2, 100 + move / 2), move) for move in (16, 48, 96, 160, 256)]
    sized += [(f"fine texture panned {pan} px, 512 x 384", detail_frame(W2, H2, 0), detail_frame(W2, H2, pan), detail_frame(W2, H2, pan // 2), 0) for pan in (8, 16, 32, 64)]
    for label_, f0, f1, truth, move in sized:
        box = (slice(int(H2 * 0.35), int(H2 * 0.35) + 40), slice(int(100 + move / 2), int(100 + move / 2) + 40))
        row = {"pair": label_}
        for label, factor in (("full", 1), ("half", 2), ("quarter", 4)):
            try:
                s0, s1 = (f0, f1) if factor == 1 else (resize_box(f0, factor), resize_box(f1, factor))
                _, g0, g1, m = session.run(None, {name: tensor(s0, s1, 0.5, channels)})[:4]
                picture = np.clip(rebuild_scaled(f0, f1, g0, g1, m, align, mask_for_first, (H2 // factor, W2 // factor)), 0, 1)
                row[f"{label}_size_db"] = round(psnr(picture, truth), 2)
                row[f"{label}_size_middle_error"] = round(float(np.abs(picture - truth)[:, box[0], box[1]].mean()), 3)
            except Exception as e:  # noqa: BLE001 - a size this model does not take
                row[f"{label}_size_db"] = f"not run: {e}"[:80]
        rows.append(row)
    return rows


GUARD_ROWS = False   # True: the study prints every row, not only its verdicts


def guard_tiles(img0, img1, reads, w0, samples, tile=16, change=0.05, unused=0.25, agree=0.08, edge=1.5):
    """PushFlow 2's Guard at tile size: where was something in one of the two real frames left out of the
    generated frame?

    reads: for each of the two frames, where every pixel of the generated frame reads it (x, y in pixels);
    w0: how much of the generated pixel is the first frame's sample; samples: the two samples themselves.

      CHANGED  what the tile shows differs between the two frames once its own change of brightness is
               taken out (a fade is not a change).
      SHOWN    a place in a real frame is shown by a generated pixel that reads it when that pixel takes
               it (its weight), or when the pixel's other sample is the same picture (within `agree`):
               the network often takes one frame only, and what it shows of the other is then the same
               thing. A tile of a frame is UNUSED when less than `unused` of it is shown anywhere.
      EDGE     tiles as near the picture's edge as the picture's motion reaches (`edge` times its 95th
               percentile, plus one tile) are left alone: a pan pushes them out of the picture, unused and right.

    Changed and unused, away from the edge = something moved that the network did not follow, and the
    generated frame shows it nowhere. Returns (risky, changed, unused_either), [tiles down, tiles across]."""
    import numpy as np
    h, w = w0.shape
    th, tw = h // tile, w // tile
    def tiles(x):
        return x[:th * tile, :tw * tile].reshape(th, tile, tw, tile)
    l0, l1 = tiles(img0.mean(axis=0)), tiles(img1.mean(axis=0))
    d = np.abs((l0 - l0.mean(axis=(1, 3), keepdims=True)) - (l1 - l1.mean(axis=(1, 3), keepdims=True))).mean(axis=(1, 3))
    changed = d > change
    same = 1.0 - np.clip(np.abs(samples[0] - samples[1]).max(axis=0) / agree, 0.0, 1.0)
    yo, xo = np.mgrid[0:h, 0:w]
    used, far = [], []
    for (xm, ym), weight in zip(reads, (w0, 1.0 - w0)):
        tx, ty = np.floor(xm / tile).astype(np.int64), np.floor(ym / tile).astype(np.int64)
        inside = (tx >= 0) & (tx < tw) & (ty >= 0) & (ty < th)
        count = np.zeros((th, tw))
        np.add.at(count, (ty[inside], tx[inside]), np.maximum(weight, same)[inside])
        used.append(count / float(tile * tile))
        far.append(np.hypot(xm - xo, ym - yo))
    unused_either = np.minimum(used[0], used[1]) < unused
    reach = int(np.ceil(edge * float(np.percentile(np.maximum(far[0], far[1]), 95)) / tile)) + 1
    inner = np.zeros((th, tw), bool)
    if th > 2 * reach and tw > 2 * reach:
        inner[reach:th - reach, reach:tw - reach] = True
    return changed & unused_either & inner, changed, unused_either


def guard_as_built(img0, img1, reads, w0, samples, tile=16, change=0.05, unused=0.25, agree=0.08, edge=1.5):
    """The rule as shaders/PushGuard.hlsl and pushflow/PushFlowGuard.h run it, where it differs from
    guard_tiles: the edge band comes from the largest motion a tile has on average (not a percentile over
    pixels), and a risky tile takes its changed neighbours with it. Returns the risky tiles and how many
    tiles lie inside the edge band (the ones the rule can call risky at all)."""
    import numpy as np
    h, w = w0.shape
    th, tw = h // tile, w // tile
    def tiles(x):
        return x[:th * tile, :tw * tile].reshape(th, tile, tw, tile)
    l0, l1 = tiles(img0.mean(axis=0)), tiles(img1.mean(axis=0))
    changed = np.abs((l0 - l0.mean(axis=(1, 3), keepdims=True)) - (l1 - l1.mean(axis=(1, 3), keepdims=True))).mean(axis=(1, 3)) > change
    same = 1.0 - np.clip(np.abs(samples[0] - samples[1]).max(axis=0) / agree, 0.0, 1.0)
    yo, xo = np.mgrid[0:h, 0:w]
    used, far = [], []
    for (xm, ym), weight in zip(reads, (w0, 1.0 - w0)):
        tx, ty = np.floor(xm / tile).astype(np.int64), np.floor(ym / tile).astype(np.int64)
        inside = (tx >= 0) & (tx < tw) & (ty >= 0) & (ty < th)
        count = np.zeros((th, tw))
        np.add.at(count, (ty[inside], tx[inside]), np.maximum(weight, same)[inside])
        used.append(count / float(tile * tile))
        far.append(np.hypot(xm - xo, ym - yo))
    furthest = float(tiles(np.minimum(np.maximum(far[0], far[1]), 255.0)).mean(axis=(1, 3)).max())
    reach = int(np.ceil(edge * furthest / tile)) + 1
    inner = np.zeros((th, tw), bool)
    if th > 2 * reach and tw > 2 * reach:
        inner[reach:th - reach, reach:tw - reach] = True
    first = changed & (np.minimum(used[0], used[1]) < unused) & inner
    grown = np.zeros_like(first)
    padded = np.pad(first, 1)
    for dy in range(3):
        for dx in range(3):
            grown |= padded[dy:dy + th, dx:dx + tw]
    return first | (grown & changed), int(inner.sum()), reach


def guard_report(path: Path, channels: int, align: int, mask_for_first: bool) -> list[dict]:
    """Guard's rule on the shipped model, before anything is built on it (docs/PUSHFLOW2.md): for each drawn
    pair, how much of the picture the rule would replace with the nearer real frame, and what that does.
    It must act where an object was left out (the object is then shown where the nearer frame has it,
    instead of nowhere) and must leave followed motion, slow motion, pans and fades alone."""
    import numpy as np
    import onnxruntime as ort
    import rife_graph
    session = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
    name = session.get_inputs()[0].name
    w, h, tile = 512, 320, 16
    yo, xo = np.mgrid[0:h, 0:w].astype(np.float64)
    rows = []
    as_built = []

    def study(label, img0, img1, truth, move=None, **rule):
        _, g0, g1, m = session.run(None, {name: tensor(img0, img1, 0.5, channels)})[:4]
        ph, pw = rife_graph.to_hw2(g0).shape[:2]
        samples, reads = [], []
        for img, grid in ((img0, g0), (img1, g1)):
            g = np.moveaxis(rife_graph.to_hw2(grid), -1, 0)
            gx, gy = bilinear_at(g, yo, xo)
            if align:
                xm = (gx + 1) * 0.5 * (pw - 1); ym = (gy + 1) * 0.5 * (ph - 1)
            else:
                xm = ((gx + 1) * pw - 1) * 0.5; ym = ((gy + 1) * ph - 1) * 0.5
            samples.append(np.clip(bilinear_at(img, ym, xm), 0, 1))
            reads.append((xm, ym))
        mask = np.clip(bilinear_at(np.asarray(m)[0], yo, xo)[0], 0, 1)
        w0 = mask if mask_for_first else 1 - mask
        picture = samples[0] * w0 + samples[1] * (1 - w0)
        risky, changed, unused = guard_tiles(img0, img1, reads, w0, samples, tile, **rule)
        if not rule:
            # The same pair through the rule as it is built, for the whole-frame decision: how many tiles
            # it calls risky, of all tiles and of those inside the edge band.
            final, inner, reach = guard_as_built(img0, img1, reads, w0, samples, tile)
            mended = np.zeros((h, w), bool)
            patch = np.kron(final, np.ones((tile, tile), bool))
            mended[:patch.shape[0], :patch.shape[1]] = patch
            as_built.append({"pair": label, "reach": reach, "of_all": round(100.0 * float(final.mean()), 1),
                             "of_inside": round(100.0 * float(final.sum()) / inner, 1) if inner else None,
                             "db": None if truth is None else [round(psnr(picture, truth), 1), round(psnr(np.where(mended[None], img0, picture), truth), 1),
                                                               round(psnr(img0, truth), 1)]})
        full = np.zeros((h, w), bool)
        area = np.kron(risky, np.ones((tile, tile), bool))
        full[:area.shape[0], :area.shape[1]] = area
        guarded = np.where(full[None], img0, picture)        # the middle time: the earlier frame is as near as the later
        row = {"pair": label, "rule": rule or "default", "changed": round(100.0 * float(changed.mean()), 2), "unused": round(100.0 * float((changed & unused).mean()), 2),
               "risky": round(100.0 * float(risky.mean()), 2), "replaced": round(100.0 * float((np.abs(guarded - picture).max(axis=0) > 0.02).mean()), 2)}
        if truth is not None:
            row["db"] = [round(psnr(picture, truth), 2), round(psnr(guarded, truth), 2)]
        if move is not None:
            def shown(image, reference, left):
                box = (slice(int(h * 0.35), int(h * 0.35) + 40), slice(int(left), int(left) + 40))
                return round(float(np.abs(image - reference)[:, box[0], box[1]].mean()), 3)
            # 0 = the object is shown there as the earlier frame has it; about 0.3 = background where it was.
            row["old_place_vs_earlier_frame"] = [shown(picture, img0, 100), shown(guarded, img0, 100)]
            row["middle_error"] = [shown(picture, truth, 100 + move / 2), shown(guarded, truth, 100 + move / 2)]
        rows.append(row)

    def over(square_x, pan):
        base = detail_frame(w, h, pan).copy()
        sq = frame(w, h, square_x)
        top, left = int(h * 0.35), int(square_x)
        base[:, top:top + 40, left:left + 40] = sq[:, top:top + 40, left:left + 40]
        return base
    def cases(**rule):
        for move in (2, 4, 8, 16, 48, 96, 128, 160, 256):
            study(f"square moved {move} px", frame(w, h, 100), frame(w, h, 100 + move), frame(w, h, 100 + move / 2), move, **rule)
        for pan in (2, 4, 8, 32, 64, 128):
            study(f"fine texture panned {pan} px", detail_frame(w, h, 0), detail_frame(w, h, pan), detail_frame(w, h, pan // 2), None, **rule)
        study("two unrelated pictures", frame(w, h, 100), detail_frame(w, h, 0), None, None, **rule)
        dim = (frame(w, h, 100) * 0.6).astype(np.float32)
        for step in (0.07, 0.3):
            bright = np.clip(dim + step, 0, 1).astype(np.float32)
            study(f"a fade ({step} brighter, nothing moves)", dim, bright, ((dim + bright) / 2).astype(np.float32), None, **rule)
        study("nothing changes", frame(w, h, 100), frame(w, h, 100), frame(w, h, 100), 0, **rule)
        study("square moved 48 px over a texture panned 8 px", over(100, 0), over(148, 8), over(124, 4), 48, **rule)
        study("square moved 8 px over a still texture", over(100, 0), over(108, 0), over(104, 0), 8, **rule)
        study("square moved 200 px over a still texture", over(100, 0), over(300, 0), over(200, 0), 200, **rule)
    cases()
    cases(agree=0.04)
    cases(agree=0.15, unused=0.15)
    # The verdict, so that the log can be read without the rows: what the rule broke, and what it mended.
    verdicts = []
    for rule in ("default", {"agree": 0.04}, {"agree": 0.15, "unused": 0.15}):
        mine = [r for r in rows if r["rule"] == rule]
        fine = [r for r in mine if "db" in r and r["db"][0] >= 30.0]                       # the model had it right
        harmed = [f"{r['pair']}: {r['replaced']}%" for r in fine if r["replaced"] > 0.1]
        lost = [r for r in mine if "old_place_vs_earlier_frame" in r and r["middle_error"][0] > 0.2]   # the object was left out
        mended = [f"{r['pair']}: old place {r['old_place_vs_earlier_frame'][0]} -> {r['old_place_vs_earlier_frame'][1]}" for r in lost]
        other = [f"{r['pair']}: {r['replaced']}%" for r in mine if r not in fine and r not in lost and r["replaced"] > 0.1]
        verdicts.append({"verdict_for_rule": rule, "right_pictures": len(fine), "harmed": harmed, "left_out": mended, "replaced_where_the_model_was_lost": other})
    # As built: [pair, edge band in tiles, risky % of all tiles, risky % of the tiles inside the band,
    # dB of the generated frame / mended in places / the earlier real frame whole]. Right pictures that
    # the built rule touches are listed apart: there must be none.
    right = [r for r in as_built if r["db"] and r["db"][0] >= 30.0]
    verdicts.append({"as_built_touches_right_pictures": [r["pair"] for r in right if r["of_all"] > 0],
                     "as_built": [[r["pair"], r["reach"], r["of_all"], r["of_inside"], r["db"]] for r in as_built if r not in right or r["of_all"] > 0]})
    return verdicts if not GUARD_ROWS else rows + verdicts


def fusion_report(path: Path, channels: int) -> dict:
    """Fusion, step 1, from the graph alone: how much of the network's work depends on ONE real frame only
    (it could be done once per frame and reused for the pair before, the pair after and every in-between
    time), how much on the pair but not on the time, and how much on the time (done for every generated
    frame whatever is reused). Work = multiply-accumulates of the convolutions at 1024 x 576.

    Every tensor is followed from the model's one input: channels 0-2 are the previous frame (A), 3-5 the
    current frame (B), 6 the time (T). vs-mlrt's exports stack the two frames on the batch axis for the
    parts that treat them alike; such a tensor still holds two separate frames ("stacked")."""
    import numpy as np
    import onnx
    from onnx import numpy_helper
    model = onnx.load(str(path))
    g = model.graph
    # Shapes from a real run (the export's sizes are free, so the graph alone does not give them): every
    # convolution's input and output is added as an output of a copy and the copy run once at 256 x 192.
    import onnxruntime as ort
    from onnx import helper
    probe = onnx.load(str(path))
    wanted = []
    for n in probe.graph.node:
        if n.op_type in ("Conv", "ConvTranspose"):
            wanted += [n.input[0], n.output[0]]
    have = {o.name for o in probe.graph.output}
    wanted = [w for w in dict.fromkeys(wanted) if w not in have and w != probe.graph.input[0].name]
    probe.graph.output.extend(helper.make_empty_tensor_value_info(w) for w in wanted)
    session = ort.InferenceSession(probe.SerializeToString(), providers=["CPUExecutionProvider"])
    z = np.zeros((3, 192, 256), np.float32)
    results = session.run(None, {session.get_inputs()[0].name: tensor(z, z, 0.5, channels)})
    shapes = {o.name: list(r.shape) for o, r in zip(session.get_outputs(), results)}
    shapes[g.input[0].name] = [1, channels, 192, 256]
    del results, session, probe
    init = {i.name: i for i in g.initializer}
    consts = {}
    for n in g.node:
        if n.op_type == "Constant":
            for a in n.attribute:
                if a.name == "value":
                    consts[n.output[0]] = numpy_helper.to_array(a.t)
    def value(name):
        if name in consts:
            return consts[name]
        if name in init and int(np.prod(init[name].dims or [1])) <= 16:
            return numpy_helper.to_array(init[name])
        return None
    def channels_to_set(lo, hi):
        out = set()
        for c in range(max(lo, 0), min(hi, channels)):
            out.add("A" if c < 3 else "B" if c < 6 else "T" if c == 6 else "K")
        out.discard("K")
        return frozenset(out)
    source = g.input[0].name
    whole = {source}   # the input itself, or the input padded / cast: its channels still mean A, B, T
    dep = {}        # tensor -> (frozenset of A/B/T, stacked)
    macs = {"one frame": 0, "the pair, not the time": 0, "the time": 0, "unknown": 0}
    features = []
    def kind(d):
        if d is None:
            return "unknown"
        s, stacked = d
        if "T" in s:
            return "the time"
        if stacked or len(s) <= 1:
            return "one frame"
        return "the pair, not the time"
    for n in g.node:
        ins = [i for i in n.input if i]
        live = [i for i in ins if i in whole or i in dep]
        out = None
        # A tensor's shape is not its content: what is worked out from shapes alone depends on no frame.
        if not live or n.op_type == "Shape":
            for o in n.output:
                consts.setdefault(o, None)
            continue
        attrs = {a.name: a for a in n.attribute}
        if n.op_type in ("Pad", "Identity", "Cast") and ins[0] in whole and not any(i in dep for i in ins[1:]):
            whole.update(n.output)
            continue
        if any(i in whole for i in live):
            if n.op_type == "Slice" and len(ins) >= 4 and value(ins[1]) is not None and value(ins[2]) is not None and value(ins[3]) is not None and int(np.ravel(value(ins[3]))[0]) == 1:
                out = (channels_to_set(int(np.ravel(value(ins[1]))[0]), int(min(np.ravel(value(ins[2]))[0], channels))), False)
            elif n.op_type == "Split" and int(attrs["axis"].i if "axis" in attrs else 0) == 1:
                sizes = np.ravel(value(ins[1])) if len(ins) > 1 and value(ins[1]) is not None else None
                at = 0
                for k, o in enumerate(n.output):
                    size = int(sizes[k]) if sizes is not None else channels // len(n.output)
                    dep[o] = (channels_to_set(at, at + size), False); at += size
                continue
            else:
                out = (frozenset("ABT"), False)
        else:
            sets = [dep[i] for i in live]
            union = frozenset().union(*[s for s, _ in sets])
            axis = int(attrs["axis"].i) if "axis" in attrs else None
            if n.op_type == "Concat" and axis == 0 and len(sets) == 2 and all(len(s) == 1 and not st for s, st in sets) and union == frozenset("AB"):
                out = (union, True)
            elif all(st for _, st in sets):
                if n.op_type in ("Slice", "Split", "Gather") and (axis == 0 or (n.op_type == "Slice" and len(ins) >= 4 and value(ins[3]) is not None and int(np.ravel(value(ins[3]))[0]) == 0)):
                    if n.op_type == "Split":
                        for k, o in enumerate(n.output):
                            dep[o] = (frozenset("A" if k == 0 else "B"), False)
                        continue
                    index = value(ins[1])
                    out = (frozenset("A" if index is not None and int(np.ravel(index)[0]) == 0 else "B"), False) if index is not None else (union, True)
                else:
                    out = (union, True)
            else:
                out = (union, False)
        for o in n.output:
            dep[o] = out
        if n.op_type in ("Conv", "ConvTranspose") and len(ins) > 1:
            wdims = list(init[ins[1]].dims) if ins[1] in init else list(consts[ins[1]].shape) if consts.get(ins[1]) is not None else None
            ref = shapes.get(n.output[0]) if n.op_type == "Conv" else shapes.get(ins[0])
            if ref and wdims:
                macs[kind(out)] += int(np.prod(ref)) * int(np.prod(wdims[1:]))
            else:
                macs["unknown"] += 1
    # What a later step reads from the one-frame part: the tensors to keep per real frame.
    consumers = {}
    for n in g.node:
        for i in n.input:
            consumers.setdefault(i, []).append(n)
    for name, d in dep.items():
        if d is not None and kind(d) == "one frame" and any(kind(dep.get(o)) != "one frame" for c in consumers.get(name, []) for o in c.output):
            features.append({"tensor": name[:40], "of": "".join(sorted(d[0])) + ("/stacked" if d[1] else ""), "shape": shapes.get(name)})
    total = sum(v for k, v in macs.items() if k != "unknown") or 1
    F, P, T = macs["one frame"], macs["the pair, not the time"], macs["the time"]
    # The part that does not depend on the time works on the two frames as a batch of two. Measured, not
    # assumed: with frame A kept and frame B replaced, A's half of every such tensor must stay what it
    # was (and the same for B, and for another time). Then that work belongs to one frame and could be
    # done once per real frame instead of twice per generated frame.
    per_frame = {"tensors": 0}
    try:
        pair_tensors = [n.output[0] for n in g.node if n.op_type in ("Conv", "ConvTranspose") and n.output and
                        dep.get(n.output[0]) is not None and kind(dep[n.output[0]]) == "the pair, not the time"]
        probe = onnx.load(str(path))
        have = {o.name for o in probe.graph.output}
        probe.graph.output.extend(helper.make_empty_tensor_value_info(w) for w in pair_tensors if w not in have)
        session = ort.InferenceSession(probe.SerializeToString(), providers=["CPUExecutionProvider"])
        names = [o.name for o in session.get_outputs()]
        rng = np.random.default_rng(7)
        A, B, C = (rng.random((3, 192, 256), dtype=np.float32) for _ in range(3))
        def run(a, b, t):
            return dict(zip(names, session.run(None, {session.get_inputs()[0].name: tensor(a, b, t, channels)})))
        ab, ac, cb, ab2 = run(A, B, 0.5), run(A, C, 0.5), run(C, B, 0.5), run(A, B, 0.25)
        first = second = other_time = changed = 0.0
        batched = 0
        for w in pair_tensors:
            if ab[w].shape[0] != 2:
                continue
            batched += 1
            first = max(first, float(np.abs(ab[w][0] - ac[w][0]).max()))
            second = max(second, float(np.abs(ab[w][1] - cb[w][1]).max()))
            other_time = max(other_time, float(np.abs(ab[w] - ab2[w]).max()))
            changed = max(changed, float(np.abs(ab[w][1] - ac[w][1]).max()))   # the half that did change, for scale
        per_frame = {"tensors": len(pair_tensors), "as_a_batch_of_two": batched,
                     "first_half_changes_with_the_other_frame_by": round(first, 7), "second_half_changes_with_the_other_frame_by": round(second, 7),
                     "changes_with_the_time_by": round(other_time, 7), "the_replaced_half_changes_by": round(changed, 4),
                     "verified": bool(batched == len(pair_tensors) and batched > 0 and max(first, second, other_time) <= 1e-5 and changed > 1e-2)}
        del session, probe, ab, ac, cb, ab2
    except Exception as error:  # a study: say why, never stop the build
        per_frame["error"] = str(error)[:200]
    if per_frame.get("verified"):
        per_frame["saving_of_convolution_work_percent"] = {
            f"{m}x": round(100.0 * (1 - (P / 2 + (m - 1) * T) / ((m - 1) * (P + T) or 1)), 1) for m in (2, 3, 4)}
    saving = {}
    for m in (2, 3, 4):
        now = (m - 1) * (F + P + T) or 1
        saving[f"{m}x_encode_once_percent"] = round(100.0 * (1 - (F / 2 + (m - 1) * (P + T)) / now), 1)
        saving[f"{m}x_and_pair_once_percent"] = round(100.0 * (1 - (F / 2 + P + (m - 1) * T) / now), 1)
    data = [n for n in g.node if n.output and (n.output[0] in dep or n.output[0] in whole)]
    head = [f"{n.op_type}({','.join(i[-14:] for i in n.input if i and (i in dep or i in whole))})->{n.output[0][-14:]}:{'input' if n.output[0] in whole else ''.join(sorted(dep[n.output[0]][0])) + ('*' if dep[n.output[0]][1] else '')}" for n in data[:44]]
    return {"work_share_percent": {k: round(100.0 * v / total, 1) for k, v in macs.items() if k != "unknown"}, "convolutions_without_a_shape": macs["unknown"],
            "encoder_per_frame": per_frame,
            "saving_of_convolution_work": saving, "kept_per_frame": features[:8], "nodes": len(g.node),
            "ops": dict(collections.Counter(n.op_type for n in g.node).most_common(14)), "first_nodes": head}


def predict_report(path: Path, channels: int, align: int) -> list[dict]:
    """Predict, as a proof of concept on the CPU and nothing more: can a frame half an interval AFTER the
    newest real frame be forecast from the two real frames that have arrived, better than holding the
    newest frame? The network is asked for time 1 (the newest frame itself): its first grid then says, for
    every pixel of the newest frame, where that pixel was in the frame before. Moving every pixel on by
    half of that gives the forecast. No frame from the future is used; the true later frame only scores it."""
    import numpy as np
    import onnxruntime as ort
    import rife_graph
    session = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
    name = session.get_inputs()[0].name
    w, h = 512, 320
    yo, xo = np.mgrid[0:h, 0:w].astype(np.float64)
    rows = []
    def forecast(label, f0, f1, truth, box=None, moved=0):
        rgb, g0 = session.run(None, {name: tensor(f0, f1, 1.0, channels)})[:2]
        ph, pw = rife_graph.to_hw2(g0).shape[:2]
        g = np.moveaxis(rife_graph.to_hw2(g0), -1, 0)
        gx, gy = bilinear_at(g, yo, xo)
        if align:
            xm = (gx + 1) * 0.5 * (pw - 1); ym = (gy + 1) * 0.5 * (ph - 1)
        else:
            xm = ((gx + 1) * pw - 1) * 0.5; ym = ((gy + 1) * ph - 1) * 0.5
        dx, dy = xm - xo, ym - yo                   # back to where it was one interval ago
        ahead = np.clip(bilinear_at(f1, yo + 0.5 * dy, xo + 0.5 * dx), 0, 1)
        row = {"pair": label, "forecast_db": round(psnr(ahead, truth), 2), "hold_db": round(psnr(f1, truth), 2),
               "time_1_gives_the_newest_frame_db": round(psnr(rgb[0], f1), 2)}
        # The second way: the half-way grids, which do follow the motion (motion_report). A pixel of the
        # half-way frame reads the newest frame d further on, so it moves 2 d a frame. The pixel that
        # reaches y half an interval after the newest frame left the half-way frame at x = y - 2 d(x)
        # (found by stepping twice), and in the newest frame it is at x + d(x). This costs no extra run
        # of the network: the grids are the ones the last generated frame was made from.
        _, _, h1, _ = session.run(None, {name: tensor(f0, f1, 0.5, channels)})[:4]
        gh = np.moveaxis(rife_graph.to_hw2(h1), -1, 0)
        def d_at(py, px):
            gx_, gy_ = bilinear_at(gh, py, px)
            if align:
                mx = (gx_ + 1) * 0.5 * (pw - 1); my = (gy_ + 1) * 0.5 * (ph - 1)
            else:
                mx = ((gx_ + 1) * pw - 1) * 0.5; my = ((gy_ + 1) * ph - 1) * 0.5
            return mx - np.clip(px, 0, w - 1), my - np.clip(py, 0, h - 1)
        sx, sy = xo, yo
        for _ in range(2):
            ddx, ddy = d_at(sy, sx)
            sx, sy = xo - 2.0 * ddx, yo - 2.0 * ddy
        ddx, ddy = d_at(sy, sx)
        halfway = np.clip(bilinear_at(f1, sy + ddy, sx + ddx), 0, 1)
        row["forecast_from_halfway_db"] = round(psnr(halfway, truth), 2)
        if box is not None:
            row["halfway_error_at_object"] = round(float(np.abs(halfway - truth)[:, slice(box[0].start, box[0].stop), slice(box[1].start + moved // 2, box[1].stop + moved // 2)].mean()), 3)
            row["hold_error_at_object"] = round(float(np.abs(f1 - truth)[:, slice(box[0].start, box[0].stop), slice(box[1].start + moved // 2, box[1].stop + moved // 2)].mean()), 3)
        if box is not None:
            row["found_motion_px"] = round(float(-dx[box].mean()), 1); row["true_motion_px"] = moved
        rows.append(row)
    for move in (8, 16, 32, 48):
        box = (slice(int(h * 0.35) + 8, int(h * 0.35) + 32), slice(100 + move + 8, 100 + move + 32))
        forecast(f"square moving {move} px a frame", frame(w, h, 100), frame(w, h, 100 + move), frame(w, h, 100 + 1.5 * move), box, move)
    for pan in (4, 8, 16):
        forecast(f"fine texture panning {pan} px a frame", detail_frame(w, h, 0), detail_frame(w, h, pan), detail_frame(w, h, pan + pan // 2))

    # Predict as built (shaders/PushPredict.hlsl, pushflow/PushFlowPredict.h): the forecast from the
    # half-way grids for a time `ahead` of an interval after the newest frame, and per pixel how far it
    # is trusted: the search for where the pixel came from must settle (`settle`), the two real frames'
    # motions must be opposite (`opposite`: an occlusion or a lost object is where they are not), the
    # place it reads must be inside the picture, and (optionally) it must not move further than `cap` of
    # the picture's width a frame. An untrusted pixel shows the newest real frame (a hold).
    def built(f0, f1, ahead, settle=(0.5, 1.5), opposite=(1.0, 3.0), cap=None, agree=None, still=(0.25, 0.5)):
        _, h0, h1, _ = session.run(None, {name: tensor(f0, f1, 0.5, channels)})[:4]
        ph, pw = rife_graph.to_hw2(h0).shape[:2]
        grids = [np.moveaxis(rife_graph.to_hw2(g), -1, 0) for g in (h0, h1)]
        def d_at(grid, py, px):
            py, px = np.clip(py, 0, h - 1), np.clip(px, 0, w - 1)
            gx_, gy_ = bilinear_at(grid, py, px)
            if align:
                mx = (gx_ + 1) * 0.5 * (pw - 1); my = (gy_ + 1) * 0.5 * (ph - 1)
            else:
                mx = ((gx_ + 1) * pw - 1) * 0.5; my = ((gy_ + 1) * ph - 1) * 0.5
            return mx - px, my - py
        k = 1.0 + 2.0 * ahead
        sx, sy = xo, yo
        for _ in range(2):
            ddx, ddy = d_at(grids[1], sy, sx)
            sx, sy = xo - k * ddx, yo - k * ddy
        ddx, ddy = d_at(grids[1], sy, sx)
        ax, ay = d_at(grids[0], sy, sx)
        def fade(x, lo, hi):
            t = np.clip((x - lo) / (hi - lo), 0.0, 1.0)
            return 1.0 - t * t * (3.0 - 2.0 * t)
        trust = fade(np.hypot(sx + k * ddx - xo, sy + k * ddy - yo), *settle) * fade(np.hypot(ax + ddx, ay + ddy), *opposite)
        px, py = sx + ddx, sy + ddy                 # where the pixel is in the newest frame
        trust = trust * ((px >= -0.5) & (px <= w - 0.5) & (py >= -0.5) & (py <= h - 0.5))
        if cap is not None:
            trust = trust * fade(2.0 * np.hypot(ddx, ddy) / w, cap * 0.8, cap * 1.2)
        if agree is not None:
            # The motion is only believed where the two real frames show the same thing at the two
            # places it joins (the half-way pixel's two samples): a cut, an uncovered background and
            # a lost object are where they do not.
            first = np.clip(bilinear_at(f0, sy + ay, sx + ax), 0, 1)
            second = np.clip(bilinear_at(f1, sy + ddy, sx + ddx), 0, 1)
            trust = trust * fade(np.abs(first - second).max(axis=0), *agree)
        # Less than a quarter of a pixel is not moved at all: a still picture (and a HUD) stays exact.
        trust = trust * (1.0 - fade(np.hypot(px - xo, py - yo), *still))
        shown = np.clip(bilinear_at(f1, py, px), 0, 1)
        return f1 + (shown - f1) * trust[None], float((trust < 0.5).mean())
    table = []
    def as_built(label, f0, f1, truth_at):
        for ahead in (0.25, 0.5, 0.75):
            truth = truth_at(ahead)
            line = [label, ahead, round(psnr(f1, truth), 1)]
            for rule in ({}, {"agree": (0.08, 0.16)}, {"agree": (0.04, 0.12)}, {"opposite": (0.5, 1.5), "agree": (0.08, 0.16)}):
                picture, held = built(f0, f1, ahead, **rule)
                line += [round(psnr(picture, truth), 1), round(100.0 * held, 1)]
            table.append(line)
    for move in (2, 8, 16, 32, 48, 96):
        as_built(f"square {move} px", frame(w, h, 100), frame(w, h, 100 + move), lambda a, move=move: frame(w, h, 100 + (1.0 + a) * move))
    for pan in (4, 8, 16, 32):
        as_built(f"texture pan {pan} px", detail_frame(w, h, 0), detail_frame(w, h, pan), lambda a, pan=pan: detail_frame(w, h, int(round((1.0 + a) * pan))))
    as_built("still", frame(w, h, 100), frame(w, h, 100), lambda a: frame(w, h, 100))
    as_built("unrelated", frame(w, h, 100), detail_frame(w, h, 0), lambda a: detail_frame(w, h, 0))
    # [pair, ahead, hold dB, then for each rule: forecast dB, % of the picture held (a still part counts
    # as held)]. Rules: geometry only; with 'agree' 0.08..0.16; with 'agree' 0.04..0.12; a stricter
    # 'opposite' with 'agree' 0.08..0.16. A speed cap was measured and dropped: it only took gains away.
    rows.append({"predict_as_built": table,
                 "worst_against_hold_db": [round(min(line[3 + 2 * r] - line[2] for line in table), 1) for r in range(4)],
                 "mean_gain_db": [round(sum(line[3 + 2 * r] - line[2] for line in table if line[0] not in ("still", "unrelated")) / max(1, sum(1 for line in table if line[0] not in ("still", "unrelated"))), 1) for r in range(4)]})
    return rows


def pad_multiple(session, name, channels) -> int:
    """The block size the model pads to inside (its grids have the padded size): PushFlow pads to it itself."""
    import numpy as np
    import rife_graph
    seen = []
    for w, h in ((256, 192), (320, 200), (448, 320)):
        z = np.zeros((3, h, w), np.float32)
        g = rife_graph.to_hw2(session.run(None, {name: tensor(z, z, 0.5, channels)})[1])
        seen.append((w, h, g.shape[1], g.shape[0]))
    for m in (8, 16, 32, 64, 128, 256):
        if all(-(-w // m) * m == gw and -(-h // m) * m == gh for w, h, gw, gh in seen):
            print(f"internal padding to multiples of {m}: {seen}")
            return m
    sys.exit(f"::error::cannot work out the model's padding from {seen}")


def check_exposed(path: Path, channels: int, align: int) -> dict:
    import numpy as np
    import onnxruntime as ort
    import rife_graph
    session = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
    name = session.get_inputs()[0].name
    pad = pad_multiple(session, name, channels)
    w, h = 256, 256
    first, second = frame(w, h, 100), frame(w, h, 116)
    rgb, g0, g1, m = session.run(None, {name: tensor(first, second, 0.5, channels)})[:4]
    a = rife_graph.reconstruct(first, second, g0, g1, m, align, True)
    b = rife_graph.reconstruct(first, second, g0, g1, m, align, False)
    pa, pb = rife_graph.psnr(a, rgb[0]), rife_graph.psnr(b, rgb[0])
    first_mask = pa >= pb
    # Fast tier: the network at half size, rebuilt at full size versus the half-size picture enlarged.
    # Full size twice a padded half size, so both the half-size network input and the full frames fit.
    H, W = 2 * (-(-192 // pad) * pad), 2 * (-(-256 // pad) * pad)
    f0, f1, gt = detail_frame(W, H, 0), detail_frame(W, H, 8), detail_frame(W, H, 4)
    s0, s1 = resize_box(f0, 2), resize_box(f1, 2)
    srgb, sg0, sg1, sm = session.run(None, {name: tensor(s0, s1, 0.5, channels)})[:4]
    yo, xo = np.mgrid[0:H, 0:W].astype(np.float64)
    old = bilinear_at(srgb[0], (yo + 0.5) / 2 - 0.5, (xo + 0.5) / 2 - 0.5)
    new = rebuild_scaled(f0, f1, sg0, sg1, sm, align, first_mask, (H // 2, W // 2))
    full = session.run(None, {name: tensor(f0, f1, 0.5, channels)})[0][0]
    fast = {"old_enlarged_db": round(rife_graph.psnr(old, gt), 2), "full_res_rebuild_db": round(rife_graph.psnr(new, gt), 2),
            "full_size_network_db": round(rife_graph.psnr(full, gt), 2), "blend_db": round(rife_graph.psnr((f0 + f1) / 2, gt), 2)}
    return {"rebuild_db": round(max(pa, pb), 2), "mask_for_first": bool(first_mask), "fast_detail": fast, "pad": pad, "output": rgb[0]}


def check_split(whole: Path, encoder0: Path, encoder1: Path, body: Path, channels: int) -> dict:
    """The encoders and the body together against the whole model, on the CPU: every output (picture, both
    grids, mask), several times between the frames, a size that is a multiple of the model's block and one
    that is not, the first frame's features taken from the pair before (as PushFlow does) and from the
    first-frame encoder (as it does when there is no pair before). Returns the largest difference found."""
    import numpy as np
    import onnxruntime as ort
    opts = ort.SessionOptions()
    opts.intra_op_num_threads = 2
    w_s, e0_s, e_s, b_s = (ort.InferenceSession(str(p), opts, providers=["CPUExecutionProvider"]) for p in (whole, encoder0, encoder1, body))
    name = w_s.get_inputs()[0].name
    worst, runs, shape = 0.0, 0, None
    rng = np.random.default_rng(11)
    for w, h in ((256, 192), (320, 200)):
        A, B, C = (rng.random((3, h, w), dtype=np.float32) for _ in range(3))
        drawn = (frame(w, h, 100), frame(w, h, 116))
        for first, second, before in ((A, B, None), (B, C, A), drawn + (None,)):
            if before is None:   # no pair before: the first-frame encoder reads it where it is
                feat0 = e0_s.run(None, {name: tensor(first, second, 0.5, channels)})[0]
            else:                # the pair before made it: (before, first)
                feat0 = e_s.run(None, {name: tensor(before, first, 0.5, channels)})[0]
            feat1 = e_s.run(None, {name: tensor(first, second, 0.5, channels)})[0]
            shape = list(feat1.shape)
            for t in (0.125, 0.25, 1.0 / 3.0, 0.5, 2.0 / 3.0, 0.75, 0.875):
                x = tensor(first, second, t, channels)
                want = w_s.run(None, {name: x})
                got = b_s.run(None, {name: x, "pushflow_feat0": feat0, "pushflow_feat1": feat1})
                if len(want) != len(got):
                    raise ValueError("the body's outputs are not the model's")
                for a, b in zip(want, got):
                    worst = max(worst, float(np.abs(a.astype(np.float64) - b.astype(np.float64)).max()))
                runs += 1
    return {"largest_difference": worst, "runs": runs, "features_shape": shape}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--study", action="store_true", help="also print the Fusion and Predict studies (information only)")
    args = ap.parse_args()
    out = Path(args.out).resolve()
    work = out / "download"
    work.mkdir(parents=True, exist_ok=True)
    runtime = out / "runtime" / "pushflow"
    shutil.rmtree(runtime, ignore_errors=True)
    (runtime / "models").mkdir(parents=True)
    licences = runtime / "licenses"
    licences.mkdir()

    nuget(ORT_ID, ORT_VERSION, work, out)
    nuget(DML_ID, DML_VERSION, work, out)
    ort_dll = out / "ort" / "runtimes" / "win-x64" / "native" / "onnxruntime.dll"
    dml_dll = out / "dml" / "bin" / "x64-win" / "DirectML.dll"
    for f in (ort_dll, dml_dll, out / "ort" / "build" / "native" / "include" / "onnxruntime_c_api.h",
              out / "ort" / "build" / "native" / "include" / "dml_provider_factory.h", out / "dml" / "include" / "DirectML.h"):
        if not f.exists():
            sys.exit(f"::error::missing {f}")
    shutil.copy2(ort_dll, runtime)
    shutil.copy2(dml_dll, runtime)
    # ONNX Runtime's NuGet has no LICENSE.txt at its root in every version: use whichever LICENSE file it
    # has, else write its MIT licence (Copyright (c) Microsoft Corporation), so the licence always ships.
    ort_licence = next((f for f in sorted((out / "ort").glob("LICENSE*")) if f.is_file()), None)
    if ort_licence:
        shutil.copy2(ort_licence, licences / "ONNX_Runtime_LICENSE.txt")
    else:
        (licences / "ONNX_Runtime_LICENSE.txt").write_text(MIT.format(holder="Microsoft Corporation", year=""), encoding="utf-8")
    (licences / "RIFE_LICENSE.txt").write_text(
        "RIFE (Real-Time Intermediate Flow Estimation), Practical-RIFE v4 weights: https://github.com/hzwer/Practical-RIFE\n"
        "ONNX conversions: https://github.com/AmusementClub/vs-mlrt (external-models)\n\n" + MIT.format(holder="hzwer", year="2021 "),
        encoding="utf-8")
    for src, dst in ( (out / "ort" / "ThirdPartyNotices.txt", "ONNX_Runtime_ThirdPartyNotices.txt"),
                     (out / "dml" / "LICENSE.txt", "DirectML_LICENSE.txt"), (out / "dml" / "ThirdPartyNotices.txt", "DirectML_ThirdPartyNotices.txt")):
        if src.exists():
            shutil.copy2(src, licences / dst)
        else:
            print(f"note: {src.name} not in the package")

    # Models.
    extracted = work / "models"
    shutil.rmtree(extracted, ignore_errors=True)
    extracted.mkdir()
    z = seven_zip()
    for archive in model_archives(work):
        target = extracted / archive.stem
        subprocess.run([z, "x", "-y", f"-o{target}", str(archive)], check=True, stdout=subprocess.DEVNULL)
    candidates = []
    for path in sorted(extracted.rglob("*.onnx")):
        version = version_of(path)
        channels = describe(path)
        print(f"model {path.relative_to(extracted)} version={version} channels={channels}")
        if not version or not channels or "ensemble" in path.name.lower():
            continue
        lite = "lite" in path.name.lower()
        candidates.append({"path": path, "version": version, "channels": channels, "lite": lite})
    chosen = {}
    for role, lite in (("fast", True), ("quality", False)):
        pool = [c for c in candidates if c["lite"] == lite and c["version"] < (4, 30)]
        # Newest version; at equal versions the 7-channel (grid inside the model) layout.
        pool.sort(key=lambda c: (c["version"], c["channels"] == 7), reverse=True)
        for c in pool:
            result = check_model(c["path"], c["channels"])
            print(f"check {role} {c['path'].name}: identical {result['identical_db']} dB, moving {result['moving_db']} dB, blend {result['blend_db']} dB")
            if result["identical_db"] > 30 and result["moving_db"] > result["blend_db"] + 2:
                chosen[role] = dict(c, check=result)
                break
        if role not in chosen and role == "fast" and candidates:
            print("no lite model passed; the fast tier uses the full model at its reduced size")
        if role not in chosen and role == "quality" and "fast" in chosen:
            chosen["quality"] = chosen["fast"]
    if "fast" not in chosen and "quality" in chosen:
        chosen["fast"] = chosen["quality"]
    if "fast" not in chosen:
        sys.exit("::error::no RIFE model passed the checks")

    # Every shipped model is the vs-mlrt export with the final warp grids and blend mask added as outputs
    # (rife_graph.expose), checked on the CPU: rebuilding from them reproduces the model's own picture, and
    # for the Fast tier (network at half size) rebuilding at full size from the full-size frames keeps more
    # detail than enlarging the network's half-size picture. Full precision is the reference; a
    # half-precision copy (convolutions fp16, warps, resizes, sigmoid and blend fp32) ships next to it when it
    # agrees on the CPU, and PushFlow only uses it after its own check on the user's graphics card.
    import onnx
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import rife_graph
    record = {"source": f"https://github.com/{MODELS_REPO}/releases/tag/{MODELS_TAG}", "runtime": f"{ORT_ID} {ORT_VERSION}, {DML_ID} {DML_VERSION}",
              "contract": {"input": "[1, C, H, W] float32, C = 7 (img0 RGB, img1 RGB, t) or 11 (+ flow scale, grid)", "pad_to": 64,
                           "color": "0..1, the encoding PushFlowPack writes", "outputs": "rgb [1,3,H,W]; pushflow_grid0/1 (normalized -1..1); pushflow_mask [1,1,H,W]"},
              "models": {}}
    for role, c in chosen.items():
        base = onnx.load(str(c["path"]))
        input_dims = name_free_dims(base)
        print(f"{role} input dimensions: {input_dims}")
        target = runtime / "models" / f"pushflow-{role}.onnx"
        best = None
        for batched in (True, False):
            exposed, info = rife_graph.expose(base, batched)
            onnx.save(exposed, str(target))
            try:
                verdict = check_exposed(target, c["channels"], info["align_corners"])
            except Exception as e:  # noqa: BLE001 - this reading of the graph does not run
                print(f"{role} batched={batched}: {e}")
                continue
            print(f"{role} batched={batched}: rebuild {verdict['rebuild_db']} dB")
            if best is None or verdict["rebuild_db"] > best[2]["rebuild_db"]:
                best = (exposed, info, verdict)
        if best is None:
            sys.exit(f"::error::no way to expose the grids of {c['path'].name}")
        exposed, info, verdict = best
        onnx.save(exposed, str(target))
        print(f"exposed {role}: {json.dumps({k: v for k, v in verdict.items() if k != 'output'})}")
        if verdict["rebuild_db"] < 45:
            print(rife_graph.describe_tail(base))
        if verdict["rebuild_db"] < 45:
            sys.exit(f"::error::rebuilding from grids and mask does not reproduce {c['path'].name} ({verdict['rebuild_db']} dB)")
        entry = {"file": target.name, "from": c["path"].name, "version": "%d.%d" % c["version"], "lite": c["lite"],
                 "channels": c["channels"], "input_dims": input_dims, "precision": "fp32", "sha256": sha256(target), "opset": [o.version for o in base.opset_import],
                 "align_corners": info["align_corners"], "padding_mode": info["padding_mode"], "batched_warp": info["batched"], "mask_for_first": verdict["mask_for_first"], "pad": verdict["pad"],
                 "cpu_check_db": {k: c["check"][k] for k in ("identical_db", "moving_db", "blend_db")},
                 "rebuild_db": verdict["rebuild_db"], "fast_detail": verdict["fast_detail"]}
        try:
            from onnxconverter_common import float16
            half = float16.convert_float_to_float16(exposed, keep_io_types=True,
                                                    op_block_list=list(float16.DEFAULT_OP_BLOCK_LIST) + ["GridSample", "Resize", "Sigmoid", "Softmax"])
            half_path = runtime / "models" / f"pushflow-{role}-fp16.onnx"
            onnx.save(half, str(half_path))
            hv = check_exposed(half_path, c["channels"], info["align_corners"])
            agreement = rife_graph.psnr(hv["output"], verdict["output"])
            print(f"fp16 {role}: agreement with fp32 {agreement:.1f} dB")
            if agreement > 40 and hv["rebuild_db"] >= 40:
                entry["fp16"] = {"file": half_path.name, "sha256": sha256(half_path), "cpu_agreement_db": round(agreement, 2)}
            else:
                half_path.unlink()
        except Exception as e:  # noqa: BLE001 - no fp16 copy; fp32 stays the only model
            print(f"fp16 {role} skipped: {e}")
        # How this model's grids and mask behave (a report in the log, see motion_report; nothing is decided by it).
        try:
            for row in motion_report(target, c["channels"], info["align_corners"], verdict["mask_for_first"]):
                print(f"motion {role}: {json.dumps(row)}")
        except Exception as e:  # noqa: BLE001 - the report is information only
            print(f"motion {role}: report failed: {e}")
        # --study (the pushflow-models workflow): the measurements docs/PUSHFLOW2.md is written from.
        if args.study:
            for label, make in (("fusion", lambda: [fusion_report(c["path"], c["channels"])]), ("predict", lambda: predict_report(target, c["channels"], info["align_corners"])),
                                ("guard", lambda: guard_report(target, c["channels"], info["align_corners"], verdict["mask_for_first"]))):
                try:
                    for row in make():
                        print(f"{label} {role}: {json.dumps(row)}")
                except Exception as e:  # noqa: BLE001 - a study is information only
                    print(f"{label} {role}: study failed: {type(e).__name__}: {e}")
        # PushFlow 2, Fusion's first step: the same model cut into its per-frame encoder and the rest
        # (rife_graph.split_encoder), shipped only when the two together give the whole model's output on the
        # CPU. PushFlow uses them only when asked to (off by default) and after its own check on the card.
        split_facts = ""
        split_files = [runtime / "models" / f"pushflow-{role}-{part}.onnx" for part in ("enc0", "enc1", "body")]
        try:
            enc0_model, enc1_model, body_model, cut = rife_graph.split_encoder(exposed)
            for part_model, part_path in zip((enc0_model, enc1_model, body_model), split_files):
                onnx.save(part_model, str(part_path))
            split = check_split(target, *split_files, c["channels"])
            print(f"split {role}: {json.dumps(dict(split, **cut))}")
            if split["largest_difference"] <= 1e-4 and split["runs"] > 0:
                entry["split"] = {"files": {f.name: sha256(f) for f in split_files}, "features": cut["features"],
                                  "cpu_largest_difference": split["largest_difference"], "cpu_runs": split["runs"]}
                split_facts = f"split_features={cut['features']}\n"
            else:
                print(f"split {role}: does not reproduce the model (largest difference {split['largest_difference']}); not shipped")
                for leftover in split_files:
                    leftover.unlink(missing_ok=True)
        except Exception as e:  # noqa: BLE001 - no split models; the whole model stays the only way
            print(f"split {role} skipped: {type(e).__name__}: {e}")
            for leftover in split_files:
                leftover.unlink(missing_ok=True)
        # Facts the native backend needs to rebuild the picture (PushFlowBackend.cpp readFacts).
        (runtime / "models" / f"pushflow-{role}.onnx.txt").write_text(
            f"align_corners={info['align_corners']}\nmask_for_first={1 if verdict['mask_for_first'] else 0}\npad={verdict['pad']}\n{split_facts}", encoding="ascii")
        record["models"][role] = entry
    (runtime / "models.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(record, indent=2))
    for f in sorted(runtime.rglob("*")):
        if f.is_file():
            print(f"{f.stat().st_size:>12}  {f.relative_to(runtime)}")


if __name__ == "__main__":
    main()

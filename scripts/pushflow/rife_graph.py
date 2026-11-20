#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""RIFE graph tools for PushFlow (used by prepare_runtime.py; CI only).

RIFE v4 ends by warping both real frames with their final flow (two GridSample nodes) and blending them
with a sigmoid mask:  out = warp(img0, grid0) * mask + warp(img1, grid1) * (1 - mask).
expose() adds grid0, grid1 and mask as extra model outputs, so PushFlow can rebuild the in-between frame
itself: at full resolution from the full-resolution real frames even when the network ran at a reduced
size (the Fast tier), instead of enlarging the network's small RGB picture. check_reconstruction() proves,
on the CPU, that rebuilding from the exposed tensors reproduces the model's own output.
"""
from __future__ import annotations

import collections

import numpy as np


def _producers(graph):
    return {o: n for n in graph.node for o in n.output}


def find_final_blend(model):
    """Returns (grid0, grid1, mask, align_corners, padding_mode) for the last warps and mask before output 0."""
    graph = model.graph
    producer = _producers(graph)
    out = graph.output[0].name
    dist = {out: 0}
    queue = collections.deque([out])
    order = []
    while queue:
        name = queue.popleft()
        node = producer.get(name)
        if node is None:
            continue
        order.append((dist[name], node))
        for i in node.input:
            if i and i not in dist:
                dist[i] = dist[name] + 1
                queue.append(i)
    warps = [n for _, n in order if n.op_type == "GridSample"]
    sigmoids = [n for _, n in order if n.op_type == "Sigmoid"]
    if len(warps) < 2 or not sigmoids:
        raise RuntimeError(f"no final blend found ({len(warps)} GridSample, {len(sigmoids)} Sigmoid near the output)")
    w0, w1 = warps[0], warps[1]
    attrs = {a.name: a for a in w0.attribute}
    align = int(attrs["align_corners"].i) if "align_corners" in attrs else 0
    pad = attrs["padding_mode"].s.decode() if "padding_mode" in attrs else "zeros"
    return w0.input[1], w1.input[1], sigmoids[0].output[0], align, pad, (w0.input[0], w1.input[0])


def expose(model, batched=True):
    """A copy of the model with grid0, grid1 and mask as outputs 1-3 (named pushflow_grid0/1, pushflow_mask).

    batched: vs-mlrt's exports warp both frames in ONE GridSample (the two frames stacked on the batch axis,
    grid [2, H, W, 2]); grid0/grid1 are its two batch entries. Otherwise the two closest GridSample nodes
    are the two warps. prepare_runtime.py tries both and keeps the one that reproduces the model exactly."""
    import onnx
    from onnx import helper, numpy_helper
    m = onnx.ModelProto()
    m.CopyFrom(model)
    g0, g1, mask, align, pad, _ = find_final_blend(m)
    if batched:
        def const(name, values):
            m.graph.initializer.append(numpy_helper.from_array(np.array(values, dtype=np.int64), name))
            return name
        const("pushflow_axis0", [0]); const("pushflow_b0", [0]); const("pushflow_b1", [1]); const("pushflow_b2", [2])
        m.graph.node.append(helper.make_node("Slice", [g0, "pushflow_b0", "pushflow_b1", "pushflow_axis0"], ["pushflow_grid0"], name="pushflow_grid0_out"))
        m.graph.node.append(helper.make_node("Slice", [g0, "pushflow_b1", "pushflow_b2", "pushflow_axis0"], ["pushflow_grid1"], name="pushflow_grid1_out"))
        m.graph.node.append(helper.make_node("Identity", [mask], ["pushflow_mask"], name="pushflow_mask_out"))
    else:
        for src, name in ((g0, "pushflow_grid0"), (g1, "pushflow_grid1"), (mask, "pushflow_mask")):
            m.graph.node.append(helper.make_node("Identity", [src], [name], name=name + "_out"))
    for name in ("pushflow_grid0", "pushflow_grid1", "pushflow_mask"):
        m.graph.output.append(helper.make_tensor_value_info(name, onnx.TensorProto.FLOAT, None))
    return m, {"align_corners": align, "padding_mode": pad, "batched": batched}


def split_encoder(model):
    """The model cut in two (PushFlow 2, Fusion's first step): what depends on ONE real frame, and the rest.

    RIFE 4.25 / 4.26 begin with a small feature encoder that takes the two frames as a batch of two and never
    sees the time. Run on its own it has to be done once for every real frame, not twice for every generated
    frame. Returns (encoder0, encoder1, body, info):

      encoder1 input: the model's own input [1, C, H, W]; output pushflow_feat [1, F, Hp, Wp], the features
               of the SECOND frame in it (channels 3..5): the one run for every new real frame.
      encoder0 the same for the FIRST frame (channels 0..2). Its features are normally the ones the pair
               before produced; this one is run only when there was no pair before (a start, a reset, a
               pair that was skipped).
      body     inputs: the model's own input, pushflow_feat0 and pushflow_feat1 (the first and second frame's
               features); outputs exactly as the model's.

    Nothing is retrained or approximated: the same nodes with the same weights, so the two together must give
    the whole model's output, which prepare_runtime.py checks before it ships them. Raises ValueError when the
    model does not begin the way this reading expects."""
    import onnx
    from onnx import helper, numpy_helper
    g = model.graph
    nodes = list(g.node)                       # nodes are told apart by their place in this list
    produced = {o: k for k, n in enumerate(nodes) for o in n.output}
    readers = {}
    for k, n in enumerate(nodes):
        for i in n.input:
            readers.setdefault(i, []).append(k)
    init = {i.name: i for i in g.initializer}
    source = [i.name for i in g.input if i.name not in init][0]
    first = next((k for k, n in enumerate(nodes) if n.op_type == "Conv"), None)
    if first is None:
        raise ValueError("no convolution")
    stack = nodes[first].input[0]
    # The encoder's input is the two frames stacked on the batch axis: a Reshape of the first six channels.
    walk, seen_ops = stack, []
    while walk in produced and len(seen_ops) < 6:
        n = nodes[produced[walk]]
        seen_ops.append(n.op_type)
        walk = n.input[0]
    if walk != source or seen_ops[:1] != ["Reshape"] or not set(seen_ops) <= {"Reshape", "Split", "Slice", "Pad", "Cast", "Identity"}:
        raise ValueError(f"the first convolution does not read the stacked frames ({seen_ops} from {walk})")
    link = {"Conv", "ConvTranspose", "LeakyRelu", "Relu", "PRelu"}
    chain, at = [], first
    while True:
        chain.append(at)
        tensor = nodes[at].output[0]
        after = readers.get(tensor, [])
        if len(after) != 1 or nodes[after[0]].op_type not in link or nodes[after[0]].input[0] != tensor:
            break
        at = after[0]
    last = nodes[chain[-1]]
    convs = [k for k in chain if nodes[k].op_type in ("Conv", "ConvTranspose")]
    if len(convs) < 2 or last.op_type not in ("Conv", "ConvTranspose") or not readers.get(tensor):
        raise ValueError(f"no encoder found at the start ({[nodes[k].op_type for k in chain]})")
    feat = tensor
    weight = init.get(last.input[1])
    if weight is None:
        raise ValueError("the encoder's last weights are not in the model")
    features = int(weight.dims[1] if last.op_type == "ConvTranspose" else weight.dims[0])
    in_chain = set(chain)

    def rebuilt(new_nodes, outputs, extra_weights=()):
        """A copy of the model with these nodes, only what the outputs need of them and of the weights."""
        need, todo, kept = set(outputs), list(outputs), set()
        by_output = {o: k for k, n in enumerate(new_nodes) for o in n.output}
        while todo:
            k = by_output.get(todo.pop())
            if k is None or k in kept:
                continue
            kept.add(k)
            for i in new_nodes[k].input:
                if i and i not in need:
                    need.add(i)
                    todo.append(i)
        m = onnx.ModelProto()
        m.CopyFrom(model)
        del m.graph.node[:]
        m.graph.node.extend([n for k, n in enumerate(new_nodes) if k in kept])
        weights = [i for i in list(model.graph.initializer) + list(extra_weights) if i.name in need]
        del m.graph.initializer[:]
        m.graph.initializer.extend(weights)
        del m.graph.value_info[:]
        return m

    def features_info(name):
        return helper.make_tensor_value_info(name, onnx.TensorProto.FLOAT, [1, features, "pushflow_fh", "pushflow_fw"])

    # --- the body: the model without the encoder, the features given from outside ---
    body_nodes = [helper.make_node("Concat", ["pushflow_feat0", "pushflow_feat1"], [feat], axis=0, name="pushflow_features_in")]
    body_nodes += [n for k, n in enumerate(nodes) if k not in in_chain]
    body = rebuilt(body_nodes, [o.name for o in g.output])
    body.graph.input.extend([features_info("pushflow_feat0"), features_info("pushflow_feat1")])

    # --- the encoders: the model's own beginning, for one of the two frames alone ---
    def encoder_for(which):
        picks = [numpy_helper.from_array(np.array(value, dtype=np.int64), name)
                 for name, value in (("pushflow_enc_from", [which]), ("pushflow_enc_to", [which + 1]), ("pushflow_enc_axis", [0]))]
        enc_nodes = []
        for k, n in enumerate(nodes):
            if k == first:
                enc_nodes.append(helper.make_node("Slice", [stack, "pushflow_enc_from", "pushflow_enc_to", "pushflow_enc_axis"], ["pushflow_one_frame"], name="pushflow_one_frame_in"))
                changed = onnx.NodeProto()
                changed.CopyFrom(n)
                changed.input[0] = "pushflow_one_frame"
                enc_nodes.append(changed)
            else:
                enc_nodes.append(n)
        enc_nodes.append(helper.make_node("Identity", [feat], ["pushflow_feat"], name="pushflow_feat_out"))
        encoder = rebuilt(enc_nodes, ["pushflow_feat"], picks)
        del encoder.graph.output[:]
        encoder.graph.output.append(features_info("pushflow_feat"))
        return encoder
    return encoder_for(0), encoder_for(1), body, {"features": features, "encoder_nodes": [nodes[k].op_type for k in chain], "cut_at": feat}


def grid_sample(img, grid, align_corners, padding="border"):
    """numpy GridSample, bilinear: img [C,H,W], grid [H',W',2] in -1..1 (x, y)."""
    c, h, w = img.shape
    gx, gy = grid[..., 0], grid[..., 1]
    if align_corners:
        x = (gx + 1) * 0.5 * (w - 1)
        y = (gy + 1) * 0.5 * (h - 1)
    else:
        x = ((gx + 1) * w - 1) * 0.5
        y = ((gy + 1) * h - 1) * 0.5
    x = np.clip(x, 0, w - 1)
    y = np.clip(y, 0, h - 1)
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    x1 = np.minimum(x0 + 1, w - 1)
    y1 = np.minimum(y0 + 1, h - 1)
    fx, fy = x - x0, y - y0
    a = img[:, y0, x0] * (1 - fx) + img[:, y0, x1] * fx
    b = img[:, y1, x0] * (1 - fx) + img[:, y1, x1] * fx
    return a * (1 - fy) + b * fy


def to_hw2(grid):
    """Grid tensor -> [H, W, 2] whatever the export's layout ([1,H,W,2] or [1,2,H,W])."""
    g = np.asarray(grid)[0]
    return g if g.shape[-1] == 2 else np.moveaxis(g, 0, -1)


def reconstruct(img0, img1, grid0, grid1, mask, align_corners, mask_for_first=True):
    w0 = grid_sample(img0, to_hw2(grid0), align_corners)
    w1 = grid_sample(img1, to_hw2(grid1), align_corners)
    m = np.asarray(mask)[0, 0]
    return w0 * m + w1 * (1 - m) if mask_for_first else w0 * (1 - m) + w1 * m


def psnr(a, b):
    mse = float(np.mean((np.clip(a, 0, 1) - np.clip(b, 0, 1)) ** 2))
    return 99.0 if mse < 1e-12 else 10 * np.log10(1.0 / mse)


def describe_tail(model, n=45):
    """The n nodes closest to output 0 (breadth-first from the output), for diagnosing an export."""
    graph = model.graph
    producer = _producers(graph)
    out = graph.output[0].name
    dist = {out: 0}
    queue = collections.deque([out])
    lines = []
    while queue and len(lines) < n:
        name = queue.popleft()
        node = producer.get(name)
        if node is None:
            continue
        attrs = ",".join(f"{a.name}={a.i if a.type == 2 else (a.s.decode() if a.type == 3 else ('[..]' if a.ints else a.f))}" for a in node.attribute)
        lines.append(f"d={dist[name]:>2} {node.op_type:<14} {list(node.input)} -> {list(node.output)} {attrs}")
        for i in node.input:
            if i and i not in dist:
                dist[i] = dist[name] + 1
                queue.append(i)
    return "\n".join(lines)

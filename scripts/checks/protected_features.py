#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Checks tests/protected-features.json against the repository (the checks workflow runs this on every push).

For every guard of every protected feature:
  file      must exist;
  ci        this text must appear in a workflow under .github/workflows (the test is really run);
  contains  this text must appear in the file (the check that matters is still in it).
A feature with no guard must say what it needs instead (needs_hardware). Exit code 1 on any problem.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    manifest = json.loads((ROOT / "tests" / "protected-features.json").read_text(encoding="utf-8"))
    workflows = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in sorted((ROOT / ".github" / "workflows").glob("*.yml")))
    problems: list[str] = []
    seen: set[str] = set()
    guards = 0
    for feature in manifest["features"]:
        fid = feature.get("id", "?")
        if fid in seen:
            problems.append(f"{fid}: listed twice")
        seen.add(fid)
        if not feature.get("what"):
            problems.append(f"{fid}: does not say what it protects")
        if not feature.get("guards") and not feature.get("needs_hardware"):
            problems.append(f"{fid}: has no guard and does not say what it needs instead")
        for guard in feature.get("guards", []):
            guards += 1
            path = ROOT / guard["file"]
            if not path.is_file():
                problems.append(f"{fid}: {guard['file']} is gone")
                continue
            if guard.get("ci") and guard["ci"] not in workflows:
                problems.append(f"{fid}: no workflow runs {guard['ci']} any more ({guard['file']})")
            if guard.get("contains") and guard["contains"] not in path.read_text(encoding="utf-8", errors="replace"):
                problems.append(f"{fid}: {guard['file']} no longer holds: {guard['contains']}")
    for line in problems:
        print(f"::error file=tests/protected-features.json::{line}")
    print(f"protected features: {len(seen)} features, {guards} guards, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())

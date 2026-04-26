#!/usr/bin/env python3
"""Trim a Lottie composition to a single marker so it loops just that
segment by default.

Use when the JSON ships with multiple LottieFiles "Interactivity"
markers (idle / yes / no / alert / thinking / jump …) and you want
the widget to sit on one of them at boot, while still letting
runtime code call lvgl.lottie.play_marker on the others.

Usage:
    python3 patch_default_segment.py <lottie.json> <marker_name>

The script only edits the composition's root `op` value (and `ip` if
the chosen marker does not start at 0). Layer-level `op` / `ip` and
the markers array are preserved untouched, so play_marker can still
reach every other segment.

Idempotent: re-running with a different marker overwrites the change.
A `.bak` of the original file is created next to it on first run.
"""
from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch_default_segment.py <lottie.json> <marker_name>",
              file=sys.stderr)
        return 2

    json_path = Path(sys.argv[1])
    marker_name = sys.argv[2]

    if not json_path.is_file():
        print(f"error: {json_path} not found", file=sys.stderr)
        return 1

    data = json.loads(json_path.read_text(encoding="utf-8"))

    markers = data.get("markers") or []
    target = next((m for m in markers if m.get("cm") == marker_name), None)
    if target is None:
        names = [m.get("cm") for m in markers]
        print(f"error: marker '{marker_name}' not found. available: {names}",
              file=sys.stderr)
        return 1

    start = int(target.get("tm", 0))
    duration = int(target.get("dr", 0))
    end = start + duration

    # Backup once
    backup = json_path.with_suffix(json_path.suffix + ".bak")
    if not backup.exists():
        shutil.copy2(json_path, backup)
        print(f"backup -> {backup}")

    old_ip = data.get("ip", 0)
    old_op = data.get("op", 0)
    data["ip"] = start
    data["op"] = end

    # Compact serialisation, no trailing whitespace, mirrors LottieFiles output
    json_path.write_text(json.dumps(data, separators=(",", ":")),
                         encoding="utf-8")

    print(f"patched {json_path}")
    print(f"  marker         : {marker_name}")
    print(f"  composition ip : {old_ip} -> {start}")
    print(f"  composition op : {old_op} -> {end}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

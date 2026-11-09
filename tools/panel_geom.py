#!/usr/bin/env python3
"""panel_geom.py — the part of a panel A/B that is actually reproducible.

WHY THIS FILE EXISTS (batch 103, measured). Every console A/B in this repository
has compared two panel crops pixel-for-pixel with `panel_crop.py diff` and quoted
the result. Batch 102's sidecar states the reason that looked safe: "the noise
floor is EXACTLY zero — two runs of the parent build produce byte-identical
panels". It is not zero. Two runs of ONE build, one emulator, one pinned
Mandarine r1.2.1, one forced llvmpipe, `--secs 50 --min-frames 600` twice, differ
in **53515 px of 96000** (bbox the whole panel). A control that reports 0 px was
comparing a capture with itself, not a build with itself.

The cause is that the capture is not frame-locked. `emurun.sh --shot` grabs the X
window at a wall-clock deadline, and the guest log prints a frame line only every
60th frame, so the frame actually on screen at the deadline is anywhere in a
~60-frame window. The level-7 scene is static in its camera and its object list
but its textures animate, so the window moves the colours of most of the panel.
The 3DS backend has no screenshot path (`render.h`: `screenshot_path` is "Linux
only"), so nothing in the guest pins the frame either.

WHAT SURVIVES, and this tool is that: the **painted-versus-clear map**. Measured on
the same two runs of one build, the map is identical (0 px), because the geometry
and the coverage do not depend on which animation frame was up. That is also the
quantity the census has always quoted (the exact clear-colour count, 58464 in both
builds), which is why the census reproduced while the full-RGB diffs did not.

So, from here on:
  * `geom` is the comparison a sidecar quotes as the picture claim;
  * `rgb` is printed alongside it, labelled, and is NEVER a control on its own —
    a full-RGB number is only meaningful against a same-build control run;
  * a real A/B needs two runs of the parent build and two of the child, and the
    parent-vs-parent number is the noise floor the child-vs-parent number has to
    clear. One run per side is not a measurement.

USAGE
  panel_geom.py census IMG [IMG ...]   exact clear-colour census per image
  panel_geom.py geom   A B             painted/clear map diff + full-RGB diff
Exit 0 on success, 1 on a size/shape violation, 2 on a usage or I/O error.
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

# The panel rectangle is panel_crop.py's contract; a differently shaped pair is
# the mistake that file exists to prevent, so it is refused here too.
PANEL_W, PANEL_H = 400, 240

# The frame's clear colour: `params.clear_colour = 0xFF102030u` in
# mdk_3ds/src/main_3ds.cpp. Exact match only — a tolerance is how batch 78's
# "61 %" stopped being comparable with anyone else's (review-4 F1).
CLEAR = (16, 32, 48)


def load(path: str):
    p = Path(path)
    if not p.is_file():
        print(f"panel_geom: no such file: {path}", file=sys.stderr)
        raise SystemExit(2)
    im = Image.open(p).convert("RGB")
    if im.size != (PANEL_W, PANEL_H):
        print(f"panel_geom: {path} is {im.size[0]}x{im.size[1]}, expected "
              f"{PANEL_W}x{PANEL_H} — use tools/panel_crop.py crop first",
              file=sys.stderr)
        raise SystemExit(1)
    return list(im.getdata())


def mask(pixels):
    """The painted/clear map: 1 where the pixel is NOT the clear colour."""
    return [0 if p == CLEAR else 1 for p in pixels]


def census(paths):
    for path in paths:
        px = load(path)
        m = mask(px)
        painted = sum(m)
        half = PANEL_W // 2
        left = sum(sum(m[y * PANEL_W:y * PANEL_W + half]) for y in range(PANEL_H))
        right = painted - left
        print(f"census: {path}  clear={sum(1 for v in m if not v)} of {PANEL_W * PANEL_H}"
              f"  painted={painted}  painted-left={left} painted-right={right}")
    return 0


def geom(a, b):
    pa, pb = load(a), load(b)
    ma, mb = mask(pa), mask(pb)
    geom_diff = sum(1 for i in range(len(ma)) if ma[i] != mb[i])
    rgb_diff = sum(1 for i in range(len(pa)) if pa[i] != pb[i])
    print(f"geom: {geom_diff} px of {PANEL_W * PANEL_H} differ in the painted/clear map"
          f"  (painted {sum(ma)} -> {sum(mb)})")
    print(f"rgb:  {rgb_diff} px of {PANEL_W * PANEL_H} differ in full colour"
          f"  — NOT a control: same-build runs differ by tens of thousands")
    if geom_diff:
        cols = [0] * PANEL_W
        rows = [0] * PANEL_H
        for i in range(len(ma)):
            if ma[i] != mb[i]:
                cols[i % PANEL_W] += 1
                rows[i // PANEL_W] += 1
        nz = [i for i, c in enumerate(cols) if c]
        nr = [i for i, c in enumerate(rows) if c]
        print(f"geom: columns {nz[0]}..{nz[-1]}, rows {nr[0]}..{nr[-1]}"
              f" (panel columns/rows, 0-based)")
    return 0


def main(argv):
    if len(argv) >= 3 and argv[1] == "census":
        return census(argv[2:])
    if len(argv) == 4 and argv[1] == "geom":
        return geom(argv[2], argv[3])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

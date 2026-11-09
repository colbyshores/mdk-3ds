#!/usr/bin/env python3
"""panel_geom.py — the part of a panel A/B that is actually reproducible.

WHY THIS FILE EXISTS (batch 103; its doctrine corrected by batch 104, review-10 F2).
Every console A/B in this repository has compared two panel crops pixel-for-pixel
with `panel_crop.py diff` and quoted the result. Batch 102's sidecar states the
reason that looked safe: "the noise floor is EXACTLY zero — two runs of the parent
build produce byte-identical panels". Batch 103 measured **53515 px of 96000**
between two runs of one build and wrote here that a control reporting 0 px "was
comparing a capture with itself, not a build with itself". That sentence was wrong
and it is retracted: batch 102's control WAS two independent runs — two lanes, two
window grabs, and `docs/evidence/2026-11-09-L7-object-pass-parent.txt:29` names the
second capture — and its panels really were byte-identical.

What the two numbers actually say is narrower, and it is the lesson: **a noise floor
is only valid for the scene it was measured on.** Batch 102's 0 px floor is the
arena-only panel; batch 103's 53515 px floor is the arena+objects panel. Quoting a
floor from a static scene at an animated A/B is the error, and batch 103 committed
it against its own parent.

The mechanism of the 53515 px is NOT established, and it is not written as fact
here. Measured on the two committed crops: the R channel is identical in every one
of them, G differs on 41462 px (max delta 128) and B on 53512 px (max delta 255),
and the differing pixels are pairs like (255,0,0) <-> (255,44,141) — a flip between
flat red and a real colour, across the whole panel, not a texture cross-fading.
"Animation phase" is a hypothesis; five runs of one ELF clustered is what would
settle it, and if it is not animation then the renderer is nondeterministic in a way
that matters (review-10 open question 1).

THE MAP IS ONLY A CONTROL WHEN IT HAS TWO VALUES (review-10 F1). The painted/clear
map is 1 wherever a pixel is not the exact clear colour. On the current build the
object pass covers the whole panel, so BOTH sides are 96000/96000 painted: the map
is a constant, a constant cannot differ, and `geom: 0 px` there is arithmetic and
not a measurement. A comparison that cannot fail enforces nothing — which is the
same argument AGENTS.md 2 makes about a permanently red gate — so `geom` now
REFUSES a degenerate side and exits 3 rather than printing a number a sidecar would
read as a pass. `census` still prints the raw counts, and the honest statement about
the current panel lives in docs/CONTINUATION.md: until the guest owns a frame-locked
screenshot path (`render.h`: `screenshot_path` is "Linux only"), no panel-derived
quantity is a picture claim for a fully painted panel.

WHAT DOES SURVIVE, where the panel has both painted and clear pixels: the
painted-versus-clear map. Measured on the same two runs of one build the map is
identical (0 px) while the colour moved by 53515, because coverage does not depend
on which frame was up. That is also the quantity the census has always quoted (the
exact clear-colour count, 58464 in both builds), which is why the census reproduced
while the full-RGB diffs did not.

So, from here on:
  * `geom` is the comparison a sidecar quotes as the picture claim **only where it
    does not refuse** — that is, where both panels have painted AND clear pixels;
  * `rgb` is printed alongside it, labelled, and is NEVER a control on its own —
    a full-RGB number is only meaningful against a same-build control run taken on
    the SAME scene (batch 102's 0 px floor does not transfer to a different scene);
  * a real A/B needs two runs of the parent build and two of the child, and the
    parent-vs-parent number is the noise floor the child-vs-parent number has to
    clear. One run per side is not a measurement.

USAGE
  panel_geom.py census IMG [IMG ...]   exact clear-colour census per image
  panel_geom.py geom   A B             painted/clear map diff + full-RGB diff
Exit 0 on success, 1 on a size/shape violation, 2 on a usage or I/O error, 3 when
`geom` refuses because one side's map is degenerate (nothing to discriminate).
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

# The panel rectangle is panel_crop.py's contract, so it is IMPORTED and not
# re-spelled (review-10 F5). panel_crop.py exists because batch 84 and batch 85
# shipped two spellings of one rectangle and one of them silently lost a row and a
# column; a second definition here is how that happens again, and this file is the
# one now quoted as canonical.
from panel_crop import PANEL_SIZE

PANEL_W, PANEL_H = PANEL_SIZE

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
    total = PANEL_W * PANEL_H
    clear_a = total - sum(ma)
    clear_b = total - sum(mb)
    # The fractions are printed BEFORE the verdict, so a refusal still says what was
    # measured and a pass still says how much of the panel the map can see.
    print(f"geom: clear {clear_a}/{total} ({100.0 * clear_a / total:.1f}%) -> "
          f"{clear_b}/{total} ({100.0 * clear_b / total:.1f}%)")
    for side, path, clear in (("first", a, clear_a), ("second", b, clear_b)):
        if clear == 0 or clear == total:
            print(f"geom: REFUSED — the {side} panel ({path}) is "
                  f"{'entirely painted' if clear == 0 else 'entirely clear'}, so its "
                  f"painted/clear map is a constant and cannot differ from anything. "
                  f"A 0 px claim here would be arithmetic, not a measurement "
                  f"(review-10 F1). Quote a full-RGB diff against a same-scene, "
                  f"same-build control run, or land the frame-locked capture path.")
            return 3
    geom_diff = sum(1 for i in range(len(ma)) if ma[i] != mb[i])
    rgb_diff = sum(1 for i in range(len(pa)) if pa[i] != pb[i])
    print(f"geom: {geom_diff} px of {total} differ in the painted/clear map"
          f"  (painted {sum(ma)} -> {sum(mb)})")
    print(f"rgb:  {rgb_diff} px of {total} differ in full colour"
          f"  — NOT a control: same-build runs of one scene differ by tens of thousands")
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

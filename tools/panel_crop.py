#!/usr/bin/env python3
"""panel_crop.py — the ONE definition of the emulator's top-panel crop.

WHY THIS FILE EXISTS (review-5 F4). Every 3DS A/B since batch 84 has compared
the top screen of a 1280x1024 Mandarine capture, and the rectangle was spelled
out by hand in each batch's own throwaway snippet. Two consecutive batches then
used two different conventions for the same documented rectangle: batch 84's
crop is 400x240 and batch 85's is 399x239, because the second applied the
inclusive far corner as an exclusive one. Both sidecars said "400x240" and
"0 px of 96000"; one of them was measuring 95361 pixels and never compared a
panel column or a panel row. A crop that silently loses a row and a column is a
crop that silently loses whatever was in them.

So the rectangle lives here, once, and the crop CHECKS its own output: a crop
that is not exactly PANEL_SIZE is an error with a non-zero exit, never a
picture. `diff` refuses anything that is not a panel crop, so a number in a
sidecar cannot come from a differently-shaped pair.

THE RECTANGLE. Mandarine's window is 1280x1024 with the two screens side by
side at 400x240 each, centred, with the emulator's own frame-time overlay on
the BOTTOM screen. The top panel is the game's 3D view; the bottom one is the
log/overlay surface that differs between any two runs of any two builds and is
therefore never compared. Far corners are INCLUSIVE, which is what the sidecars
have always said: (440,253)-(839,492) is 400 columns by 240 rows.

USAGE
  panel_crop.py crop CAPTURE.png OUT.png   crop the top panel, assert its size
  panel_crop.py diff   A.png B.png         differing pixels + their bounding box
  panel_crop.py sha    IMG.png             sha256 of the raw RGB bytes

`crop` prints `crop: WxH sha256=... path=...`; `diff` prints `diff: N px of M`
plus a bbox line when N > 0; `sha` prints `sha256: ...`. Exit 0 on success, 1
on a size/shape violation (that is the whole point), 2 on a usage or I/O error.
"""
from __future__ import annotations

import hashlib
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.stderr.write("panel_crop: needs Pillow (as tools/publish_frame.sh does)\n")
    raise SystemExit(2)

# Inclusive near and far corners in the emulator's 1280x1024 window, as
# derived when the emulator window sat where the derivation put it. The
# window's position comes from the seeded Qt config, which drifts (a
# measured -13,-22 shift put the whole panel outside this rectangle
# while the game drew it perfectly), so these constants are now only
# the FALLBACK: find_panel() locates the panel in the capture itself.
PANEL_X0, PANEL_Y0, PANEL_X1, PANEL_Y1 = 440, 253, 839, 492
PANEL_SIZE = (PANEL_X1 - PANEL_X0 + 1, PANEL_Y1 - PANEL_Y0 + 1)


def find_panel(image: "Image.Image") -> tuple[int, int]:
    """The panel's top-left corner in a full-window capture.

    The emulator draws on black; the game always clears its screen to a
    colour, so the first non-black pixel row and column are the top
    screen's own. (The bottom screen and the frame-time overlay sit
    right and below, so they cannot move the origin.) Falls back to the
    documented constants when the capture holds no non-black pixel or
    the panel would not fit -- a wrong origin found loudly by the
    canary verdict is better than a silent crop of the wrong place.
    """
    px = image.load()
    # A capture whose window is filled from its own corner has no black
    # frame to locate against (the harness's synthetic captures are built
    # that way, with the panel at the documented constants): keep them.
    if px[0, 0] != (0, 0, 0):
        return PANEL_X0, PANEL_Y0
    min_x, min_y = image.width, image.height
    for y in range(image.height):
        for x in range(image.width):
            if px[x, y] != (0, 0, 0):
                if x < min_x:
                    min_x = x
                if y < min_y:
                    min_y = y
                break  # this row's first content; only the minima matter
    if min_x == image.width or min_x + PANEL_SIZE[0] > image.width or \
            min_y + PANEL_SIZE[1] > image.height:
        return PANEL_X0, PANEL_Y0
    # The location is only believed when the panel's own EDGES are there: at
    # least kEdgeRun non-black pixels along the found top row and along the
    # found left column. A synthetic capture with a lone marker on black (an
    # 11x11 square) has edges of 11 and keeps the documented constants, so it
    # cannot anchor the panel on the marker itself (the wrong-corner failure
    # the marker rect exists to catch). This replaced a "half the rectangle is
    # non-black" rule, which held while the 3DS drew only a clear colour and
    # objects, and broke once the scene drew (2026-11-17): a dark level-7
    # tunnel is under half non-black, the rule fell back to stale constants
    # (440,253) instead of the found (427,231), and `--expect hud` read the
    # wrong rectangle -- 0 canary pixels while the capture held all 121.
    edge_run = 32
    top = sum(1 for x in range(min_x, min_x + PANEL_SIZE[0]) if px[x, min_y] != (0, 0, 0))
    left = sum(1 for y in range(min_y, min_y + PANEL_SIZE[1]) if px[min_x, y] != (0, 0, 0))
    if top < edge_run or left < edge_run:
        return PANEL_X0, PANEL_Y0
    return min_x, min_y


def die(message: str, code: int = 1) -> "None":
    sys.stderr.write(f"panel_crop: {message}\n")
    raise SystemExit(code)


def open_rgb(path: str) -> "Image.Image":
    try:
        return Image.open(path).convert("RGB")
    except Exception as exc:  # unreadable, not an image, missing
        die(f"cannot read {path}: {exc}", 2)


def raw_sha(image: "Image.Image") -> str:
    return hashlib.sha256(image.tobytes()).hexdigest()


def require_panel(path: str) -> "Image.Image":
    """A crop that is not the documented panel is not comparable; refuse it."""
    image = open_rgb(path)
    if image.size != PANEL_SIZE:
        die(
            f"{path} is {image.size[0]}x{image.size[1]}, not the documented "
            f"panel {PANEL_SIZE[0]}x{PANEL_SIZE[1]}. Re-crop it with "
            f"`panel_crop.py crop`; do not compare mixed shapes."
        )
    return image


def do_crop(capture_path: str, out_path: str) -> int:
    image = open_rgb(capture_path)
    x0, y0 = find_panel(image)
    if image.width < x0 + PANEL_SIZE[0] or image.height < y0 + PANEL_SIZE[1]:
        die(f"{capture_path} is {image.size[0]}x{image.size[1]}, too small for "
            f"the panel at ({x0},{y0}) ({PANEL_SIZE[0]}x{PANEL_SIZE[1]} needed)")
    # PIL's box is [left, upper, right, lower) -- hence the size form here.
    # Forgetting that is exactly the 399x239 bug this file exists to stop.
    crop = image.crop((x0, y0, x0 + PANEL_SIZE[0], y0 + PANEL_SIZE[1]))
    if crop.size != PANEL_SIZE:
        die(f"crop produced {crop.size[0]}x{crop.size[1]}, expected "
            f"{PANEL_SIZE[0]}x{PANEL_SIZE[1]}")
    Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    crop.save(out_path, optimize=True)
    # Re-read what was actually written: the sidecar's hash has to describe the
    # artifact that gets committed, not the image object that was in memory.
    written = require_panel(out_path)
    print(f"crop: {written.size[0]}x{written.size[1]} "
          f"sha256={raw_sha(written)} path={out_path}")
    return 0


def do_diff(a_path: str, b_path: str) -> int:
    a, b = require_panel(a_path), require_panel(b_path)
    pa, pb = a.tobytes(), b.tobytes()
    width, height = PANEL_SIZE
    total = width * height
    differing = 0
    min_x, min_y = width, height
    max_x, max_y = -1, -1
    for y in range(height):
        row = y * width * 3
        if pa[row:row + width * 3] == pb[row:row + width * 3]:
            continue
        for x in range(width):
            i = row + x * 3
            if pa[i:i + 3] != pb[i:i + 3]:
                differing += 1
                if x < min_x:
                    min_x = x
                if x > max_x:
                    max_x = x
                if y < min_y:
                    min_y = y
                if y > max_y:
                    max_y = y
    print(f"diff: {differing} px of {total}")
    print(f"diff: sha256 {raw_sha(a)} vs {raw_sha(b)}")
    if differing:
        print(f"diff: bbox x {min_x}..{max_x}, y {min_y}..{max_y} "
              f"(panel columns/rows, 0-based)")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) == 3 and argv[0] == "crop":
        return do_crop(argv[1], argv[2])
    if len(argv) == 3 and argv[0] == "diff":
        return do_diff(argv[1], argv[2])
    if len(argv) == 2 and argv[0] == "sha":
        print(f"sha256: {raw_sha(open_rgb(argv[1]))}")
        return 0
    sys.stderr.write(__doc__ or "")
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

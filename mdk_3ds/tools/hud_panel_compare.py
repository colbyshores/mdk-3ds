#!/usr/bin/env python3
"""Compare a 400x240 top-screen capture's HUD panel with the two emulator
evidence crops (row-major mutant vs Morton build) of 2026-11-17.

The evidence PNGs are crop (330,190)-(400,240) enlarged 8x nearest-neighbour.
Their registration is NOT exact (the emulator window geometry drifts), so the
capture is searched over a shift window and the best-aligned luminance
correlation is reported for each build, on the pixels where the evidence crop
shows the panel rather than the background. Also prints the record's period
metric (rows y=200..237, cols x=342..399, quadratic detrend) for comparison.

  python3 mdk_3ds/tools/hud_panel_compare.py <capture.bmp|png>
"""
import os
import sys

import numpy as np
from PIL import Image

EVIDENCE = [("row-major", "docs/evidence/2026-11-17-hud-panel-a-rowmajor.png"),
            ("Morton", "docs/evidence/2026-11-17-hud-panel-b-morton.png")]


def lum(a):
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


def period(img):
    sub = img[200:238, 342:400]
    p = lum(sub).mean(axis=0)
    x = np.arange(len(p))
    p = p - np.polyval(np.polyfit(x, p, 2), x)
    spec = np.abs(np.fft.rfft(p)) ** 2
    f = np.fft.rfftfreq(len(p))
    per = np.where(f > 0, 1 / np.where(f > 0, f, 1), np.inf)
    band = (per >= 3) & (per <= 9)
    return per[1:][np.argmax(spec[1:])], per[band][np.argmax(spec[band])]


def main():
    # Evidence paths are repo-relative; resolve them from this file, not the cwd.
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    cap = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(float)
    if cap.shape[:2] != (240, 400):
        sys.exit(f"expected a 400x240 capture, got {cap.shape[1]}x{cap.shape[0]}")
    dom, fine = period(cap)
    print(f"capture period metric: dominant={dom:.1f}px fine-band-peak={fine:.1f}px "
          f"(evidence: row-major 11.6/5.3, Morton 29.0/6.4)")
    for label, path in EVIDENCE:
        em = np.asarray(Image.open(path).convert("RGB")).astype(float)[::8, ::8]
        mask = np.abs(em - em[0, 0]).max(axis=2) > 12
        best = None
        # The crop's window ends at the screen's right/bottom edge, so only
        # up/left shifts exist.
        for dy in range(-20, 1):
            for dx in range(-24, 1):
                y0, x0 = 190 + dy, 330 + dx
                win = cap[y0:y0 + 50, x0:x0 + 70]
                r = np.corrcoef(lum(win)[mask], lum(em)[mask])[0, 1]
                if np.isnan(r):
                    continue
                if best is None or r > best[0]:
                    best = (r, dx, dy)
        r, dx, dy = best
        print(f"vs {label:9s}: best shift ({dx:+d},{dy:+d}) luminance r={r:+.3f} over {mask.sum()} panel px")


if __name__ == "__main__":
    main()

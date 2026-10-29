#!/usr/bin/env python3
"""compare_frames.py -- the frame golden comparator (SPEC.md 11.3, SPEC-COMPLETION 3.4).

Two uncompressed 32-bit BMPs (the C# oracle's Bmp.Write format: 54-byte
header, negative height, BGRA, top-down) and a tolerance, and a verdict:

    PASS   every pixel channel differs by at most --tolerance
    FAIL   any pixel is further apart than that

The tolerance is deliberately small and it is deliberately NOT a knob to turn
until a run goes green. The original look is a flat-colour / indexed look with
no dithering in the path, so the reference tolerance is 0 (byte-exact). The
enhanced look's dithering is the only case SPEC.md 6.6 allows +/-1, and it is
still passed explicitly, never defaulted.

The exit code is 0 on PASS and 1 on FAIL, and a FAIL prints the first offending
pixels so a regression is diagnosable from the gate log alone. A harness that
cannot say WHERE two frames differ cannot tell a regression from a camera that
was never in the same place, so the first ten differing pixels are listed.

    python3 tests/frames/compare_frames.py reference.bmp actual.bmp [--tolerance N]
"""

import argparse
import struct
import sys

HEADER = 54


class FrameError(Exception):
    pass


def read_bmp(path):
    """Return (width, height, pixels) for a 32-bit uncompressed BMP."""
    with open(path, "rb") as handle:
        blob = handle.read()
    if len(blob) < HEADER:
        raise FrameError(f"{path}: too small to be a BMP")
    if blob[0:2] != b"BM":
        raise FrameError(f"{path}: not a BMP (no 'BM' magic)")
    offset = struct.unpack_from("<I", blob, 10)[0]
    size = struct.unpack_from("<I", blob, 14)[0]
    if size != 40:
        raise FrameError(f"{path}: unexpected DIB header size {size} (want 40)")
    width = struct.unpack_from("<i", blob, 18)[0]
    height = struct.unpack_from("<i", blob, 22)[0]
    planes = struct.unpack_from("<H", blob, 26)[0]
    bpp = struct.unpack_from("<H", blob, 28)[0]
    compression = struct.unpack_from("<I", blob, 30)[0]
    if bpp != 32 or compression != 0:
        raise FrameError(f"{path}: {bpp} bpp, compression {compression} (want 32 bpp, none)")
    if planes != 1:
        raise FrameError(f"{path}: planes={planes} (want 1)")
    top_down = height < 0
    rows = abs(height)
    need = offset + width * rows * 4
    if len(blob) < need:
        raise FrameError(f"{path}: truncated pixel data ({len(blob)} < {need})")
    pixels = blob[offset:offset + width * rows * 4]
    if not top_down:
        # A bottom-up BMP is stored in the opposite row order; normalise it so
        # the comparison is on the picture, not on the container's choice.
        row = width * 4
        pixels = b"".join(pixels[(rows - 1 - r) * row:(rows - r) * row] for r in range(rows))
    return width, rows, pixels


def measure(reference_path, actual_path, tolerance):
    """Return (ok, bad_percent, max_delta, mean_delta) for the machine-readable
    mode. The gate records these numbers and compares live measurements against
    them, so they must come from THIS function and not from parsing the human
    summary text."""
    ref_w, ref_h, ref = read_bmp(reference_path)
    act_w, act_h, act = read_bmp(actual_path)
    if (ref_w, ref_h) != (act_w, act_h):
        return False, 100.0, 255, 255.0
    bad = 0
    worst = 0
    total_channels = 0
    row_bytes = ref_w * 4
    for y in range(ref_h):
        base = y * row_bytes
        for x in range(ref_w):
            at = base + x * 4
            pixel_bad = False
            for c in range(4):
                d = abs(ref[at + c] - act[at + c])
                total_channels += d
                if d > tolerance:
                    pixel_bad = True
                if d > worst:
                    worst = d
            if pixel_bad:
                bad += 1
    total = ref_w * ref_h
    return (bad == 0,
            (100.0 * bad / total) if total else 0.0,
            worst,
            (total_channels / (total * 4.0)) if total else 0.0)


def compare(reference_path, actual_path, tolerance):
    ref_w, ref_h, ref = read_bmp(reference_path)
    act_w, act_h, act = read_bmp(actual_path)
    if (ref_w, ref_h) != (act_w, act_h):
        return False, [f"size differs: reference {ref_w}x{ref_h}, actual {act_w}x{act_h}"]

    worst = 0
    worst_at = None
    bad_pixels = 0
    total = 0
    channel_total = 0
    first = []
    row_bytes = ref_w * 4
    for y in range(ref_h):
        base = y * row_bytes
        for x in range(ref_w):
            at = base + x * 4
            total += 1
            pixel_bad = False
            for c in range(4):
                d = abs(ref[at + c] - act[at + c])
                channel_total += d
                if d > tolerance:
                    pixel_bad = True
                if d > worst:
                    worst = d
                    worst_at = (x, y, c)
            if pixel_bad:
                bad_pixels += 1
                if len(first) < 10:
                    r0 = ref[at:at + 4]
                    a0 = act[at:at + 4]
                    first.append(
                        f"  ({x},{y}) reference BGRA={tuple(r0)} actual BGRA={tuple(a0)}"
                    )

    mean = channel_total / (total * 4) if total else 0.0
    summary = (
        f"{total} pixels, {bad_pixels} outside +/-{tolerance} "
        f"({(100.0 * bad_pixels / total) if total else 0.0:.3f}%), "
        f"max channel delta {worst}"
        + (f" at ({worst_at[0]},{worst_at[1]}) channel {worst_at[2]}" if worst_at else "")
        + f", mean channel delta {mean:.4f}"
    )
    if bad_pixels == 0:
        return True, [summary]
    return False, [summary] + first


def main():
    parser = argparse.ArgumentParser(description="compare two 32-bit BMP frames")
    parser.add_argument("reference")
    parser.add_argument("actual")
    parser.add_argument("--tolerance", type=int, default=0,
                        help="largest accepted per-channel delta (0 = byte-exact)")
    parser.add_argument("--measure", action="store_true",
                        help="print '<bad%%> <max> <mean>' only, for the manifest")
    args = parser.parse_args()

    if args.measure:
        try:
            ok, bad_pct, worst, mean = measure(args.reference, args.actual, args.tolerance)
        except (FrameError, OSError) as error:
            print(f"FRAME FAIL: {error}", file=sys.stderr)
            return 1
        print(f"{bad_pct:.3f} {worst} {mean:.4f}")
        return 0

    try:
        ok, lines = compare(args.reference, args.actual, args.tolerance)
    except (FrameError, OSError) as error:
        print(f"FRAME FAIL: {error}")
        return 1

    for line in lines:
        print(line)
    if ok:
        print(f"FRAME PASS: {args.actual} matches {args.reference} within +/-{args.tolerance}")
        return 0
    print(f"FRAME FAIL: {args.actual} differs from {args.reference} beyond +/-{args.tolerance}")
    return 1


if __name__ == "__main__":
    sys.exit(main())

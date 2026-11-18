#!/usr/bin/env python3
# ============================================================================
# hd_pack_convert.py -- the HD texture mod, converted for the 3DS.
#
#   mdk_3ds/tools/hd_pack_convert.py <mod dir> <out dir> [--max 512] [--jobs N]
#
# <mod dir> is an HD texture mod in the PC layout (docs/modding.md): the one the
# C# port's `--upscale-textures` makes (`mods/hd-textures/`, Real-ESRGAN, from the
# player's own copy of the game -- nothing upscaled is distributed, so there is
# no pack to download; mdk_3ds/tools/build_hd_pack.sh makes it). Every PNG under
# its `textures/` and `images/` becomes a `.t3x` at the same relative path under
# <out dir>, plus <out dir>/manifest.txt. docs/HD_TEXTURES_3DS_SPEC.md is the spec.
#
# PER IMAGE, IN ORDER:
#   1. BLEED. Every alpha-0 texel takes the colour of its nearest opaque texels,
#      flooded until none is left. tex3ds' gaussian mip filter and its ETC1 block
#      fit both average colour across alpha-0 texels; unbled, a cut-out's edge is
#      pulled toward black at every level. (The runtime's own rule for the
#      original textures is pica::prepareForFiltering; this is the same idea
#      carried to the whole image, because a mip chain reaches further than one
#      texel.)
#   2. SIZE. Each axis to a power of two (the PICA's rule; ETC1's 4x4 blocks are
#      then whole), at most --max (512: the pack's cap) and at least 8. KURT'S
#      SPRITE FRAMES (textures named K_*) are capped at --sprite-max (128): he is
#      about 80 px tall on the 3DS's 240 lines, and at 512 his 360 level-3 frames
#      were 12.6 MB -- an animation cycle (57 idle frames) overflowed the whole HD
#      budget and thrashed it (measured, 2026-11-18). A
#      texture's uv spans its whole image, so a non-POT image is RESAMPLED to the
#      POT size, not padded: nothing on the 3DS side has to undo a padding.
#   3. FORMAT. Any alpha below 255 -> ETC1A4 (ETC1 + 4-bit alpha), else ETC1.
#   4. ENCODE. `tex3ds -f etc1|etc1a4 -q high -m gaussian` (the ../forsaken
#      settings): the mip chain is built and compressed HERE, on the host, so the
#      3DS imports it whole and never encodes.
#
# Deterministic (same input bytes -> same output bytes) and incremental: an image
# whose source hash, size cap and tool settings match the manifest's line is not
# encoded again.
# ============================================================================
import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image

TEX3DS = os.environ.get("TEX3DS", "/opt/devkitpro/tools/bin/tex3ds")
SETTINGS = "q=high;m=gaussian;bleed=flood;v1"


def pow2_at_most(n, cap):
    p = 8
    while p < n and p < cap:
        p <<= 1
    return min(p, cap)


def bleed(rgba):
    """Every alpha-0 texel gets the mean colour of its already-coloured 8-neighbours,
    pass after pass, until every texel reachable from an opaque one is coloured."""
    rgb = rgba[..., :3].astype(np.float32)
    known = rgba[..., 3] > 0
    if known.all() or not known.any():
        return rgba
    h, w = known.shape
    for _ in range(max(h, w)):
        if known.all():
            break
        acc = np.zeros_like(rgb)
        cnt = np.zeros((h, w), np.float32)
        kf = known.astype(np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dy == 0 and dx == 0:
                    continue
                ys = slice(max(dy, 0), h + min(dy, 0))
                yd = slice(max(-dy, 0), h + min(-dy, 0))
                xs = slice(max(dx, 0), w + min(dx, 0))
                xd = slice(max(-dx, 0), w + min(-dx, 0))
                acc[yd, xd] += rgb[ys, xs] * kf[ys, xs, None]
                cnt[yd, xd] += kf[ys, xs]
        grow = (~known) & (cnt > 0)
        if not grow.any():
            break
        rgb[grow] = acc[grow] / cnt[grow, None]
        known = known | grow
    out = rgba.copy()
    out[..., :3] = np.clip(np.rint(rgb), 0, 255).astype(np.uint8)
    return out


def convert(job):
    src, dst, cap = job
    with open(src, "rb") as f:
        data = f.read()
    digest = hashlib.sha256(data).hexdigest()[:16]
    img = Image.open(src).convert("RGBA")
    w0, h0 = img.size
    rgba = bleed(np.asarray(img, dtype=np.uint8))
    tw, th = pow2_at_most(w0, cap), pow2_at_most(h0, cap)
    img = Image.fromarray(rgba, "RGBA")
    if (tw, th) != (w0, h0):
        img = img.resize((tw, th), Image.LANCZOS)
    alpha = np.asarray(img, dtype=np.uint8)[..., 3]
    fmt = "etc1a4" if (alpha < 255).any() else "etc1"
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        staged = os.path.join(tmp, "in.png")
        img.save(staged)
        out = dst + ".tmp"
        r = subprocess.run([TEX3DS, "-f", fmt, "-q", "high", "-m", "gaussian", staged, "-o", out],
                           capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(out):
            return (src, None, "tex3ds failed: " + (r.stderr or r.stdout).strip()[:200])
        os.replace(out, dst)
    return (src, (fmt, w0, h0, tw, th, os.path.getsize(dst), digest), None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mod")
    ap.add_argument("out")
    ap.add_argument("--max", type=int, default=512)
    ap.add_argument("--sprite-max", type=int, default=128)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    for v in (a.max, a.sprite_max):
        if v not in (8, 16, 32, 64, 128, 256, 512, 1024):
            sys.exit("hd_pack_convert: --max/--sprite-max must be a power of two from 8 to 1024 "
                     "(the PICA's limit)")
    if not os.path.exists(TEX3DS):
        sys.exit("hd_pack_convert: no tex3ds at %s (devkitPro tools; set TEX3DS)" % TEX3DS)

    manifest_path = os.path.join(a.out, "manifest.txt")
    old = {}
    if os.path.exists(manifest_path):
        for line in open(manifest_path):
            if line.startswith("#") or not line.strip():
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) == 10:
                old[f[0]] = f

    jobs, kept = [], {}
    for sub in ("textures", "images"):
        root = os.path.join(a.mod, sub)
        for dirpath, _, files in os.walk(root):
            for name in sorted(files):
                if not name.lower().endswith(".png"):
                    continue
                src = os.path.join(dirpath, name)
                rel = os.path.relpath(src, a.mod)[:-4]
                dst = os.path.join(a.out, rel + ".t3x")
                with open(src, "rb") as fh:
                    digest = hashlib.sha256(fh.read()).hexdigest()[:16]
                cap = a.sprite_max if (sub == "textures" and name.startswith("K_")) else a.max
                prev = old.get(rel)
                if prev and prev[7] == digest and prev[8] == str(cap) and prev[9] == SETTINGS \
                        and os.path.exists(dst):
                    kept[rel] = prev
                else:
                    jobs.append((src, dst, cap))
    if not jobs and not kept:
        sys.exit("hd_pack_convert: no PNG under %s/textures or %s/images" % (a.mod, a.mod))

    lines = dict(kept)
    failed = 0
    caps = {src: cap for src, _, cap in jobs}
    with ProcessPoolExecutor(max_workers=a.jobs) as pool:
        for src, info, err in pool.map(convert, jobs, chunksize=4):
            rel = os.path.relpath(src, a.mod)[:-4]
            if err:
                failed += 1
                print("hd_pack_convert: %s: %s" % (rel, err), file=sys.stderr)
                continue
            fmt, w0, h0, tw, th, size, digest = info
            lines[rel] = [rel, fmt, str(w0), str(h0), str(tw), str(th), str(size), digest,
                          str(caps[src]), SETTINGS]

    os.makedirs(a.out, exist_ok=True)
    tmp = manifest_path + ".tmp"
    with open(tmp, "w") as f:
        f.write("# key\tformat\tsrc_w\tsrc_h\tw\th\tbytes\tsrc_sha256_16\tmax\tsettings\n")
        for rel in sorted(lines):
            f.write("\t".join(lines[rel]) + "\n")
    os.replace(tmp, manifest_path)
    total = sum(int(v[6]) for v in lines.values())
    etc1a4 = sum(1 for v in lines.values() if v[1] == "etc1a4")
    print("hd_pack_convert: %d textures (%d ETC1A4, %d ETC1), %d encoded now, %d kept, %d failed, "
          "%.1f MB" % (len(lines), etc1a4, len(lines) - etc1a4, len(jobs) - failed, len(kept),
                       failed, total / 1048576.0))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

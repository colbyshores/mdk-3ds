#!/usr/bin/env python3
# ============================================================================
# hd_pack_convert.py -- the HD texture mod, converted for the 3DS.
#
#   mdk_3ds/tools/hd_pack_convert.py <mod dir> <out dir> [--max 512] [--jobs N]
#   mdk_3ds/tools/hd_pack_convert.py --from-pack <old pack> <out dir>
#
# <mod dir> is an HD texture mod in the PC layout (docs/modding.md): the one
# mdk_3ds/tools/build_hd_mod.py makes, the same as the C# port's
# `--upscale-textures` (`mods/hd-textures/`, Real-ESRGAN, from the player's own
# copy of the game -- nothing upscaled is distributed, so there is no pack to
# download; mdk_3ds/tools/build_hd_pack.sh makes it). Every PNG under
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
#   4. ENCODE. `tex3ds -f etc1|etc1a4 -q high -m gaussian -z none` (the
#      ../forsaken settings): the mip chain is built and compressed HERE, on the
#      host, so the 3DS imports it whole and never encodes.
#
# STORED UNCOMPRESSED (-z none, owner 2026-11-26; docs/3DS_ARCHITECTURE.md 5).
# tex3ds' default (-z auto) packs each file with LZ10/LZ11/Huffman/RLE, and the
# console then spends core 1's time share unpacking it: measured on the Old 3DS
# (old3ds-lan167, 268 MHz, 100 level-8 files), reading and unpacking ran at 0.77
# MB/s of file on core 1 at its 30% share (one file held the worker 390 ms),
# while reading alone ran at 3.5 MB/s. Raw, the worker only reads: about 1.7x the
# SD bytes (55 -> ~90 MB for levels 3-8) for ~2.7x the texels per second.
#
# --from-pack rewrites an existing pack (any tex3ds compression) uncompressed
# without re-encoding: the same ETC blocks, the same mip chain, byte for byte the
# file `tex3ds -z none` writes for the same image (unpack() below; the self-test
# `--check-unpack` proves it against tex3ds for every compression it picks). It
# is how a pack made before 2026-11-26 is brought over without its mod.
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
SETTINGS = "q=high;m=gaussian;bleed=flood;z=none;v1"


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
        r = subprocess.run([TEX3DS, "-f", fmt, "-q", "high", "-m", "gaussian", "-z", "none",
                            staged, "-o", out],
                           capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(out):
            return (src, None, "tex3ds failed: " + (r.stderr or r.stdout).strip()[:200])
        os.replace(out, dst)
    return (src, (fmt, w0, h0, tw, th, os.path.getsize(dst), digest), None)


# ---------------------------------------------------------------------------
# THE COMPRESSIONS tex3ds' -z auto picks (libctru decompress()'s formats: the
# 4-byte header is the type byte and a 24-bit size, or 0 and a 32-bit size).
# ---------------------------------------------------------------------------
def _lz(src, pos, size, lz11):
    out = bytearray()
    while len(out) < size:
        flags = src[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if not flags & (0x80 >> bit):
                out.append(src[pos])
                pos += 1
                continue
            b0 = src[pos]
            if not lz11:
                length = (b0 >> 4) + 3
                disp = (((b0 & 0xF) << 8) | src[pos + 1]) + 1
                pos += 2
            elif b0 >> 4 == 0:
                length = (((b0 & 0xF) << 4) | (src[pos + 1] >> 4)) + 0x11
                disp = (((src[pos + 1] & 0xF) << 8) | src[pos + 2]) + 1
                pos += 3
            elif b0 >> 4 == 1:
                length = (((b0 & 0xF) << 12) | (src[pos + 1] << 4) | (src[pos + 2] >> 4)) + 0x111
                disp = (((src[pos + 2] & 0xF) << 8) | src[pos + 3]) + 1
                pos += 4
            else:
                length = (b0 >> 4) + 1
                disp = (((b0 & 0xF) << 8) | src[pos + 1]) + 1
                pos += 2
            for _ in range(length):
                out.append(out[-disp])
    return bytes(out[:size])


def _rle(src, pos, size):
    out = bytearray()
    while len(out) < size:
        flag = src[pos]
        pos += 1
        if flag & 0x80:
            out += bytes([src[pos]]) * ((flag & 0x7F) + 3)
            pos += 1
        else:
            n = (flag & 0x7F) + 1
            out += src[pos:pos + n]
            pos += n
    return bytes(out[:size])


def _huff8(src, pos, size, start):
    # The tree size byte follows the header (at `pos`). Node addresses are even/odd
    # relative to the header's start (`start`), not to the file: a .t3x header is
    # 5 + 12n bytes, odd for one subtexture.
    tree = pos
    tree_size = (src[tree] + 1) * 2
    bits = tree + tree_size
    out = bytearray()
    node = tree + 1
    word = 0
    left = 0
    while len(out) < size:
        if left == 0:
            word = int.from_bytes(src[bits:bits + 4], "little")
            bits += 4
            left = 32
        bit = (word >> 31) & 1
        word = (word << 1) & 0xFFFFFFFF
        left -= 1
        value = src[node]
        child = start + ((node - start) & ~1) + (value & 0x3F) * 2 + 2 + bit
        if value & (0x80 >> bit):
            out.append(src[child])
            node = tree + 1
        else:
            node = child
    return bytes(out)


def unpack(t3x):
    """A tex3ds file with its texture data stored raw (the -z none layout), or
    None for a file this does not understand."""
    n = t3x[0] | (t3x[1] << 8)
    if not 1 <= n <= 64:
        return None
    head = 5 + 12 * n
    kind = t3x[head]
    size = t3x[head + 1] | (t3x[head + 2] << 8) | (t3x[head + 3] << 16)
    pos = head + 4
    if size == 0:
        size = int.from_bytes(t3x[pos:pos + 4], "little")
        pos += 4
    if kind == 0x00:
        raw = t3x[pos:pos + size]
    elif kind in (0x10, 0x11):
        raw = _lz(t3x, pos, size, kind == 0x11)
    elif kind == 0x30:
        raw = _rle(t3x, pos, size)
    elif kind == 0x28:
        raw = _huff8(t3x, pos, size, head)
    else:
        return None
    if len(raw) != size or size >= 1 << 24:
        return None
    return bytes(t3x[:head]) + bytes([0, size & 0xFF, (size >> 8) & 0xFF, size >> 16]) + raw


def from_pack(old, out):
    """Every .t3x of `old` (its manifest's lines) rewritten raw into `out`."""
    lines, failed = [], 0
    for line in open(os.path.join(old, "manifest.txt")):
        if line.startswith("#") or not line.strip():
            continue
        f = line.rstrip("\n").split("\t")
        with open(os.path.join(old, f[0] + ".t3x"), "rb") as fh:
            raw = unpack(fh.read())
        if raw is None:
            failed += 1
            print("hd_pack_convert: %s: compression not understood" % f[0], file=sys.stderr)
            continue
        dst = os.path.join(out, f[0] + ".t3x")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst + ".tmp", "wb") as fh:
            fh.write(raw)
        os.replace(dst + ".tmp", dst)
        f[6] = str(len(raw))
        f[9] = f[9].replace(";v1", ";z=none;v1") if "z=none" not in f[9] else f[9]
        lines.append(f)
    with open(os.path.join(out, "manifest.txt.tmp"), "w") as fh:
        fh.write("# key\tformat\tsrc_w\tsrc_h\tw\th\tbytes\tsrc_sha256_16\tmax\tsettings\n")
        for f in sorted(lines):
            fh.write("\t".join(f) + "\n")
    os.replace(os.path.join(out, "manifest.txt.tmp"), os.path.join(out, "manifest.txt"))
    print("hd_pack_convert: %d textures rewritten raw, %d failed, %.1f MB" %
          (len(lines), failed, sum(int(f[6]) for f in lines) / 1048576.0))
    return 1 if failed else 0


def check_unpack():
    """unpack() against tex3ds itself: random images through every compression
    -z auto can pick, each unpacked and compared with the -z none file."""
    rng = np.random.default_rng(7)
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for i, (w, h) in enumerate(((8, 8), (64, 32), (128, 256), (512, 512))):
            rgba = rng.integers(0, 256, (h, w, 4), dtype=np.uint8)
            if i % 2:
                rgba[..., :3] //= 16   # runs, for RLE and LZ
            rgba[..., 3] = np.where(rgba[..., 3] > 60, 255, 0)
            png = os.path.join(tmp, "in.png")
            Image.fromarray(rgba, "RGBA").save(png)
            files = {}
            for z in ("none", "lz10", "lz11", "huff", "rle", "auto"):
                o = os.path.join(tmp, z + ".t3x")
                subprocess.run([TEX3DS, "-f", "etc1a4", "-q", "low", "-z", z, png, "-o", o],
                               check=True, capture_output=True)
                files[z] = open(o, "rb").read()
            for z, data in files.items():
                if unpack(data) != files["none"]:
                    bad += 1
                    print("hd_pack_convert: unpack MISMATCH %dx%d -z %s" % (w, h, z),
                          file=sys.stderr)
    print("hd_pack_convert: unpack check %s" % ("FAILED" if bad else "PASS"))
    return 1 if bad else 0


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--check-unpack":
        return check_unpack()
    if len(sys.argv) == 4 and sys.argv[1] == "--from-pack":
        return from_pack(sys.argv[2], sys.argv[3])
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

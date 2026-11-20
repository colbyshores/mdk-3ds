#!/usr/bin/env python3
# ============================================================================
# kurt_etc1_convert.py -- Kurt's ORIGINAL sprite frames as ETC1A4, for the 3DS.
#
#   mdk_3ds/tools/kurt_etc1_convert.py [<out dir>] [--levels 3,4,5,6,7,8]
#                                      [--mips] [--jobs N] [--check]
#
# Default <out dir>: build-kurt-etc1/ at the repo root (ignored as build-*/, and
# OUTSIDE mdk_3ds/build/ so `make clean` keeps it). mdk_3ds/tools/stage_romfs.sh
# runs this and links ONE file of the result into the RomFS, romfs:/kurt/kurt.pak
# (kurt_pack.py; main_3ds.cpp, kKurtPackPath). NOTHING HERE IS COMMITTED: the
# frames are the player's own TRAVSPRT.BNI, read from MDK_DATA_DIR (default
# <repo>/MDK).
#
# WHY ROMFS AND NOT THE HD PACK'S sdmc:/3ds/mdk/hd/. The HD pack is an optional
# add-on made on a GPU (Real-ESRGAN) and copied to the card by hand; this set is
# the ORIGINAL frames, derived from the same game data the build already stages
# into the RomFS, so it ships with every .3dsx/.cia the build makes and the
# hardware loop (hw_cycle.sh pushes only the .3dsx) needs no separate copy. It is
# small: 408 frames, 1.2 MB of .t3x (tex3ds LZ-compresses by default; the GPU
# copies are 14.75 MB for the whole set, which is why only the start set is
# loaded at level load). The WORK LAYOUT is the HD pack's own: a manifest.txt
# with the same ten columns, textures/NAME.t3x for every level,
# textures/LEVELn/NAME.t3x where a level's palette shows a frame differently (one
# frame, K_LOOKD_0, measured on the GOG data). WHAT SHIPS is kurt.pak, those
# files behind one index (kurt_pack.py, mdk_core/src/formats/kurt_pack.h): 408
# loose files opened one by one cost 4.6 s of an Old-3DS level load.
#
# PER FRAME (the exporter, mdk_3ds/tools/kurt_etc1/kurt_frames_export.cpp, does
# the decoding with mdk_core's own parsers and the level's own palette):
#   1. EXPAND by the 3DS's rule: index 0 transparent, else the palette's RGB at
#      alpha 255 (Kurt's alpha IS a colour key: 0 or 255, nothing between).
#   2. BLEED (hd_pack_convert.bleed): every alpha-0 texel takes its nearest opaque
#      colours, flooded. ETC1 fits one colour pair per 4x4 block and alpha-0
#      texels are in those blocks; unbled they pull the edge toward black.
#   3. PAD, NOT RESAMPLE, to a power of two each way (>= 8, the PICA's rule), the
#      image at the TOP-LEFT and the bleed carried into the padding. The original
#      resolution is kept texel for texel: the runtime maps the frame's rectangle
#      (pica::compressedFrameRect) instead of the whole texture.
#      EXCEPT A FRAME PAST 256 (kurt_pack.fit_size): one whose pad would pass
#      256x256 -- the chute's K_CHUTEC (339..347 x 288..295) and K_CHUTE_4/5, and
#      K_LOOKD_0 (287x343) -- would be a 512x512 texture, 262,144 bytes as ETC1A4,
#      four times a 256x256 one; the twelve K_CHUTEC frames alone were 3 MB, and
#      the chute's open/sway cycle churned them through the residency every frame
#      (the owner's level-7 hitches, 2026-11-19). Such a frame is DOWNSCALED to fit
#      256x256 (aspect kept, the long side 256): the colour LANCZOS-resampled from
#      the bled frame (so no transparent texel pulls an edge dark), the alpha
#      area-resampled and RE-THRESHOLDED at half (so it is still a colour key, 0
#      or 255, never between), then padded and bled as any frame. The manifest's
#      `max` column says "256" for it; the pack carries the image's size beside the
#      frame's, and the runtime draws the frame at its ORIGINAL size through the
#      image's rectangle -- Kurt and the chute are exactly as large on screen as
#      before, at about three quarters of the texel density (on the 3DS's 240
#      lines a sprite pixel is two thirds of a screen pixel anyway).
#   4. ENCODE: `tex3ds -f etc1a4 -q high` (+ `-m gaussian` under --mips; off by
#      default: owner 2026-11-19, conserve memory -- a chain is a third more, and
#      a sprite pixel is 1/360 of the view's height, so on the 3DS's 240 lines
#      Kurt is drawn at two thirds of his frame's size, where level 0 alone is
#      sampled without aliasing worth a third more memory). NO Y-FLIP: the image is fed
#      top row first, exactly as hd_pack_convert.py feeds the HD frames, and the
#      backend draws both through the same tex3ds orientation (06_draw.inc).
#
# THE LOOK, AND WHY IT IS ACCEPTED. ETC1's colour is one base pair and a
# modifier table per 4x4 block, so a sharp colour edge inside a block softens
# (the "4x4 block look"). The owner has ruled performance over accuracy on the
# 3DS and saw little difference against RGBA4. The ALPHA is lossless: ETC1A4
# carries 4 explicit bits a texel, and Kurt's 0/255 become 0/15 exactly, so the
# alpha test's cut-out is the original's texel for texel (--check proves it on
# every frame it encodes).
#
# Deterministic (same input bytes -> same output bytes) and incremental: a frame
# whose image hash and settings match the manifest's line is not encoded again.
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hd_pack_convert import bleed  # noqa: E402  (the HD pack's own flood)
from kurt_pack import FIT_CAP, fit_size, write_pack  # noqa: E402  (the one file the RomFS carries)

TEX3DS = os.environ.get("TEX3DS", "/opt/devkitpro/tools/bin/tex3ds")
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
EXPORTER_DIR = os.path.join(ROOT, "mdk_3ds", "tools", "kurt_etc1")


def settings(mips):
    return "orig;etc1a4;q=high;%s;bleed=flood;pad=pot;v1" % ("m=gaussian" if mips else "nomip")


def pot(n):
    p = 8
    while p < n:
        p <<= 1
    return p


def downscale(rgba, iw, ih):
    """A colour-keyed frame resampled to iw x ih: the colour by LANCZOS from the
    BLED frame (its alpha-0 texels carry their opaque neighbours' colours, so the
    filter's taps across the key do not pull the edge toward black), the alpha by
    area (BOX: the share of the texel the frame covered) RE-THRESHOLDED at half,
    so the result is a colour key again -- 0 or 255 exactly, nothing between."""
    bled = bleed(rgba)
    rgb = np.asarray(Image.fromarray(np.ascontiguousarray(bled[..., :3]), "RGB")
                     .resize((iw, ih), Image.LANCZOS), np.uint8)
    cover = np.asarray(Image.fromarray(np.ascontiguousarray(rgba[..., 3]), "L")
                       .resize((iw, ih), Image.BOX), np.uint8)
    out = np.zeros((ih, iw, 4), np.uint8)
    out[..., :3] = rgb
    out[..., 3] = np.where(cover >= 128, 255, 0).astype(np.uint8)
    return out


def pad_bled(rgba, tw, th):
    """The image at the top-left of a tw x th canvas, the padding bled from it."""
    h, w = rgba.shape[:2]
    canvas = np.zeros((th, tw, 4), np.uint8)
    canvas[:h, :w] = rgba
    return bleed(canvas) if (tw, th) != (w, h) else bleed(rgba)


def alpha_plane_etc1a4(raw, w, h):
    """The alpha nibbles of a raw ETC1A4 level 0 (`tex3ds -r -z none`: a 4-byte
    "uncompressed" header, then the texels), as an image top row first. The 3DS
    layout (cf. ../forsaken tools/texture/t3x2png.py): 8x8 tiles of 2x2 blocks in
    raster order, each block 8 alpha bytes then 8 colour bytes, the nibbles
    column-major in the block (index x*4+y), the LOW nibble first. Measured here
    against tex3ds on a Kurt frame (every other nibble order, and a row flip, all
    disagree with the source in thousands of texels): tex3ds' own block rows run
    from the image's top, as its subtexture's top = 1.0 says."""
    raw = raw[4:]
    out = np.zeros((h, w), np.uint8)
    bw = w // 4
    tiles_x = bw // 2
    for by in range(h // 4):
        for bx in range(bw):
            tile = (by // 2) * tiles_x + (bx // 2)
            off = (tile * 4 + (by % 2) * 2 + (bx % 2)) * 16
            block = raw[off:off + 8]
            for x in range(4):
                for y in range(4):
                    n = x * 4 + y
                    byte = block[n // 2]
                    out[by * 4 + y, bx * 4 + x] = (byte >> 4) if (n & 1) else (byte & 15)
    return out


def convert(job):
    src, dst, w, h, mips, check = job
    with open(src, "rb") as f:
        data = f.read()
    if len(data) != w * h * 4:
        return (src, None, "size %d is not %dx%dx4" % (len(data), w, h))
    rgba = np.frombuffer(data, np.uint8).reshape(h, w, 4)
    if w > 1024 or h > 1024:
        return (src, None, "%dx%d is over the PICA's 1024" % (w, h))
    iw, ih = fit_size(w, h)
    if (iw, ih) != (w, h):
        rgba = downscale(rgba, iw, ih)
    tw, th = pot(iw), pot(ih)
    img = Image.fromarray(pad_bled(rgba, tw, th), "RGBA")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        staged = os.path.join(tmp, "in.png")
        img.save(staged)
        args = [TEX3DS, "-f", "etc1a4", "-q", "high"] + (["-m", "gaussian"] if mips else [])
        out = dst + ".tmp"
        r = subprocess.run(args + [staged, "-o", out], capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(out):
            return (src, None, "tex3ds failed: " + (r.stderr or r.stdout).strip()[:200])
        if check:
            # The same encode, raw and uncompressed: its alpha must be the frame's
            # colour key exactly, at the top-left (the orientation the runtime's
            # rectangle assumes), and transparent in the padding.
            rawf = os.path.join(tmp, "raw.bin")
            r = subprocess.run([TEX3DS, "-f", "etc1a4", "-q", "high", "-r", "-z", "none",
                                staged, "-o", rawf], capture_output=True, text=True)
            if r.returncode != 0:
                return (src, None, "tex3ds -r failed")
            alpha = alpha_plane_etc1a4(open(rawf, "rb").read(), tw, th)
            want = np.zeros((th, tw), np.uint8)
            want[:ih, :iw] = np.where(rgba[..., 3] > 0, 15, 0)
            if not np.array_equal(alpha, want):
                os.remove(out)
                return (src, None, "CHECK: the encoded alpha is not the colour key "
                                   "(%d texels differ)" % int((alpha != want).sum()))
        os.replace(out, dst)
    return (src, (tw, th, os.path.getsize(dst), "orig" if (iw, ih) == (w, h) else str(FIT_CAP)),
            None)


def build_exporter(build_dir):
    # THE HOST COMPILER, named on the command line: stage_romfs.sh runs this from
    # the 3DS Makefile, which exports CXX=arm-none-eabi-g++ (a command-line
    # variable beats the environment's), and a 3DS-compiled exporter cannot run
    # here. HOST_CXX overrides it.
    r = subprocess.run(["make", "-s", "-C", EXPORTER_DIR, "BUILD=" + build_dir,
                        "CXX=" + os.environ.get("HOST_CXX", "g++")] +
                       # Under a parent make, its jobserver is inherited.
                       ([] if "MAKEFLAGS" in os.environ else ["-j%d" % (os.cpu_count() or 4)]))
    if r.returncode != 0:
        sys.exit("kurt_etc1_convert: the exporter did not build")
    return os.path.join(build_dir, "kurt_frames_export")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default=os.path.join(ROOT, "build-kurt-etc1"))
    ap.add_argument("--levels", default="3,4,5,6,7,8")
    ap.add_argument("--mips", action="store_true",
                    help="a gaussian mip chain per frame (a third more bytes)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--check", action="store_true",
                    help="re-encode raw and prove the alpha is the colour key, per frame")
    a = ap.parse_args()
    if not os.path.exists(TEX3DS):
        sys.exit("kurt_etc1_convert: no tex3ds at %s (devkitPro tools; set TEX3DS)" % TEX3DS)
    levels = [int(v) for v in a.levels.split(",") if v]
    out = os.path.abspath(a.out)
    work = os.path.join(out, ".work")
    os.makedirs(work, exist_ok=True)

    exporter = build_exporter(os.path.join(work, "exporter-host"))
    frames_dir = os.path.join(work, "frames")
    env = dict(os.environ)
    env.setdefault("MDK_DATA_DIR", os.path.join(ROOT, "MDK"))
    if not os.path.isdir(env["MDK_DATA_DIR"]):
        sys.exit("kurt_etc1_convert: no MDK data at %s (set MDK_DATA_DIR)" % env["MDK_DATA_DIR"])
    r = subprocess.run([exporter, frames_dir] + [str(v) for v in levels], env=env)
    if r.returncode != 0:
        sys.exit("kurt_etc1_convert: the exporter failed")

    # (level, name) -> key; a name's all-levels file is its most common key (ties:
    # the lowest key, so the choice is deterministic), the other keys go to the
    # levels that show them.
    by_name = {}
    sizes = {}
    for line in open(os.path.join(frames_dir, "index.tsv")):
        if line.startswith("#") or not line.strip():
            continue
        level, name, w, h, key = line.rstrip("\n").split("\t")
        by_name.setdefault(name, {})[int(level)] = key
        sizes[(name, key)] = (int(w), int(h))
    placed = {}   # rel (no extension) -> (name, key)
    for name in sorted(by_name):
        keys = by_name[name]
        counts = {}
        for k in keys.values():
            counts[k] = counts.get(k, 0) + 1
        common = sorted(counts, key=lambda k: (-counts[k], k))[0]
        placed["textures/" + name] = (name, common)
        for level in sorted(keys):
            if keys[level] != common:
                placed["textures/LEVEL%d/%s" % (level, name)] = (name, keys[level])

    manifest_path = os.path.join(out, "manifest.txt")
    old = {}
    if os.path.exists(manifest_path):
        for line in open(manifest_path):
            if line.startswith("#") or not line.strip():
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) == 10:
                old[f[0]] = f

    setting = settings(a.mips)
    jobs, kept, digests = [], {}, {}
    for rel in sorted(placed):
        name, key = placed[rel]
        w, h = sizes[(name, key)]
        src = os.path.join(frames_dir, "%s@%s.rgba" % (name, key))
        with open(src, "rb") as fh:
            digest = hashlib.sha256(fh.read()).hexdigest()[:16]
        digests[rel] = digest
        dst = os.path.join(out, rel + ".t3x")
        prev = old.get(rel)
        cap = "orig" if fit_size(w, h) == (w, h) else str(FIT_CAP)
        if prev and prev[7] == digest and prev[8] == cap and prev[9] == setting \
                and os.path.exists(dst) and not a.check:
            kept[rel] = prev
        else:
            jobs.append((src, dst, w, h, a.mips, a.check))

    lines = dict(kept)
    failed = 0
    with ProcessPoolExecutor(max_workers=a.jobs) as pool:
        for job, (src, info, err) in zip(jobs, pool.map(convert, jobs, chunksize=4)):
            rel = os.path.relpath(job[1], out)[:-4]
            if err:
                failed += 1
                print("kurt_etc1_convert: %s: %s" % (rel, err), file=sys.stderr)
                continue
            tw, th, size, cap = info
            w, h = job[2], job[3]
            lines[rel] = [rel, "etc1a4", str(w), str(h), str(tw), str(th), str(size),
                          digests[rel], cap, setting]

    # Files no longer named by the set (a re-export with other data) go.
    wanted = set(os.path.join(out, rel + ".t3x") for rel in lines)
    for dirpath, _, files in os.walk(os.path.join(out, "textures")):
        for name in files:
            path = os.path.join(dirpath, name)
            if name.endswith(".t3x") and path not in wanted:
                os.remove(path)

    tmp = manifest_path + ".tmp"
    with open(tmp, "w") as f:
        f.write("# key\tformat\tsrc_w\tsrc_h\tw\th\tbytes\tsrc_sha256_16\tmax\tsettings\n")
        for rel in sorted(lines):
            f.write("\t".join(lines[rel]) + "\n")
    os.replace(tmp, manifest_path)
    # THE PACK, from the manifest just written: rewritten only when its bytes
    # change. A failed frame is not in the manifest, so it is not in the pack
    # (the game draws it from its palette indices).
    pack_path, pack_frames, pack_bytes, start_bytes, rewritten = write_pack(out)
    print("kurt_etc1_convert: %s: %d frames, %d bytes, start set %d bytes%s" %
          (os.path.basename(pack_path), pack_frames, pack_bytes, start_bytes,
           "" if rewritten else " (unchanged)"))
    files = sum(int(v[6]) for v in lines.values())
    gpu = sum(int(v[4]) * int(v[5]) for v in lines.values())
    # Per animation, every-level copies only (a level's own copy replaces one):
    # the GPU bytes each animation costs whole, and how many frames were fitted.
    per = {}
    for rel, v in lines.items():
        if rel.count("/") != 1:
            continue
        anim = rel.split("/")[-1].rpartition("_")[0]
        row = per.setdefault(anim, [0, 0, 0])
        row[0] += 1
        row[1] += int(v[4]) * int(v[5])
        row[2] += v[8] != "orig"
    for anim in sorted(per):
        n, gpu_bytes, fitted = per[anim]
        print("kurt_etc1_convert: %-9s %3d frames, %8d GPU bytes%s" %
              (anim, n, gpu_bytes, (" (%d fitted to %d)" % (fitted, FIT_CAP)) if fitted else ""))
    print("kurt_etc1_convert: %d frames, %d encoded now, %d kept, %d failed%s; %.2f MB of "
          ".t3x, %.2f MB as ETC1A4 level 0 on the GPU" %
          (len(lines), len(jobs) - failed, len(kept), failed, " (checked)" if a.check else "",
           files / 1048576.0, gpu / 1048576.0))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

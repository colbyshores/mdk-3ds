#!/usr/bin/env python3
# ============================================================================
# arena_etc1_convert.py -- the ORIGINAL arena textures as ETC1/ETC1A4, for the
# 3DS: built ahead of time so the console never expands a palette or builds a
# mip chain at runtime.
#
#   mdk_3ds/tools/arena_etc1_convert.py [<out dir>] [--levels 3..8]
#                                       [--jobs N] [--check]
#
# Default <out dir>: build-arena-etc1/ at the repo root (ignored as build-*/,
# and OUTSIDE mdk_3ds/build/ so `make clean` keeps it). mdk_3ds/tools/
# stage_romfs.sh runs this and hard-links the result into the RomFS as
# romfs:/orig/ (manifest.txt + textures/). NOTHING HERE IS COMMITTED: the
# textures are the player's own level data, read from MDK_DATA_DIR (default
# <repo>/MDK).
#
# THE WORK LAYOUT is the HD pack's own (hd_pack_convert.py): a manifest.txt with
# the same ten columns and textures/<NAME>@<hdkey>.t3x for every distinct image.
# The NAME is the 8-char archive name lowercased and the key is game/hd_key.h's
# hdKeyOf -- the '@' form is what makes ModImages' indexPaths key the file by
# the hd key (mdu_core/src/game/mod_images.cpp: '@' splits the stem), so the
# runtime's surface lookup (main_3ds.cpp's hdPackLookup -> ModImages::resolve)
# finds it by exactly the key it computes from the parsed texture and the
# arena's palette. One file per key: a texture that looks the same through two
# arenas' (or two levels') palettes is encoded once.
#
# PER TEXTURE (the exporter, mdk_3ds/tools/arena_etc1/arena_tex_export.cpp,
# does the decoding with mdk_core's own parsers and the arena's own palette --
# LevelData::paletteOf, corridors borrowing by dropping the leading C -- exactly
# what the 3DS draws with):
#   1. EXPAND by the 3DS's rule: index 0 transparent, else the palette's RGB at
#      alpha 255. THE ZERO RULE IS THE PARSER'S (texture_archive.cpp replaces
#      index 0 with black in every non-keyed texture at parse time), so the
#      exporter's indices -- and the key -- are the parsed ones, and only a
#      keyed texture (archive kind bit 0) still has a transparent texel.
#      An animated texture is the whole vertical strip (height * frameCount,
#      the delta form decoded), the image ModImages' strip() asks for.
#   2. RGB565. The owner's rule: the ETC1 source is the RGB565 look (the current
#      texture16=1 store), so every channel goes to 5/6/5 bits and back to 8 by
#      the PICA's own round and widen (pica_matrix.h: round(v*max/255), then
#      bit-replicate: 5 bits v<<3|v>>2, 6 bits v<<2|v>>4).
#   3. BLEED (hd_pack_convert.bleed): every alpha-0 texel takes its nearest
#      opaque colour, flooded. ETC1 fits one colour pair per 4x4 block and the
#      gaussian mips average across texels; unbled, a keyed texture's edge is
#      pulled toward black at every level.
#   4. PAD, NOT RESAMPLE, to a power of two each way (>= 8, the PICA's rule),
#      the image at the TOP-LEFT and the bleed carried into the padding. The
#      original strip is kept texel for texel (the manifest's src_w/src_h say
#      how much of the padded image is real); a strip that pads past 1024 on
#      either axis is REFUSED by name -- the PICA's limit -- and the game draws
#      that texture from its palette indices.
#   5. ENCODE: `tex3ds -f etc1 -q high -m gaussian` for a texture with no
#      alpha-0 texel, `tex3ds -f etc1a4 -q high -m gaussian` otherwise (the
#      ../forsaken settings; the mip chain is built and compressed HERE, on the
#      host). NO Y-FLIP: the image is fed top row first, exactly as
#      hd_pack_convert.py and kurt_etc1_convert.py feed theirs, and the backend
#      draws all three through the same tex3ds orientation (06_draw.inc).
#
# THE LOOK. ETC1's colour is one base pair and a modifier table per 4x4 block,
# so a sharp colour edge inside a block softens (the "4x4 block look"); the
# owner has ruled performance over accuracy on the 3DS. The ALPHA of an ETC1A4
# texture is lossless: 4 explicit bits a texel, and the colour key's 0/255
# become 0/15 exactly, so the alpha test's cut-out is the original's texel for
# texel (--check proves it on every ETC1A4 texture it encodes).
#
# Deterministic (same input bytes -> same output bytes) and incremental: a
# texture whose image hash, cap and settings match the manifest's line is not
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hd_pack_convert import bleed  # noqa: E402  (the HD pack's own flood)
from kurt_etc1_convert import alpha_plane_etc1a4  # noqa: E402  (the ETC1A4 alpha reader)

TEX3DS = os.environ.get("TEX3DS", "/opt/devkitpro/tools/bin/tex3ds")
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
EXPORTER_DIR = os.path.join(ROOT, "mdk_3ds", "tools", "arena_etc1")
CAP = 1024  # the PICA's largest texture, per axis (the HD pack budgets 512; arenas go to 1024)
SETTINGS = "orig;etc1+etc1a4;q=high;m=gaussian;bleed=flood;pad=pot;rgb565;v1"


def parse_levels(spec):
    """'3..8', '3,4,5', or a mix like '3..5,7' -> [3,4,5,...]."""
    out = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if ".." in part:
            lo, hi = part.split("..", 1)
            out.extend(range(int(lo), int(hi) + 1))
        else:
            out.append(int(part))
    return out


def pot(n):
    p = 8
    while p < n:
        p <<= 1
    return p


def quantize565(rgba):
    """The RGB channels through the PICA's RGB565 store and back: the same
    round (channelBits: (v*max+127)/255) and the same widen (bit replication,
    5 bits v<<3|v>>2, 6 bits v<<2|v>>4) as pica_matrix.h's rgb565Word and the
    GPU's sampler, so the ETC1 source is the texture16=1 look. The alpha is
    the colour key and passes through untouched."""
    out = rgba.copy()
    r = (rgba[..., 0].astype(np.uint32) * 31 + 127) // 255
    g = (rgba[..., 1].astype(np.uint32) * 63 + 127) // 255
    b = (rgba[..., 2].astype(np.uint32) * 31 + 127) // 255
    out[..., 0] = ((r << 3) | (r >> 2)).astype(np.uint8)
    out[..., 1] = ((g << 2) | (g >> 4)).astype(np.uint8)
    out[..., 2] = ((b << 3) | (b >> 2)).astype(np.uint8)
    return out


def pad_bled(rgba, tw, th):
    """The image at the top-left of a tw x th canvas, the bleed carried into the
    padding (kurt_etc1_convert's rule: the padding's alpha stays 0, its colour
    is flooded from the image, so no mip level of the padding pulls an edge
    toward black)."""
    h, w = rgba.shape[:2]
    if (tw, th) == (w, h):
        return bleed(rgba)
    canvas = np.zeros((th, tw, 4), np.uint8)
    canvas[:h, :w] = rgba
    return bleed(canvas)


def convert(job):
    src, dst, w, h, frames, check = job
    sw, sh = w, h * frames
    if sw > CAP or sh > CAP:
        # A named REFUSAL, not a failure: the PICA cannot hold it, the game
        # draws that texture from its palette indices, and the pack is still
        # good. (The manifest not naming it is how the runtime learns this.)
        return (src, "REFUSED", "%dx%d strip is over the PICA's %d" % (sw, sh, CAP))
    with open(src, "rb") as f:
        data = f.read()
    if len(data) != sw * sh * 4:
        return (src, None, "size %d is not %dx%dx4" % (len(data), sw, sh))
    rgba = quantize565(np.frombuffer(data, np.uint8).reshape(sh, sw, 4))
    keyed = bool((rgba[..., 3] == 0).any())
    fmt = "etc1a4" if keyed else "etc1"
    tw, th = pot(sw), pot(sh)
    img = Image.fromarray(pad_bled(rgba, tw, th), "RGBA")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        staged = os.path.join(tmp, "in.png")
        img.save(staged)
        out = dst + ".tmp"
        r = subprocess.run([TEX3DS, "-f", fmt, "-q", "high", "-m", "gaussian", staged, "-o", out],
                           capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(out):
            return (src, None, "tex3ds failed: " + (r.stderr or r.stdout).strip()[:200])
        if check and keyed:
            # The same encode, raw and uncompressed: its alpha must be the colour
            # key exactly, at the top-left (the orientation the runtime's
            # rectangle assumes), and transparent in the padding. ETC1 has no
            # alpha plane to check.
            rawf = os.path.join(tmp, "raw.bin")
            r = subprocess.run([TEX3DS, "-f", "etc1a4", "-q", "high", "-r", "-z", "none",
                                staged, "-o", rawf], capture_output=True, text=True)
            if r.returncode != 0:
                return (src, None, "tex3ds -r failed")
            alpha = alpha_plane_etc1a4(open(rawf, "rb").read(), tw, th)
            want = np.zeros((th, tw), np.uint8)
            want[:sh, :sw] = np.where(rgba[..., 3] > 0, 15, 0)
            if not np.array_equal(alpha, want):
                os.remove(out)
                return (src, None, "CHECK: the encoded alpha is not the colour key "
                                   "(%d texels differ)" % int((alpha != want).sum()))
        os.replace(out, dst)
    return (src, (fmt, sw, sh, tw, th, os.path.getsize(dst)), None)


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
        sys.exit("arena_etc1_convert: the exporter did not build")
    return os.path.join(build_dir, "arena_tex_export")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default=os.path.join(ROOT, "build-arena-etc1"))
    ap.add_argument("--levels", default="3..8")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--check", action="store_true",
                    help="re-encode raw and prove the alpha is the colour key, per ETC1A4 texture")
    a = ap.parse_args()
    if not os.path.exists(TEX3DS):
        sys.exit("arena_etc1_convert: no tex3ds at %s (devkitPro tools; set TEX3DS)" % TEX3DS)
    levels = parse_levels(a.levels)
    out = os.path.abspath(a.out)
    work = os.path.join(out, ".work")
    os.makedirs(work, exist_ok=True)

    exporter = build_exporter(os.path.join(work, "exporter-host"))
    frames_dir = os.path.join(work, "frames")
    env = dict(os.environ)
    env.setdefault("MDK_DATA_DIR", os.path.join(ROOT, "MDK"))
    if not os.path.isdir(env["MDK_DATA_DIR"]):
        sys.exit("arena_etc1_convert: no MDK data at %s (set MDK_DATA_DIR)" % env["MDK_DATA_DIR"])
    r = subprocess.run([exporter, frames_dir] + [str(v) for v in levels], env=env)
    if r.returncode != 0:
        sys.exit("arena_etc1_convert: the exporter failed")

    # One image per hd key. The exporter writes the .rgba under the FIRST name
    # a key was seen under; the same first occurrence names the .t3x, so the
    # file the converter encodes is the file the exporter wrote.
    images = {}          # key -> (name, w, h, frames)
    key_levels = {}      # key -> set of levels that use it
    for line in open(os.path.join(frames_dir, "index.tsv")):
        if line.startswith("#") or not line.strip():
            continue
        level, _arena, name, w, h, frames, key = line.rstrip("\n").split("\t")
        if key not in images:
            images[key] = (name, int(w), int(h), int(frames))
        key_levels.setdefault(key, set()).add(int(level))

    manifest_path = os.path.join(out, "manifest.txt")
    old = {}
    if os.path.exists(manifest_path):
        for line in open(manifest_path):
            if line.startswith("#") or not line.strip():
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) == 10:
                old[f[0]] = f

    jobs, kept, digests = [], {}, {}
    for key in sorted(images):
        name, w, h, frames = images[key]
        src = os.path.join(frames_dir, "%s@%s.rgba" % (name, key))
        if not os.path.exists(src):
            # The exporter named the file under another spelling of this key's
            # first name (it cannot happen: both take the first occurrence).
            print("arena_etc1_convert: %s@%s: no exported image, skipped" % (name, key),
                  file=sys.stderr)
            continue
        with open(src, "rb") as fh:
            digest = hashlib.sha256(fh.read()).hexdigest()[:16]
        rel = "textures/%s@%s" % (name.lower(), key)
        digests[rel] = digest
        dst = os.path.join(out, rel + ".t3x")
        prev = old.get(rel)
        if prev and prev[7] == digest and prev[8] == str(CAP) and prev[9] == SETTINGS \
                and os.path.exists(dst) and not a.check:
            kept[rel] = prev
        else:
            jobs.append((src, dst, w, h, frames, a.check))

    lines = dict(kept)
    failed = 0
    refused = 0
    with ProcessPoolExecutor(max_workers=a.jobs) as pool:
        for job, (src, info, err) in zip(jobs, pool.map(convert, jobs, chunksize=4)):
            rel = os.path.relpath(job[1], out)[:-4]
            if info == "REFUSED":
                refused += 1
                print("arena_etc1_convert: %s: %s -- not encoded; the game draws it from its "
                      "palette indices" % (rel, err), file=sys.stderr)
                continue
            if err:
                failed += 1
                print("arena_etc1_convert: %s: %s" % (rel, err), file=sys.stderr)
                continue
            fmt, sw, sh, tw, th, size = info
            lines[rel] = [rel, fmt, str(sw), str(sh), str(tw), str(th), str(size),
                          digests[rel], str(CAP), SETTINGS]

    # Files no longer named by the set (a re-export with other data) go.
    wanted = set(os.path.join(out, rel + ".t3x") for rel in lines)
    if os.path.isdir(os.path.join(out, "textures")):
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

    # THE NAME-KEYED INDEX (byname.txt): (level, arena, texture name) -> rel.
    # manifest.txt is keyed by the CONTENT hash, so the runtime must re-hash a
    # texture's whole index array (hdKeyOf) to find its file -- 60-150 ms per
    # lookup on the ARM11, which is the whole cost of the texture pumps. The
    # build already knows every texture by name (the exporter's index.tsv), so
    # this file lets the runtime look the file up by (arena, name) for free;
    # the hash path stays as the fallback for anything not named here. A name
    # whose key was first encoded under another name points at THAT file --
    # the same rel manifest.txt names. Refused/failed encodings are absent, so
    # absence is how the runtime learns to leave a texture to expansion.
    byname_path = os.path.join(out, "byname.txt")
    tmp = byname_path + ".tmp"
    with open(tmp, "w") as f:
        f.write("# level\tarena\tname\trel\n")
        seen = set()
        for line in open(os.path.join(frames_dir, "index.tsv")):
            if line.startswith("#") or not line.strip():
                continue
            level, arena, name, w, h, frames, key = line.rstrip("\n").split("\t")
            rel = "textures/%s@%s" % (images[key][0].lower(), key)
            if rel not in lines:
                continue
            row = (level, arena, name, rel)
            if row in seen:
                continue
            seen.add(row)
            f.write("\t".join(row) + "\n")
    os.replace(tmp, byname_path)

    # ---- the size summary (the RomFS budget) --------------------------------
    bytes_of = {}  # key -> manifest bytes (only encoded ones)
    for rel, v in lines.items():
        bytes_of[rel.rpartition("@")[2]] = int(v[6])
    total = sum(bytes_of.values())
    etc1a4 = sum(1 for v in lines.values() if v[1] == "etc1a4")
    gpu = sum(int(v[4]) * int(v[5]) * (1 if v[1] == "etc1a4" else 0.5) for v in lines.values())
    print("arena_etc1_convert: %d images (%d ETC1A4, %d ETC1), %d encoded now, %d kept, "
          "%d refused (over %d), %d failed%s" % (len(lines), etc1a4, len(lines) - etc1a4,
                                                len(jobs) - failed - refused, len(kept), refused,
                                                CAP, failed, " (checked)" if a.check else ""))
    for level in levels:
        n = sum(1 for k in key_levels if level in key_levels[k] and k in bytes_of)
        b = sum(bytes_of[k] for k in key_levels if level in key_levels[k] and k in bytes_of)
        print("arena_etc1_convert:   level %d: %4d images, %8.2f MB" % (level, n, b / 1048576.0))
    print("arena_etc1_convert: TOTAL %d images, %.2f MB of .t3x in the RomFS, %.2f MB as "
          "level-0 GPU copies (a mip chain adds a third)" %
          (len(lines), total / 1048576.0, gpu / 1048576.0))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

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
#      how much of the padded image is real). An ANIMATED strip taller than
#      1024 (EXPLODE 128x3328, FIRE 64x2628, SL_BIG 64x1920) is built as PAGES
#      of whole frames, <name>@<key>.p<k>.t3x, each up to 1024 rows (paged());
#      anything else past 1024 on either axis is REFUSED by name -- the PICA's
#      limit -- and the game draws that texture from its palette indices.
#   5. ENCODE: `tex3ds -f etc1 -q high -m gaussian` for a texture with no
#      alpha-0 texel, `tex3ds -f etc1a4 -q high -m gaussian` otherwise (the
#      ../forsaken settings; the mip chain is built and compressed HERE, on the
#      host). NO Y-FLIP: the image is fed top row first, exactly as
#      hd_pack_convert.py and kurt_etc1_convert.py feed theirs, and the backend
#      draws all three through the same tex3ds orientation (06_draw.inc).
#      `-z none`: NO CONTAINER COMPRESSION. tex3ds' default (auto, mostly LZ11)
#      halves the pack on disk, but the 3DS then decompresses every texture on
#      the main thread at first use -- measured on the Old 3DS 2026-11-24
#      (docs/evidence/2026-11-24-orig-pack-uncompressed.txt): 39 arena makes
#      imported in 690 ms with LZ against 37 ms without, the same ETC blocks
#      byte for byte. The pack grows from 21 to 37 MiB of RomFS.
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
import struct
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
SETTINGS = "orig;etc1+etc1a4;q=high;m=gaussian;bleed=flood;pad=pot;rgb565;z=none;v1"


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


def paged(w, h, frames):
    """An animated strip taller than the PICA's 1024 whose frames fit it: built
    as PAGES (page_paths), each as many whole frames as 1024 rows hold. The
    3DS backend splits it by the same rule (05_resources.inc createStrip)."""
    return frames > 1 and w <= CAP and h <= CAP and h * frames > CAP


def page_paths(dst, h, frames):
    """Page k of a paged strip is <name>@<key>.p<k>.t3x beside where the whole
    strip's file would be; its frames are [k * per, k * per + n)."""
    per = CAP // h
    base = dst[:-4]
    return [(base + ".p%d.t3x" % k, k * per, min(per, frames - k * per))
            for k in range((frames + per - 1) // per)]


def encode(rgba, fmt, dst, check, mips):
    """One image to one .t3x (steps 3-5 above); an error string, or None."""
    sh, sw = rgba.shape[:2]
    keyed = fmt == "etc1a4"
    tw, th = pot(sw), pot(sh)
    img = Image.fromarray(pad_bled(rgba, tw, th), "RGBA")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        staged = os.path.join(tmp, "in.png")
        img.save(staged)
        out = dst + ".tmp"
        r = subprocess.run([TEX3DS, "-f", fmt, "-q", "high"] +
                           (["-m", "gaussian"] if mips else []) +
                           ["-z", "none", staged, "-o", out],
                           capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(out):
            return "tex3ds failed: " + (r.stderr or r.stdout).strip()[:200]
        if check and keyed:
            # The same encode, raw and uncompressed: its alpha must be the colour
            # key exactly, at the top-left (the orientation the runtime's
            # rectangle assumes), and transparent in the padding. ETC1 has no
            # alpha plane to check.
            rawf = os.path.join(tmp, "raw.bin")
            r = subprocess.run([TEX3DS, "-f", "etc1a4", "-q", "high", "-r", "-z", "none",
                                staged, "-o", rawf], capture_output=True, text=True)
            if r.returncode != 0:
                return "tex3ds -r failed"
            alpha = alpha_plane_etc1a4(open(rawf, "rb").read(), tw, th)
            want = np.zeros((th, tw), np.uint8)
            want[:sh, :sw] = np.where(rgba[..., 3] > 0, 15, 0)
            if not np.array_equal(alpha, want):
                os.remove(out)
                return ("CHECK: the encoded alpha is not the colour key "
                        "(%d texels differ)" % int((alpha != want).sum()))
        os.replace(out, dst)
    return None


def convert(job):
    src, dst, w, h, frames, check = job
    sw, sh = w, h * frames
    if (sw > CAP or sh > CAP) and not paged(w, h, frames):
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
    if not paged(w, h, frames):
        err = encode(rgba, fmt, dst, check, True)
        if err:
            return (src, None, err)
        return (src, (fmt, sw, sh, pot(sw), pot(sh), os.path.getsize(dst)), None)
    # PAGES (the strips over 1024: EXPLODE 128x3328, FIRE 64x2628, SL_BIG
    # 64x1920). One format for every page, the strip's. NO MIP CHAIN: an
    # animated texture is always sampled at level 0 (06_draw.inc), so a chain
    # would be a third more memory nobody reads. The manifest's row is the
    # strip's (its h column the pages' padded heights summed, its bytes theirs).
    th, size = 0, 0
    for path, first, n in page_paths(dst, h, frames):
        err = encode(rgba[first * h:(first + n) * h], fmt, path, check, False)
        if err:
            return (src, None, "page %s: %s" % (os.path.basename(path), err))
        th += pot(n * h)
        size += os.path.getsize(path)
    return (src, (fmt, sw, sh, pot(sw), th, size), None)


# ---- THE SKY ----------------------------------------------------------------
# The level's sky panorama as ETC1, drawn with HD on in place of its 16-bit
# palette expansion (mdk_3ds/src/level_load_3ds.cpp buildSky3ds). The source art
# is an 8-bit palette image whose gradients were error-diffused at authoring
# time; that dither is what showed on the 3DS, and ETC1 alone keeps it (worse:
# it smears it into 4x4 blocks). So the panorama is DE-DITHERED first: a
# gaussian blur of SKY_SIGMA texels over the WHOLE wrap -- wrapped
# horizontally, edge-clamped vertically -- so the two slices and the 360-degree
# seam blur the same neighbours and still meet exactly. Then each slice is cut
# by the dome's own rule (stored column x = first - 1 + x, wrapped: the exporter
# wrote the slices buildSkyDome made, and the cut is checked against them
# before the blur), padded to a power of two by repeating its last column and
# row (the slices are whole 4x4 blocks wide and 360 = 90 blocks tall, so no
# block straddles image and padding), and encoded ETC1 with NO mip chain (the
# sky is drawn Original-shaded, level 0 only). Not quantized to RGB565 first:
# that would put the gradients' steps back.
SKY_SIGMA = 1.0
SKY_SETTINGS = "sky;etc1;q=high;dedither=gauss%.2f;pad=edge;nomips;z=none;v1" % SKY_SIGMA


def gauss_wrap(rgb, sigma):
    """A separable gaussian (radius 3 sigma), wrapped across the columns (the
    panorama is 360 degrees) and clamped at the top and bottom rows."""
    r = int(3 * sigma + 0.5)
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    a = rgb.astype(np.float64)
    h, w = a.shape[:2]
    rows = np.zeros_like(a)
    for i, t in enumerate(k):
        rows += t * np.roll(a, r - i, axis=1)
    padded = np.concatenate([np.repeat(rows[:1], r, 0), rows, np.repeat(rows[-1:], r, 0)], 0)
    out = np.zeros_like(a)
    for i, t in enumerate(k):
        out += t * padded[i:i + h]
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def sky_cut(wrap_rgb, first, stored):
    w = wrap_rgb.shape[1]
    return wrap_rgb[:, [(first - 1 + x) % w for x in range(stored)]]


def convert_skies(frames_dir, out):
    """Every exported sky's slices as sky/level<L>_<k>.t3x, listed in
    sky/manifest.txt. Returns (encoded now, kept, failed)."""
    index_path = os.path.join(frames_dir, "sky", "index.tsv")
    sky_out = os.path.join(out, "sky")
    os.makedirs(sky_out, exist_ok=True)
    manifest_path = os.path.join(sky_out, "manifest.txt")
    old = {}
    if os.path.exists(manifest_path):
        for line in open(manifest_path):
            if not line.startswith("#") and line.strip():
                f = line.rstrip("\n").split("\t")
                old[f[0]] = f
    rows = [line.rstrip("\n").split("\t") for line in open(index_path)
            if line.strip() and not line.startswith("#")]
    lines, wraps = {}, {}
    encoded = kept = failed = 0
    for level, k, first, stored, height, wrap in rows:
        first, stored, height, wrap = int(first), int(stored), int(height), int(wrap)
        name = "level%s_%s" % (level, k)
        slice_rgb = np.fromfile(os.path.join(frames_dir, "sky", name + ".rgba"),
                                np.uint8).reshape(height, stored, 4)[..., :3]
        if level not in wraps:
            raw = np.fromfile(os.path.join(frames_dir, "sky", "level%s.rgba" % level),
                              np.uint8).reshape(height, wrap, 4)[..., :3]
            wraps[level] = (raw, gauss_wrap(raw, SKY_SIGMA))
        raw, smooth = wraps[level]
        if not np.array_equal(sky_cut(raw, first, stored), slice_rgb):
            print("arena_etc1_convert: sky %s: the cut is not the dome's slice -- not encoded"
                  % name, file=sys.stderr)
            failed += 1
            continue
        img = sky_cut(smooth, first, stored)
        digest = hashlib.sha256(img.tobytes()).hexdigest()[:16]
        dst = os.path.join(sky_out, name + ".t3x")
        prev = old.get(name)
        tw, th = pot(stored), pot(height)
        if prev and prev[5] == digest and prev[6] == SKY_SETTINGS and os.path.exists(dst):
            lines[name] = prev
            kept += 1
            continue
        canvas = np.zeros((th, tw, 3), np.uint8)
        canvas[:height, :stored] = img
        canvas[height:, :stored] = img[height - 1:height]
        canvas[:, stored:] = canvas[:, stored - 1:stored]
        with tempfile.TemporaryDirectory() as tmp:
            staged = os.path.join(tmp, "in.png")
            Image.fromarray(canvas, "RGB").save(staged)
            r = subprocess.run([TEX3DS, "-f", "etc1", "-q", "high", "-z", "none", staged,
                                "-o", dst + ".tmp"], capture_output=True, text=True)
            if r.returncode != 0 or not os.path.exists(dst + ".tmp"):
                print("arena_etc1_convert: sky %s: tex3ds failed: %s"
                      % (name, (r.stderr or r.stdout).strip()[:200]), file=sys.stderr)
                failed += 1
                continue
            os.replace(dst + ".tmp", dst)
        lines[name] = [name, str(stored), str(height), str(tw), str(th), digest, SKY_SETTINGS]
        encoded += 1
    for f in os.listdir(sky_out):
        if f.endswith(".t3x") and f[:-4] not in lines:
            os.remove(os.path.join(sky_out, f))
    with open(manifest_path + ".tmp", "w") as f:
        f.write("# name\tsrc_w\tsrc_h\tw\th\tsrc_sha256_16\tsettings\n")
        for name in sorted(lines):
            f.write("\t".join(lines[name]) + "\n")
    os.replace(manifest_path + ".tmp", manifest_path)
    return encoded, kept, failed


# THE MIRROR CUBES (mdk_core/src/render/mirror_cube.h, THE BAKED CUBES): the six
# faces the exporter made for each (level, row shift), each ETC1 by tex3ds (no
# mips, no container compression, fed row y = 0 first: tex3ds' top is t = 1, the
# face's first memory row, as the runtime cube writes it), then ONE file a cube,
# mirror/level<L>_<shift>.bin: the header mirror_cube.h describes (its stamp the
# exporter's) and the six faces' ETC1 blocks exactly as tex3ds laid them out.
# Listed in mirror/manifest.txt; incremental on the faces' digest.
MIRROR_SETTINGS = "mirror;etc1;q=high;nomips;z=none;v1"
MIRROR_SIZE = 512                 # kMirrorBakedSize
MIRROR_FACE_BYTES = MIRROR_SIZE * MIRROR_SIZE // 2
MIRROR_VERSION = 1                # kMirrorBakedVersion
MIRROR_ETC1 = 1                   # kMirrorBakedEtc1
GPU_ETC1 = 0x0C


def t3x_etc1_payload(path, size):
    """The ETC1 blocks of a one-sub-texture, mip-free, uncompressed tex3ds file,
    or None. tex3ds' header: u16 sub-textures, u8 log2 dims (3 bits each, -3),
    u8 format, u8 mip levels; per sub-texture six u16; then the container header
    (type 0 = none, 24-bit size) and the data."""
    data = open(path, "rb").read()
    if len(data) < 21:
        return None
    subs = data[0] | (data[1] << 8)
    dims = data[2]
    want_log = size.bit_length() - 1 - 3
    if subs != 1 or (dims & 7) != want_log or ((dims >> 3) & 7) != want_log or \
            data[3] != GPU_ETC1 or data[4] != 0:
        return None
    at = 5 + 12 * subs
    kind = data[at]
    length = data[at + 1] | (data[at + 2] << 8) | (data[at + 3] << 16)
    payload = data[at + 4:]
    if kind != 0 or length != size * size // 2 or len(payload) != length:
        return None
    return payload


def encode_mirror_face(job):
    src, size = job
    rgb = np.fromfile(src, np.uint8)
    if rgb.size != size * size * 3:
        return None, "%s: %d bytes, not a %dx%d RGB face" % (src, rgb.size, size, size)
    with tempfile.TemporaryDirectory() as tmp:
        staged = os.path.join(tmp, "in.png")
        Image.fromarray(rgb.reshape(size, size, 3), "RGB").save(staged)
        out = os.path.join(tmp, "out.t3x")
        r = subprocess.run([TEX3DS, "-f", "etc1", "-q", "high", "-z", "none", staged, "-o", out],
                           capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(out):
            return None, "%s: tex3ds failed: %s" % (src, (r.stderr or r.stdout).strip()[:200])
        payload = t3x_etc1_payload(out, size)
        if payload is None:
            return None, "%s: tex3ds wrote a file this converter does not read" % src
        return payload, None


def mirror_header(shift, stamp):
    h = bytearray(64)
    h[0:8] = b"MDKMIRR\0"
    struct.pack_into("<IIIiII", h, 8, MIRROR_VERSION, MIRROR_SIZE, MIRROR_ETC1, shift,
                     MIRROR_FACE_BYTES, 0)
    struct.pack_into("<Q", h, 32, stamp)
    return bytes(h)


def convert_mirrors(frames_dir, out, jobs):
    """Every exported mirror cube as mirror/level<L>_<shift>.bin, listed in
    mirror/manifest.txt. Returns (encoded now, kept, failed)."""
    index_path = os.path.join(frames_dir, "mirror", "index.tsv")
    mirror_out = os.path.join(out, "mirror")
    os.makedirs(mirror_out, exist_ok=True)
    manifest_path = os.path.join(mirror_out, "manifest.txt")
    old = {}
    if os.path.exists(manifest_path):
        for line in open(manifest_path):
            if not line.startswith("#") and line.strip():
                f = line.rstrip("\n").split("\t")
                if len(f) == 5:
                    old[f[0]] = f
    rows = [line.rstrip("\n").split("\t") for line in open(index_path)
            if line.strip() and not line.startswith("#")] if os.path.exists(index_path) else []
    lines = {}
    todo = []
    kept = failed = 0
    for level, shift, size, stamp in rows:
        size = int(size)
        name = "level%s_%s" % (level, shift)
        if size != MIRROR_SIZE:
            print("arena_etc1_convert: mirror %s: face size %d is not %d -- not encoded"
                  % (name, size, MIRROR_SIZE), file=sys.stderr)
            failed += 1
            continue
        faces = [os.path.join(frames_dir, "mirror", "%s_%d.rgb" % (name, f)) for f in range(6)]
        digest = hashlib.sha256()
        for face in faces:
            with open(face, "rb") as fh:
                digest.update(fh.read())
        digest = digest.hexdigest()[:16]
        dst = os.path.join(mirror_out, name + ".bin")
        prev = old.get(name)
        if prev and prev[3] == digest and prev[2] == stamp and prev[4] == MIRROR_SETTINGS \
                and os.path.exists(dst):
            lines[name] = prev
            kept += 1
            continue
        todo.append((name, int(shift), int(stamp, 16), faces, digest, stamp, dst))
    encoded = 0
    if todo:
        face_jobs = [(face, MIRROR_SIZE) for item in todo for face in item[3]]
        with ProcessPoolExecutor(max_workers=max(1, min(jobs, 4))) as pool:
            results = list(pool.map(encode_mirror_face, face_jobs))
        for i, (name, shift, stamp, faces, digest, stamp_hex, dst) in enumerate(todo):
            mine = results[i * 6:(i + 1) * 6]
            errors = [err for _, err in mine if err]
            if errors:
                for err in errors:
                    print("arena_etc1_convert: mirror %s" % err, file=sys.stderr)
                failed += 1
                continue
            with open(dst + ".tmp", "wb") as fh:
                fh.write(mirror_header(shift, stamp))
                for payload, _ in mine:
                    fh.write(payload)
            os.replace(dst + ".tmp", dst)
            lines[name] = [name, str(MIRROR_SIZE), stamp_hex, digest, MIRROR_SETTINGS]
            encoded += 1
    for f in os.listdir(mirror_out):
        if f.endswith(".bin") and f[:-4] not in lines:
            os.remove(os.path.join(mirror_out, f))
    with open(manifest_path + ".tmp", "w") as f:
        f.write("# name\tsize\tstamp\tfaces_sha256_16\tsettings\n")
        for name in sorted(lines):
            f.write("\t".join(lines[name]) + "\n")
    os.replace(manifest_path + ".tmp", manifest_path)
    return encoded, kept, failed


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
        files = [p for p, _, _ in page_paths(dst, h, frames)] if paged(w, h, frames) else [dst]
        prev = old.get(rel)
        if prev and prev[7] == digest and prev[8] == str(CAP) and prev[9] == SETTINGS \
                and all(os.path.exists(f) for f in files) and not a.check:
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
    wanted = set()
    for key, (name, w, h, frames) in images.items():
        rel = "textures/%s@%s" % (name.lower(), key)
        if rel in lines:
            dst = os.path.join(out, rel + ".t3x")
            if paged(w, h, frames):
                wanted.update(p for p, _, _ in page_paths(dst, h, frames))
            wanted.add(dst)
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
    sky_new, sky_kept, sky_failed = convert_skies(frames_dir, out)
    print("arena_etc1_convert: sky %d slices encoded now, %d kept, %d failed" %
          (sky_new, sky_kept, sky_failed))
    mirror_new, mirror_kept, mirror_failed = convert_mirrors(frames_dir, out, a.jobs)
    print("arena_etc1_convert: mirror %d cubes encoded now, %d kept, %d failed" %
          (mirror_new, mirror_kept, mirror_failed))
    return 1 if failed or sky_failed or mirror_failed else 0


if __name__ == "__main__":
    sys.exit(main())

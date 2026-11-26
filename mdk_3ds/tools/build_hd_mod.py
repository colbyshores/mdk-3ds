#!/usr/bin/env python3
# ============================================================================
# build_hd_mod.py -- the HD texture mod (mods/hd-textures/, the PC layout of
# docs/modding.md), made from hd_tex_export's output with Real-ESRGAN.
#
#   mdk_3ds/tools/build_hd_mod.py <export dir> <mod dir> --tools <dir>
#
# The native replacement for the C# generator's `--upscale-textures`
# (src/Mdk.Game/HdTextures/HdGenerator.cs and UpscaleImages.cs, still the
# behaviour reference). mdk_3ds/tools/build_hd_pack.sh runs it after
# mdk_3ds/tools/hd_export/hd_tex_export, which reads the game data (C++, the
# game's own loaders) and writes <export dir>/index.tsv and one <key>.rgba per
# image. Everything after the export is here, as the C# does it:
#
#   1. THE UPSCALER. Real-ESRGAN's portable build (realesrgan-ncnn-vulkan
#      v0.2.5.0, 2022-04-24, BSD-3-Clause) in <tools>/realesrgan/, downloaded
#      once: the archive goes into a new empty directory, its SHA-256 must be
#      the pinned one, it is unpacked into another new empty directory (no
#      script from it is run; only the upscaler binary is) and moved into place
#      with installed.sha256. MDK_REALESRGAN_URL / MDK_REALESRGAN_SHA256 replace
#      the source (a file:// URL works offline); the checksum is always checked.
#   2. THE MOD'S STATE. manifest.txt (format=1, model, scale, one line per
#      image); a manifest made another way is dropped with its images. mod.txt
#      is written. Per level, images of keys no longer exported are deleted;
#      what the manifest has (and is on disk) is kept; a key shared by levels is
#      made once, under the first level that has it.
#   3. BEFORE THE UPSCALER (UpscaleImages.Input): each frame's clear texels take
#      the rounded mean colour of their filled 4-neighbours, ring after ring,
#      then the frame wraps around by 8 texels; opaque RGB PNGs into work/in/.
#   4. THE UPSCALER: -n realesrgan-x4plus -s 4 -f png, run from its folder.
#   5. AFTER (UpscaleImages.Output): the margin cut, a 2x2 box mean (4x to the
#      mod's 2x), and the alpha the source's, bilinear at the texel centres:
#      hard (>= 128 -> 255, else 0) for surfaces, soft (rounded) for Kurt and
#      the 2D images; an all-opaque frame stays 255. Frames stacked downwards;
#      RGB when the texture has no clear texel, else RGBA. work/ is deleted.
#
# Needs Python 3 with numpy and Pillow, and a Vulkan GPU for the upscaler
# (there is no CPU path), network once for the download.
# ============================================================================
import argparse
import hashlib
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image

# UpscalerSource.Default (Linux).
UPSCALER_URL = ("https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/"
                "realesrgan-ncnn-vulkan-20220424-ubuntu.zip")
UPSCALER_SHA256 = "e5aa6eb131234b87c0c51f82b89390f5e3e642b7b70f2b9bbe95b6a285a40c96"
TOOL_NAME = "realesrgan-ncnn-vulkan"
MARKER = "installed.sha256"

# HdOptions.Default: Real-ESRGAN x4plus (4x only), the mod's images at 2x.
MODEL = "realesrgan-x4plus"
FACTOR = 4
SCALE = 2
MARGIN = 8           # HdGenerator.Margin: texels wrapped around each frame
MANIFEST_FORMAT = 1  # HdManifest.Format
MENUS = 0            # CanvasExport.NoLevel
MOD_INFO = ("name=HD textures (Real-ESRGAN)\nauthor=made on this computer\nversion=\n"
            "description=%s, %dx\npriority=-100\n" % (MODEL, SCALE))


def fail(message):
    sys.exit("build_hd_mod: " + message)


# ---- 1. the upscaler --------------------------------------------------------

def install(tools):
    """The upscaler's executable in <tools>/realesrgan, downloaded first when missing."""
    url = os.environ.get("MDK_REALESRGAN_URL", UPSCALER_URL)
    sha = os.environ.get("MDK_REALESRGAN_SHA256", UPSCALER_SHA256).lower()
    folder = os.path.join(tools, "realesrgan")
    exe = os.path.join(folder, TOOL_NAME)
    marker = os.path.join(folder, MARKER)
    if os.path.isfile(exe) and os.path.isfile(marker) and open(marker).read().strip() == sha:
        return exe
    if not sha:
        fail("MDK_REALESRGAN_SHA256 is needed for " + url)
    os.makedirs(tools, exist_ok=True)
    download = tempfile.mkdtemp(prefix="realesrgan-download-", dir=tools)
    unpacked = tempfile.mkdtemp(prefix="realesrgan-unpack-", dir=tools)
    try:
        archive = os.path.join(download, "download.zip")
        print("build_hd_mod: downloading %s" % url, flush=True)
        digest = hashlib.sha256()
        with urllib.request.urlopen(url, timeout=60) as response, open(archive, "wb") as out:
            while True:
                block = response.read(1 << 16)
                if not block:
                    break
                out.write(block)
                digest.update(block)
        if digest.hexdigest() != sha:
            fail("%s: SHA-256 %s, expected %s" % (url, digest.hexdigest(), sha))
        # zipfile drops absolute and '..' parts and makes no links.
        with zipfile.ZipFile(archive) as z:
            z.extractall(unpacked)
        tool = os.path.join(unpacked, TOOL_NAME)
        if not os.path.isfile(tool):
            fail("the archive has no %s" % TOOL_NAME)
        os.chmod(tool, os.stat(tool).st_mode | stat.S_IXUSR)
        with open(os.path.join(unpacked, MARKER), "w") as f:
            f.write(sha)
        shutil.rmtree(folder, ignore_errors=True)
        os.rename(unpacked, folder)
    finally:
        shutil.rmtree(download, ignore_errors=True)
        shutil.rmtree(unpacked, ignore_errors=True)
    return exe


def upscale(exe, src, dst):
    """Every PNG of src into dst, FACTOR times larger (RealEsrgan.Run)."""
    os.makedirs(dst, exist_ok=True)
    expected = sum(1 for n in os.listdir(src) if n.endswith(".png"))
    r = subprocess.run([exe, "-i", src, "-o", dst, "-n", MODEL, "-s", str(FACTOR), "-f", "png"],
                       cwd=os.path.dirname(exe), capture_output=True, text=True)
    written = sum(1 for n in os.listdir(dst) if n.endswith(".png"))
    if r.returncode != 0 or written != expected:
        tail = "\n".join((r.stdout + r.stderr).splitlines()[-20:])
        fail("%s failed (exit %d, %d of %d images; a Vulkan GPU is needed):\n%s"
             % (TOOL_NAME, r.returncode, written, expected, tail))


# ---- 2. the mod's state -----------------------------------------------------

def safe_name(name):
    return "".join(c if (c.isascii() and c.isalnum()) or c in "_-" else "_" for c in name)


def file_of(level, kind, name, key):
    if level == MENUS:
        return "%s/%s@%s.png" % (kind, safe_name(name), key)
    return "%s/LEVEL%d/%s@%s.png" % (kind, level, safe_name(name), key)


def load_manifest(folder):
    """(settings, entries by key) of manifest.txt, or None (HdManifest.Load)."""
    path = os.path.join(folder, "manifest.txt")
    if not os.path.exists(path):
        return None
    settings, entries = {}, {}
    for line in open(path).read().splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            settings[k] = v
            continue
        f = line.split(" ")
        if len(f) == 7 and all(n.isdigit() for n in (f[1], f[3], f[4], f[5])):
            entries[f[0]] = (f[0], int(f[1]), f[2], int(f[3]), int(f[4]), int(f[5]), f[6])
    if not (settings.get("format", "").isdigit() and settings.get("scale", "").isdigit()):
        return None
    return settings, entries


def save_manifest(folder, entries):
    lines = ["format=%d" % MANIFEST_FORMAT, "model=%s" % MODEL, "scale=%d" % SCALE]
    for e in sorted(entries.values(), key=lambda e: e[6]):
        lines.append("%s %d %s %d %d %d %s" % e)
    path = os.path.join(folder, "manifest.txt")
    with open(path + ".tmp", "w") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(path + ".tmp", path)


def current_entries(folder):
    """The manifest's entries when it was made the same way; else its images go."""
    loaded = load_manifest(folder)
    if loaded:
        settings, entries = loaded
        if (settings.get("format") == str(MANIFEST_FORMAT) and settings.get("model") == MODEL
                and settings.get("scale") == str(SCALE)):
            return entries
        for e in entries.values():
            try:
                os.remove(os.path.join(folder, e[6]))
            except FileNotFoundError:
                pass
    return {}


def prune(folder, entries, level, keys):
    for e in [e for e in entries.values() if e[1] == level and e[0] not in keys]:
        try:
            os.remove(os.path.join(folder, e[6]))
        except FileNotFoundError:
            pass
        del entries[e[0]]


def read_index(export):
    """The exporter's sources by level, in its order: dicts of the index's fields."""
    levels = {}
    for line in open(os.path.join(export, "index.tsv")):
        if line.startswith("#") or not line.strip():
            continue
        level, kind, alpha, name, key, w, h, frames = line.rstrip("\n").split("\t")
        levels.setdefault(int(level), []).append(dict(
            level=int(level), kind=kind, alpha=alpha, name=name, key=key,
            width=int(w), height=int(h), frames=int(frames)))
    return levels


def chain_size(w, h):
    """ColourMips.ChainSize: RGBA8 bytes of all mip levels."""
    size = 0
    for _ in range(max(w, h, 1).bit_length()):
        size += w * h * 4
        w, h = max(w // 2, 1), max(h // 2, 1)
    return size


# ---- 3-5. the images --------------------------------------------------------

def frames_of(export, s):
    rgba = np.fromfile(os.path.join(export, s["key"] + ".rgba"), dtype=np.uint8)
    if rgba.size != s["width"] * s["height"] * s["frames"] * 4:
        fail("%s.rgba is not %dx%dx%d" % (s["key"], s["width"], s["height"], s["frames"]))
    return rgba.reshape(s["frames"], s["height"], s["width"], 4)


def bleed(frame):
    """UpscaleImages.Bleed: clear texels take the rounded mean colour of their
    filled 4-neighbours, ring after ring (an all-clear frame stays black); all opaque."""
    rgb = frame[..., :3].astype(np.int32)
    filled = frame[..., 3] != 0
    h, w = filled.shape
    while not filled.all():
        total = np.zeros_like(rgb)
        count = np.zeros((h, w), np.int32)
        for dy, dx in ((0, -1), (0, 1), (-1, 0), (1, 0)):
            # target [y, x] reads source [y + dy, x + dx]
            ty = slice(max(-dy, 0), h - max(dy, 0))
            tx = slice(max(-dx, 0), w - max(dx, 0))
            sy = slice(max(dy, 0), h - max(-dy, 0))
            sx = slice(max(dx, 0), w - max(-dx, 0))
            f = filled[sy, sx]
            total[ty, tx] += rgb[sy, sx] * f[..., None]
            count[ty, tx] += f
        ring = ~filled & (count > 0)
        if not ring.any():
            break
        n = count[ring][:, None]
        rgb[ring] = (total[ring] + n // 2) // n
        filled |= ring
    return rgb.astype(np.uint8)


def padded_input(frame):
    """UpscaleImages.Input: bled, wrapped by MARGIN, RGB."""
    rgb = bleed(frame)
    h, w = rgb.shape[:2]
    ys = np.arange(-MARGIN, h + MARGIN) % h
    xs = np.arange(-MARGIN, w + MARGIN) % w
    return rgb[ys][:, xs]


def cover(alpha, scale, soft):
    """UpscaleImages.Cover over the whole output: the source's alpha, bilinear at
    each output texel's centre (texel centres at whole numbers, edges clamped)."""
    h, w = alpha.shape
    a = alpha.astype(np.float32)

    def axis(size):
        p = (np.arange(size * scale, dtype=np.float32) + np.float32(0.5)) / np.float32(scale) \
            - np.float32(0.5)
        p = np.clip(p, np.float32(0), np.float32(size - 1))
        p0 = p.astype(np.int32)
        return p0, np.minimum(p0 + 1, size - 1), p - p0.astype(np.float32)

    x0, x1, fx = axis(w)
    y0, y1, fy = axis(h)
    top = a[y0][:, x0] + (a[y0][:, x1] - a[y0][:, x0]) * fx
    bottom = a[y1][:, x0] + (a[y1][:, x1] - a[y1][:, x0]) * fx
    c = top + (bottom - top) * fy[:, None]
    if soft:
        return np.rint(c).astype(np.uint8)
    return np.where(c >= np.float32(128), 255, 0).astype(np.uint8)


def finished(upscaled, frame, soft):
    """UpscaleImages.Output: margin cut, box shrink to SCALE, the source's alpha."""
    h, w = frame.shape[:2]
    shrink = FACTOR // SCALE
    m = MARGIN * FACTOR
    block = upscaled[m:m + h * SCALE * shrink, m:m + w * SCALE * shrink].astype(np.int32)
    block = block.reshape(h * SCALE, shrink, w * SCALE, shrink, 3).sum(axis=(1, 3))
    n = shrink * shrink
    out = np.empty((h * SCALE, w * SCALE, 4), np.uint8)
    out[..., :3] = (block + n // 2) // n
    alpha = frame[..., 3]
    out[..., 3] = 255 if (alpha == 255).all() else cover(alpha, SCALE, soft)
    return out


def export_frames(job):
    """One texture's frames into the upscaler's input folder; returns how many."""
    export, src, s = job
    frames = frames_of(export, s)
    for f, frame in enumerate(frames):
        Image.fromarray(padded_input(frame), "RGB").save(os.path.join(src, "%s_%d.png" % (s["key"], f)))
    return len(frames)


def save_texture(job):
    """One texture's upscaled frames finished, stacked and saved; returns its manifest entry."""
    export, dst, folder, s = job
    frames = frames_of(export, s)
    pw, ph = (s["width"] + 2 * MARGIN) * FACTOR, (s["height"] + 2 * MARGIN) * FACTOR
    parts = []
    for f, frame in enumerate(frames):
        up = np.asarray(Image.open(os.path.join(dst, "%s_%d.png" % (s["key"], f))).convert("RGB"))
        if up.shape[1] != pw or up.shape[0] != ph:
            fail("%s: the upscaler made %d x %d" % (s["name"], up.shape[1], up.shape[0]))
        parts.append(finished(up, frame, s["alpha"] == "soft"))
    image = np.concatenate(parts, axis=0)
    file = file_of(s["level"], s["kind"], s["name"], s["key"])
    path = os.path.join(folder, file)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if (frames[..., 3] == 255).all():  # no clear texel: RGB, as the C# writes it
        Image.fromarray(image[..., :3], "RGB").save(path)
    else:
        Image.fromarray(image, "RGBA").save(path)
    return (s["key"], s["level"], s["name"], s["width"], s["height"], s["frames"], file)


def make(exe, export, folder, todo, entries, jobs):
    work = os.path.join(folder, "work")
    shutil.rmtree(work, ignore_errors=True)
    src, dst = os.path.join(work, "in"), os.path.join(work, "out")
    os.makedirs(src)
    try:
        with ProcessPoolExecutor(max_workers=jobs) as pool:
            count = sum(pool.map(export_frames, [(export, src, s) for s in todo], chunksize=8))
            print("build_hd_mod: exported %d frames; upscaling (Real-ESRGAN)" % count, flush=True)
            clock = time.time()
            upscale(exe, src, dst)
            print("build_hd_mod: upscaled in %.0f s; saving" % (time.time() - clock), flush=True)
            for entry in pool.map(save_texture, [(export, dst, folder, s) for s in todo], chunksize=8):
                entries[entry[0]] = entry
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("export", help="hd_tex_export's output")
    ap.add_argument("mod", help="the mod's folder (mods/hd-textures)")
    ap.add_argument("--tools", required=True, help="where the upscaler is kept")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    clock = time.time()
    exe = install(a.tools)
    folder = a.mod
    os.makedirs(folder, exist_ok=True)
    entries = current_entries(folder)
    with open(os.path.join(folder, "mod.txt"), "w") as f:
        f.write(MOD_INFO)

    by_level = read_index(a.export)
    todo, keys, sizes = [], set(), []
    for level in by_level:  # the exporter's order: the levels as asked, then the menus
        sources = by_level[level]
        prune(folder, entries, level, {s["key"] for s in sources})
        for s in sources:
            e = entries.get(s["key"])
            if (e is None or not os.path.exists(os.path.join(folder, e[6]))) and s["key"] not in keys:
                keys.add(s["key"])
                todo.append(s)
        if level != MENUS:
            original = sum(chain_size(s["width"], s["height"]) * s["frames"] for s in sources)
            hd = sum(chain_size(s["width"] * SCALE, s["height"] * SCALE) * s["frames"]
                     for s in sources)
            sizes.append("LEVEL%d: %d textures, colour textures %d MB, HD %d MB"
                         % (level, len(sources), original >> 20, hd >> 20))
    save_manifest(folder, entries)
    if todo:
        make(exe, a.export, folder, todo, entries, a.jobs)
    save_manifest(folder, entries)
    total = sum(os.path.getsize(os.path.join(d, n)) for d, _, ns in os.walk(folder) for n in ns)
    for line in sizes:
        print(line)
    print("build_hd_mod: %d textures made, %d in the mod, %d MB on disk, %.0f s"
          % (len(todo), len(entries), total >> 20, time.time() - clock))


if __name__ == "__main__":
    main()

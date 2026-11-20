#!/usr/bin/env python3
# ============================================================================
# kurt_pack.py -- Kurt's ETC1A4 frames as ONE file, kurt.pak, for romfs:/kurt.
#
#   mdk_3ds/tools/kurt_pack.py <set dir> [<out .pak>]
#
# <set dir> is kurt_etc1_convert.py's output (manifest.txt + textures/**.t3x);
# the default output is <set dir>/kurt.pak. kurt_etc1_convert.py calls write_pack
# after every run; this CLI exists for the host tests and for rebuilding a pack
# from a set without re-encoding. Standard library only.
#
# THE FORMAT is mdk_core/src/formats/kurt_pack.h's (the reader, and the layout's
# documentation): a 32-byte header ("KPAK", version 1), a 40-byte index entry per
# file, then each .t3x at a 16-byte-aligned offset. Little-endian.
#
# THE ORDER IS THE POINT. The start set (KurtSprite's kStartSet: K_STILL,
# K_FALL, K_LAND, K_IDLE, in that order) comes first and contiguous, so a level
# load reads the index and the start set in ONE read; then the other animations
# by name, each animation's frames in frame order with a level's own copy after
# the every-level one, so one animation is one contiguous range for any level
# (the stream's one read per animation).
#
# DETERMINISTIC: the bytes depend only on the manifest's rows and the .t3x
# bytes (sorted order, zero padding, no timestamps). INCREMENTAL: an unchanged
# pack is not rewritten, so its mtime (and the RomFS hard link) stays.
# ============================================================================
import os
import struct
import sys

MAGIC = b"KPAK"
VERSION = 1
HEADER_BYTES = 32
ENTRY_BYTES = 40
NAME_BYTES = 24
ALIGN = 16
FLAG_START_SET = 1
# KurtSprite::kStartSet (mdk_core/src/game/kurt_sprite.cpp), in its order.
START_SET = ("K_STILL", "K_FALL", "K_LAND", "K_IDLE")


def _align(n):
    return (n + ALIGN - 1) // ALIGN * ALIGN


def split_name(name):
    """K_RUN_12 -> ("K_RUN", 12). A name with no numeric tail is its own
    animation, frame 0."""
    anim, sep, frame = name.rpartition("_")
    if sep and frame.isdigit():
        return anim, int(frame)
    return name, 0


def read_set(set_dir):
    """The set's frames from its manifest: (name, level, width, height, bytes)."""
    frames = []
    with open(os.path.join(set_dir, "manifest.txt")) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            cols = line.rstrip("\n").split("\t")
            if len(cols) < 7:
                raise ValueError("manifest row has %d columns: %r" % (len(cols), line))
            rel, w, h, size = cols[0], int(cols[2]), int(cols[3]), int(cols[6])
            parts = rel.split("/")
            level = 0
            if len(parts) == 3 and parts[1].startswith("LEVEL"):
                level = int(parts[1][5:])
            elif len(parts) != 2:
                raise ValueError("unexpected manifest key %r" % rel)
            with open(os.path.join(set_dir, rel + ".t3x"), "rb") as t:
                data = t.read()
            if len(data) != size:
                raise ValueError("%s.t3x is %d bytes, the manifest says %d" % (rel, len(data), size))
            frames.append((parts[-1], level, w, h, data))
    return frames


def order_key(frame):
    name, level = frame[0], frame[1]
    anim, number = split_name(name)
    group = (0, START_SET.index(anim), "") if anim in START_SET else (1, 0, anim)
    return group + (number, level, name)


def build_pack(frames):
    """The pack's bytes for (name, level, width, height, bytes) rows."""
    frames = sorted(frames, key=order_key)
    seen = set()
    for name, level, w, h, data in frames:
        raw = name.encode("ascii")
        if not raw or len(raw) >= NAME_BYTES:
            raise ValueError("frame name %r does not fit %d bytes" % (name, NAME_BYTES))
        if not (0 <= level <= 255) or not (0 < w <= 1024) or not (0 < h <= 1024) or not data:
            raise ValueError("frame %s (level %d) %dx%d %d bytes is out of range" %
                             (name, level, w, h, len(data)))
        if (name, level) in seen:
            raise ValueError("frame %s (level %d) twice" % (name, level))
        seen.add((name, level))

    data_offset = _align(HEADER_BYTES + ENTRY_BYTES * len(frames))
    offsets = []
    at = data_offset
    start_end = data_offset
    for name, level, w, h, data in frames:
        offsets.append(at)
        at = _align(at + len(data))
        if split_name(name)[0] in START_SET:
            start_end = offsets[-1] + len(data)
    file_bytes = offsets[-1] + len(frames[-1][4]) if frames else data_offset

    out = bytearray(struct.pack("<4sHHIIIIII", MAGIC, VERSION, HEADER_BYTES, len(frames),
                                ENTRY_BYTES, data_offset, start_end, file_bytes, ALIGN))
    for (name, level, w, h, data), offset in zip(frames, offsets):
        flags = FLAG_START_SET if split_name(name)[0] in START_SET else 0
        out += struct.pack("<24sBBHHHII", name.encode("ascii"), level, flags, 0, w, h,
                           offset, len(data))
    for (name, level, w, h, data), offset in zip(frames, offsets):
        out += b"\0" * (offset - len(out))
        out += data
    assert len(out) == file_bytes
    return bytes(out)


def write_pack(set_dir, out_path=None):
    """Writes <set dir>/kurt.pak (or out_path) unless it already holds these
    bytes. Returns (path, frames, bytes, start set bytes, rewritten)."""
    out_path = out_path or os.path.join(set_dir, "kurt.pak")
    frames = read_set(set_dir)
    pack = build_pack(frames)
    start_end = struct.unpack_from("<I", pack, 20)[0]
    data_offset = struct.unpack_from("<I", pack, 16)[0]
    rewritten = True
    if os.path.exists(out_path):
        with open(out_path, "rb") as f:
            rewritten = f.read() != pack
    if rewritten:
        tmp = out_path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(pack)
        os.replace(tmp, out_path)
    return out_path, len(frames), len(pack), start_end - data_offset, rewritten


def main(argv):
    if len(argv) not in (2, 3):
        print("usage: kurt_pack.py <set dir> [<out .pak>]", file=sys.stderr)
        return 2
    try:
        path, count, size, start, rewritten = write_pack(argv[1], argv[2] if len(argv) == 3 else None)
    except (OSError, ValueError) as e:
        print("kurt_pack: %s" % e, file=sys.stderr)
        return 1
    print("kurt_pack: %s: %d frames, %d bytes (start set %d)%s" %
          (path, count, size, start, "" if rewritten else ", unchanged"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

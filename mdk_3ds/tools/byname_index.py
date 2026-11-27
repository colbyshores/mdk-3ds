#!/usr/bin/env python3
# ============================================================================
# byname_index.py -- the orig pack's name index as a binary table
# (romfs:/orig/byname.bin), at build time.
#
# USAGE:  byname_index.py <orig-dir> [--out <file>]
#
# Reads <orig-dir>/byname.txt and <orig-dir>/manifest.txt (arena_etc1_convert.py
# writes both) and writes <orig-dir>/byname.bin (or --out). stage_romfs.sh runs it
# on the STAGED orig/, so the table is made from exactly the files the RomFS
# ships, every build.
#
# WHY: the game built this index from byname.txt at every level load -- every
# level's rows parsed to keep one level's -- 2.53-2.64 s of the Old 3DS's main
# core per load (docs/WORK_LEDGER.md). The table is the same rows cut by level
# and joined with the manifest's source sizes, so the console reads a header and
# its level's block and parses nothing.
#
# THE FORMAT is mdk_core/src/formats/byname_index.h, and this file is its other
# half: BynameIndex::buildBlock makes the same block bytes from the same text
# (the game's fallback when the table is refused), and the host tests
# (tests/formats/byname_index_tests.cpp) check the two against each other and
# against the text index the game used to build. Change one, change all three.
#
# THE STAMP: the size and FNV-1a 64 of manifest.txt and byname.txt. The console
# refuses a table whose stamp is not the RomFS's (game/orig_byname.cpp) and
# builds the index from byname.txt instead.
#
# Deterministic: the same inputs give the same bytes. No dependencies beyond
# the standard library.
# ============================================================================

import argparse
import os
import struct
import sys

MAGIC = b"MDKBYNM\0"
VERSION = 1
LEVEL_SLOTS = 16
HEADER_BYTES = 64 + LEVEL_SLOTS * 16
NO_KEY = 0xFFFFFFFF
FLAG_DIMS = 1
MAX_ROWS = 0xFFFF
MAX_LINE_CHARS = 510   # the game's fgets(512) lines: longer ones are skipped


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def c_atoi(data):
    """The C library's atoi on a NUL-free byte string (wraps like int32)."""
    i = 0
    while i < len(data) and data[i] in b" \t\n\v\f\r":
        i += 1
    negative = False
    if i < len(data) and data[i] in b"+-":
        negative = data[i] == ord("-")
        i += 1
    value = 0
    while i < len(data) and 48 <= data[i] <= 57:
        value = (value * 10 + data[i] - 48) & 0xFFFFFFFF
        i += 1
    if negative:
        value = (-value) & 0xFFFFFFFF
    return value - (1 << 32) if value >= (1 << 31) else value


def lower(data):
    return bytes(c + 32 if 65 <= c <= 90 else c for c in data)


def lines(data):
    """(line bytes without the newline, had a newline) for the lines the game
    reads: one over MAX_LINE_CHARS is skipped."""
    pos = 0
    while pos < len(data):
        nl = data.find(b"\n", pos)
        end = nl if nl >= 0 else len(data)
        line = data[pos:end]
        pos = end + 1 if nl >= 0 else len(data)
        if len(line) > MAX_LINE_CHARS:
            continue
        yield line, nl >= 0


def manifest_dims(data):
    """rel -> (src_w << 16) | (src_h & 0xFFFF), as the game's manifest loader
    (packs_3ds.cpp loadCompressedManifest) reads columns 3 and 4."""
    dims = {}
    for line, had_newline in lines(data):
        text = (line + b"\n" if had_newline else line).split(b"\0", 1)[0]
        if not text or text[:1] in (b"#", b"\n"):
            continue
        tab = text.find(b"\t")
        if tab < 0:
            continue
        key = text[:tab]
        fmt = tab + 1
        src_w = text.find(b"\t", fmt)
        src_h = text.find(b"\t", src_w + 1) if src_w >= 0 else -1
        if src_w >= 0 and src_h >= 0:
            w = c_atoi(text[src_w + 1:]) & 0xFFFFFFFF
            h = c_atoi(text[src_h + 1:]) & 0xFFFF
            dims[key] = ((w << 16) | h) & 0xFFFFFFFF
    return dims


def byname_rows(data):
    """(level, arena, name, rel) for every row the game's text index reads."""
    rows = []
    for line, _ in lines(data):
        if not line or line[:1] == b"#" or b"\0" in line:
            continue
        fields = line.split(b"\t")
        if len(fields) != 4:
            continue
        rows.append((c_atoi(fields[0]), fields[1], fields[2], fields[3]))
    return rows


def build_block(rows, dims):
    """One level's block (byname_index.h), the bytes BynameIndex::buildBlock makes."""
    if len(rows) > MAX_ROWS:
        raise ValueError("%d rows: more than a block holds" % len(rows))
    strings = bytearray()
    offsets = {}

    def intern(s):
        if s not in offsets:
            offsets[s] = len(strings)
            strings.extend(s + b"\0")
        return offsets[s]

    arena_index = {}
    arena_table = []
    packed = []
    names = []
    for _, arena, name, rel in rows:
        if arena not in arena_index:
            if len(arena_table) >= MAX_ROWS:
                raise ValueError("more arenas than a block holds")
            arena_index[arena] = len(arena_table)
            arena_table.append(intern(arena))
        lname = lower(name)
        names.append(lname)
        name_off = intern(lname)
        rel_off = intern(rel)
        mark = rel.find(b"@")
        key_off = NO_KEY if mark < 0 else intern(lower(rel[mark + 1:]))
        size = dims.get(rel)
        packed.append(struct.pack("<HHIIII", arena_index[arena],
                                  FLAG_DIMS if size is not None else 0,
                                  name_off, rel_off, key_off, size if size is not None else 0))
    order = sorted(range(len(rows)), key=lambda i: (names[i], i))
    out = bytearray(struct.pack("<IIII", len(rows), len(arena_table), len(strings), 0))
    for offset in arena_table:
        out += struct.pack("<I", offset)
    for row in packed:
        out += row
    for i in order:
        out += struct.pack("<I", i)
    out += strings
    while len(out) % 4:
        out.append(0)
    return bytes(out)


def build(manifest, byname):
    dims = manifest_dims(manifest)
    by_level = {}
    for row in byname_rows(byname):
        if not 0 <= row[0] < LEVEL_SLOTS:
            raise ValueError("a row for level %d: the table has slots for 0-%d"
                             % (row[0], LEVEL_SLOTS - 1))
        by_level.setdefault(row[0], []).append(row)
    blocks = {level: build_block(rows, dims) for level, rows in by_level.items()}
    slots = []
    body = bytearray()
    for level in range(LEVEL_SLOTS):
        block = blocks.get(level)
        if block is None:
            slots.append((0, 0, 0))
            continue
        slots.append((HEADER_BYTES + len(body), len(block), fnv1a64(block)))
        body += block
    file_bytes = HEADER_BYTES + len(body)
    header = bytearray(MAGIC)
    header += struct.pack("<III", VERSION, LEVEL_SLOTS, file_bytes)
    header += struct.pack("<IQ", len(manifest), fnv1a64(manifest))
    header += struct.pack("<IIQ", len(byname), 0, fnv1a64(byname))
    header += b"\0" * (64 - len(header))
    for offset, size, digest in slots:
        header += struct.pack("<IIQ", offset, size, digest)
    assert len(header) == HEADER_BYTES
    return bytes(header) + bytes(body), {level: len(rows) for level, rows in by_level.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0] if __doc__ else None)
    ap.add_argument("orig_dir")
    ap.add_argument("--out", help="the table's path (default <orig-dir>/byname.bin)")
    a = ap.parse_args()
    with open(os.path.join(a.orig_dir, "manifest.txt"), "rb") as f:
        manifest = f.read()
    with open(os.path.join(a.orig_dir, "byname.txt"), "rb") as f:
        byname = f.read()
    try:
        table, counts = build(manifest, byname)
    except ValueError as e:
        print("byname_index: ERROR: %s" % e, file=sys.stderr)
        return 1
    out = a.out or os.path.join(a.orig_dir, "byname.bin")
    tmp = out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(table)
    os.replace(tmp, out)
    print("byname_index: %s: %d bytes, %s" % (
        out, len(table),
        ", ".join("level %d %d rows" % (level, counts[level]) for level in sorted(counts))))
    return 0


if __name__ == "__main__":
    sys.exit(main())

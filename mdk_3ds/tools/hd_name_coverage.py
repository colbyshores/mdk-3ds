#!/usr/bin/env python3
# ============================================================================
# hd_name_coverage.py -- which HD textures the console can find by name.
#
#   mdk_3ds/tools/hd_name_coverage.py <hd pack dir> <orig pack dir>
#
# The 3DS finds a surface's HD file without hashing it (docs/HD_TEXTURES_3DS_
# SPEC.md 3.1): the orig pack's byname.txt names every arena/level texture by
# (level, arena, name) and its file carries the content key, and the HD file of
# the same key is that texture's upscale. An HD texture whose key no byname.txt
# row carries is never found on the console -- with an HD pack installed its
# surface is drawn from its standard-definition pixels -- so this lists them,
# per level, with their names and sizes, and the per-level set sizes the
# residency tiers are chosen from (512 as built, 256 with the levels over 256
# dropped). Host only; reads the two manifests, writes nothing.
# ============================================================================
import collections
import os
import sys


def chain(w, h, a4, cap):
    while cap and (w > cap or h > cap) and w >= 16 and h >= 16:
        w //= 2
        h //= 2
    total = 0
    while w >= 8 and h >= 8:
        total += w * h * (8 if a4 else 4) // 8
        if w == 8 or h == 8:
            break
        w //= 2
        h //= 2
    return total


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: hd_name_coverage.py <hd pack dir> <orig pack dir>")
    hd_dir, orig_dir = sys.argv[1], sys.argv[2]
    keyed = set()
    for line in open(os.path.join(orig_dir, "byname.txt")):
        if line.startswith("#") or not line.strip():
            continue
        level, arena, name, rel = line.rstrip("\n").split("\t")
        if "@" in rel:
            keyed.add((level, rel.split("@", 1)[1].lower()))
    missing = collections.defaultdict(list)
    sizes = collections.defaultdict(lambda: [0, 0, 0])
    for line in open(os.path.join(hd_dir, "manifest.txt")):
        if line.startswith("#") or not line.strip():
            continue
        f = line.rstrip("\n").split("\t")
        parts = f[0].split("/")
        if parts[0] != "textures" or "@" not in f[0] or len(parts) < 3:
            continue
        level = parts[1].replace("LEVEL", "")
        key = f[0].split("@", 1)[1].lower()
        w, h, a4 = int(f[4]), int(f[5]), f[1] == "etc1a4"
        if (level, key) in keyed:
            s = sizes[level]
            s[0] += 1
            s[1] += chain(w, h, a4, 0)
            s[2] += chain(w, h, a4, 256)
        else:
            missing[level].append("%s (%sx%s source %sx%s)" % (parts[-1], f[4], f[5], f[2], f[3]))
    for level in sorted(set(list(sizes) + list(missing))):
        s = sizes[level]
        kurt = sum(1 for m in missing[level] if m.startswith("K_"))
        print("level %s: %d HD textures found by name, set %.2f MiB at 512, %.2f MiB at 256; "
              "%d not found by name (%d of them Kurt's frames, which the 3DS draws from kurt.pak)"
              % (level, s[0], s[1] / 1048576.0, s[2] / 1048576.0, len(missing[level]), kurt))
        for m in missing[level]:
            if not m.startswith("K_"):
                print("  not found by name: " + m)
    return 0


if __name__ == "__main__":
    sys.exit(main())

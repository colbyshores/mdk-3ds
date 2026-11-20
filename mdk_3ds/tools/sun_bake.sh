#!/usr/bin/env bash
# ============================================================================
# sun_bake.sh -- bake every level's static light for the 3DS (EnhancedToggles::
# lightBaked; mdk_3ds/src/render/sun_bake.h), at BUILD time: each Lit side's
# light as a vertex colour (in the .sbk) and the static shadow as a grey L4
# multiplier map (the .t3x).
#
#   mdk_3ds/tools/sun_bake.sh [<out dir>]
#
# Default <out dir>: build-sun-bake/ at the repo root (ignored as build-*/, and
# OUTSIDE mdk_3ds/build/ so `make clean` keeps it). stage_romfs.sh runs this and
# links <out>/level<N>.sbk and <out>/level<N>.t3x into the RomFS as
# romfs:/sunbake/. NOTHING HERE IS COMMITTED: the bake is derived from the
# player's own game data, read from MDK_DATA_DIR (default <repo>/MDK).
#
# THE BAKER is a host build of mdk_core's own parsers and arena build
# (mdk_3ds/tools/sun_bake/: sun_bake_level + sun_baker), so the triangles it keys
# are the ones the 3DS meshes hold. It writes level<N>.sbk (charts, keys with each
# side's vertex colour, the objects' depth map) and level<N>.grey (the atlas of the
# shadow's multiplier m); this script turns the atlas into the .t3x OFFLINE with
# tex3ds -- L4 by default (tex3ds owns the PICA's tiling and nibble order) -- and
# prints the encoding's error against the baked m, from tex3ds' own decoded
# preview. ~2-4 s a level.
#
# INCREMENTAL AND DETERMINISTIC: the same data and the same baker give the same
# bytes. A stamp records the baker binary's hash, tex3ds' and this script's, the
# options and the game data's sizes and times (TRAVERSE/, recursively); when all
# match and every .sbk and .t3x is there, nothing runs. Any change re-bakes
# every level (~25 s).
#
# SUN_BAKE_ARGS passes options to the baker (and is part of the stamp): "--l8"
# for the 8-bit atlas (twice L4's bytes, the shadow's shade to a byte),
# "--texel <units>", "--budget <L4 bytes>", "--chart-max <texels>", "--cone <cos>"
# (sun_bake_level.cpp; defaults L4, 2.0, 262144 doubled for a level that needs
# it, 256, 0.99).
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="${1:-$REPO/build-sun-bake}"
DATA="${MDK_DATA_DIR:-$REPO/MDK}"
TEX3DS="${TEX3DS:-/opt/devkitpro/tools/bin/tex3ds}"

if [ ! -d "$DATA/TRAVERSE" ]; then
    echo "sun_bake: no MDK data at $DATA (set MDK_DATA_DIR); nothing baked" >&2
    exit 1
fi
if [ ! -x "$TEX3DS" ]; then
    echo "sun_bake: no tex3ds at $TEX3DS; nothing baked" >&2
    exit 1
fi
mkdir -p "$OUT"

# The host baker, built with the host compiler: the 3DS Makefile exports
# CXX=arm-none-eabi-g++ (kurt_etc1_convert.py says the same). Under a parent make
# its jobserver is inherited; alone, at most 8 jobs (the owner's cap).
jobs=""
[ -n "${MAKEFLAGS:-}" ] || jobs="-j8"
# shellcheck disable=SC2086
make -s $jobs -C "$HERE/sun_bake" BUILD="$OUT/build" CXX="${HOST_CXX:-g++}" >&2
BAKER="$OUT/build/sun_bake_level"

# The levels the data has (TRAVERSE/LEVEL<N>).
LEVELS=""
for dir in "$DATA"/TRAVERSE/LEVEL*; do
    [ -d "$dir" ] || continue
    n="${dir##*LEVEL}"
    case "$n" in (*[!0-9]*|"") continue ;; esac
    LEVELS="$LEVELS $n"
done
LEVELS="$(echo $LEVELS | tr ' ' '\n' | sort -n | tr '\n' ' ')"
if [ -z "${LEVELS// /}" ]; then
    echo "sun_bake: no TRAVERSE/LEVEL<N> in $DATA; nothing baked" >&2
    exit 1
fi

FORMAT="l4"
case " ${SUN_BAKE_ARGS:-} " in (*" --l8 "*) FORMAT="l8" ;; esac

STAMP="$OUT/stamp.txt"
want="$( {
    echo "baker $(sha256sum "$BAKER" | cut -d' ' -f1)"
    echo "tex3ds $(sha256sum "$TEX3DS" | cut -d' ' -f1)"
    echo "script $(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)"
    echo "levels $LEVELS"
    echo "args ${SUN_BAKE_ARGS:-}"
    ( cd "$DATA" && find -L TRAVERSE -type f -printf '%s %T@ %p\n' | sort -k3 )
} | sha256sum | cut -d' ' -f1)"

have_all=1
for n in $LEVELS; do
    { [ -f "$OUT/level$n.sbk" ] && [ -f "$OUT/level$n.t3x" ]; } || have_all=0
done
if [ "$have_all" = 1 ] && [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$want" ]; then
    echo "sun_bake: up to date ($LEVELS)" >&2
    exit 0
fi

rm -f "$STAMP"
# shellcheck disable=SC2086
MDK_DATA_DIR="$DATA" "$BAKER" "$OUT" $LEVELS ${SUN_BAKE_ARGS:-} >&2

# The atlases: raw m -> PNG (the size from the .sbk's header) -> tex3ds, and the
# encoding's error from tex3ds' decoded preview (the preview is not kept).
for n in $LEVELS; do
    # (L4: rounded to its 16 levels here -- tex3ds truncates, which would darken
    # a shade by up to 15/255.)
    python3 - "$OUT/level$n.sbk" "$OUT/level$n.grey" "$OUT/level$n.png" "$FORMAT" <<'PY'
import struct, sys
from PIL import Image
with open(sys.argv[1], "rb") as f:
    head = f.read(136)
w, h = struct.unpack_from("<HH", head, 20)
with open(sys.argv[2], "rb") as f:
    grey = f.read()
if len(grey) != w * h:
    sys.exit("sun_bake: %s is %d bytes, not %dx%d" % (sys.argv[2], len(grey), w, h))
if sys.argv[4] == "l4":
    grey = bytes(((v * 15 + 127) // 255) * 17 for v in grey)
Image.frombytes("L", (w, h), grey).save(sys.argv[3])
PY
    "$TEX3DS" -f "$FORMAT" -o "$OUT/level$n.t3x.tmp" -p "$OUT/level$n.preview.png" \
        "$OUT/level$n.png" >&2
    mv "$OUT/level$n.t3x.tmp" "$OUT/level$n.t3x"
    python3 - "$n" "$FORMAT" "$OUT/level$n.grey" "$OUT/level$n.preview.png" "$OUT/level$n.t3x" <<'PY' >&2
import os, sys
import numpy as np
from PIL import Image
enc = np.asarray(Image.open(sys.argv[4]).convert("L"), dtype=np.int16)
src = np.fromfile(sys.argv[3], dtype=np.uint8).astype(np.int16).reshape(enc.shape)  # the baked m
d = np.abs(src - enc)
print("sun_bake: level %s %s: %d bytes of .t3x (%d on the GPU); the encoding against the "
      "baked m: mean |d| %.2f, max %d (0..255)" % (
          sys.argv[1], sys.argv[2], os.path.getsize(sys.argv[5]),
          src.size // (2 if sys.argv[2] == "l4" else 1), d.mean(), int(d.max())))
PY
    rm -f "$OUT/level$n.preview.png" "$OUT/level$n.png"
done
echo "$want" > "$STAMP"

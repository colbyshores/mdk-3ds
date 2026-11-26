#!/usr/bin/env bash
# ============================================================================
# build_hd_pack.sh -- the 3DS HD texture pack, made locally, end to end.
#
#   mdk_3ds/tools/build_hd_pack.sh [--levels 3,4,5,6,7,8] [--max 512] [--out DIR] [--mod DIR]
#
# THERE IS NO PACK TO DOWNLOAD, AND THAT IS UPSTREAM'S DESIGN: the C# port
# (github.com/nautilusbits/mdk-sdl) makes its HD textures "locally on your GPU,
# from your copy of the game ... Nothing upscaled is distributed", and its
# releases carry no pack. So the build makes it the same way and converts it.
# No .NET is needed: the C# generator (src/, `--upscale-textures`) is the
# behaviour reference, and these steps reproduce it:
#
#   1. THE EXPORT (C++). mdk_3ds/tools/hd_export/hd_tex_export, built here with
#      the host compiler from mdk_core's own loaders: every texture and 2D image
#      the HD mod replaces, read from MDK_DATA_DIR in the C# generator's order
#      and keyed as the game keys them. Into a temporary folder.
#   2. THE MOD (Python). mdk_3ds/tools/build_hd_mod.py: fetches the pinned
#      Real-ESRGAN release (SHA-256 checked) into --tools once, pads and bleeds
#      the frames, upscales them 2x on the Vulkan GPU and writes the mod in the
#      PC layout (docs/modding.md) to --mod. It keeps what is current, so a
#      re-run is cheap (the manifest is the C#'s, so a mod the C# made is
#      picked up as it is).
#   3. THE 3DS PACK (Python). mdk_3ds/tools/hd_pack_convert.py: bleed,
#      power-of-two at most --max (512; Kurt's sprite frames at most 128), ETC1 /
#      ETC1A4 by alpha, tex3ds -q high -m gaussian (mip chain prebuilt),
#      manifest.txt. Incremental.
#
# Output: --out (default build-hd-pack/ at the repo root, ignored as build-*/),
# the tree the 3DS reads from sdmc:/3ds/mdk/hd/ (docs/HD_TEXTURES_3DS_SPEC.md).
# It is NOT committed: it is derived from the commercial game data. It lives
# OUTSIDE mdk_3ds/build/ on purpose: `make -C mdk_3ds clean` is `rm -rf build`,
# required on every -D flag change, and it deleted the first full pack
# (2026-11-18). The mod (--mod, default build-hd-mod/) and the upscaler
# (--tools, default build-hd-tools/) are kept beside it for the next run.
#
# Needs: MDK_DATA_DIR (default <repo>/MDK), a host C++ compiler and zlib (as
# the other host exporters), Python 3 with numpy and Pillow, a Vulkan GPU for
# Real-ESRGAN (there is no CPU fallback upstream either), network once for the
# Real-ESRGAN download (MDK_REALESRGAN_URL may name a local file:// copy; its
# SHA-256 is still checked), and tex3ds (devkitPro).
# ============================================================================
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LEVELS=3,4,5,6,7,8
MAX=512
OUT="$ROOT/build-hd-pack"
MOD="$ROOT/build-hd-mod"
TOOLS="$ROOT/build-hd-tools"
while [ $# -gt 0 ]; do
    case "$1" in
        --levels) LEVELS=${2:?--levels needs a list}; shift 2 ;;
        --max)    MAX=${2:?--max needs a size}; shift 2 ;;
        --out)    OUT=${2:?--out needs a directory}; shift 2 ;;
        --mod)    MOD=${2:?--mod needs a directory}; shift 2 ;;
        --tools)  TOOLS=${2:?--tools needs a directory}; shift 2 ;;
        -h|--help) awk 'NR>2 { if ($0 ~ /^# ={10,}/) exit; print }' "$0"; exit 0 ;;
        *) echo "build_hd_pack: unknown argument $1" >&2; exit 2 ;;
    esac
done
case "$LEVELS" in *[!0-9,]*|'') echo "build_hd_pack: --levels is a list like 3,7" >&2; exit 2 ;; esac

export MDK_DATA_DIR=${MDK_DATA_DIR:-$ROOT/MDK}
[ -d "$MDK_DATA_DIR" ] || { echo "build_hd_pack: no MDK data at $MDK_DATA_DIR" >&2; exit 1; }

# 1. The export.
EXPORTER="$TOOLS/hd_export/hd_tex_export"
make -s -C "$ROOT/mdk_3ds/tools/hd_export" BUILD="$TOOLS/hd_export"
EXPORT=$(mktemp -d "${TMPDIR:-/tmp}/mdk-hd-export.XXXXXX")
trap 'rm -rf "$EXPORT"' EXIT
echo "build_hd_pack: exporting levels $LEVELS"
# shellcheck disable=SC2046  # the level list splits into arguments on purpose
"$EXPORTER" "$EXPORT" $(echo "$LEVELS" | tr ',' ' ')

# 2. The mod (incremental).
echo "build_hd_pack: making the HD mod in $MOD (Real-ESRGAN, local GPU)"
python3 "$ROOT/mdk_3ds/tools/build_hd_mod.py" "$EXPORT" "$MOD" --tools "$TOOLS"
[ -d "$MOD/textures" ] || { echo "build_hd_pack: no $MOD/textures after the mod step" >&2; exit 1; }

# 3. The 3DS pack.
mkdir -p "$OUT"
python3 "$ROOT/mdk_3ds/tools/hd_pack_convert.py" "$MOD" "$OUT" --max "$MAX"
echo "build_hd_pack: pack in $OUT ($(du -sh "$OUT" | cut -f1))"

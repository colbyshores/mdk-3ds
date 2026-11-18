#!/usr/bin/env bash
# ============================================================================
# build_hd_pack.sh -- the 3DS HD texture pack, made locally, end to end.
#
#   mdk_3ds/tools/build_hd_pack.sh [--levels 3,4,5,6,7,8] [--max 512] [--out DIR]
#
# THERE IS NO PACK TO DOWNLOAD, AND THAT IS UPSTREAM'S DESIGN: the C# port
# (github.com/nautilusbits/mdk-sdl) makes its HD textures "locally on your GPU,
# from your copy of the game ... Nothing upscaled is distributed", and its
# releases carry no pack. So the build makes it the same way and converts it:
#
#   1. THE GENERATOR. The C# oracle (this repo's src/, read-only) is built into
#      a scratch tree by tests/frames/capture_oracle.sh's own staging (reused,
#      not copied) unless it is already built there.
#   2. THE MOD. `mdk_sdl --upscale-textures=<levels>` (HdGenerator): it fetches
#      the pinned Real-ESRGAN release itself (SHA-256 checked) into tools/ next to
#      the program, exports each level's textures from MDK_DATA_DIR, upscales
#      them 2x on the Vulkan GPU and writes mods/hd-textures/ in the PC layout
#      (docs/modding.md). It keeps what is current, so a re-run is cheap.
#   3. THE 3DS PACK. mdk_3ds/tools/hd_pack_convert.py: bleed, power-of-two at
#      most --max (512; Kurt's sprite frames at most 128), ETC1 / ETC1A4 by alpha, tex3ds -q high -m gaussian (mip
#      chain prebuilt), manifest.txt. Incremental.
#
# Output: --out (default mdk_3ds/build/hd/), the tree the 3DS reads from
# sdmc:/3ds/mdk/hd/ (docs/HD_TEXTURES_3DS_SPEC.md). It is NOT committed: it is
# derived from the commercial game data.
#
# Needs: MDK_DATA_DIR (default <repo>/MDK), a .NET 10 host (MDK_DOTNET, default
# /tmp/mdk-dotnet10/dotnet), a Vulkan GPU for Real-ESRGAN (there is no CPU
# fallback upstream either), network once for the Real-ESRGAN download, tex3ds.
# ============================================================================
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LEVELS=3,4,5,6,7,8
MAX=512
OUT="$ROOT/mdk_3ds/build/hd"
while [ $# -gt 0 ]; do
    case "$1" in
        --levels) LEVELS=${2:?--levels needs a list}; shift 2 ;;
        --max)    MAX=${2:?--max needs a size}; shift 2 ;;
        --out)    OUT=${2:?--out needs a directory}; shift 2 ;;
        -h|--help) awk 'NR>2 { if ($0 ~ /^# ={10,}/) exit; print }' "$0"; exit 0 ;;
        *) echo "build_hd_pack: unknown argument $1" >&2; exit 2 ;;
    esac
done
case "$LEVELS" in *[!0-9,]*|'') echo "build_hd_pack: --levels is a list like 3,7" >&2; exit 2 ;; esac

export MDK_DATA_DIR=${MDK_DATA_DIR:-$ROOT/MDK}
export MDK_DOTNET=${MDK_DOTNET:-/tmp/mdk-dotnet10/dotnet}
export DOTNET_ROOT=${DOTNET_ROOT:-$(dirname "$MDK_DOTNET")}
SCRATCH=${MDK_FRAME_ORACLE_DIR:-${TMPDIR:-/tmp}/mdk-frame-oracle}
export MDK_FRAME_ORACLE_DIR=$SCRATCH
BIN_DIR="$SCRATCH/src/Mdk.App/bin/Debug/net10.0"

[ -d "$MDK_DATA_DIR" ] || { echo "build_hd_pack: no MDK data at $MDK_DATA_DIR" >&2; exit 1; }
[ -x "$MDK_DOTNET" ] || { echo "build_hd_pack: no dotnet at $MDK_DOTNET (set MDK_DOTNET)" >&2; exit 1; }

# 1. The generator: built by the frame oracle's staging when it is not there yet
#    (that script builds the scratch tree as its first step; its frame is thrown away).
if [ ! -f "$BIN_DIR/mdk_sdl.dll" ]; then
    echo "build_hd_pack: building the C# oracle into $SCRATCH"
    tmp_frame=$(mktemp --suffix=.bmp)
    "$ROOT/tests/frames/capture_oracle.sh" "$tmp_frame" > "$SCRATCH.build.log" 2>&1 || true
    rm -f "$tmp_frame"
    [ -f "$BIN_DIR/mdk_sdl.dll" ] || {
        echo "build_hd_pack: the oracle did not build; see $SCRATCH.build.log" >&2; exit 1; }
fi

# 2. The mod (incremental inside HdGenerator).
echo "build_hd_pack: generating mods/hd-textures for levels $LEVELS (Real-ESRGAN, local GPU)"
(cd "$BIN_DIR" && "$MDK_DOTNET" mdk_sdl.dll --upscale-textures="$LEVELS") |
    awk '/^Upscaling/ { last=$0; next } { print } END { if (last) print last }'
MOD="$BIN_DIR/mods/hd-textures"
[ -d "$MOD/textures" ] || { echo "build_hd_pack: no $MOD/textures after the generator" >&2; exit 1; }

# 3. The 3DS pack.
mkdir -p "$OUT"
python3 "$ROOT/mdk_3ds/tools/hd_pack_convert.py" "$MOD" "$OUT" --max "$MAX"
echo "build_hd_pack: pack in $OUT ($(du -sh "$OUT" | cut -f1))"

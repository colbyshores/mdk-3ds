#!/usr/bin/env bash
# ============================================================================
# compare_linux.sh -- the 3DS frame against the Linux frame, frame-exact.
#
#   mdk_3ds/tools/compare_linux.sh <level> <frame> [outdir]
#
# Builds the 3DS harness payload with a fixed 1/60 s clock and one capture
# (`make AUTOQUIT=90 FIXED=1 CAPTURE=<frame> LEVEL=<level>`), runs it in
# Mandarine on the dedicated display (emurun.sh, DISPLAY=:99 by default --
# AGENTS.md P7), pulls the capture from the lane's emulated SD card, captures
# the Linux build at the same frame (`--fixed-step --capture-frame=<frame>`,
# 400x240), and prints the difference. Linux frames frame-1, frame, frame+1 are
# all measured and the closest is reported, so a one-frame difference in how
# the two counters number frames shows up as an offset, not as a mismatch.
#
# EMULATOR evidence: the 3DS picture is the left-eye render target read back by
# the game (03_frame.inc writeScreenshotBmp), not the emulator's window.
# Writes <outdir>/{3ds,linux,diff}-L<level>-f<frame>.png and a side-by-side.
# ============================================================================
set -uo pipefail

LEVEL=${1:?usage: compare_linux.sh <level> <frame> [outdir]}
FRAME=${2:?usage: compare_linux.sh <level> <frame> [outdir]}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${3:-$ROOT/logs/compare}
mkdir -p "$OUT"
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro} DEVKITARM=${DEVKITARM:-/opt/devkitpro/devkitARM}
export MDK_DATA_DIR=${MDK_DATA_DIR:-$ROOT/MDK}
export DISPLAY=${DISPLAY_FOR_EMULATOR:-:99}

# The -D flags are not tracked by make: these two units read them.
touch "$ROOT/mdk_3ds/src/main_3ds.cpp" "$ROOT/mdk_3ds/src/platform/harness_3ds.cpp"
for try in 1 2 3; do
    if make -C "$ROOT/mdk_3ds" -j"$(nproc)" AUTOQUIT=90 FIXED=1 CAPTURE="$FRAME" LEVEL="$LEVEL" \
         > "$OUT/build.log" 2>&1; then
        break
    fi
    [ "$try" = 3 ] && { echo "compare: 3DS build failed; see $OUT/build.log" >&2; exit 1; }
    sleep 5   # RomFS staging races on NFS ("Directory not empty"); a retry passes
done

LANE_TMP=$(mktemp -d)
trap 'rm -rf "$LANE_TMP" 2>/dev/null' EXIT   # the lane holds emulator runtime mounts; ignore their rm noise
# Enough wall time for load + <frame> fixed frames under emulation.
SECS=$(( 40 + FRAME / 20 ))
[ "$SECS" -gt 600 ] && SECS=600
TMPDIR=$LANE_TMP "$ROOT/mdk_3ds/tools/emurun.sh" --secs "$SECS" > "$OUT/emurun.log" 2>&1
CAP=$(find "$LANE_TMP" -path '*sdmc*/3ds/mdk/hwcap.bmp.log' | head -1)
if [ -z "$CAP" ]; then
    echo "compare: the 3DS run wrote no capture (frame $FRAME not reached in ${SECS}s?)" >&2
    grep -h 'screenshot:\|exiting' $(find "$LANE_TMP" -name mdk.log) 2>/dev/null | tail -3 >&2
    exit 1
fi
cp "$CAP" "$OUT/3ds-L$LEVEL-f$FRAME.bmp"

for f in $((FRAME - 1)) "$FRAME" $((FRAME + 1)); do
    "$ROOT/build-linux/mdk" --headless --fixed-step --level="$LEVEL" --size=400x240 --seed 1 \
        --capture-frame="$f" --capture="$OUT/linux-L$LEVEL-f$f.bmp" > /dev/null 2>&1
done


python3 - "$OUT" "$LEVEL" "$FRAME" <<'PY'
import sys
import numpy as np
from PIL import Image
out, level, frame = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
d3 = np.asarray(Image.open(f"{out}/3ds-L{level}-f{frame}.bmp").convert("RGB"), dtype=int)
best = None
for f in (frame - 1, frame, frame + 1):
    try:
        lx = np.asarray(Image.open(f"{out}/linux-L{level}-f{f}.bmp").convert("RGB"), dtype=int)
    except OSError:
        continue
    if lx.shape != d3.shape:
        continue
    delta = np.abs(lx - d3).max(axis=2)
    bad = float((delta > 16).mean() * 100)
    if best is None or bad < best[0]:
        best = (bad, f, lx, delta)
if best is None:
    print("compare: no comparable Linux frame"); sys.exit(1)
bad, f, lx, delta = best
print(f"compare L{level} 3ds frame {frame} vs linux frame {f}: "
      f"{bad:.2f}% px differ by >16, mean |d| {np.abs(lx - d3).mean():.2f}, max {int(delta.max())}")
Image.fromarray(d3.astype("uint8")).save(f"{out}/3ds-L{level}-f{frame}.png")
Image.fromarray(lx.astype("uint8")).save(f"{out}/linux-L{level}-f{frame}.png")
Image.fromarray(np.clip(delta * 4, 0, 255).astype("uint8")).save(f"{out}/diff-L{level}-f{frame}.png")
side = np.concatenate([lx, d3, np.repeat(np.clip(delta * 4, 0, 255)[..., None], 3, axis=2)], axis=1)
Image.fromarray(side.astype("uint8")).save(f"{out}/side-L{level}-f{frame}.png")
PY

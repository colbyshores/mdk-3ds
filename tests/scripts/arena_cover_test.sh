#!/bin/sh
# arena_cover_test.sh -- the camera's arena drawn over the other one (upstream
# e1eb36bb, the original's 0x41df35), through the real Linux frontend.
#
# WHAT IT PINS. At LEVEL4's snowboard ride end in CMEAT_1 (by the tube to MEAT_3)
# both arenas are drawn, and MEAT_3's flat shell lies 2.3 units above CMEAT_1's
# floor. Depth-tested, the shell won: it covered the tube's floor and walls and
# Kurt below the knees ("half sunk"). The original has no depth buffer and draws
# the camera's arena over the other one, so Kurt shows whole. This counts Kurt's
# dark legs in the lower middle of the frame -- upstream's rides_test.sh check,
# the same place, size and threshold: 0 with the bug, ~2900 fixed -- in both
# looks. The C++ draws the other arena first and the camera arena's depth over it
# (render/draw_queue.h) instead of upstream's depth bands, at full precision.
#
# Needs build-linux/mdk and the game data; skips without them, like
# stream_screen_test.sh. Headless (lavapipe), on DISPLAY :99 (AGENTS.md P7).
ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
BIN="$ROOT/build-linux/mdk"
DATA=${MDK_DATA_DIR:-$ROOT/MDK}

if [ ! -x "$BIN" ]; then
    echo "SKIP ArenaCover (build-linux/mdk is not built; there is no binary to run)"
    exit 0
fi
if [ ! -d "$DATA" ]; then
    echo "SKIP ArenaCover (no game data at $DATA)"
    exit 0
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/mdk-cover.XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT INT TERM

export DISPLAY=:99
if [ -z "${VK_ICD_FILENAMES:-}" ] && [ -f /usr/share/vulkan/icd.d/lvp_icd.json ]; then
    export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
fi
export MDK_DATA_DIR="$DATA"

fail=0
bad() {
    echo "  FAIL $1"
    fail=$((fail + 1))
}

# The pixels of x 290-369, y 340-439 whose brightest channel is under 40, in a
# 640x480 capture (a top-down 32-bit BMP: 54 header bytes, then B G R A).
legs() {
    tail -c +55 "$1" | od -An -v -tu1 | tr -s ' ' '\n' | grep -v '^$' |
        awk '{ c = (NR - 1) % 4; if (c == 0) { m = $1 } else if (c < 3) { if ($1 > m) m = $1 } else {
            p = int((NR - 1) / 4); x = p % 640; y = int(p / 640)
            if (m < 40 && x >= 290 && x < 370 && y >= 340 && y < 440) n++ } } END { print n + 0 }'
}

for look in original enhanced; do
    shot="$WORK/$look.bmp"
    timeout 300 "$BIN" --headless --fixed-step --seed 1 --level=4 --size=640x480 --"$look" \
        --arena=CMEAT_1 --at=62.35,2582.81,-392.90,90 --capture-frame=60 --capture="$shot" \
        >"$WORK/$look.log" 2>&1 || bad "[$look] exited $?"
    if [ ! -s "$shot" ]; then
        bad "[$look] no capture"
        continue
    fi
    n=$(legs "$shot")
    echo "  Kurt's legs ($look): $n"
    [ "$n" -ge 1000 ] || bad "[$look] the tube does not cover MEAT_3's shell: Kurt's legs are hidden"
done

if [ "$fail" -ne 0 ]; then
    echo "FAIL ArenaCover ($fail check(s))"
    exit 1
fi
echo "PASS ArenaCover"

#!/bin/sh
# stream_screen_test.sh — the stream screen (package #18b) against the C# oracle.
#
# WHAT IT PINS. `mdk --stream=N` (the C#'s --stream=N) runs the stream after level N
# through the real frontend: the screen built as StreamScreen builds it (the level's
# index and kind, the default seed of a direct start, full health), its frames fed
# the fixed 1/60 s step a test run takes, Kurt flying with no keys held. Its report
# -- the "Stream after LEVELn" line, the first wall hit, Bones, the end -- must be the
# C# app's own, byte for byte: golden/stream_screen_L{7,8,3}.txt are the C#
# oracle's stdout (`mdk_sdl --hidden --original --gpu=vulkan --size=320x240 --mute
# --wait=50 --stream=N`). Those lines carry Kurt's place, speed and health at each
# moment, so the screen's frame order (Back, Update, sounds, report) and the
# simulation's hookup are what they check; the simulation itself is
# golden/stream_*.txt's (tick1).
#
# And what the C# does with the stream's end: the flow after it (statistics or,
# after LEVEL8, the save prompt then LEVEL5 at the STREAM's health, Game.cs
# AfterStream), and the screen's renderer scope released to its mark.
#
# Needs build-linux/mdk and the game data; skips without them, like
# level_chain_test.sh. Headless (lavapipe), on DISPLAY :99 (AGENTS.md P7).
ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
BIN="$ROOT/build-linux/mdk"
DATA=${MDK_DATA_DIR:-$ROOT/MDK}
GOLDEN="$ROOT/tests/scripts/golden"

if [ ! -x "$BIN" ]; then
    echo "SKIP StreamScreen (build-linux/mdk is not built; there is no binary to run)"
    exit 0
fi
if [ ! -d "$DATA" ]; then
    echo "SKIP StreamScreen (no game data at $DATA)"
    exit 0
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/mdk-stream.XXXXXX") || exit 1
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

# level, frames (past the stream's end and the next level's first frame), and the
# flow line the stream's end must lead to. The fall before the next level is shown
# since package #17b; --skip-falls skips it the C#'s Esc way before its first frame
# (as level_chain_test.sh does), so the frame numbers stay the stream's own.
check() {
    level=$1 frames=$2 after=$3
    log="$WORK/s$level.log"
    timeout 600 "$BIN" --headless --fixed-step --size=320x240 --skip-falls --stream="$level" --frames "$frames" \
        >"$log" 2>&1 || bad "[L$level] exited $?"
    grep '^Stream' "$log" >"$WORK/got$level.txt"
    grep -v '^#' "$GOLDEN/stream_screen_L$level.txt" >"$WORK/want$level.txt"
    if ! cmp -s "$WORK/got$level.txt" "$WORK/want$level.txt"; then
        bad "[L$level] the report differs from the C# oracle's:"
        diff "$WORK/want$level.txt" "$WORK/got$level.txt" | sed 's/^/      /'
    fi
    grep -qF -- "$after" "$log" || bad "[L$level] the log has no \"$after\""
    grep -q 'mem\[stream-teardown\]: .* renderer-log=0$' "$log" ||
        bad "[L$level] the stream's renderer scope was not released to its mark"
}

check 7 2110 "flow: next level 6 at frame 2105, entered at health 100"
check 8 1880 "flow: next level 5 at frame 1877, entered at health 63"
check 3 2025 "flow: the stream after level 3 ended (health 1); the flow shows Statistics"

if [ "$fail" -ne 0 ]; then
    echo "FAIL StreamScreen ($fail check(s))"
    exit 1
fi
echo "PASS StreamScreen (--stream=7, 8 and 3 report what the C# oracle reports, byte for byte; LEVEL5 enters at the Gunter stream's 63 health; the screen's renderer scope is released)"

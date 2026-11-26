#!/bin/sh
# fall_frontend_test.sh -- package #17b, the fall before a level, on the Linux
# frontend: the screen is shown (from --fall=N, and after a new game's briefing),
# Esc skips it, it lands in the level with Kurt's health AND the pickups he took
# (fall::carryIntoLevel on the level's Kurt), and the 3DS's lean way draws it too.
# Headless (DISPLAY :99, lavapipe), the shipped binary, --fixed-step --seed 1 (the
# fall's seed is 1 then), keys from --press (the C#'s TestPresses).
#
# The fall's SIMULATION is pinned against the C# in tick1 (golden/L*_skydive_*.txt)
# and its VIEW's arithmetic too (golden/L*_fallview_*.txt); this is the frontend's
# wiring: which screen follows which, the carry reaching the level, the frames.
#
#   1. --fall=7 --fall-chase (a test aid: the fall probe's `chase` steering): the
#      whole fall, 38 s, into LEVEL7 with three pickups. The level's Kurt holds them
#      (the super chain gun in a slot, the homing missiles as ammo) at the fall's
#      health. The run's own lines are the evidence.
#   2. New Game -> briefing -> the fall, Esc in its intro (Menu.Back) -> LEVEL7 at
#      full health with nothing carried (FallSim.Skip).
#   3. --fall=8 --fall-lean: LEVEL8's fall (Bones) drawn the 3DS's way for 700
#      frames, past the intro.
#
#   usage: sh tests/scripts/fall_frontend_test.sh [repo-root]
#
# Skips (rc 0, with the reason) when the Linux binary or the game data is absent.

ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
BIN="$ROOT/build-linux/mdk"
DATA=${MDK_DATA_DIR:-$ROOT/MDK}

if [ ! -x "$BIN" ]; then
    echo "SKIP FallFrontend (build-linux/mdk is not built; there is no binary to run)"
    exit 0
fi
if [ ! -d "$DATA" ]; then
    echo "SKIP FallFrontend (no game data at $DATA)"
    exit 0
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/mdk-fall.XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT INT TERM

export DISPLAY=:99
if [ -z "${VK_ICD_FILENAMES:-}" ] && [ -f /usr/share/vulkan/icd.d/lvp_icd.json ]; then
    export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
fi
export MDK_DATA_DIR="$DATA"
# A settings file of this test's own (the difficulty is Normal by default).
export MDK_USER_DIR="$WORK"

fail=0
bad() {
    echo "  FAIL $1"
    fail=$((fail + 1))
}

run() {
    log=$1
    shift
    timeout 900 "$BIN" --headless --fixed-step --size=320x240 --seed 1 "$@" >"$log" 2>&1
}

has() {
    grep -qF -- "$2" "$1"
}

# --- 1. The whole fall, chased, into the level ---------------------------------
CHASE="$WORK/chase.log"
run "$CHASE" --fall=7 --fall-chase --frames 2320
rc=$?
[ "$rc" -eq 0 ] || bad "[chase] exited $rc"
for line in \
    "Fall: index 0 difficulty 1 beam 117.65 missiles 2 spread 6.50 missile gap 32 radar gap 63 pickups SW_HOME,SW_GATT,SW_HOME seed 1" \
    "flow: the fall before level 7" \
    "Fall ended t 33.0" \
    "health 84 pickups SW_HOME,SW_GATT,SW_HOME" \
    "level: the fall's carry: health 84, pickups SW_HOME,SW_GATT,SW_HOME; inventory slots 6:1 ammo 16:0:0:0:0 sgun 200" \
    "exiting after 2320 frames"; do
    has "$CHASE" "$line" || bad "[chase] the log has no \"$line\""
done

# --- 2. New game: the briefing, the fall, Esc ----------------------------------
# --menu: the menu without the INTRO1A splash (package #20c), so the presses keep
# their times.
NEW="$WORK/newgame.log"
run "$NEW" --menu --press=Menu.Accept@0.5,Menu.Back@1.2,Menu.Accept@1.4,Menu.Back@3 --frames 260
for line in \
    "flow: the briefing of level 7 (L1_MAP)" \
    "flow: the fall before level 7" \
    "Fall ended t 0.0" \
    "health 100 pickups " \
    "mem[run-end, unsettled]: level 7"; do
    has "$NEW" "$line" || bad "[new game] the log has no \"$line\""
done
if has "$NEW" "level: the fall's carry"; then
    bad "[new game] a skipped fall carried pickups"
fi

# --- 3. The lean way (the 3DS's) -------------------------------------------------
LEAN="$WORK/lean.log"
run "$LEAN" --fall=8 --fall-lean --frames 700
rc=$?
[ "$rc" -eq 0 ] || bad "[lean] exited $rc"
for line in \
    "flow: the fall before level 8 (the 3DS's lean way)" \
    "Fall: index 4" \
    "exit reason: frame budget reached"; do
    has "$LEAN" "$line" || bad "[lean] the log has no \"$line\""
done

if [ "$fail" -eq 0 ]; then
    echo "PASS FallFrontend (--fall=7 chased into LEVEL7 with health 84 and three pickups in Kurt's inventory; new game -> briefing -> fall -> Esc -> level; LEVEL8's fall the lean way)"
    exit 0
fi
echo "FAIL FallFrontend ($fail check(s))"
exit 1

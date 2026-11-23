#!/bin/sh
# harness_aids_test.sh -- the set pieces' test aids (game/harness_aids.h) on the Linux
# frontend, which mirrors the 3DS harness's GIVE=, RIDE= and AIM= as --give, --ride and
# --aim. Each arm starts Kurt at a set piece the way the parent's emulator playthroughs
# do (mdk_3ds/Makefile's harness section lists the same four), headless on DISPLAY :99:
#
#   1. THE XD2. L7 DANT_9, --ride=XD2: mounted by RideWalker (the C#'s --ride) at 1 s,
#      and it walks under a scripted walk.
#   2. THE BOMBER. L7 DANT_5, --ride=XE: no XE is rideable at 1 s, so it is called the
#      C#'s RideTest way (DANT_5's group 16 hit); it lands and Kurt is mounted on it.
#   3. THE AIR STRIKE. L5 at MUSE_4's edge, --give=SW_BONES --aim=0,1.25: Bones in the
#      inventory at spawn, the scope at yaw 0 pitch 1.25 when he snipes, and the shot
#      plays the strike scene (golden/L5_strike_gunter.txt's aim, here at spawn).
#   4. THE HOLES. L8 GUNT_10, --aim at XGUNTAM (golden/L8_holes_guntam.txt's first aim):
#      the scope set exactly, the shot fired.
#   5. THE LOCKED DOOR. L7 at the corridor door, a scripted walk into it: without
#      --unlock the locked X7DOOR is solid and stops Kurt at y=478.4 (the C#'s
#      SolidsWithin); with --unlock=200 the lock clears on schedule and the same
#      walk crosses into CDANT_1 -- the corridor probe's `unlock:` as a harness aid.
#
#   usage: sh tests/scripts/harness_aids_test.sh [repo-root]
#
# Skips (rc 0, with the reason) when the Linux binary or the game data is absent: a
# skip says so and is not a pass.

ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
BIN="$ROOT/build-linux/mdk"
DATA=${MDK_DATA_DIR:-$ROOT/MDK}

if [ ! -x "$BIN" ]; then
    echo "SKIP HarnessAids (build-linux/mdk is not built; there is no binary to run)"
    exit 0
fi
if [ ! -d "$DATA" ]; then
    echo "SKIP HarnessAids (no game data at $DATA)"
    exit 0
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/mdk-aids.XXXXXX") || exit 1
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

run() {
    log=$1
    shift
    timeout 600 "$BIN" --headless --fixed-step --size=320x240 --seed 1 "$@" >"$log" 2>&1 ||
        bad "[$log] exited $?"
}

# has <log> <label> <text...>: each fragment is in the log (grep -F, no pipe).
has() {
    log=$1
    label=$2
    shift 2
    for line in "$@"; do
        grep -qF -- "$line" "$log" || bad "[$label] the log has no \"$line\""
    done
}

run "$WORK/xd2.log" --level=7 --at=45,4868,-25,90 --arena=DANT_9 --ride=XD2 \
    --walk='forward@120-200;turnleft@200-230;forward@230-280' --frames 300
has "$WORK/xd2.log" xd2 \
    "harness: RIDE=XD2: mounted XD2_1002 in DANT_9 at (45.4,4878.0,-27.1), 1.02 s in" \
    "segment 2 turnleft@200-230 starts at feet (46.5,4856.2,-27.1)"

run "$WORK/xe.log" --level=7 --at=100,2320,-60,90 --arena=DANT_5 --ride=XE --frames 700
has "$WORK/xe.log" bomber \
    "harness: RIDE=XE: none rideable yet; called by hitting DANT_5 group 16" \
    "harness: RIDE=XE: mounted XE_9000 in DANT_5"

run "$WORK/strike.log" --level=5 --at=250,121,-1622,0 --give=SW_BONES --aim=0,1.25 \
    --walk='sniper@20-22;fire@80-82' --frames 200
has "$WORK/strike.log" strike \
    "harness: GIVE SW_BONES" \
    "harness: AIM the scope at yaw 0.000 pitch 1.250" \
    "strike: the scene plays at frame"

run "$WORK/holes.log" --level=8 --at=20,3360,-360,0 --arena=GUNT_10 \
    --aim=123.80164,-26.158365 --walk='sniper@150-152;fire@190-200' --frames 210
has "$WORK/holes.log" holes \
    "harness: AIM yaw 123.802 at spawn" \
    "harness: AIM the scope at yaw 123.802 pitch -26.158" \
    "segment 2 fire@190-200 starts"

# The locked door stops the walk at the door (the C#'s solid locked door), and the
# unlock on the schedule lets the SAME walk through. The frames and the crossing
# are the level 7 two-doorway route's (mdk_3ds/Makefile, the arena-entry table).
run "$WORK/door_locked.log" --level=7 --at=0,380,15,90 \
    --walk='forward+turbo@60-260' --frames 300
has "$WORK/door_locked.log" door-locked \
    "feet (-0.0,478.4,15.0) yaw 90.0 arena DANT_1"
grep -qF -- "arena DANT_1 -> CDANT_1" "$WORK/door_locked.log" &&
    bad "door-locked: the locked door let Kurt through"

run "$WORK/door_unlocked.log" --level=7 --at=0,380,15,90 --unlock=200 \
    --walk='forward+turbo@60-260' --frames 300
has "$WORK/door_unlocked.log" door-unlocked \
    "harness: UNLOCK step 200: 1 locked door(s) freed" \
    "arena DANT_1 -> CDANT_1 (second DANT_1 active)"

if [ "$fail" -ne 0 ]; then
    echo "FAIL HarnessAids ($fail check(s))"
    exit 1
fi
echo "PASS HarnessAids (--ride mounts the XD2 by RideWalker and the XE after the C#'s call; --give puts Bones in at spawn and --aim's scope plays the strike; --aim sets the scope at XGUNTAM; --unlock frees the corridor's locked door on schedule)"

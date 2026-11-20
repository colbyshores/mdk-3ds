#!/bin/sh
# level_chain_test.sh -- package 2, the next-level transition, on the Linux frontend:
# a level ends, the level is torn down, the stream plays (package #18b), the flow
# walks the screens this port still lacks (skipped the C#'s way), and the next level
# is loaded and played. Three runtime arms,
# all headless (DISPLAY :99, lavapipe), all on the shipped binary:
#
#   1. THE CHAIN. `--level=3 --event=1 --event-every-level`: special_event 1 one
#      second into EVERY level ends it, so the run plays 3 -> 4 -> 8 -> 5 (the C#'s
#      order, GameState.Order) and goes to the main menu after 5 (since package
#      #20a it is shown, until the frame budget ends the run). Each level's end plays
#      its stream (a --fixed-step run's streams take the default seed, so these are
#      the C# --stream=N runs' tubes: golden/stream_screen_L*.txt and its test); the
#      log must name each skipped screen (Statistics, Fall; after 8's Gunter stream
#      the save prompt) and each next level -- LEVEL5 entered at the Gunter stream's
#      end health, 63.
#      The fall is shown since package #17b; every arm passes --skip-falls (a test
#      aid: each fall skipped the C#'s Esc way before its first frame), so the
#      frame numbers below are the ones the stream's package wrote.
#
#   2. A CLEAN RELOAD. The frame the chained run draws 111 frames into level 4 must be
#      BYTE-IDENTICAL to the frame a run started directly in level 4 draws at its own
#      111th (and the same for level 8 at 122). Anything a teardown left behind that
#      the next level can see -- a texture slot, a palette, a pose cache, the sky, a
#      script or random state -- moves pixels; a reload that is the same as a fresh
#      start does not.
#
#   3. NO LEAK. `--replay-level` reloads the level that ended instead of the next one
#      (a test aid): four teardowns of level 3, each followed by its stream. The C heap in use after each teardown
#      (mallinfo2, the line `mem[level-teardown]`) must not climb from the second one
#      on -- the first carries the backend's one-time pools and pipelines -- beyond a
#      small allocator slack, the file descriptors must not climb, and the renderer's
#      creation log must be back at its mark (0) every time.
#
#   usage: sh tests/scripts/level_chain_test.sh [repo-root]
#
# Skips (rc 0, with the reason) when the Linux binary or the game data is absent: a
# skip says so and is not a pass.

ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
BIN="$ROOT/build-linux/mdk"
DATA=${MDK_DATA_DIR:-$ROOT/MDK}

if [ ! -x "$BIN" ]; then
    echo "SKIP LevelChain (build-linux/mdk is not built; there is no binary to run)"
    exit 0
fi
if [ ! -d "$DATA" ]; then
    echo "SKIP LevelChain (no game data at $DATA)"
    exit 0
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/mdk-levelchain.XXXXXX") || exit 1
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
    timeout 600 "$BIN" --headless --fixed-step --size=320x240 --seed 1 --skip-falls "$@" >"$log" 2>&1
}

# has <log> <text>: the log holds this line fragment (grep -F, no pipe).
has() {
    grep -qF -- "$2" "$1"
}

# --- 1. The chain ------------------------------------------------------------
CHAIN="$WORK/chain.log"
run "$CHAIN" --level=3 --event=1 --event-every-level --frames 9000
rc=$?
[ "$rc" -eq 0 ] || bad "[chain] exited $rc"
for line in \
    "flow: level 3 ended (health 100); the flow shows Stream" \
    "Stream ended after LEVEL3: Survived, health 1, segment 185, 33.7 s" \
    "flow: the stream after level 3 ended (health 1); the flow shows Statistics" \
    "flow: skipped Statistics (package #20 not ported)" \
    "flow: skipped Fall (--skip-falls)" \
    "flow: next level 4 at frame 2513, entered at health 100" \
    "Stream ended after LEVEL4: Survived, health 1, segment 187, 33.9 s" \
    "flow: next level 8 at frame 5037, entered at health 100" \
    "Stream ended after LEVEL8: Survived, health 63, segment 184, 31.3 s" \
    "flow: the stream after level 8 ended (health 63); the flow shows SavePrompt" \
    "flow: skipped SavePrompt (package #19 not ported)" \
    "flow: next level 5 at frame 7403, entered at health 63" \
    "flow: level 5 ended (health 63); the flow shows Menu" \
    "flow: the menu follows" \
    "flow: the main menu" \
    "Menu: New Game | Level: 6 | Saved Game | Options | Quit" \
    "exit reason: frame budget reached" \
    "arena: MEAT_1" \
    "arena: GUNT_1" \
    "arena: MUSE_1"; do
    has "$CHAIN" "$line" || bad "[chain] the log has no \"$line\""
done

# --- 2. A clean reload -------------------------------------------------------
# chained frame index F is level-local index F - start; --capture-frame is 1-based.
compare() {
    label=$1 chained=$2 level=$3 direct=$4
    run "$WORK/c$chained.log" --level=3 --event=1 --event-every-level \
        --capture="$WORK/chain$chained.bmp" --capture-frame="$chained" ||
        bad "[$label] the chained capture run failed"
    run "$WORK/d$direct.log" --level="$level" --event=1 \
        --capture="$WORK/direct$direct.bmp" --capture-frame="$direct" ||
        bad "[$label] the direct capture run failed"
    if ! cmp -s "$WORK/chain$chained.bmp" "$WORK/direct$direct.bmp"; then
        bad "[$label] chained frame $chained differs from level $level's own frame $direct: the teardown left something the next level sees"
    fi
}
compare reload-L4 2624 4 111
compare reload-L8 5159 8 122

# --- 3. No leak ----------------------------------------------------------------
REPLAY="$WORK/replay.log"
run "$REPLAY" --level=3 --event=1 --event-every-level --replay-level --frames 10100
rc=$?
[ "$rc" -eq 0 ] || bad "[replay] exited $rc"
# mem[level-teardown]: level 3 heap=<n> fds=<n> renderer-log=<n>
awk -v slack=262144 '
    /mem\[level-teardown\]/ {
        n++
        for (i = 1; i <= NF; i++) {
            split($i, kv, "=")
            if (kv[1] == "heap") heap[n] = kv[2]
            if (kv[1] == "fds") fds[n] = kv[2]
            if (kv[1] == "renderer-log") logged[n] = kv[2]
        }
    }
    END {
        if (n < 4) { print "  FAIL [replay] " n " teardown line(s), want 4"; exit 1 }
        bad = 0
        for (i = 1; i <= n; i++) {
            if (logged[i] != 0) { print "  FAIL [replay] teardown " i ": the renderer log is at " logged[i] ", not its mark"; bad = 1 }
            if (fds[i] != fds[1]) { print "  FAIL [replay] teardown " i ": " fds[i] " fds against " fds[1]; bad = 1 }
        }
        for (i = 3; i <= n; i++) {
            if (heap[i] - heap[2] > slack) { print "  FAIL [replay] teardown " i ": heap " heap[i] " is " heap[i] - heap[2] " bytes over teardown 2 (" heap[2] ")"; bad = 1 }
        }
        printf "  replay: heap after teardowns"
        for (i = 1; i <= n; i++) printf " %d", heap[i]
        printf "; fds %d; renderer log %d\n", fds[n], logged[n]
        exit bad
    }' "$REPLAY" || fail=$((fail + 1))

if [ "$fail" -ne 0 ]; then
    echo "FAIL LevelChain ($fail check(s))"
    exit 1
fi
echo "PASS LevelChain (3 -> 4 -> 8 -> 5 with each stream played and the statistics, fall and save prompt skipped; levels 4 and 8 reload byte-identical to a direct start; four level-3 teardowns leave the heap flat)"

#!/bin/sh
# death_reload_test.sh -- a death's reload must not grow the heap.
#
# THE FLOW UNDER TEST is the console's death-menu loop: Kurt dies, the main menu
# opens with Continue first (LASTGAME, package #19), Continue reloads the level,
# and he dies again. On the Old 3DS the main heap is ~34 MiB, so a reload that
# leaves even a few KB behind is a failed allocation after enough deaths. The
# Linux frontend runs the same shared code (mdk_core: the level data, the
# runtime, Kurt, the views, the menu, the saves), headless:
#
#   --die --die-every-level  Kurt hurt by 1000 one second into EVERY level
#   --sniper                 ...in sniper mode, so the scope's path is in the loop
#   --press=Menu.Accept@...  an Accept every half second: in the menu the first
#                            skips the INTRO1A splash and the next takes Continue;
#                            in play the closed pause menu ignores them
#
# WHAT IT GUARDS, AND WHAT IT CANNOT. It measures bytes IN USE: a reload that
# keeps something. The 3DS's other failure, the heap's top rising because blocks
# that outlive a level sit in the middle of its data (fragmentation, found on
# 2026-11-27 with in-use bytes flat), is the console allocator's layout and is
# measured in Mandarine and on the console (the `mem[loop]` arena), not here.
#
# The C heap in use after each teardown (mallinfo2, `mem[level-teardown]`) must
# not climb. A teardown's figure also holds whatever a worker thread had in
# flight at that moment (the menu's movie and slides are read and decoded on
# the renderer's file thread, the music on its own): those are spikes of 1-3 MB
# on some teardowns and gone by the next, and when they land depends on the
# threads' timing, not the game's. So the test compares FLOORS: the least
# figure of the last four teardowns against the least of teardowns 3-6. The
# first two are left out: the first precedes the first menu visit and the
# first reload (one-time pools: the Vulkan driver's, the menu's first open),
# and the second carries what the first reload made for good. The slack is
# 16 KB. Measured 2026-11-27 (docs/WORK_LEDGER.md): clean runs rise 4.4-6.1 KB
# (~500 bytes a reload in this process, none of it in the game's own
# allocations: an LD_PRELOAD census by call stack found every game stack flat
# over 27 reloads; what moves is glibc's and lavapipe's), and a 4 KB-a-reload
# leak injected for the purpose rose 41 KB over 9 reloads and failed. The file descriptors must stay flat and the renderer's
# creation log must be back at its mark (0) every time.
#
#   usage: sh tests/scripts/death_reload_test.sh [repo-root]
#
# Skips (rc 0, with the reason) when the Linux binary or the game data is
# absent: a skip says so and is not a pass.

ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
BIN="$ROOT/build-linux/mdk"
DATA=${MDK_DATA_DIR:-$ROOT/MDK}

if [ ! -x "$BIN" ]; then
    echo "SKIP DeathReload (build-linux/mdk is not built; there is no binary to run)"
    exit 0
fi
if [ ! -d "$DATA" ]; then
    echo "SKIP DeathReload (no game data at $DATA)"
    exit 0
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/mdk-deathreload.XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT
trap 'exit 1' INT TERM

export DISPLAY=:99
if [ -z "${VK_ICD_FILENAMES:-}" ] && [ -f /usr/share/vulkan/icd.d/lvp_icd.json ]; then
    export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
fi
export MDK_DATA_DIR="$DATA"
# The saves (LASTGAME) go to a scratch folder, not the player's.
export MDK_USER_DIR="$WORK/user"
mkdir -p "$MDK_USER_DIR"

# An Accept every 0.5 s of the presses' clock, which runs in play and on the
# front screens alike (mdk_linux/src/main.cpp, menu_screens.cpp), for longer than
# the frame budget: so no visit depends on how long a death or a load takes.
PRESSES=""
k=2
while [ "$k" -le 480 ]; do
    PRESSES="$PRESSES,Menu.Accept@$((k / 2)).$((k % 2 * 5))"
    k=$((k + 1))
done
PRESSES=${PRESSES#,}

LOG="$WORK/death.log"
timeout 900 "$BIN" --headless --fixed-step --size=320x240 --seed 1 --level=3 \
    --die --die-every-level --sniper --press="$PRESSES" --frames 5400 >"$LOG" 2>&1
rc=$?
fail=0
if [ "$rc" -ne 0 ]; then
    echo "  FAIL [death] exited $rc"
    tail -5 "$LOG" | sed 's/^/    /'
    fail=1
fi

# mem[level-teardown]: level 3 heap=<n> fds=<n> renderer-log=<n>
awk -v slack=16384 '
    /--die: Kurt hurt/ { died++ }
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
        if (n < 12) { print "  FAIL [death] " n " teardown line(s) (" died " deaths), want 12 or more"; exit 1 }
        bad = 0
        for (i = 1; i <= n; i++) {
            if (logged[i] != 0) { print "  FAIL [death] teardown " i ": the renderer log is at " logged[i] ", not its mark"; bad = 1 }
            if (fds[i] != fds[1]) { print "  FAIL [death] teardown " i ": " fds[i] " fds against " fds[1]; bad = 1 }
        }
        early = heap[3]
        for (i = 4; i <= 6; i++) if (heap[i] < early) early = heap[i]
        late = heap[n]
        for (i = n - 3; i < n; i++) if (heap[i] < late) late = heap[i]
        if (late - early > slack) {
            print "  FAIL [death] the heap floor rose " late - early " bytes over " n - 6 " reloads (teardowns 3-6: " early ", last four: " late ")"
            bad = 1
        }
        printf "  death: %d deaths, heap after teardowns", died
        for (i = 1; i <= n; i++) printf " %d", heap[i]
        printf "; floor %+d bytes from teardowns 3-6 to the last four; fds %d\n", late - early, fds[n]
        exit bad
    }' "$LOG" || fail=1

if [ "$fail" -ne 0 ]; then
    echo "FAIL DeathReload"
    exit 1
fi
echo "PASS DeathReload (LEVEL3 died in the scope and reloaded through the main menu's Continue, over and over; the heap's floor after the teardowns flat)"

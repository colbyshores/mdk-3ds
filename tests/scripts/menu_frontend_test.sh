#!/bin/sh
# menu_frontend_test.sh -- package #20a on the Linux frontend: the main menu is the
# entry screen, its options are the C#'s settings file, a new game goes through
# the briefing into the first level, Esc in a level opens the pause menu whose
# "Main menu" leaves for the menu, and "Really Quit?" ends the run. Headless
# (no window: DISPLAY :99 is exported only for symmetry with level_chain_test),
# lavapipe, the shipped binary, keys from --press (the C#'s TestPresses).
#
# The menus' LOGIC is pinned against the C# in vm_tests (golden/menu_*.txt);
# this is the frontend's wiring: which screen follows which, the file, the
# `Menu: ...` lines (the C#'s own spelling, so tests/flow_test.sh's lines can be
# compared by eye).
#
#   usage: sh tests/scripts/menu_frontend_test.sh [repo-root]
#
# Skips (rc 0, with the reason) when the Linux binary or the game data is absent.

ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
BIN="$ROOT/build-linux/mdk"
DATA=${MDK_DATA_DIR:-$ROOT/MDK}

if [ ! -x "$BIN" ]; then
    echo "SKIP MenuFrontend (build-linux/mdk is not built; there is no binary to run)"
    exit 0
fi
if [ ! -d "$DATA" ]; then
    echo "SKIP MenuFrontend (no game data at $DATA)"
    exit 0
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/mdk-menufront.XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT INT TERM

export DISPLAY=:99
if [ -z "${VK_ICD_FILENAMES:-}" ] && [ -f /usr/share/vulkan/icd.d/lvp_icd.json ]; then
    export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
fi
export MDK_DATA_DIR="$DATA"
export MDK_USER_DIR="$WORK/user"
mkdir -p "$MDK_USER_DIR"

fail=0
bad() {
    echo "  FAIL $1"
    fail=$((fail + 1))
}

run() {
    log=$1
    shift
    timeout 600 "$BIN" --headless --fixed-step --size=320x240 --seed 1 "$@" >"$log" 2>&1
}

has() {
    grep -qF -- "$2" "$1"
}

# --- 1. The entry screen, and the options' file ----------------------------------
# Options (row 3), Game (row 2 of Graphics | Audio | Game | Back), Right: Hard;
# Esc: the options again on Game, Up: Audio, Left on Master volume: 70.
LOG="$WORK/options.log"
run "$LOG" --press=Menu.Down@0.5,Menu.Down@0.6,Menu.Down@0.7,Menu.Accept@1,Menu.Down@1.2,Menu.Down@1.3,Menu.Accept@1.5,Menu.Right@1.7,Menu.Back@2,Menu.Up@2.2,Menu.Accept@2.4,Menu.Left@2.6 --frames 200
for line in \
    "flow: the main menu" \
    "Menu: New Game | Level: 1 | Saved Game | Options | Quit" \
    "Menu: Graphics | Audio | Game | Back" \
    "Menu: Difficulty: Normal | Back" \
    "Menu: Master volume: 80 | Music volume: 50 | Effects volume: 70 | Music filter: On | Back"; do
    has "$LOG" "$line" || bad "[options] the log has no \"$line\""
done
# The file holds each change as it was made (C# Ui.ApplySettings: applied, saved).
grep -q "^difficulty=Hard$" "$MDK_USER_DIR/settings.cfg" || bad "[options] settings.cfg has no difficulty=Hard"
grep -q "^master_volume=70$" "$MDK_USER_DIR/settings.cfg" || bad "[options] settings.cfg has no master_volume=70"
grep -q "^gore=True$" "$MDK_USER_DIR/settings.cfg" || bad "[options] settings.cfg lost the gore default"
# A second run reads it back: the Game page says Hard.
LOG2="$WORK/reread.log"
run "$LOG2" --press=Menu.Down@0.5,Menu.Down@0.6,Menu.Down@0.7,Menu.Accept@1,Menu.Down@1.2,Menu.Down@1.3,Menu.Accept@1.5 --frames 120
has "$LOG2" "Menu: Difficulty: Hard | Back" || bad "[options] the second run did not read difficulty=Hard back"

# --- 2. New game: the briefing, the fall skipped, the level; the pause menu -------
# The fall is shown since package #17b (fall_frontend_test.sh); --skip-falls skips it
# the C#'s Esc way before its first frame, so the presses keep their times.
LOG3="$WORK/newgame.log"
run "$LOG3" --skip-falls --press=Menu.Accept@0.5,Menu.Back@1.2,Menu.Accept@1.4,Menu.Back@4,Menu.Down@4.2,Menu.Down@4.3,Menu.Accept@4.5 --frames 330
for line in \
    "flow: the briefing of level 7 (L1_MAP)" \
    "flow: skipped Fall (--skip-falls)" \
    "Menu: Resume | Options | Main menu | Quit" \
    "flow: pause menu: the main menu" \
    "mem[level-teardown]: level 7"; do
    has "$LOG3" "$line" || bad "[new game] the log has no \"$line\""
done
[ "$(grep -c 'flow: the main menu' "$LOG3")" -eq 2 ] || bad "[new game] the menu was not shown again after the pause menu's Main menu"

# --- 3. Really Quit? -----------------------------------------------------------
LOG4="$WORK/quit.log"
run "$LOG4" --press=Menu.Back@0.5,Menu.Accept@1 --frames 200
rc=$?
[ "$rc" -eq 0 ] || bad "[quit] exited $rc"
has "$LOG4" "Menu: Really Quit? | Yes | No" || bad "[quit] Esc did not ask Really Quit?"
has "$LOG4" "exit reason: menu Quit" || bad "[quit] Yes did not end the run"

# --- 4. --level still starts in the level (every existing run) --------------------
LOG5="$WORK/level.log"
run "$LOG5" --level=3 --frames 2
if has "$LOG5" "flow: the main menu"; then
    bad "[--level] a --level run showed the menu"
fi

if [ "$fail" -eq 0 ]; then
    echo "PASS MenuFrontend (entry menu, options file, new game -> briefing -> level, pause -> menu, quit)"
    exit 0
fi
echo "FAIL MenuFrontend ($fail check(s))"
exit 1

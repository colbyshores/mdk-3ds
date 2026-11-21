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
run "$LOG" --menu --press=Menu.Down@0.5,Menu.Down@0.6,Menu.Down@0.7,Menu.Accept@1,Menu.Down@1.2,Menu.Down@1.3,Menu.Accept@1.5,Menu.Right@1.7,Menu.Back@2,Menu.Up@2.2,Menu.Accept@2.4,Menu.Left@2.6 --frames 200
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
run "$LOG2" --menu --press=Menu.Down@0.5,Menu.Down@0.6,Menu.Down@0.7,Menu.Accept@1,Menu.Down@1.2,Menu.Down@1.3,Menu.Accept@1.5 --frames 120
has "$LOG2" "Menu: Difficulty: Hard | Back" || bad "[options] the second run did not read difficulty=Hard back"

# --- 2. New game: the briefing, the fall skipped, the level; the pause menu -------
# The fall is shown since package #17b (fall_frontend_test.sh); --skip-falls skips it
# the C#'s Esc way before its first frame, so the presses keep their times.
LOG3="$WORK/newgame.log"
run "$LOG3" --menu --skip-falls --press=Menu.Accept@0.5,Menu.Back@1.2,Menu.Accept@1.4,Menu.Back@4,Menu.Down@4.2,Menu.Down@4.3,Menu.Accept@4.5 --frames 330
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
run "$LOG4" --menu --press=Menu.Back@0.5,Menu.Accept@1 --frames 200
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
# ... after its loading screen (package #20c: C# Play's LoadingScreen.Show).
has "$LOG5" "flow: the loading screen of level 3" || bad "[--level] no loading screen before level 3"

# --- 5. The INTRO1A splash (package #20c): without --menu it comes first, as the
# C#'s; the first key is the splash's (Accept does NOT start a new game), so Esc
# then Accept is "Really Quit?" -> Yes. --------------------------------------------
LOG6="$WORK/splash.log"
run "$LOG6" --press=Menu.Accept@0.5,Menu.Back@1,Menu.Accept@1.5 --frames 200
has "$LOG6" "flow: the main menu (the INTRO1A splash first)" || bad "[splash] the menu did not open on the splash"
has "$LOG6" "Menu: Really Quit? | Yes | No" || bad "[splash] the key after the splash did not reach the menu"
has "$LOG6" "exit reason: menu Quit" || bad "[splash] the run did not quit from the menu"
if has "$LOG6" "flow: the briefing"; then
    bad "[splash] the key that skipped the splash also chose New Game"
fi

# --- 6. The end movies (--end, the C#'s Start.EndMovie): the GOG data has neither
# MDKEND.FLC nor MDKBZK.MVE, so the menu follows at once, with the splash. ---------
LOG7="$WORK/end.log"
run "$LOG7" --end --press=Menu.Accept@0.5,Menu.Back@1,Menu.Accept@1.5 --frames 200
for line in \
    "flow: the end movies (end movies: done)" \
    "flow: the main menu (the INTRO1A splash first)" \
    "exit reason: menu Quit"; do
    has "$LOG7" "$line" || bad "[end] the log has no \"$line\""
done

# --- 7. The statistics after a level (package #20b) -------------------------------
# LEVEL3 ends at 1 s (--event=1, frame 489): its stream plays (package #18b, ~2024
# frames of the default seed's tube; the presses' clock is the level's and the
# menus', so the stream moves no press time, only the frame budget), then the
# statistics are shown. Accept leaves the intermission, three Escs finish the debriefing's three
# texts, Accept leaves it; twelve Escs run the Score-O-matic's five rows (two waits
# each, nothing to count) and the heads' two waits (no head shots), Accept leaves
# it; the save prompt opens, Yes, then
# Enter saves under "4" (LEVEL4's number); Esc finishes the briefing's typing,
# Accept leaves it; the fall is skipped (--skip-falls, package #17b), and LEVEL4
# (MEAT_1) loads.
LOG8="$WORK/stats.log"
BACKS=""
for t in 12.1 12.2 12.3 12.4 12.5 12.6 12.7 12.8 12.9 13.0 13.1 13.2; do
    BACKS="$BACKS,Menu.Back@$t"
done
run "$LOG8" --level=3 --event=1 --skip-falls --frames 3200 \
    --press=Menu.Accept@9,Menu.Back@10.2,Menu.Back@10.4,Menu.Back@10.6,Menu.Accept@11$BACKS,Menu.Accept@14,Menu.Accept@15,Menu.Accept@15.2,Menu.Back@15.8,Menu.Accept@16.2
for line in \
    "flow: the statistics follow" \
    "Statistics: Intermission" \
    "flow: the statistics after level 3 (from Intermission)" \
    "Statistics: Debriefing" \
    "Statistics: Score" \
    "Score-O-matic: 0/0, 0/100, 0/0, 0/100, 0/6, heads 0" \
    "statistics: the save prompt opens" \
    "Saved game 4: level 4" \
    "Statistics: Briefing" \
    "arena: MEAT_1"; do
    has "$LOG8" "$line" || bad "[statistics] the log has no \"$line\""
done
[ -f "$MDK_USER_DIR/saves/4.sav" ] || bad "[statistics] the save prompt wrote no saves/4.sav"
# --stats starts there (the C#'s --stats/--phase/--counts).
LOG9="$WORK/stats-start.log"
run "$LOG9" --stats=7 --phase=1 --counts=57,31,9,4,12,20,3 --frames 5
has "$LOG9" "Score-O-matic: 57/57, 54/100, 9/9, 44/100, 12/20, heads 3" ||
    bad "[--stats] the Score-O-matic did not start with the counts"

if [ "$fail" -eq 0 ]; then
    echo "PASS MenuFrontend (entry menu, options file, new game -> briefing -> level, pause -> menu, quit, splash, end movies, loading screen, level -> statistics -> save -> next level)"
    exit 0
fi
echo "FAIL MenuFrontend ($fail check(s))"
exit 1

#!/usr/bin/env bash
# ============================================================================
# emurun.sh — run mdk.3dsx in Mandarine OFFSCREEN and print only the verdict.
#
# WHY THIS EXISTS. Adapted from ../perfect_dark/tools/emurun.sh, which earned
# every one of its guards the expensive way (AGENTS.md P8). Each failure mode
# below is SILENT: the run "succeeds" and reports numbers that are wrong.
#
#   1. `pkill` returns 1 when nothing matches, which aborts a chained command.
#      A "clean + launch" one-liner then never launches, and the next log read
#      is the PREVIOUS run's data.
#   2. mdk.log is opened in APPEND mode (log_3ds.cpp). A failed delete silently
#      MIXES two builds' output, and a log with markers from a build that no
#      longer exists reads as a fresh measurement.
#   3. The AppImage execs an `AppRun.wrapped` child in place from a
#      /tmp/.mount_mandarXXXXXX path. `pkill -f mandarine` does not match it, so
#      liveness-by-name reports a dead emulator as alive and vice versa.
#   4. A bare long `sleep` is blocked by the agent harness. Poll instead.
#   5. Launching on the operator's display puts a window on their screen.
#      AGENTS.md P7: always the dedicated headless display, :99 by default.
#   6. Two lanes sharing one XDG_DATA_HOME share one sdmc and therefore one
#      mdk.log, so each lane reads the other's output.
#
# And the reason this file is pinned to a version: Mandarine r1.3.0 segfaults
# deterministically during level load. Running it produces an empty log, which
# under every other harness looks like a bug in the game.
#
# USAGE
#   mdk_3ds/tools/emurun.sh                      # run mdk_3ds/mdk.3dsx
#   mdk_3ds/tools/emurun.sh --secs 90            # seconds past readiness
#   mdk_3ds/tools/emurun.sh --grep 'tex:|draws' # extra patterns to extract
#   mdk_3ds/tools/emurun.sh --shot /tmp/f.png    # capture the offscreen display
#   mdk_3ds/tools/emurun.sh --min-frames 60      # refuse to report a short run
#   mdk_3ds/tools/emurun.sh path/to/other.3dsx
#
# EXIT CODES
#   0 ok | 2 launch/environment failed | 3 STALE LOG, refused to report
#   4 emulator died before producing a log | 5 run too short to measure
#   6 emulator version is not the pinned known-good build
# ============================================================================

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

DSX="$REPO/mdk_3ds/mdk.3dsx"
APPIMAGE="${MANDARINE:-/mnt/nas/Development/mandarine.AppImage}"
DISPLAY_NUM="${EMURUN_DISPLAY:-99}"
SECS=45
EXTRA_GREP=""
SHOT=""
MIN_FRAMES=0

# The known-good Mandarine build (AGENTS.md P8). r1.3.0
# (77e2e887f3af1b62fa52cfcfe65709f2) segfaults during level load.
PINNED_MD5="0d2adab6ad078c78e6d03e72086f2eb6"
BANNED_MD5="77e2e887f3af1b62fa52cfcfe65709f2"

# Tokens the guest actually writes. log_3ds.cpp / main_3ds.cpp print the
# backend name, then "draws issued: N", then "frame N elapsed=...".
READY_TOKEN='backend:'
FRAME_RE='^frame [0-9]+ '

while [ $# -gt 0 ]; do
  case "$1" in
    --secs)       SECS="$2";       shift 2 ;;
    --grep)       EXTRA_GREP="$2"; shift 2 ;;
    --shot)       SHOT="$2";       shift 2 ;;
    --min-frames) MIN_FRAMES="$2"; shift 2 ;;
    --display)    DISPLAY_NUM="$2"; shift 2 ;;
    -h|--help)    sed -n '2,45p' "$0"; exit 0 ;;
    *)            DSX="$1";        shift ;;
  esac
done

[ -f "$DSX" ] || { echo "emurun: no such build: $DSX (make -C mdk_3ds)" >&2; exit 2; }
[ -x "$APPIMAGE" ] || { echo "emurun: no emulator at $APPIMAGE (set \$MANDARINE)" >&2; exit 2; }

# ---- version pin ----------------------------------------------------------
# An emulator is a measuring instrument. Refuse to measure with a broken one.
EMU_MD5="$(md5sum "$APPIMAGE" 2>/dev/null | cut -d' ' -f1)"
if [ "$EMU_MD5" = "$BANNED_MD5" ]; then
  echo "emurun: $APPIMAGE is r1.3.0, which segfaults on level load. Point \$MANDARINE at r1.2.1." >&2
  exit 6
fi
if [ "$EMU_MD5" != "$PINNED_MD5" ]; then
  echo "emurun: WARNING md5 $EMU_MD5 is not the pinned r1.2.1 ($PINNED_MD5)." >&2
  echo "emurun:         Continuing, but this run is not comparable with pinned evidence." >&2
fi

# ---- (6) one private lane per run ------------------------------------------
# Mandarine puts the guest's sdmc under XDG_DATA_HOME, and MDK appends to
# sdmc:/3ds/mdk/mdk.log. Isolating XDG_DATA_HOME is the only way to run two
# lanes without each reading the other's log.
LANE="${TMPDIR:-/tmp}/mdk-emurun.$$"
mkdir -p "$LANE/xdg" || { echo "emurun: cannot create $LANE" >&2; exit 2; }
export XDG_DATA_HOME="$LANE/xdg"
STDOUT_LOG="$LANE/stdout.log"

OWN_ROOT=""
emurun_own_pids() {
  # (3)(7) liveness by PID tree, never by name: the AppImage execs
  # AppRun.wrapped from a /tmp/.mount_mandarXXXXXX path.
  [ -n "$OWN_ROOT" ] || return 0
  pgrep -P "$OWN_ROOT" 2>/dev/null
  echo "$OWN_ROOT"
}

emurun_cleanup() {
  local p
  for p in $(emurun_own_pids 2>/dev/null); do
    kill "$p" 2>/dev/null || true
  done
  sleep 1
  for p in $(emurun_own_pids 2>/dev/null); do
    kill -9 "$p" 2>/dev/null || true
  done
  # Only ever unmount the FUSE root this run started.
  [ -n "$OWN_ROOT" ] && fusermount -uz "$OWN_ROOT" 2>/dev/null || true
}
trap emurun_cleanup EXIT INT TERM

# ---- (5) dedicated headless display, never the operator's -------------------
if ! DISPLAY=":$DISPLAY_NUM" xdpyinfo >/dev/null 2>&1; then
  Xvfb ":$DISPLAY_NUM" -screen 0 1280x1024x24 >"$LANE/xvfb.log" 2>&1 &
  XVFB_PID=$!
  for _ in $(seq 1 50); do
    DISPLAY=":$DISPLAY_NUM" xdpyinfo >/dev/null 2>&1 && break
    sleep 0.2
  done
fi
DISPLAY=":$DISPLAY_NUM" xdpyinfo >/dev/null 2>&1 || {
  echo "emurun: Xvfb :$DISPLAY_NUM did not come up" >&2; exit 2; }
export DISPLAY=":$DISPLAY_NUM"

# ---- (2) delete the guest log and PROVE it is gone --------------------------
# The sdmc path depends on Mandarine's layout and the title's database id, so
# find it rather than hardcode it. Anything found now is by definition stale.
find_guest_logs() {
  find "$XDG_DATA_HOME" -type f -path '*sdmc*/3ds/mdk/mdk.log' 2>/dev/null
}
find "$XDG_DATA_HOME" -type f -path '*sdmc*/3ds/mdk/mdk.log' -delete 2>/dev/null
if [ -n "$(find_guest_logs)" ]; then
  echo "emurun: could not remove the guest mdk.log; a mixed log would be unreadable" >&2
  exit 3
fi

# ---- launch -----------------------------------------------------------------
# Software GL: a GPU-visible emulator contends for the operator's GPU and its
# frame timings then mean nothing.
nohup "$APPIMAGE" "$DSX" >"$STDOUT_LOG" 2>&1 &
OWN_ROOT=$!
LAUNCH_TS=$(date +%s)

# ---- (4) poll for readiness; never a bare long sleep ------------------------
# Software GL is slow enough that a level's RomFS mount and first texture
# uploads can take minutes, so the window is configurable rather than guessed.
READY_TIMEOUT="${EMURUN_READY_TIMEOUT:-300}"
LOG=""
for _ in $(seq 1 "$READY_TIMEOUT"); do
  LOG="$(find_guest_logs | head -1)"
  [ -n "$LOG" ] && grep -q "$READY_TOKEN" "$LOG" 2>/dev/null && break
  kill -0 "$OWN_ROOT" 2>/dev/null || {
    echo "emurun: emulator exited before producing a log" >&2
    sed -n '1,20p' "$STDOUT_LOG" >&2
    exit 4
  }
  sleep 1
done
[ -n "$LOG" ] && grep -q "$READY_TOKEN" "$LOG" 2>/dev/null || {
  echo "emurun: no readiness token ('$READY_TOKEN') within ${READY_TIMEOUT} s. Refusing to report." >&2
  exit 4
}

# ---- settle window ----------------------------------------------------------
END=$(( $(date +%s) + SECS ))
while [ "$(date +%s)" -lt "$END" ]; do
  kill -0 "$OWN_ROOT" 2>/dev/null || {
    echo "emurun: emulator DIED during the settle window. Refusing to report." >&2
    exit 4
  }
  sleep 1
done

[ -z "$SHOT" ] || import -window root "$SHOT" 2>/dev/null || \
  import -display "$DISPLAY" -window root "$SHOT" 2>/dev/null || \
  echo "emurun: WARNING no screenshot tool (ImageMagick 'import'); continuing" >&2

# ---- (1)(2) FRESHNESS — never report a stale or mixed log --------------------
[ -f "$LOG" ] || { echo "emurun: guest log vanished: $LOG" >&2; exit 3; }
LOG_TS=$(stat -c %Y "$LOG" 2>/dev/null || echo 0)
[ "$LOG_TS" -ge "$LAUNCH_TS" ] || {
  echo "emurun: $LOG is older than this launch. Refusing to report." >&2
  exit 3
}

# ---- verdict ----------------------------------------------------------------
FRAMES=$(grep -c "$FRAME_RE" "$LOG" 2>/dev/null || echo 0)

echo "== emurun verdict =="
echo "display   : $DISPLAY"
echo "emulator  : $(basename "$APPIMAGE") md5 ${EMU_MD5:0:8}"
echo "build     : $DSX"
echo "guest log : $LOG"
echo "frames    : $FRAMES"
grep "$READY_TOKEN" "$LOG" | head -3
grep 'romfsInit\|FAILED\|level\|draws issued' "$LOG" | head -8
grep "$FRAME_RE" "$LOG" | tail -3
[ -z "$EXTRA_GREP" ] || grep -E "$EXTRA_GREP" "$LOG" | head -20

if [ "$FRAMES" -lt "$MIN_FRAMES" ]; then
  echo "emurun: only reached frame $FRAMES (< --min-frames $MIN_FRAMES). Refusing to report." >&2
  exit 5
fi

echo "log: $LOG"
exit 0

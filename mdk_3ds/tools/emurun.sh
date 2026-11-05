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
#   7. An unasserted instrument is a silent one. This box has two real GPUs, so
#      an emulator launched bare can answer from hardware and its frame timings
#      then mean nothing — and nothing in the verdict would say so. The launch
#      line now FORCES software GL and mutes host audio, and the verdict prints
#      the instrument it forced plus the API the config selected.
#   8. Mandarine's stereoscopy (render_3d/factor_3d) and audio settings live in
#      a config shared with every other lane on this box, and it is rewritten
#      on graceful exit. Two lanes on one XDG_CONFIG_HOME change each other's
#      arms; AGENTS P6 also requires the stereoscopic mode of a capture to be
#      RECORDED, which is impossible while the mode belongs to whoever wrote
#      the file last. So the config is private, seeded from the user's copy.
#   9. The FUSE root the AppImage mounts is a PATH, not the emulator's PID.
#      Handing the PID to `fusermount -uz` fails every time, silently, and the
#      dead /tmp/.mount_mandarXXXXXX mounts accumulate until a launch fails for
#      a reason that looks like the game dying.
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
# BRE, NOT ERE — `grep -c '^frame [0-9]+ '` matches a literal `+` and therefore
# counts ZERO frames on a run that reached frame 3540. Measured 2026-11-05.
FRAME_RE='^frame [0-9][0-9]* '

while [ $# -gt 0 ]; do
  case "$1" in
    --secs)       SECS="$2";       shift 2 ;;
    --grep)       EXTRA_GREP="$2"; shift 2 ;;
    --shot)       SHOT="$2";       shift 2 ;;
    --min-frames) MIN_FRAMES="$2"; shift 2 ;;
    --display)    DISPLAY_NUM="$2"; shift 2 ;;
    -h|--help)    awk 'NR>2 { if ($0 ~ /^# ={10,}/) exit; print }' "$0"; exit 0 ;;
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
mkdir -p "$LANE/xdg" "$LANE/cfg/mandarine-emu" "$LANE/runtime" || {
  echo "emurun: cannot create $LANE" >&2; exit 2; }
chmod 700 "$LANE/runtime" 2>/dev/null || true
export XDG_DATA_HOME="$LANE/xdg"
# Qt's QStandardPaths warns and picks a shared default when this is unset, and
# the session bus puts its socket there. Give the lane its own.
export XDG_RUNTIME_DIR="$LANE/runtime"
STDOUT_LOG="$LANE/stdout.log"

# (8) PRIVATE CONFIG, SEEDED FROM THE USER'S. Citra-lineage resolves its config
# dir from XDG_CONFIG_HOME and its sdmc from XDG_DATA_HOME, so the two can be
# isolated independently. Seeding (not starting bare) matters: a bare config
# re-derives every default, including use_custom_storage and graphics_api, so a
# lane would silently measure with a different emulator configuration than the
# one the operator proved. The copy is written, the original is only read.
CFG_SRC="${EMURUN_CFG_SRC:-$HOME/.config/mandarine-emu}"
if [ -f "$CFG_SRC/qt-config.ini" ]; then
  cp "$CFG_SRC/qt-config.ini" "$LANE/cfg/mandarine-emu/" 2>/dev/null || true
fi
if [ -f "$CFG_SRC/sdl2-config.ini" ]; then
  cp "$CFG_SRC/sdl2-config.ini" "$LANE/cfg/mandarine-emu/" 2>/dev/null || true
fi
export XDG_CONFIG_HOME="$LANE/cfg"
EMU_CFG="$LANE/cfg/mandarine-emu/qt-config.ini"

# Every override must clear its `\default=true` companion or Citra-lineage
# ignores the value — the same trap as use_gdbstub.
cfg_insert() { # section key value — writes `key=value` directly under [section]
  awk -v sec="$1" -v k="$2" -v v="$3" '
    { print }
    $0 == "[" sec "]" { print k "=" v }
  ' "$EMU_CFG" > "$EMU_CFG.tmp" && mv "$EMU_CFG.tmp" "$EMU_CFG"
}
cfg_set() { # key value [section]
  local key="$1" val="$2" sec="${3:-}"
  mkdir -p "$(dirname "$EMU_CFG")" 2>/dev/null || true
  [ -f "$EMU_CFG" ] || : >"$EMU_CFG"
  if grep -q "^$key=" "$EMU_CFG" 2>/dev/null; then
    sed -i "s|^$key=.*|$key=$val|" "$EMU_CFG"
  elif [ -n "$sec" ] && grep -q "^\[$sec\]$" "$EMU_CFG" 2>/dev/null; then
    # AN ABSENT KEY CANNOT GO AT THE END OF THE FILE. An INI key belongs to the
    # section whose header precedes it, so appending `volume=0` after the last
    # header ([WebService] on this box) writes a key in the wrong section, where
    # it is inert — review-1 F5(a): the append branch was the exact trap the
    # comment above names. Insert under the header instead.
    cfg_insert "$sec" "$key" "$val"
  else
    printf '%s=%s\n' "$key" "$val" >>"$EMU_CFG"
  fi
  if grep -q "^$key\\\\default=" "$EMU_CFG" 2>/dev/null; then
    sed -i "s|^$key\\\\default=.*|$key\\\\default=false|" "$EMU_CFG"
  elif [ -n "$sec" ] && grep -q "^\[$sec\]$" "$EMU_CFG" 2>/dev/null; then
    cfg_insert "$sec" "$key\\default" false
  else
    printf '%s\\default=false\n' "$key" >>"$EMU_CFG"
  fi
}
cfg_get() { grep -m1 "^$1=" "$EMU_CFG" 2>/dev/null | head -1 | cut -d= -f2; }
cfg_default_get() { grep -m1 "^$1\\\\default=" "$EMU_CFG" 2>/dev/null | head -1 | cut -d= -f2; }
# (P6) A recorded value is only evidence if it is the value that GOVERNED. In
# Citra-lineage every key has a `key\default=` companion, and the emulator WRITES
# THE WHOLE FILE BACK on exit — measured 2026-11-05: the harness cleared
# `graphics_api\default` before the launch and the file read back `true`
# afterwards, because Mandarine re-saved it and considered 1 to be its compiled
# default. So the flags printed here are a READBACK of the emulator's own final
# state, not a restatement of what this script wrote, which is the stronger of the
# two: `default=true` means the stored value equals the emulator's default (the
# number is right, it just was not forced by us), `default=false` means the stored
# value is an override the emulator accepted and kept (review-1 F5(b)).
cfg_default_note() {
  case "$(cfg_default_get "$1")" in
    false) printf ' [default=false — an explicit override the emulator kept]' ;;
    true)  printf ' [default=true — the emulator counts this as its compiled default, so the stored value matches what ran]' ;;
    *)     printf ' [no default flag — unproven]' ;;
  esac
}
# ALWAYS-MUTE, in the lane copy only: the env block below is the belt, this is
# the braces, because a config alone does not help if the engine ignores it and
# the operator has to hear it.
cfg_set volume 0 Audio
cfg_set enable_realtime_audio false Audio
# THE INSTRUMENT IS FORCED, NOT HOPED FOR. graphics_api is Citra's BackendAPI
# (1 = OpenGL, 2 = Vulkan) and the Mesa software-GL env below binds ONLY the
# OpenGL path; leaving the API to a seed the operator may have left on Vulkan
# would make the forced instrument a fiction. Stereo is pinned mono for the same
# reason: P6 requires a capture to record its stereoscopic mode, and a mode that
# belongs to whatever the last lane wrote is not a recorded mode.
# [Layout] and [Renderer] are the sections this build actually reads them from
# (verified against the operator's seed on 2026-11-05: render_3d/factor_3d under
# [Layout], graphics_api under [Renderer]); naming the section is what makes the
# insert branch land where the emulator looks.
cfg_set graphics_api 1 Renderer
cfg_set render_3d 0 Layout
cfg_set factor_3d 0 Layout

OWN_ROOT=""
OUR_MOUNT=""
# The mandarine FUSE mounts that already existed when this lane started, taken
# BEFORE the launch (review-1 F3). Anything that appears after this point belongs
# to somebody else and this harness never touches it — a co-tenant that launches
# at the same second is exactly the case a name-based reaper destroys.
PRE_MOUNTS=""

# (3)(9) OWNERSHIP BY THE LANE, not by name and not by the PID tree alone.
# A `pgrep -P` walk cannot see the whole emulator: the AppImage runtime forks
# AppRun.wrapped and that child gets REPARENTED (measured 2026-11-05, PPID 1),
# so even a transitive walk leaves a live emulator running when the harness
# gives up. Every process this run starts therefore carries MDK_EMURUN_LANE, a
# token unique to this lane and inherited by every descendant. It is matched as a
# WHOLE environ entry, terminated by the newline that `tr '\0' '\n'` produces:
# matching a bare prefix let lane `/tmp/mdk-emurun.1234` claim the co-tenant lane
# `/tmp/mdk-emurun.12345` and kill its emulator (review-1 F8), and the only
# consumer of this match is `kill`. Unreadable environ (another user) means "not
# ours", which stays the fail-safe direction: this declines to kill what it cannot
# positively own.
emurun_own_pids() {
  local p env
  [ -n "$OWN_ROOT" ] && echo "$OWN_ROOT"
  for p in $(pgrep -f 'mandarine\.AppImage|AppRun\.wrapped' 2>/dev/null); do
    env=$( { tr '\0' '\n' < "/proc/$p/environ"; printf '\n'; } 2>/dev/null || true )
    case "$env" in *$'\n'"MDK_EMURUN_LANE=$LANE"$'\n'*) echo "$p" ;; esac
  done
  return 0
}

# The AppImage execs AppRun.wrapped from inside its own FUSE mount, so the
# mount root is the `.mount_mandar*` ANCESTOR of a process exe — a path, which
# is what fusermount wants. Scan the whole tree, because the launcher wrapper
# (dbus-run-session) is the PID we own while the emulator is its child.
emurun_find_mount() {
  local p exe m
  for p in $(emurun_own_pids 2>/dev/null); do
    exe=$(readlink -f "/proc/$p/exe" 2>/dev/null || true)
    [ -n "$exe" ] || continue
    m="$exe"
    while [ "$m" != "/" ] && [ "$m" != "." ]; do
      case "$(basename "$m")" in
        .mount_mandar*) OUR_MOUNT="$m"; return 0 ;;
      esac
      m=$(dirname "$m")
    done
  done
  return 1
}

# A dead child that has not been reaped is a ZOMBIE, and `kill -0` on a zombie
# SUCCEEDS — so a liveness check that only asks kill-0 can read a dead emulator
# as alive for its whole timeout. Read the process state instead.
emurun_alive() {
  local st
  st=$(cut -d' ' -f3 "/proc/${OWN_ROOT}/stat" 2>/dev/null)
  [ -n "$st" ] && [ "$st" != "Z" ]
}

# A squashfs mount may be released only if no live process executes out of it;
# yanking a co-tenant's live mount destroys someone else's run.
emurun_mount_in_use() {
  local m="$1" p exe
  for p in $(pgrep -f 'mandarine\.AppImage|AppRun\.wrapped' 2>/dev/null); do
    exe=$(readlink -f "/proc/$p/exe" 2>/dev/null || true)
    case "$exe" in "$m"/*) return 0 ;; esac
  done
  return 1
}
# Was this mount already here when the lane started? grep -xF is a whole-line
# fixed match, so /tmp/.mount_mandarAAAA never matches a longer path that merely
# starts with it.
emurun_pre_existing_mount() {
  printf '%s\n' "$PRE_MOUNTS" | grep -qxF "$1"
}

emurun_cleanup() {
  local p m
  for p in $(emurun_own_pids 2>/dev/null); do
    kill "$p" 2>/dev/null || true
  done
  sleep 1
  for p in $(emurun_own_pids 2>/dev/null); do
    kill -9 "$p" 2>/dev/null || true
  done
  # (9) unmount the FUSE root THIS run started, by path.
  if [ -n "$OUR_MOUNT" ]; then
    fusermount -uz "$OUR_MOUNT" 2>/dev/null || true
    rmdir "$OUR_MOUNT" 2>/dev/null || true
  else
    echo "emurun: NOTE the AppImage FUSE mount was never identified; it may linger." >&2
  fi
  # Reclaim DEAD endpoints left over from earlier runs of this harness. The old
  # arm consulted the in-use test only when `stat` succeeded, so a merely SLOW
  # mount — the likely state of a lane under llvmpipe load — was lazily detached
  # by whoever exited first, and its owner's guest lost its RomFS and came back as
  # rc=4 "the game DIED" (review-1 F3). Three changes, all in the safe direction:
  #   * only mounts that existed BEFORE this lane launched are candidates;
  #   * the in-use test is consulted unconditionally, before anything else;
  #   * death must be PROVEN. A mount that answers `stat` is live and is left
  #     alone even if nothing executes from it (this lane's own mount was already
  #     unmounted above by path), and a `stat` that times out or fails for any
  #     reason other than "Transport endpoint is not connected" is inconclusive,
  #     which means leave it alone. An inconclusive probe is never evidence of
  #     death — the same rule every other guard in this file follows.
  # Enumerate /proc/mounts, NOT a glob: a dead FUSE endpoint fails stat(), so
  # `[ -d ]` skips exactly the mounts that need reclaiming.
  for m in $(awk '$2 ~ /mount_mandar/ {print $2}' /proc/mounts 2>/dev/null); do
    [ "$m" = "${OUR_MOUNT:-}" ] && continue
    emurun_pre_existing_mount "$m" || continue
    emurun_mount_in_use "$m" && continue
    stat_out=$(timeout 3 stat "$m" 2>&1)
    stat_rc=$?
    if [ "$stat_rc" -eq 0 ]; then
      continue                      # it answers: it is live, and live is not ours to take
    elif [ "$stat_rc" -eq 124 ]; then
      continue                      # slow, not dead: inconclusive is never proof
    else
      case "$stat_out" in
        *"Transport endpoint is not connected"*) : ;;   # proven dead
        *) continue ;;                                  # unknown cause: leave it
      esac
    fi
    fusermount -uz "$m" 2>/dev/null || true
    rmdir "$m" 2>/dev/null || true
  done
  return 0
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
# Mandarine needs a session bus: without one, Qt's GameMode probe runs
# `dbus-launch`, that segfaults, and the emulator aborts with a core dump
# BEFORE the guest boots — which under every other harness reads as rc=4
# "the game died". Measured here 2026-11-05: `Aborted (core dumped)` plus
# "Could not connect to bus: /usr/bin/dbus-launch terminated abnormally".
# `dbus-run-session` gives it a private bus per lane (the sibling port's
# harness has required the same since its first Mandarine week).
DBUS_WRAP=()
if command -v dbus-run-session >/dev/null 2>&1; then
  DBUS_WRAP=(dbus-run-session --)
else
  echo "emurun: NOTE no dbus-run-session; Mandarine may abort without a session bus" >&2
fi
# Snapshot the leftovers BEFORE anything of ours exists (review-1 F3): see
# emurun_pre_existing_mount / the reclaim pass in emurun_cleanup.
PRE_MOUNTS="$(awk '$2 ~ /mount_mandar/ {print $2}' /proc/mounts 2>/dev/null)"
# (7) Force the instrument instead of inheriting one. This box has two real
# GPUs; a bare launch can answer from hardware, and then the frame timings in
# the verdict mean nothing and nothing in the evidence says so.
#   LIBGL_ALWAYS_SOFTWARE/GALLIUM_DRIVER -> llvmpipe for the OpenGL path.
#   SDL_AUDIODRIVER/ALSA_CARD/AUDIODEV  -> no host audio device is opened at
#     all, so nothing can reach the operator's speakers.
#   XDG_CONFIG_HOME -> the private lane config from (8).
# NB these bind the OpenGL path; if the lane config selects Vulkan
# (graphics_api=2) Mesa's env vars are inert and the verdict says so.
nohup env \
  DISPLAY=":$DISPLAY_NUM" \
  MDK_EMURUN_LANE="$LANE" \
  XDG_CONFIG_HOME="$XDG_CONFIG_HOME" \
  XDG_DATA_HOME="$XDG_DATA_HOME" \
  XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
  LIBGL_ALWAYS_SOFTWARE=1 \
  GALLIUM_DRIVER=llvmpipe \
  SDL_AUDIODRIVER=dummy \
  ALSA_CARD=null \
  AUDIODEV=null \
  "${DBUS_WRAP[@]}" "$APPIMAGE" "$DSX" >"$STDOUT_LOG" 2>&1 &
OWN_ROOT=$!
LAUNCH_TS=$(date +%s)

# (9) The mount appears a moment after the fork; poll for it so cleanup has a
# path rather than a PID.
for _ in $(seq 1 20); do
  emurun_find_mount && break
  emurun_alive || break
  sleep 0.2
done

# ---- (4) poll for readiness; never a bare long sleep ------------------------
# Software GL is slow enough that a level's RomFS mount and first texture
# uploads can take minutes, so the window is configurable rather than guessed.
READY_TIMEOUT="${EMURUN_READY_TIMEOUT:-300}"
LOG=""
for _ in $(seq 1 "$READY_TIMEOUT"); do
  LOG="$(find_guest_logs | head -1)"
  [ -n "$LOG" ] && grep -q "$READY_TOKEN" "$LOG" 2>/dev/null && break
  emurun_alive || {
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
  emurun_alive || {
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
# `grep -c` prints its count AND exits nonzero on zero matches, so the usual
# `$(grep -c ... || echo 0)` yields the two-line string "0\n0" and every later
# integer test dies with "integer expression expected". Count, then sanitise.
FRAMES=$(grep -c "$FRAME_RE" "$LOG" 2>/dev/null || true)
case "$FRAMES" in ''|*[!0-9]*) FRAMES=0 ;; esac
# The guest logs `frame N` for the first five frames and then every 60th, so
# the number of LINES is a sampling artefact. The frame NUMBER it reached is the
# run length, and that is what --min-frames has to mean.
LAST_FRAME=$(grep "$FRAME_RE" "$LOG" 2>/dev/null | tail -1 | awk '{print $2}')
case "${LAST_FRAME:-}" in ''|*[!0-9]*) LAST_FRAME=0 ;; esac

# What the instrument actually IS, read back from the lane config AFTER the run,
# so it is the emulator's own final state and not this script's intention: see
# cfg_default_note for why the `\default` flag is the part that carries the
# evidence. graphics_api is Citra's BackendAPI (1 = OpenGL, 2 = Vulkan); only the
# OpenGL path is affected by LIBGL_ALWAYS_SOFTWARE, so saying which one answered
# is the difference between a recorded instrument and a guessed one.
GL_API="$(cfg_get graphics_api)"
STEREO_3D="$(cfg_get render_3d)"
STEREO_DEPTH="$(cfg_get factor_3d)"
CFG_VOLUME="$(cfg_get volume)"

echo "== emurun verdict =="
echo "display   : $DISPLAY"
echo "emulator  : $(basename "$APPIMAGE") md5 ${EMU_MD5:0:8}"
echo "build     : $DSX"
echo "guest log : $LOG"
echo "frames    : reached $LAST_FRAME ($FRAMES frame lines logged, every 60th after #5)"
# (7)(8) The instrument, recorded rather than assumed (AGENTS P6: a capture
# must record its clock/cache settings AND its stereoscopic mode).
echo "gl        : forced llvmpipe (LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe)"
echo "          graphics_api=${GL_API:-unset}$(cfg_default_note graphics_api) (1=OpenGL 2=Vulkan; Mesa env binds OpenGL only)"
echo "stereo    : render_3d=${STEREO_3D:-unset}$(cfg_default_note render_3d) factor_3d=${STEREO_DEPTH:-unset}$(cfg_default_note factor_3d) (0 = mono)"
echo "audio     : SDL_AUDIODRIVER=dummy ALSA_CARD=null AUDIODEV=null cfg volume=${CFG_VOLUME:-unset}$(cfg_default_note volume)"
echo "lane      : $LANE (mount ${OUR_MOUNT:-unidentified})"
echo "session   : ${DBUS_WRAP[0]:-no dbus wrapper}; XDG_RUNTIME_DIR private"
grep -iE 'renderer|opengl|vulkan|llvmpipe|GL:|Device' "$STDOUT_LOG" 2>/dev/null | head -5
grep "$READY_TOKEN" "$LOG" | head -3
grep 'romfsInit\|FAILED\|level\|draws issued' "$LOG" | head -8
grep "$FRAME_RE" "$LOG" | tail -3
[ -z "$EXTRA_GREP" ] || grep -E "$EXTRA_GREP" "$LOG" | head -20

if [ "$LAST_FRAME" -lt "$MIN_FRAMES" ]; then
  echo "emurun: only reached frame $LAST_FRAME (< --min-frames $MIN_FRAMES). Refusing to report." >&2
  exit 5
fi

echo "log: $LOG"
exit 0

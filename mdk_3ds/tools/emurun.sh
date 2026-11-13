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
#  10. That mount is not reliably a private path. The AppImage runtime mints a
#      fresh /tmp/.mount_mandarXXXX for a launch, but when it finds one it left
#      behind for the same AppImage it REUSES it — measured 2026-11-05, three
#      concurrent launches held three distinct mounts, while review-2 watched a
#      stale one get handed to two campaigns. So unmounting "the mount I saw" can
#      still detach a co-tenant's RomFS, and a status discarded from
#      `fusermount -uz` makes the mount that stayed behind invisible (review-2 F4).
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
#   mdk_3ds/tools/emurun.sh --expect hud         # fail unless the HUD's canvas drew,
#                                               # in the panel, WHERE THE CANVAS MATRIX
#                                               # SAYS IT GOES (see canary_verdict)
#   mdk_3ds/tools/emurun.sh --expect hud-visible --shot S.png
#                                               # the pixel verdict on its own
#   mdk_3ds/tools/emurun.sh path/to/other.3dsx
#
# EXIT CODES
#   0 ok | 2 launch/environment failed | 3 STALE LOG, refused to report
#   4 emulator died before producing a log | 5 run too short to measure
#   6 emulator version is not the pinned known-good build
#   7 the run was healthy but a --expect evidence line was not there
#
# WHY 7 IS NOT 2 OR 5. A missing HUD is not a broken launch and not a short run:
# the emulator ran, the game ran, and the thing the batch was for did not happen.
# Folding it into either of those would let a caller that only checks "did the
# harness work" read a failure as a pass -- which is the same mistake as an
# `--expect` that exits 0 whatever the game does.
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
EXPECT=""

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
    --expect)     EXPECT="$2";     shift 2 ;;
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

# (11) THE DSP FIRMWARE, SEEDED FROM THE OPERATOR'S. A private XDG_DATA_HOME has no
# sysdata/dspfirm.cdc, and Mandarine's auto-populate copies from the sysdata it no
# longer looks at once the data home is isolated — so the same isolation that stops
# two lanes reading each other's log also silently removes the DSP firmware, and a
# game that checks for it (ndspInit returns 0xD880A7FA without it, on hardware)
# goes mute and its audio path is never exercised at all. Silence in the guest log
# is exactly what this harness exists to make loud.
#
# So the operator's own dump is copied to the lane's sdmc, which is what
# Mandarine would have put in the shared one. It is read, never written, never
# committed and never shipped: a CIA must not carry the firmware blob (the same
# split Forsaken ships). EMURUN_DSPFIRM=/dev/null forces the missing-firmware path
# on purpose; EMURUN_DSPFIRM=<path> names a dump.
DSPFIRM_SRC="${EMURUN_DSPFIRM-}"
if [ -z "$DSPFIRM_SRC" ]; then
  for candidate in "$HOME/.local/share/mandarine-emu/sysdata/dspfirm.cdc" \
                   "$HOME/.local/share/citra-emu/sysdata/dspfirm.cdc" \
                   /home/*/.local/share/mandarine-emu/sysdata/dspfirm.cdc; do
    if [ -r "$candidate" ]; then DSPFIRM_SRC="$candidate"; break; fi
  done
fi
if [ -n "$DSPFIRM_SRC" ] && [ -r "$DSPFIRM_SRC" ] && [ -s "$DSPFIRM_SRC" ]; then
  mkdir -p "$LANE/xdg/mandarine-emu/sdmc/3ds" && cp "$DSPFIRM_SRC" \
    "$LANE/xdg/mandarine-emu/sdmc/3ds/dspfirm.cdc" 2>/dev/null || true
  EMURUN_DSPFIRM_SEEDED="$(basename "$DSPFIRM_SRC")"
else
  EMURUN_DSPFIRM_SEEDED="none (the guest will report audio muted)"
fi

# Every override must clear its `\default=true` companion or Citra-lineage
# ignores the value — the same trap as use_gdbstub.
cfg_insert() { # section key value — writes `key=value` directly under [section]
  # THE KEY AND THE VALUE TRAVEL THROUGH THE ENVIRONMENT, NOT `-v`. `awk -v` runs
  # its assignment through escape processing, so a key containing a DEFINED escape
  # is written wrong: measured on this box's mawk 1.3.4, `-v k='a\b'` yields `a`
  # followed by a backspace. An undefined one survives (`-v k='a\d'` stays `a\d`,
  # which is why the `name\default` companions were not already broken), but this
  # harness writes keys whose spelling contains a backslash and should not depend
  # on which escape table the local awk happens to implement. ENVIRON performs no
  # escape processing at all, so the class of bug is gone, not just one instance.
  EMURUN_AWK_SEC="$1" EMURUN_AWK_KEY="$2" EMURUN_AWK_VAL="$3" awk '
    { print }
    $0 == "[" ENVIRON["EMURUN_AWK_SEC"] "]" {
      print ENVIRON["EMURUN_AWK_KEY"] "=" ENVIRON["EMURUN_AWK_VAL"]
    }
  ' "$EMU_CFG" > "$EMU_CFG.tmp" && mv "$EMU_CFG.tmp" "$EMU_CFG"
}
# Does a line START with `key=`? Fixed-string on purpose: the companion keys are
# `name\default`, and a backslash in a grep BRE is an escape, not a backslash —
# measured, the pattern ^a\d= does NOT match the line a\b=. A pattern that matches
# nothing makes every caller believe the key is missing and write a second one.
cfg_present() { # key -> 0 when some line begins with `key=`
  # ENVIRON for the same reason cfg_insert does: `-v` would hand the program the
  # key with its escapes already resolved, and then it agrees with a broken writer.
  EMURUN_AWK_K="$1=" awk 'index($0, ENVIRON["EMURUN_AWK_K"]) == 1 { found = 1 }
                          END { exit found ? 0 : 1 }' "$EMU_CFG" 2>/dev/null
}
# Which section owns `key` — the last [Header] before its line, which is the
# QSettings rule the harness has to satisfy. PRESENT IS NOT GOVERNING: a key that
# sits under a header the emulator does not read this value from is inert, and
# `cfg_present` cannot tell the two apart (review-3 F2: it reported an inert key
# under [General] as "an explicit override the emulator kept"). Prints the bare
# section name, or NONE for a key that precedes every header.
cfg_section_of() { # key -> owning section, or NONE
  EMURUN_AWK_K="$1=" awk '
    /^\[.*\]$/ { sec = substr($0, 2, length($0) - 2) }
    index($0, ENVIRON["EMURUN_AWK_K"]) == 1 && !seen { print (sec == "" ? "NONE" : sec); seen = 1 }
  ' "$EMU_CFG" 2>/dev/null | head -1
}
# Rewrite every line that begins with `key=` to `key=value`, wherever it sits.
# awk + ENVIRON rather than sed, and not for style: the companion keys are
# `name\default` and a BRE reads the `\d` as an escape, which is the same trap
# cfg_present exists to avoid. A sed pattern built from such a key matches nothing
# and every caller then believes the rewrite succeeded.
cfg_rewrite() { # key value
  EMURUN_AWK_K="$1=" EMURUN_AWK_LINE="$1=$2" awk '
    index($0, ENVIRON["EMURUN_AWK_K"]) == 1 { print ENVIRON["EMURUN_AWK_LINE"]; next }
    { print }
  ' "$EMU_CFG" >"$EMU_CFG.tmp" && mv "$EMU_CFG.tmp" "$EMU_CFG"
}
# Drop every line that begins with `key=`, wherever it sits. Used only to MOVE a
# key out of a section the emulator does not read it from, so the file ends with
# exactly one governing line rather than two that disagree — and so cfg_get's
# first match is the value that actually governs.
cfg_drop() { # key
  EMURUN_AWK_K="$1=" awk 'index($0, ENVIRON["EMURUN_AWK_K"]) != 1' "$EMU_CFG" >"$EMU_CFG.tmp" \
    && mv "$EMU_CFG.tmp" "$EMU_CFG"
}
# Where each forced key actually landed, and it is part of the verdict because a
# key in the wrong section is inert. `existing` = the seed had the key and it was
# rewritten in place; `inserted` = it went under a header that was already there;
# `new-section` = the header had to be created first; `UNPLACED` = it never landed
# in a named section, which proves nothing and must never read as a forced value.
declare -A CFG_PLACED=()
cfg_placed() { printf '%s' "${CFG_PLACED[$1]:-UNPLACED}"; }
# Put `key=value` under [section], creating the header if the file has none.
# Returns 0 only if the key is present afterwards.
cfg_place() { # key value section -> 0 when the key is now in the file
  local key="$1" val="$2" sec="$3" had=1
  if grep -q "^\[$sec\]$" "$EMU_CFG" 2>/dev/null; then
    had=1
  else
    printf '\n[%s]\n' "$sec" >>"$EMU_CFG" || return 1
    had=0
  fi
  cfg_insert "$sec" "$key" "$val"
  cfg_present "$key" || return 1
  CFG_PLACED[$key]=$([ "$had" -eq 1 ] && printf inserted || printf new-section)
  return 0
}
cfg_set() { # key value [section]
  local key="$1" val="$2" sec="${3:-}" owner
  # ONE backslash, because this is the key as it appears in the file. It must not
  # reach a BRE, which is why every use below is cfg_rewrite/cfg_present/cfg_drop
  # (fixed-string) and none of them is sed.
  local ckey="$key\\default"
  mkdir -p "$(dirname "$EMU_CFG")" 2>/dev/null || true
  [ -f "$EMU_CFG" ] || : >"$EMU_CFG"
  if cfg_present "$key"; then
    # (review-3 F2) Only a key already under $sec may be called `existing`. The
    # headerless F5 fix closed the append-at-EOF case and left this one: the seed
    # had `graphics_api` under [General], the rewrite landed there, the emulator
    # read [Renderer], found nothing of its own, and the verdict still printed
    # "an explicit override the emulator kept". Move it to where the emulator
    # looks and say that it moved.
    owner="$(cfg_section_of "$key")"
    if [ -n "$sec" ] && [ "$owner" != "$sec" ]; then
      cfg_drop "$key"
      if cfg_place "$key" "$val" "$sec"; then CFG_PLACED[$key]=moved; else CFG_PLACED[$key]=UNPLACED; fi
    elif cfg_rewrite "$key" "$val"; then
      CFG_PLACED[$key]=existing
    fi
  elif [ -n "$sec" ]; then
    # AN ABSENT KEY CANNOT GO AT THE END OF THE FILE. An INI key belongs to the
    # section whose header precedes it, so appending `volume=0` after the last
    # header ([WebService] on this box) writes a key in the wrong section, where
    # it is inert — review-1 F5(a): the append branch was the exact trap the
    # comment above names. Insert under the header instead.
    #
    # AND NEITHER CAN AN ABSENT SECTION (review-2 F5). The first fix only took
    # the insert branch when the header already existed, so a fresh or badly
    # seeded config — the CI case, and any box where EMURUN_CFG_SRC points at
    # nothing — fell back to the bare append while the verdict still printed
    # "an explicit override the emulator kept". QSettings writes a header for
    # every section it uses, so creating the header is what the emulator itself
    # would do, and the key then sits where the emulator looks for it.
    cfg_place "$key" "$val" "$sec" || CFG_PLACED[$key]=UNPLACED
  else
    # No section named: the key cannot be placed, so say so instead of guessing.
    printf '%s=%s\n' "$key" "$val" >>"$EMU_CFG"
    CFG_PLACED[$key]=UNPLACED
  fi
  # The companion gets the same section test the key does (review-3 F2): a
  # `name\default` sitting under the wrong header governs nothing, and the
  # readback note would quote a flag the emulator never consulted.
  if cfg_present "$ckey"; then
    owner="$(cfg_section_of "$ckey")"
    if [ -n "$sec" ] && [ "$owner" != "$sec" ]; then
      cfg_drop "$ckey"
      cfg_place "$ckey" false "$sec" || CFG_PLACED[$key]=UNPLACED
    else
      cfg_rewrite "$ckey" false
    fi
  elif [ -n "$sec" ]; then
    cfg_place "$ckey" false "$sec" || CFG_PLACED[$key]=UNPLACED
  else
    printf '%s\\default=false\n' "$key" >>"$EMU_CFG"
    CFG_PLACED[$key]=UNPLACED
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
  # Placement comes FIRST, because a key that never reached a section cannot
  # have governed anything no matter what the readback says (review-2 F5).
  case "$(cfg_placed "$1")" in
    UNPLACED)
      printf ' [placement UNPLACED — the key never landed in a named section, so it is inert and proves nothing]'
      return ;;
    moved)
      # It did land in the right section, so the readback below is meaningful; the
      # reader just has to know the harness had to move it to get there.
      printf ' [moved by this harness out of a section the emulator does not read]' ;;
    new-section)
      printf ' [section created by this harness]' ;;
  esac
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

# Where per-process state is read: /proc, unless a test points it at a fixture
# directory shaped like one. The seam exists because review-4 F5 caught the
# harness test pinning "an unreadable process" with pid 999999 — a pid that does
# not exist, so every read answers ENOENT, which is a different condition from
# the EACCES a cross-uid co-tenant produces and a different decision from the
# one that had to fail safe. A fixture directory whose process dir is mode 000
# yields a real EACCES, so the branch that must refuse is actually executed.
PROC="${MDK_EMURUN_PROC:-/proc}"

# (review-4 F5) `pgrep -f` matches COMMAND LINES, and on this box the AppImage
# path appears in command lines that are nothing to do with an emulator. Measured
# live 2026-11-05: `pgrep -f 'mandarine\.AppImage|AppRun\.wrapped'` named six
# processes and only two were the emulator (comm `AppRun.wrapped`, exe inside
# /tmp/.mount_mandare*); the other four were another agent's `timeout`/`sudo`
# wrappers whose argv quoted the path (pids 1179774/1179777/1279067/1279070,
# uid 1000/root, environ EACCES, exe unreadable). A command-line hit is a
# CANDIDATE, never a verdict. The ownership guards below ask a narrow question
# about a specific mount, and an unclaimable false positive used to answer
# "foreign", which leaked this lane's own mount on every run.
# A candidate is an emulator when the kernel's own comm says so — comm is world
# readable and a quoted argv cannot forge it — or when its exe lives under a
# `.mount_mandar*` FUSE root. An unreadable comm is KEPT: being unable to rule a
# process out is not the same as ruling it out.
emurun_emulator_pids() {
  local p comm exe
  for p in $(pgrep -f 'mandarine\.AppImage|AppRun\.wrapped' 2>/dev/null); do
    comm=$(cat "$PROC/$p/comm" 2>/dev/null || true)
    if [ -z "$comm" ]; then
      echo "$p"                      # unreadable comm: cannot rule it out
      continue
    fi
    case "$comm" in
      *mandarine*|*AppRun*|*AppImage*) echo "$p"; continue ;;
    esac
    exe=$(readlink "$PROC/$p/exe" 2>/dev/null || true)
    case "$exe" in */.mount_mandar*/*) echo "$p" ;; esac
  done
  return 0
}

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
    env=$( { tr '\0' '\n' < "$PROC/$p/environ"; printf '\n'; } 2>/dev/null || true )
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
  for p in $(emurun_emulator_pids 2>/dev/null); do
    exe=$(readlink -f "$PROC/$p/exe" 2>/dev/null || true)
    case "$exe" in "$m"/*) return 0 ;; esac
  done
  return 1
}
# (review-2 F4) The same question, but EXCLUDING this lane's own processes, so it
# answers "does anybody ELSE live in this mount?". The lane token is the only
# ownership proof this file trusts (see emurun_own_pids): a process that does not
# carry it is not ours, and one whose environ this uid cannot read is not ours
# either — an unclaimable process inside a mount means leave the mount alone.
# `readlink` without -f on purpose: the raw symlink is the path the kernel
# recorded, and resolving it walks INTO the FUSE mount, which fails for exactly
# the dead endpoints this question has to survive.
emurun_mount_foreign() {
  local m="$1" p env exe
  for p in $(emurun_emulator_pids 2>/dev/null); do
    # A pid pgrep listed that has already exited says nothing about this mount.
    # Every read below fails on it, and the old code read that as "unclaimable,
    # therefore foreign", so an unrelated process that happened to exit during
    # cleanup held this lane's mount forever (review-4 F5). ENOENT is the absence
    # of a process; EACCES is the absence of proof about one that is live, and
    # only the second is this guard's problem.
    [ -d "$PROC/$p" ] || continue
    # Can THIS lane claim it? The token is the only proof. A failed environ read
    # is the absence of proof, NOT proof of foreignness: it falls through to the
    # mount question below (review-4 F1 — the old order answered "foreign" here,
    # before ever asking where the process runs from, so any unclaimable process
    # on the box — and this box always has some, see emurun_emulator_pids — made
    # every lane leak its own mount and print a NOTE the guard never established).
    # The `2>/dev/null` wraps the GROUP, not `tr`: a failed `<` redirect is
    # reported by the shell before tr runs, so a per-command redirect does not
    # catch it and the harness verdict used to be followed by a bare
    # "emurun.sh: line N: /proc/PID/environ: Permission denied" (measured live
    # 2026-11-05 with a cross-uid co-tenant present). The refusal must be silent
    # and still counted.
    if env="$( { tr '\0' '\n' < "$PROC/$p/environ"; } 2>/dev/null)"; then
      case $'\n'"$env"$'\n' in
        *$'\n'"MDK_EMURUN_LANE=$LANE"$'\n'*) continue ;;   # ours: not foreign
      esac
    fi
    # Not ours. Now ask the question that actually decides it: does it execute out
    # of THE MOUNT UNDER TEST? The raw symlink, not readlink -f — resolving it
    # walks INTO the FUSE mount, which fails for exactly the dead endpoints this
    # question has to survive.
    exe="$(readlink "$PROC/$p/exe" 2>/dev/null)" || exe=""
    # Unclaimable AND unlocatable: we cannot say it is outside the mount, and
    # "unknown" must never read as "free". This is the only place a failed read
    # still answers foreign, and it now needs BOTH reads to fail.
    [ -n "$exe" ] || return 0
    case "$exe" in "$m"/*) return 0 ;; esac
  done
  return 1
}
# Is this path still a mount after an unmount attempt? Field 2 of /proc/mounts,
# compared whole, so a prefix can never match a longer mountpoint.
emurun_is_mounted() {
  awk -v m="$1" '$2 == m { found = 1 } END { exit found ? 0 : 1 }' /proc/mounts 2>/dev/null
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
  # (review-5 F4) The verdict samples the frame counter at the deadline and then
  # this trap runs, so the emulator keeps writing while it is reaped: a measured
  # run printed "reached 1680 (33 frame lines)" while the retained log ended at
  # frame 1740 with 34 lines. The quote was faithful to the tool and still
  # re-checked wrong against the log it came from. Re-read AFTER the reap and print
  # the number the log actually ends at, so the verdict's figure is visibly a
  # lower bound taken at the deadline and not the run's length.
  if [ -n "${LOG:-}" ] && [ -f "${LOG:-}" ]; then
    local end_frame end_lines
    end_frame=$(grep "$FRAME_RE" "$LOG" 2>/dev/null | tail -1 | awk '{print $2}')
    end_lines=$(grep -c "$FRAME_RE" "$LOG" 2>/dev/null || true)
    case "${end_frame:-}" in ''|*[!0-9]*) end_frame=0 ;; esac
    case "${end_lines:-}" in ''|*[!0-9]*) end_lines=0 ;; esac
    echo "emurun: after the reap the guest log ends at frame $end_frame ($end_lines frame lines logged)" >&2
  fi
  # (9) unmount the FUSE root THIS run started, by path — but only what this
  # lane can PROVE is its own (review-2 F4: this was unconditional, and the
  # status was discarded). The AppImage runtime reuses an existing mount for the
  # same AppImage file, so two campaigns can execute out of one
  # /tmp/.mount_mandarXXXX, and whichever exits first used to lazily detach the
  # mount the other was running from: that guest loses its RomFS under it and
  # comes back as rc=4 "the game DIED", which is the exact symptom review-1 F3
  # was written about. Either proof is enough to leave it alone:
  #   * it was already mounted when this lane launched (PRE_MOUNTS), so the
  #     runtime handed us a mount somebody else made; or
  #   * a process without this lane's token executes out of it.
  if [ -n "$OUR_MOUNT" ]; then
    if emurun_pre_existing_mount "$OUR_MOUNT"; then
      echo "emurun: NOTE $OUR_MOUNT predates this lane (the AppImage runtime shared it); left mounted." >&2
    elif emurun_mount_foreign "$OUR_MOUNT"; then
      echo "emurun: NOTE $OUR_MOUNT has a process outside this lane executing from it; left mounted." >&2
    else
      um_out=$(fusermount -uz "$OUR_MOUNT" 2>&1)
      um_rc=$?
      rmdir "$OUR_MOUNT" 2>/dev/null || true
      # A discarded status is how a mount that never went away reads as cleaned
      # up, and the reviewer's open question 1 could not be answered for want of
      # exactly this. Report it with the reason instead of swallowing it.
      if emurun_is_mounted "$OUR_MOUNT"; then
        echo "emurun: WARNING $OUR_MOUNT is STILL MOUNTED after fusermount -uz (rc=$um_rc${um_out:+: $um_out}); not this harness's to force." >&2
      fi
    fi
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
  #     alone even if nothing executes from it (this lane's own mount was handled
  #     above, and only if this lane could prove it was ours), and a `stat` that
  #     times out or fails for any
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

# An `--expect` run ALWAYS has a frame to judge, even when the caller did not ask
# for one. The ledger's `3ds.hud-canvas` acceptance cell is `emurun.sh --expect hud`
# with no `--shot`, and a verdict that needs pixels cannot depend on the caller
# remembering to ask for a picture — that is how a gate goes back to being
# self-referential the moment nobody is looking (review-1 F1).
if [ -n "$EXPECT" ] && [ -z "$SHOT" ]; then
  SHOT="$(mktemp "${TMPDIR:-/tmp}/mdk-emurun-expect.XXXXXX.png")"
fi

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
echo "frames    : reached $LAST_FRAME by the deadline ($FRAMES frame lines logged, every 60th after #5); the post-reap line below is the log's real last frame"
# (7)(8) The instrument, recorded rather than assumed (AGENTS P6: a capture
# must record its clock/cache settings AND its stereoscopic mode).
echo "gl        : forced llvmpipe (LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe)"
echo "          graphics_api=${GL_API:-unset}$(cfg_default_note graphics_api) (1=OpenGL 2=Vulkan; Mesa env binds OpenGL only)"
echo "stereo    : render_3d=${STEREO_3D:-unset}$(cfg_default_note render_3d) factor_3d=${STEREO_DEPTH:-unset}$(cfg_default_note factor_3d) (0 = mono)"
echo "audio     : SDL_AUDIODRIVER=dummy ALSA_CARD=null AUDIODEV=null cfg volume=${CFG_VOLUME:-unset}$(cfg_default_note volume)"
echo "dspfirm   : ${EMURUN_DSPFIRM_SEEDED:-none} (the lane's sdmc/3ds/dspfirm.cdc; ndsp needs it)"
echo "lane      : $LANE (mount ${OUR_MOUNT:-unidentified})"
echo "session   : ${DBUS_WRAP[0]:-no dbus wrapper}; XDG_RUNTIME_DIR private"
grep -iE 'renderer|opengl|vulkan|llvmpipe|GL:|Device' "$STDOUT_LOG" 2>/dev/null | head -5
grep "$READY_TOKEN" "$LOG" | head -3
grep 'romfsInit\|FAILED\|level\|draws issued' "$LOG" | head -8
grep "$FRAME_RE" "$LOG" | tail -3
[ -z "$EXTRA_GREP" ] || grep -E "$EXTRA_GREP" "$LOG" | head -20

# --- expectations -----------------------------------------------------------
# `--expect NAME` turns a ledger row's acceptance cell into this harness's exit
# code, so "the HUD works on the 3DS" is a command that fails when the HUD does
# not. An acceptance that cannot fail is not evidence.
#
# It is a FUNCTION and not a couple of lines at the bottom for one reason: the
# harness cannot be run end to end by the host suite (it needs a console
# emulator, a display and a minute of wall clock), so the only way to gate the
# adjudication is to LIFT it and run it against a synthetic log. That is what
# tests/scripts/emurun_harness_test.sh does, with the same technique it already
# uses for the config functions: lifted from this file, not retyped there, or the
# test certifies a copy.
#
# `--expect hud` is TWO verdicts and not one, and the second one is the recent half.
#
# THE LOG LINE is the CPU's half: it proves the canvas was QUEUED and EMITTED, not
# PAINTED. `draws` counts the records the replay reached `C3D_DrawArrays` with and
# nothing downstream feeds back, so a frame whose draws the GPU never rasterises
# prints the same line as a frame that painted (measured, 2026-11-11: the panel
# contained zero pixels of the canary colour while this line was green). What the
# two numbers DO separate is the CPU-side half of the story -- the quads the frontend
# asked for against the draws the per-eye replay emitted, so a canvas that was queued
# but dropped by a mesh or texture check, or drawn into one eye only, is a different
# pair of numbers.
#
# THE PIXELS are the GPU's half, and they are a visibility verdict now: the old
# paragraph here said a green `--expect hud` "is not a visibility verdict and must not
# be recorded as one", and that sentence outlived the code it described by two batches
# (review-4 F3). It was written when the flag read only the log line. It does not any
# more, and a stale disclaimer is how a real verdict gets under-reported for a year.
#
# WHAT THE PIXELS CANNOT DO, and the honest limit: they are a KNOWN-COLOUR canary and
# not a reference diff. A HUD-off build does not exist and would need a renderer switch
# this project does not have, so there is no "same frame, no HUD" picture to subtract.
# That costs one thing: the canary proves that a draw in the canvas's own state and
# space reached the panel at the canvas's own corner. It does not prove the HUD's own
# quads are correct, correctly coloured, or complete -- the dial's fidelity is a
# separate question with a separate oracle (the C# renderer), and this flag says
# nothing about it.
canary_stats() { # canary_stats <png> <rect_x1> <rect_y1>
                 # -> "<total> <inside> <bbox_x0> <bbox_y0> <bbox_x1> <bbox_y1>"
    python3 - "$1" "$2" "$3" "${REPO:-${ROOT:-.}}/tools/panel_crop.py" <<'PY' 2>/dev/null
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(sys.argv[4])))
from panel_crop import PANEL_X0, PANEL_Y0, PANEL_X1, PANEL_Y1, open_rgb
rx1, ry1 = int(sys.argv[2]), int(sys.argv[3])
px = open_rgb(sys.argv[1]).load()
total = inside = 0
bx0 = by0 = 1 << 30
bx1 = by1 = -1
for y in range(PANEL_Y0, PANEL_Y1 + 1):
    for x in range(PANEL_X0, PANEL_X1 + 1):
        if px[x, y] != (0, 0, 255):
            continue
        total += 1
        cx, cy = x - PANEL_X0, y - PANEL_Y0
        bx0, by0 = min(bx0, cx), min(by0, cy)
        bx1, by1 = max(bx1, cx), max(by1, cy)
        if cx <= rx1 and cy <= ry1:
            inside += 1
if bx1 < 0:
    print("0 0 - - - -")
else:
    print(total, inside, bx0, by0, bx1, by1)
PY
}

# THE MARKER'S EXPECTED RECT, in top-panel crop coordinates, inclusive.
#
# This is DERIVED, not remembered, and tests/scripts/emurun_harness_test.sh re-derives
# it from the shipped constants and fails if it disagrees -- a hand-copied expectation
# is a second source of truth that drifts, which is the mistake batch 147's word-order
# bug hid behind for eleven runs.
#
#   canvas      600 x 360 units  (kCanvasHeight 360 x the frame's aspect: 360*400/240,
#                                 renderer_c3d.cpp's mdk_render_canvas_width)
#   marker      the canvas's own [0,16] x [0,16]  (drawDiagnostic's kSize)
#   portrait    240 x 400        (kScreenWidth x kScreenHeight -- the ROTATED
#                                 framebuffer axes, not the landscape 400x240)
#   canvasMatrix maps canvas (x,y) to portrait
#       px = (1 - y/360) * 240,  py = (x/600) * 400
#   and the display rotates the portrait target back to the landscape crop as
#       crop_x = py,  crop_y = 240 - px
#   so the marker's far corner lands at
#       crop_x = 16*400/600 = 10.667   crop_y = 16*240/360 = 10.667
#   Pixel centres below 10.667 are columns/rows 0..10 -- an 11x11 block, 121 pixels,
#   which is exactly what the console measures (docs/evidence/2026-11-12-147-*.txt).
#   ceil() adds one column and one row of rasterisation slack, giving 0,0-11,11.
#
# WHY A RECTANGLE AND NOT JUST A COUNT (review-4 F1). A count of the canary colour
# cannot see the SIGN of the canvas transform. pica::canvasMatrix's panel quarter turn
# is out.x = m.y, out.y = -m.x; flip that second sign and the determinant goes from +1
# to -1 -- a REFLECTION, the exact family of defect that made the canvas invisible for
# eleven runs -- and the marker moves to the panel's other corner while painting the
# same 121 pixels. A count-only canary passes that build unchanged.
#
# WHY THE RECTANGLE NEEDS TWO EDGES AND NOT ONE (review-1 F1). A rect tested only as
# "enough pixels fell inside it" still cannot see the marker's EXTENT. Ask only how
# many landed inside 0,0-11,11 and a marker that paints 27 columns wide -- what
# mdk_render_canvas_width returning 240 instead of 600 draws -- keeps 132 of its 297
# pixels inside the window and passes, with the contradicting bbox printed on the very
# same success line. So the rect is enforced from BOTH sides: nothing of the marker may
# lie outside it (containment), and the marker must reach its derived far corner
# (extent). Together those pin the marker's bbox to the derived [floor,ceil] of 10.667
# on each axis, which is the extent the canvas's own width produces.
CANARY_X1=11
CANARY_Y1=11
# The marker's derived far corner is 10.667 on both axes, so ceil() gives the outer
# bound above and floor() gives the reach bound here: a marker that stops short of
# column/row 10 is a canvas whose width is too LARGE, the mirror-image defect of the
# one the containment half catches. tests/scripts/emurun_harness_test.sh re-derives
# all four numbers from the shipped renderer and reddens on disagreement.
CANARY_REACH_X=10
CANARY_REACH_Y=10
# 25 and not 1: the marker is ~121 panel pixels, and a floor above a stray edge pixel
# keeps one lucky sample from reading as a painted HUD, while staying far below the
# marker's size so a HUD that paints at all clears it.
CANARY_MIN=25
#
# THE PANEL'S OWN MEASUREMENT LIMITS, and why a verdict needs them (review-2 F2).
#
# Every arm below is `[ field -lt constant ]`, and a `[ ]` whose operands it cannot
# parse exits 2 -- NOT 1 -- so the `if` skips its body and the verdict walks past the
# rejection into `HUD VISIBLE`. "Is the field made of digits" does not prevent that:
# `9223372036854775808` is all digits and is past LONG_MAX, so the comparison dies
# exactly as dead as `[ x -lt 25 ]` does, and the verdict printed
#     HUD VISIBLE -- 9223372036854775808 canary pixels in the panel, ...
# with rc 0. A field therefore has to be a number the comparison can actually MAKE,
# which means a bound and not only a character class. The bound is the panel's own
# geometry, so it is a measurement limit rather than a magic ceiling: canary_stats
# counts inside tools/panel_crop.py's crop, so no honest count exceeds the crop's area
# and no honest crop coordinate exceeds its width. tests/scripts/emurun_harness_test.sh
# re-derives both numbers from panel_crop.py and reddens on disagreement, so neither is
# a second spelling of a rectangle this file does not own.
CANARY_MAX_PIXELS=96000   # PANEL_SIZE: 400 columns x 240 rows
CANARY_MAX_COORD=399      # the panel's widest axis, inclusive far index

# WHY THE CANARY IS A COLOUR AND NOT A DIFF (and where it comes from).
#
# A pixel verdict needs something to look for, and the obvious answer -- diff the HUD
# frame against a HUD-off frame -- needs a HUD-off build, which would need a renderer
# switch this project does not have and should not grow for a test.
#
# So the renderer ships a 16x16 marker drawn in the canvas's own space, through the
# canvas's own matrix, texture, combiner, tint, cull and depth state
# (renderer_c3d.cpp's `drawDiagnostic`), in pure (0,0,255) -- a colour that does not
# occur anywhere in the level-7 capture, so an exact hit in the panel can only have
# come from the canvas's draw path. That is a visibility verdict from one frame and
# no reference; the rect above is what turns it from "some pixels appeared" into "the
# canvas's own corner was painted".

# Is this field a measurement the panel could physically produce?
canary_measurement() { # canary_measurement <field> <max> ; 0 when it is a decimal in [0,max]
    # The three `case` arms are: empty, not-digits, and SIX OR MORE DIGITS. The third is
    # the one review-2 F2 found: it keeps `[ "$1" -le "$2" ]` below inside the range the
    # parser can read, which is the whole point -- and every honest field is at most five
    # digits (96000), so the arm cannot reject a real measurement.
    case "$1" in ''|*[!0-9]|??????*) return 1 ;; esac
    [ "$1" -le "$2" ]
}

canary_verdict() { # canary_verdict <shot> ; 0 when the canvas reached the panel,
                   # at the corner AND at the extent the canvas's own matrix puts
                   # its top-left marker at
    local shot="$1" stats total inside bx0 by0 bx1 by1
    if [ -z "$shot" ] || [ ! -f "$shot" ]; then
        echo "emurun: no screenshot to judge the HUD on (the capture failed)" >&2
        return 1
    fi
    stats="$(canary_stats "$shot" "$CANARY_X1" "$CANARY_Y1")"
    read -r total inside bx0 by0 bx1 by1 <<EOF
${stats:-0 0 - - - -}
EOF
    # MALFORMED EVIDENCE IS A FAILED VERDICT AND NEVER A GREEN ONE (review-1 F5, and
    # review-2 F2 for the half of it F5 missed). The guard used to test the CONCATENATION
    # "${total}${inside}", so a stats line whose FIRST field was blank and second numeric
    # read as "all digits", both comparisons then died on "integer expression expected",
    # no branch was taken, and the function FELL THROUGH TO `HUD VISIBLE`. F5 replaced
    # that with a per-field digit test, which fixed the blank field and left the hole
    # open, because the failure was never about characters: it is about a comparison that
    # cannot be made, and an `if` cannot tell that apart from a comparison that came out
    # false. So nothing is silently reset to a safe-looking value any more -- an
    # unevaluable field is NAMED and REJECTED, which is the only way the third outcome
    # ("I could not check it") cannot be read as either of the other two.
    #
    # THE COUNTS FIRST, and the bbox later, because "nothing painted" is a real verdict
    # with a real name and its bbox is legitimately `-`; rejecting it as malformed would
    # rename the most common honest failure.
    if ! canary_measurement "$total" "$CANARY_MAX_PIXELS" \
       || ! canary_measurement "$inside" "$CANARY_MAX_PIXELS"; then
        {
            echo "emurun: HUD MALFORMED: the canary count line is [$stats] and" >&2
            echo "          [$total]/[$inside] are not counts a $CANARY_MAX_PIXELS-pixel" >&2
            echo "          panel can produce. A comparison this function cannot make is"
            echo "          a rejection, never a pass (review-2 F2)."
        } >&2
        return 1
    fi
    # THE HARNESS'S OWN CONSTANTS, for the same reason: a comparison has two sides and
    # either one can be the unevaluable one, and a CANARY_MIN of `x` fails open exactly
    # the way an impossible pixel count does.
    if ! canary_measurement "$CANARY_MIN" "$CANARY_MAX_PIXELS" \
       || ! canary_measurement "$CANARY_X1" "$CANARY_MAX_COORD" \
       || ! canary_measurement "$CANARY_Y1" "$CANARY_MAX_COORD" \
       || ! canary_measurement "$CANARY_REACH_X" "$CANARY_MAX_COORD" \
       || ! canary_measurement "$CANARY_REACH_Y" "$CANARY_MAX_COORD"; then
        echo "emurun: HUD MALFORMED: the harness's own canary constants are not" >&2
        echo "          measurements (MIN=$CANARY_MIN RECT=$CANARY_X1,$CANARY_Y1" >&2
        echo "          REACH=$CANARY_REACH_X,$CANARY_REACH_Y); refusing to judge." >&2
        return 1
    fi
    if [ "$total" -lt "$CANARY_MIN" ]; then
        {
            echo "emurun: HUD NOT VISIBLE: $total canary pixels of pure (0,0,255) in the"
            echo "          panel, $CANARY_MIN needed. The canvas's draws are emitted (see"
            echo "          the 'canvas:' line) but nothing drawn in the canvas's state"
            echo "          and space reaches the screen."
        } >&2
        return 1
    fi
    if [ "$inside" -lt "$CANARY_MIN" ]; then
        {
            echo "emurun: HUD MISPLACED: $total canary pixels painted, but only $inside"
            echo "          of them inside the marker's expected rect (crop"
            echo "          0,0-$CANARY_X1,$CANARY_Y1); their bbox is"
            echo "          ($bx0,$by0)-($bx1,$by1). The canvas drew, through the canvas's"
            echo "          own state, somewhere ELSE: a panel rotation with the wrong"
            echo "          sign, a reflection (determinant -1), or a matrix that is not"
            echo "          the canvas's. A count alone cannot tell them apart"
            echo "          (review-4 F1)."
        } >&2
        return 1
    fi
    # CONTAINMENT: every canary pixel must lie inside the marker's rect. Pixels beyond
    # the rect mean the marker is bigger than the canvas's own width can produce, or
    # sits off the corner -- either way the canvas's matrix is not the one derived from
    # the shipped constants (review-1 F1).
    if [ "$inside" -ne "$total" ]; then
        {
            echo "emurun: HUD OVERSIZED: $total canary pixels painted and only $inside"
            echo "          inside the marker's rect 0,0-$CANARY_X1,$CANARY_Y1, with bbox"
            echo "          ($bx0,$by0)-($bx1,$by1) reaching past it. The marker is drawn"
            echo "          through the canvas's own matrix, so a marker that spills out"
            echo "          of the rect that matrix implies is a canvas whose width is"
            echo "          not mdk_render_canvas_width's (a wrong-aspect HUD). A count"
            echo "          that only asks how many landed inside cannot see this"
            echo "          (review-1 F1)."
        } >&2
        return 1
    fi
    # The bbox is only consulted by the EXTENT arm below, so it is validated here rather
    # than with the counts: by this point the marker really did paint, so a bbox that is
    # not a coordinate pair is broken evidence and not the honest no-pixels case.
    if ! canary_measurement "$bx1" "$CANARY_MAX_COORD" \
       || ! canary_measurement "$by1" "$CANARY_MAX_COORD"; then
        {
            echo "emurun: HUD MALFORMED: the marker's bbox is ($bx0,$by0)-($bx1,$by1), which"
            echo "          is not a pair of coordinates inside a $CANARY_MAX_COORD-wide"
            echo "          panel, so the extent cannot be adjudicated (review-2 F2)."
        } >&2
        return 1
    fi
    # EXTENT: the marker must reach its derived far corner. A marker that stops short
    # is a canvas whose width is too LARGE, the mirror image of the defect containment
    # catches, and it passes a count-only and an inside-only rule alike.
    if [ "$bx1" -lt "$CANARY_REACH_X" ] || [ "$by1" -lt "$CANARY_REACH_Y" ]; then
        {
            echo "emurun: HUD UNDERSIZED: the marker's bbox is ($bx0,$by0)-($bx1,$by1)"
            echo "          but it must reach ($CANARY_REACH_X,$CANARY_REACH_Y), the floor"
            echo "          of the far corner the shipped constants put it at. The canvas"
            echo "          painted, in the right corner, at the WRONG SIZE: a canvasWidth"
            echo "          larger than mdk_render_canvas_width's shrinks the marker while"
            echo "          leaving it entirely inside the rect."
        } >&2
        return 1
    fi
    echo "expect    : HUD VISIBLE -- $total canary pixels in the panel, $inside inside"
    echo "expect    : the marker's rect 0,0-$CANARY_X1,$CANARY_Y1 (bbox $bx0,$by0-$bx1,$by1,"
    echo "expect    : reaching the derived ($CANARY_REACH_X,$CANARY_REACH_Y))"
    return 0
}

expect_check() { # expect_check <name> <log> <shot> ; 0 when the evidence is there
    local name="$1" log="$2" shot="${3:-}"
    local line quads draws
    case "$name" in
        hud-visible)
            # The pixel verdict on its own, for when the emitted line is not the
            # question. `hud` below is this check AND the log line.
            canary_verdict "$shot"
            return $?
            ;;
        hud)
            # The line is found by its OWN SHAPE (`canvas: quads=`) and the LAST one
            # wins, for two reasons. A prefix as loose as `canvas: ` is shadowed by an
            # unrelated line that happens to share it -- measured on the first run of
            # this flag, where the mesh-allocation note `canvas: stream mesh 384
            # corners` came first and the check reported a HUD failure over a HUD that
            # had drawn. And the last line, not the first, is the one that says the HUD
            # was still drawing when the run ended.
            line="$(grep '^canvas: quads=' "$log" 2>/dev/null | tail -1)"
            if [ -z "$line" ]; then
                echo "emurun: --expect hud FAILED: the guest wrote no 'canvas:' line, " >&2
                echo "          so the HUD's canvas was never queued (hud.load failed, " >&2
                echo "          or the frontend never calls the canvas seams)." >&2
                return 1
            fi
            quads="$(printf '%s' "$line" | sed -n 's/.*quads=\([0-9][0-9]*\).*/\1/p')"
            draws="$(printf '%s' "$line" | sed -n 's/.*draws=\([0-9][0-9]*\).*/\1/p')"
            # Both numbers, and both non-zero. `quads` alone would also be printed
            # by a batch that was queued and then dropped (no mesh, no texture);
            # `draws` alone could not say whether the frontend asked for anything.
            if [ -z "$quads" ] || [ "$quads" -le 0 ] || [ -z "$draws" ] || \
               [ "$draws" -le 0 ]; then
                echo "emurun: --expect hud FAILED: the canvas line is '$line'" >&2
                echo "          (quads and draws both have to be above zero)." >&2
                return 1
            fi
            echo "expect    : hud emitted -- $line"
            # AND THE PIXELS. The log line is the CPU's half of the story and this
            # is the GPU's; the ledger row is accepted on both, so a canvas that is
            # queued, emitted and never rasterised cannot close it (review-1 F1).
            canary_verdict "$shot"
            return $?
            ;;
        *)
            echo "emurun: --expect '$name' is not a known expectation (hud, hud-visible)" >&2
            return 2
            ;;
    esac
}
# --- end expectations ---------------------------------------------------------

if [ -n "$EXPECT" ]; then
  expect_check "$EXPECT" "$LOG" "$SHOT"
  rc=$?
  if [ "$rc" -eq 2 ]; then
    exit 2
  elif [ "$rc" -ne 0 ]; then
    exit 7
  fi
fi

if [ "$LAST_FRAME" -lt "$MIN_FRAMES" ]; then
  echo "emurun: only reached frame $LAST_FRAME (< --min-frames $MIN_FRAMES). Refusing to report." >&2
  exit 5
fi

echo "log: $LOG"
exit 0

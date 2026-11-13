#!/usr/bin/env bash
# emurun_harness_test.sh — the emulator harness's own instrument rules, gated.
#
# WHY THIS EXISTS. `mdk_3ds/tools/emurun.sh` is the only thing that turns a 3DS
# build into evidence (AGENTS P8), and nothing else in the suite can reach it:
# it launches a GUI emulator, so no host test can run it end to end. That left
# its two instrument rules pinned by prose only, and both were broken while the
# verdict printed a confident PASS (external review 2026-11-05,
# run-20261105-062743/review-2.md F4 and F5):
#
#   * F5 — an INI key belongs to the section whose header precedes it. When the
#     seed config had no `[Renderer]` header at all, `cfg_set` appended bare keys
#     at EOF, QSettings read them as `[General]`, the emulator never saw the
#     forced instrument, and the verdict still printed
#     "[default=false — an explicit override the emulator kept]". A recorded
#     value is only evidence if it is the value that GOVERNED (P6).
#   * F4 — the AppImage runtime hands every launch of the same AppImage the same
#     /tmp/.mount_mandarXXXXXX, so the lane's own mount is not automatically
#     the lane's to release. It was unmounted unconditionally, by path, with the
#     status discarded: the first lane out detached a co-tenant's RomFS and that
#     run came back as rc=4 "the game DIED".
#
# WHAT THIS ACTUALLY RUNS. The config functions are LIFTED OUT OF THE REAL FILE
# and executed against a temp file — not retyped here, or this test would drift
# from the harness and certify a copy. The mount guards cannot be executed
# without a live FUSE mount and a co-tenant, so they are pinned as source shape
# inside the cleanup arm, and the lift is measured (a fragment that lifted
# nothing would pass every presence check in it).
#
# usage: bash tests/scripts/emurun_harness_test.sh [path-to-emurun.sh]
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
EMU=${1:-$ROOT/mdk_3ds/tools/emurun.sh}
[ -f "$EMU" ] || { echo "  FAIL emurun harness: cannot read $EMU"; exit 1; }

fails=0
checks=0
check() { # check <label> <expected> <actual>
    checks=$((checks + 1))
    if [ "$2" = "$3" ]; then
        printf 'PASS %s\n' "$1"
    else
        printf 'FAIL %s — expected %s, got %s\n' "$1" "'$2'" "'$3'"
        fails=$((fails + 1))
    fi
}

# --- lift the config block out of the harness --------------------------------
CFG_FN="$(awk '
    /^# Every override must clear/ { f = 1 }
    /^# ALWAYS-MUTE/               { f = 0 }
    f
' "$EMU")"
n_fn=$(printf '%s\n' "$CFG_FN" | grep -cE '^[A-Za-z_]+\(\)')
check "the config block was lifted from emurun.sh (non-vacuity)" "1" \
      "$([ "$n_fn" -ge 6 ] && echo 1 || echo 0)"
check "and it is syntactically complete, not a truncation of it" "0" \
      "$(printf '%s\n' "$CFG_FN" | bash -n 2>/dev/null; printf '%s' "$?")"
for fn in cfg_insert cfg_place cfg_set cfg_get cfg_default_get cfg_default_note cfg_placed cfg_present; do
    check "the lift contains $fn, so its pins cannot read a copy" "1" \
          "$(printf '%s\n' "$CFG_FN" | grep -cE "^$fn\(\)" | head -1)"
done
check "the lift declares the placement map it reports from" "1" \
      "$(printf '%s\n' "$CFG_FN" | grep -c 'declare -A CFG_PLACED' | head -1)"

WORK=$(mktemp -d) || exit 1
# The mount fixtures below chmod a directory to 000 on purpose (that is what
# produces a real EACCES), so the cleanup has to undo that before it can remove
# it. GNU rm will not recurse into a directory it cannot read.
trap 'chmod -R u+rwX "$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT
EMU_CFG="$WORK/qt-config.ini"
# shellcheck disable=SC2016
printf '%s\n' "$CFG_FN" >"$WORK/fn.sh"

# Which section owns a key: the last [Header] before its line. This is the
# QSettings rule the harness has to satisfy, and it is what the first F5 fix got
# wrong — the key was IN the file, in the wrong section, and inert. Fixed-string
# matching, because the companion keys contain a backslash and a regex would eat
# it. The key arrives with its `=` already on it.
owns_key() { # <file> <key=> -> the section that owns it, or [NONE]
    # ENVIRON, not -v: awk -v resolves escape sequences in its assignment (a
    # defined one like \b comes out as a backspace, measured on this box's mawk),
    # and a key that matches nothing reads as "no section". Fixed-string index()
    # plus ENVIRON keeps the test asking the same question the harness asks.
    EMURUN_AWK_K="$2" awk '
        /^\[.*\]$/ { sec = $0 }
        index($0, ENVIRON["EMURUN_AWK_K"]) == 1 && !seen {
            print (sec == "" ? "[NONE]" : sec); seen = 1
        }
    ' "$1"
}
# Any key line that no header owns is a bare key at EOF — the F5 defect itself.
bare_keys() { # <file> -> count of key lines outside every section
    awk '/^\[/ { hdr = 1 } /=/ && !hdr { bad++ } END { print bad + 0 }' "$1"
}

run_case() { # <label> <seed-file-content> — sources the lifted functions fresh
    ( set -u
      : >"$EMU_CFG"
      [ -n "${1:-}" ] && printf '%s\n' "$1" >>"$EMU_CFG"
      # shellcheck disable=SC1090
      . "$WORK/fn.sh"
      cfg_set graphics_api 1 Renderer
      cfg_set volume 0 Audio
      printf 'placed=%s\n' "$(cfg_placed graphics_api)"
      printf 'section=%s\n' "$(owns_key "$EMU_CFG" 'graphics_api=')"
      printf 'dsection=%s\n' "$(owns_key "$EMU_CFG" 'graphics_api\default=')"
      printf 'dkey=%s\n' "$(awk '/^graphics_api\\default=/{print "present"} END{print}' "$EMU_CFG" | head -1)"
      printf 'value=%s\n' "$(cfg_get graphics_api)"
      printf 'note=%s\n' "$(cfg_default_note graphics_api)" )
}

# --- case 1: a bare config (fresh box, or EMURUN_CFG_SRC pointing at nothing) --
out=$(run_case "")
check "F5 bare config: the header is created, not skipped" "[Renderer]" \
      "$(printf '%s\n' "$out" | sed -n 's/^section=//p')"
check "F5 bare config: and the key is inside it" "1" \
      "$(printf '%s\n' "$out" | grep -c '^value=1$')"
check "F5 bare config: the \\default companion lands in the same section" "[Renderer]" \
      "$(printf '%s\n' "$out" | sed -n 's/^dsection=//p')"
check "F5 bare config: and it is written with a real backslash" "present" \
      "$(printf '%s\n' "$out" | sed -n 's/^dkey=//p')"
check "F5 bare config: nothing was appended outside a section" "0" \
      "$(bare_keys "$EMU_CFG")"
check "F5 bare config: placement is reported as a created section" "new-section" \
      "$(printf '%s\n' "$out" | sed -n 's/^placed=//p')"
check "F5 bare config: and the verdict says so instead of claiming a kept override" "1" \
      "$(printf '%s\n' "$out" | grep -c 'note=.*section created by this harness')"

# --- case 2: a seeded config that already has the headers ---------------------
out=$(run_case '[Renderer]
graphics_api=2
graphics_api\default=true

[Audio]
volume=100
volume\default=true')
check "F5 seeded config: an existing key is rewritten in place" "1" \
      "$(printf '%s\n' "$out" | grep -c '^value=1$')"
check "F5 seeded config: and it stays in its own section" "[Renderer]" \
      "$(printf '%s\n' "$out" | sed -n 's/^section=//p')"
check "F5 seeded config: placement is reported as an in-place rewrite" "existing" \
      "$(printf '%s\n' "$out" | sed -n 's/^placed=//p')"
check "F5 seeded config: the readback note is the emulator's own flag" "1" \
      "$(printf '%s\n' "$out" | grep -c 'note=.*default=false')"
check "F5 seeded config: no key was appended after the last header" "0" \
      "$(bare_keys "$EMU_CFG")"
check "F5 seeded config: the companion was rewritten, not duplicated" "1" \
      "$(awk '/^graphics_api\\default=/{n++} END{print n+0}' "$EMU_CFG")"

# --- case 2b: the header exists but the key does not --------------------------
out=$(run_case '[Renderer]
other_key=2')
check "F5 header-present case: the key is inserted under it" "[Renderer]" \
      "$(printf '%s\n' "$out" | sed -n 's/^section=//p')"
check "F5 header-present case: placement says inserted" "inserted" \
      "$(printf '%s\n' "$out" | sed -n 's/^placed=//p')"
check "F5 header-present case: the companion is written once, with its backslash" "present" \
      "$(printf '%s\n' "$out" | sed -n 's/^dkey=//p')"
check "F5 header-present case: and it is under the same header" "[Renderer]" \
      "$(printf '%s\n' "$out" | sed -n 's/^dsection=//p')"

# --- case 2c: the key is present but under the WRONG section (review-3 F2) ----
# The headerless F5 fix closed "appended at EOF" and left this: `cfg_present` said
# yes, the rewrite landed under [General], the emulator read [Renderer], found
# nothing of its own there, and the verdict still printed "an explicit override
# the emulator kept". A key in the wrong section is inert — the harness's own
# comment says so — so an inert key must never be reported as a forced value.
out=$(run_case '[General]
graphics_api=2
graphics_api\default=false

[Audio]
volume=100
volume\default=true')
check "F2 wrong-section key: it is MOVED, not rewritten where it sits" "moved" \
      "$(printf '%s\n' "$out" | sed -n 's/^placed=//p')"
check "F2 wrong-section key: and it lands under the section the emulator reads" "[Renderer]" \
      "$(printf '%s\n' "$out" | sed -n 's/^section=//p')"
check "F2 wrong-section key: the companion moves with it" "[Renderer]" \
      "$(printf '%s\n' "$out" | sed -n 's/^dsection=//p')"
check "F2 wrong-section key: exactly one governing line survives, not two" "1" \
      "$(awk '/^graphics_api=/{n++} END{print n+0}' "$EMU_CFG")"
check "F2 wrong-section key: the inert [General] copy is gone" "0" \
      "$(awk '/^\[General\]$/{g=1;next} /^\[/{g=0} g && /^graphics_api=/{n++} END{print n+0}' "$EMU_CFG")"
check "F2 wrong-section key: the readback is the moved value, so it is the one that governs" "1" \
      "$(printf '%s\n' "$out" | grep -c '^value=1$')"
check "F2 wrong-section key: the verdict discloses the move before it claims a kept override" "1" \
      "$(printf '%s\n' "$out" | grep -c 'note=.*moved by this harness out of a section')"

# --- case 2d: only the \default companion is in the wrong section -------------
# The companion decides whether the value is read as an override at all, so an
# inert companion is as much a false instrument as an inert key.
out=$( ( set -u; : >"$EMU_CFG"
        printf '[General]\nvolume\\default=true\n\n[Audio]\nvolume=100\n' >>"$EMU_CFG"
        . "$WORK/fn.sh"
        cfg_set volume 0 Audio
        printf 'dsection=%s\n' "$(owns_key "$EMU_CFG" 'volume\default=')"
        printf 'dcount=%s\n' "$(awk '/^volume\\default=/{n++} END{print n+0}' "$EMU_CFG")"
        printf 'dflt=%s\n' "$(cfg_default_get volume)" ) )
check "F2 wrong-section companion: moved under the header the emulator reads" "[Audio]" \
      "$(printf '%s\n' "$out" | sed -n 's/^dsection=//p')"
check "F2 wrong-section companion: and it is not duplicated" "1" \
      "$(printf '%s\n' "$out" | sed -n 's/^dcount=//p')"
check "F2 wrong-section companion: the rewrite actually landed (false, not the seed's true)" "false" \
      "$(printf '%s\n' "$out" | sed -n 's/^dflt=//p')"

# --- case 3: a key with no section named is UNPROVEN, never a forced value ----
out=$( ( set -u; : >"$EMU_CFG"; . "$WORK/fn.sh"; cfg_set mystery 7
        printf 'placed=%s\n' "$(cfg_placed mystery)"
        printf 'note=%s\n' "$(cfg_default_note mystery)" ) )
check "F5 unplaceable key: placement is UNPLACED, not silently appended" "UNPLACED" \
      "$(printf '%s\n' "$out" | sed -n 's/^placed=//p')"
check "F5 unplaceable key: and the note refuses to claim it governed" "1" \
      "$(printf '%s\n' "$out" | grep -c 'note=.*UNPLACED')"
check "F5 unplaceable key: the note does not claim a kept override" "0" \
      "$(printf '%s\n' "$out" | grep -c 'note=.*override the emulator kept')"

# --- the mount-ownership guards, as source shape -----------------------------
# The behaviour needs a live FUSE mount and a co-tenant, so what is pinned here
# is the arm's shape: the lane may only release a mount it can prove is its own,
# and a failed unmount must be reported. Scoped to the OUR_MOUNT arm of
# emurun_cleanup, because a file-wide grep is satisfied by the reclaim pass,
# which is a different rule about a different set of mounts.
MOUNT_ARM="$(awk '
    /^  # \(9\) unmount the FUSE root/ { f = 1 }
    f { print }
    /^  # Reclaim DEAD endpoints/     { exit }
' "$EMU")"
m_lines=$(printf '%s\n' "$MOUNT_ARM" | wc -l)
check "the OUR_MOUNT arm was lifted from emurun_cleanup (non-vacuity)" "1" \
      "$([ "$m_lines" -ge 15 ] && echo 1 || echo 0)"
check "and it lifted one arm, not the whole file" "1" \
      "$([ "$m_lines" -le 60 ] && echo 1 || echo 0)"
for probe in 'emurun_pre_existing_mount "$OUR_MOUNT"' 'emurun_mount_foreign "$OUR_MOUNT"' \
             'emurun_is_mounted "$OUR_MOUNT"'; do
    check "F4 the arm asks $probe before it can lose a mount" "1" \
          "$(printf '%s\n' "$MOUNT_ARM" | grep -cF "$probe" | head -1)"
done
check "F4 the unmount status is captured, not discarded" "1" \
      "$(printf '%s\n' "$MOUNT_ARM" | grep -cE 'um_rc=\$\?' | head -1)"
check "F4 and a mount that survived the attempt is a WARNING" "1" \
      "$(printf '%s\n' "$MOUNT_ARM" | grep -c 'WARNING' | head -1)"
check "F4 no fusermount call in the arm throws away its status" "0" \
      "$(printf '%s\n' "$MOUNT_ARM" | grep -cE 'fusermount .*2>/dev/null \|\| true' | head -1)"
# The predicate itself must exclude this lane, or the guard answers the wrong
# question: every lane is "in use" by its own emulator.
FOREIGN_FN="$(awk '/^emurun_mount_foreign\(\)/ { f = 1 } f { print } /^}/ { if (f) exit }' "$EMU")"
check "F4 emurun_mount_foreign exists and is scoped to one definition" "1" \
      "$([ "$(printf '%s\n' "$FOREIGN_FN" | wc -l)" -ge 6 ] && echo 1 || echo 0)"
check "F4 and it skips processes carrying this lane's token" "1" \
      "$(printf '%s\n' "$FOREIGN_FN" | grep -cF 'MDK_EMURUN_LANE=$LANE' | head -1)"

# The candidate set the guard is allowed to ask about, lifted the same way.
EMUPIDS_FN="$(awk '/^emurun_emulator_pids\(\)/ { f = 1 } f { print } /^}/ { if (f) exit }' "$EMU")"
check "F5 emurun_emulator_pids exists and is scoped to one definition" "1" \
      "$([ "$(printf '%s\n' "$EMUPIDS_FN" | wc -l)" -ge 8 ] && echo 1 || echo 0)"
check "F5 it is what mount_foreign iterates, not a parallel copy" "1" \
      "$([ "$(printf '%s\n' "$FOREIGN_FN" | grep -cF 'emurun_emulator_pids' | head -1)" -ge 1 ] && echo 1 || echo 0)"

# The predicate, RUN. The mount itself cannot be conjured, but the ownership
# question can: pgrep is stubbed to name processes, and their /proc entries are
# either the real thing (PROC=/proc) or a fixture directory shaped like one
# (PROC=$FAKEPROC). This is what catches review-3 F4, which a source-shape grep
# could not: the old body swallowed a failed environ read with `|| true`, so a
# process this uid cannot read at all fell through to a readlink that also fails
# on it, and the guard answered "free" for a mount it could not prove was free.
MOUNT=/tmp/.mount_mandarZZTEST
foreign_of() { # <proc-root> <pids...> -> exit 0 foreign / 1 not foreign
    (
        LANE="$FAKE_LANE"
        PROC="$1"; shift
        FAKE_PIDS="$*"
        pgrep() { printf '%s\n' $FAKE_PIDS; }   # the args are the real caller's pattern
        # shellcheck disable=SC2034
        eval "$(printf 'emurun_emulator_pids() { %s\n' "$(printf '%s\n' "$EMUPIDS_FN" | tail -n +2)")"
        eval "$(printf 'emurun_mount_foreign() { %s\n' "$(printf '%s\n' "$FOREIGN_FN" | tail -n +2)")"
        emurun_mount_foreign "$MOUNT" >/dev/null 2>&1
    )
}
# A live process carrying this lane's token is ours, whoever else is running.
FAKE_LANE="testlane$$"
MDK_EMURUN_LANE="$FAKE_LANE" sleep 30 & OWN_PID=$!
# A live process WITHOUT the token, executing outside the test mount: not ours,
# but provably not in the mount either.
sleep 30 & STRANGER_PID=$!
check "F4 a process carrying the lane token is ours, not foreign" "1" \
      "$(foreign_of /proc "$OWN_PID"; printf '%s' "$?")"
check "F4 a tokenless process outside the mount is not foreign either" "1" \
      "$(foreign_of /proc "$STRANGER_PID"; printf '%s' "$?")"
# (review-4 F5) A pid pgrep listed that has already exited is not an unreadable
# process: /proc/PID is gone, so every read fails for a reason that has nothing
# to do with ownership. This pin used to hand it 999999 and demand "foreign", so
# it certified the defect — a co-tenant that exited during cleanup would have
# held this lane's mount forever.
check "F5 a VANISHED pid is not an unreadable process, so it is not foreign" "1" \
      "$(foreign_of /proc 999999; printf '%s' "$?")"
check "F5 and a mix of ours and vanished still does not hold the mount" "1" \
      "$(foreign_of /proc "$OWN_PID" 999999; printf '%s' "$?")"
kill $OWN_PID $STRANGER_PID 2>/dev/null
wait 2>/dev/null

# --- the EACCES fixtures: a /proc shaped like a cross-uid co-tenant ----------
# The branch that has to REFUSE is the one a missing directory cannot reach, so
# the fixture makes a real EACCES: a mode-000 process directory is unreadable
# for its owner in exactly the way another uid's is.
FAKEPROC="$WORK/fakeproc"; mkdir -p "$FAKEPROC"
mkdir -p "$FAKEPROC/4101" "$FAKEPROC/4102" "$FAKEPROC/4103" \
         "$FAKEPROC/4104" "$FAKEPROC/4105" || exit 1
# 4101 — live, and nothing about it can be read: comm, environ and exe all EACCES.
# The directory itself is mode 000, so every entry lookup inside it is refused.
chmod 000 "$FAKEPROC/4101"
# 4102 — review-4 F1, the case that leaked every lane: an emulator-shaped comm,
# an UNREADABLE environ, and an exe provably OUTSIDE the mount under test. The
# old order answered "foreign" on the environ read alone, before ever asking
# where the process runs from.
printf 'mandarine.AppIm\n' >"$FAKEPROC/4102/comm"
ln -s /bin/sleep "$FAKEPROC/4102/exe"
: >"$FAKEPROC/4102/environ"; chmod 000 "$FAKEPROC/4102/environ"
# 4103 — a foreign emulator executing INSIDE the mount, no lane token.
printf 'AppRun.wrapped\n' >"$FAKEPROC/4103/comm"
ln -s "$MOUNT/usr/bin/mandarine" "$FAKEPROC/4103/exe"
printf 'OTHER=1\0' >"$FAKEPROC/4103/environ"
# 4104 — this lane's own emulator inside the mount, token present.
printf 'AppRun.wrapped\n' >"$FAKEPROC/4104/comm"
ln -s "$MOUNT/usr/bin/mandarine" "$FAKEPROC/4104/exe"
printf 'MDK_EMURUN_LANE=%s\0' "$FAKE_LANE" >"$FAKEPROC/4104/environ"
# 4105 — another agent's `timeout` wrapper whose argv quoted the AppImage path:
# a pgrep -f hit that is not an emulator at all (measured live 2026-11-05).
printf 'timeout\n' >"$FAKEPROC/4105/comm"
ln -s /usr/bin/timeout "$FAKEPROC/4105/exe"
: >"$FAKEPROC/4105/environ"; chmod 000 "$FAKEPROC/4105/environ"

check "F4 a LIVE process this uid cannot read at all is foreign, so the mount is left alone" "0" \
      "$(foreign_of "$FAKEPROC" 4101; printf '%s' "$?")"
check "F1 an unreadable environ on a process OUTSIDE the mount is not foreign" "1" \
      "$(foreign_of "$FAKEPROC" 4102; printf '%s' "$?")"
check "F1 and the mount question is asked before the environ is allowed to decide" "0" \
      "$(printf '%s\n' "$FOREIGN_FN" | awk '
            /readlink/   && !r { r = NR }
            /return 0/   && !f { f = NR }
            END { print (r && f && f > r) ? 0 : 1 }')"
check "F4 a foreign emulator executing inside the mount IS foreign" "0" \
      "$(foreign_of "$FAKEPROC" 4103; printf '%s' "$?")"
check "F4 this lane's own emulator inside the mount is ours" "1" \
      "$(foreign_of "$FAKEPROC" 4104; printf '%s' "$?")"
check "F5 a pgrep hit that is not an emulator is not even asked" "1" \
      "$(foreign_of "$FAKEPROC" 4105; printf '%s' "$?")"
check "F1 the mix that used to leak every lane now releases it" "1" \
      "$(foreign_of "$FAKEPROC" 4105 4102 4104; printf '%s' "$?")"
check "F4 but one real foreign emulator in the mix still holds it" "0" \
      "$(foreign_of "$FAKEPROC" 4105 4103; printf '%s' "$?")"

# The candidate set itself, RUN: a command-line mention is a candidate, not a
# verdict.
candidates_of() { # <pids...> -> the survivors, one per line
    (
        PROC="$FAKEPROC"
        FAKE_PIDS="$*"
        pgrep() { printf '%s\n' $FAKE_PIDS; }
        eval "$(printf 'emurun_emulator_pids() { %s\n' "$(printf '%s\n' "$EMUPIDS_FN" | tail -n +2)")"
        emurun_emulator_pids 2>/dev/null
    )
}
check "F5 the timeout wrapper whose argv quoted the path is dropped" "" \
      "$(candidates_of 4105)"
check "F5 and both real emulators survive the filter" "4102
4103" "$(candidates_of 4102 4103 4105)"
check "F5 an unreadable comm is kept: not ruling out is not ruling in" "4101" \
      "$(candidates_of 4101)"

# --- the `--expect` adjudication, lifted and RUN -----------------------------
#
# `--expect hud` is a ledger row's acceptance command, and the only thing that
# makes an acceptance command evidence is that it fails when the thing did not
# happen. It cannot be exercised here end to end -- that needs the emulator, a
# display and a minute -- so the function is lifted out of the real harness (not
# retyped, or this certifies a copy) and run against synthetic guest logs.
EXPECT_FN="$(awk '/^# --- expectations ---/{f=1;next} /^# --- end expectations/{f=0} f' "$EMU")"
check "the expectation block was lifted from emurun.sh (non-vacuity)" "1" \
      "$(printf '%s\n' "$EXPECT_FN" | grep -q "expect_check()" && echo 1 || echo 0)"
check "and the lift is syntactically complete, not a truncation of it" "0" \
      "$(printf '%s\n' "$EXPECT_FN" | bash -n 2>/dev/null; printf '%s' "$?")"

# The canary's own numbers, read back out of the harness so this file never carries a
# second copy of the threshold or the rectangle.
CANARY_X1="$(sed -n 's/^CANARY_X1=\([0-9][0-9]*\).*/\1/p' "$EMU")"
CANARY_Y1="$(sed -n 's/^CANARY_Y1=\([0-9][0-9]*\).*/\1/p' "$EMU")"
CANARY_REACH_X="$(sed -n 's/^CANARY_REACH_X=\([0-9][0-9]*\).*/\1/p' "$EMU")"
CANARY_REACH_Y="$(sed -n 's/^CANARY_REACH_Y=\([0-9][0-9]*\).*/\1/p' "$EMU")"
CANARY_MIN="$(sed -n 's/^CANARY_MIN=\([0-9][0-9]*\).*/\1/p' "$EMU")"
check "the harness declares the marker rect, its reach and the floor" "5" \
      "$([ -n "$CANARY_X1" ] && [ -n "$CANARY_Y1" ] && [ -n "$CANARY_REACH_X" ] \
         && [ -n "$CANARY_REACH_Y" ] && [ -n "$CANARY_MIN" ] && echo 5 || echo 0)"
# AND THE FIXTURE REFUSES TO RUN WITHOUT THEM (review-1 F4). Silently carrying an
# empty CANARY_X1 into the capture builder made python raise ValueError, which left
# CAP_YES empty, and four PRE-EXISTING rows then failed for a fixture reason and
# masqueraded as four new rules biting. A missing constant is a broken harness, not a
# verdict: say so and stop, so a parent-comparison reds only on the rows that are the
# new rule's work.
for v in CANARY_X1 CANARY_Y1 CANARY_REACH_X CANARY_REACH_Y CANARY_MIN; do
    case "${!v}" in
        ''|*[!0-9]*)
            echo "EMURUN HARNESS: $EMU declares no numeric $v -- the fixture cannot be built" >&2
            exit 1 ;;
    esac
done

# A synthetic emulator capture, so the PIXEL half of `--expect hud` is exercised
# here too. `--expect hud` now demands the canary as well as the log line (review-1
# F1: the log line alone was green over a HUD with no pixels), and an acceptance that
# cannot be exercised is not evidence. The capture is the emulator's documented
# 1280x1024 window. The scenes, because the canary now asks THREE questions -- did
# anything paint, did it paint in the canvas's own corner, and did it paint at the
# canvas's own size:
#   here     -- the marker where pica::canvasMatrix puts it (the passing case)
#   none     -- nothing painted at all (the review-1 F1 hole)
#   turn     -- "drop the minus": (a,b) = (1,+1), determinant -1. THE reflection
#               review-4 F1 named, and the disposition the old `mirror` fixture
#               claimed to be but was not (review-1 F2).
#   flip     -- (a,b) = (-1,-1), the other determinant -1 disposition
#   rot180   -- (a,b) = (-1,+1), determinant +1: the opposite-sense PROPER rotation,
#               which is what the old `mirror` fixture actually painted
#   wide     -- a marker 3x too wide: 132 of its 297 pixels still land inside the
#               rect, so an inside-count rule passes a wrong-aspect canvas (review-1 F1)
#   small    -- a marker shrunk wholly inside the rect: the mirror-image extent defect
#
# THE DISPOSITIONS ARE BUILT FROM THE SIGN PAIR, NOT FROM A DESCRIPTION (review-1 F2).
# pica::canvasMatrix's quarter turn is `out col0 = a·m col1`, `out col1 = b·m col0`
# with the shipped (a,b) = (1,-1). Each block's corner below is COMPUTED from (a,b)
# through the mapping the harness documents, so a fixture cannot drift from the
# transform it names -- and all four dispositions are rows here, not one.
cap() { # cap <here|none|turn|flip|rot180|wide|small> -> path to a synthetic capture
    python3 - "$1" "$ROOT/tools/panel_crop.py" "$CANARY_REACH_X" "$CANARY_REACH_Y" <<'PY'
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(sys.argv[2])))
from panel_crop import PANEL_X0, PANEL_Y0, PANEL_X1, PANEL_Y1
from PIL import Image
mode = sys.argv[1]
# The marker's size comes from the reach the harness DECLARES, not from a literal here:
# the derived far corner is 10.667, whose floor is REACH, so the marker spans REACH+1
# columns. Move the harness's reach and these fixtures move with it, instead of this
# file quietly certifying a different marker than the shipped one draws.
size_x, size_y = int(sys.argv[3]) + 1, int(sys.argv[4]) + 1
W, H = PANEL_X1 - PANEL_X0 + 1, PANEL_Y1 - PANEL_Y0 + 1
img = Image.new("RGB", (1280, 1024), (16, 32, 48))
px = img.load()


def block(x0, y0, w, h):
    for j in range(h):
        for i in range(w):
            px[PANEL_X0 + x0 + i, PANEL_Y0 + y0 + j] = (0, 0, 255)


def disposition(a, b):
    """Where canvasMatrix's (a,b) turn paints canvas [0,size]^2, in crop pixels.

    The harness documents px = (1 - y/360)*240, py = (x/canvasW)*400, crop_x = py and
    crop_y = 240 - px. Negating a column negates that axis's NDC term, which mirrors
    the painted span against the far edge of the portrait target -- so each sign just
    chooses which edge the marker sits flush against.
    """
    return (0 if b < 0 else W - size_x, 0 if a > 0 else H - size_y)


if mode == "here":                    # (a,b) = ( 1,-1)  shipped,  det +1
    block(*disposition(1, -1), size_x, size_y)
elif mode == "turn":                  # (a,b) = ( 1,+1)  det -1
    block(*disposition(1, 1), size_x, size_y)
elif mode == "flip":                  # (a,b) = (-1,-1)  det -1
    block(*disposition(-1, -1), size_x, size_y)
elif mode == "rot180":                # (a,b) = (-1,+1)  det +1
    block(*disposition(-1, 1), size_x, size_y)
elif mode == "wide":
    # mdk_render_canvas_width returning 240 instead of 600: the marker paints 26.7
    # columns wide instead of 10.7, so most of its pixels still land INSIDE the rect
    # and clear the floor. A rule that only counts what falls inside passes it.
    block(0, 0, 3 * size_x, size_y)
elif mode == "small":
    # The mirror-image defect: a canvasWidth too LARGE shrinks the marker so it sits
    # wholly inside the rect and still clears the floor. Only an extent test sees it.
    block(0, 0, size_x - 3, size_y - 3)
path = os.path.join(os.environ.get("TMPDIR", "/tmp"), "mdk-harness-cap-%s.png" % mode)
img.save(path)
print(path)
PY
}
CAP_YES="$(cap here)"
CAP_NO="$(cap none)"
CAP_TURN="$(cap turn)"
CAP_FLIP="$(cap flip)"
CAP_ROT180="$(cap rot180)"
CAP_WIDE="$(cap wide)"
CAP_SMALL="$(cap small)"

# THE RECTANGLE IS DERIVED, NOT REMEMBERED. The expectation lives in the harness as a
# pair of integers; the geometry that produces it lives in the shipped renderer. If
# nobody recomputes one from the other, the harness quietly starts certifying a rect
# the renderer no longer draws to -- the same second-source-of-truth failure batch 147
# hit from the other direction (the host test modelled the register as the code
# believed it was, and the two wrongs cancelled). So recompute here from the shipped
# constants and require agreement; a constant this file cannot find is a hard failure,
# never a skipped check.
#
# AND canvasWidth IS IMPORTED FROM mdk_render_canvas_width, NOT RE-SPELLED (review-1
# F1). The previous version restated the formula as `ch*sh/sw` here. That is a SECOND
# SPELLING of the thing it was written to pin: change mdk_render_canvas_width to use
# kScreenWidth where kScreenHeight belongs and the marker moves to 26.7 columns while
# this derivation still prints 10.667 and still agrees with the harness. So the return
# EXPRESSION is now read out of the function and evaluated, and an expression this file
# cannot parse is a hard failure -- the derivation follows the code, or it stops.
derive_rect() { # derive_rect <root> -> "<ceilX> <ceilY> <floorX> <floorY>"
python3 - "$1" <<'PY'
import math, re, sys
root = sys.argv[1]


def read(path):
    try:
        return open(path, encoding="utf-8").read()
    except OSError as exc:
        sys.exit("derive: cannot read %s (%s)" % (path, exc))


def grab(text, pattern, cast, what, path):
    m = re.search(pattern, text)
    if not m:
        sys.exit("derive: %s is not in %s -- the derivation is stale, not the rect" % (what, path))
    return cast(m.group(1))


cpp = root + "/mdk_3ds/src/render/renderer_c3d.cpp"
hdr = root + "/mdk_core/src/render/render_types.h"
ctext, htext = read(cpp), read(hdr)
sw = grab(ctext, r"kScreenWidth\s*=\s*(\d+)\s*;", int, "kScreenWidth", cpp)
sh = grab(ctext, r"kScreenHeight\s*=\s*(\d+)\s*;", int, "kScreenHeight", cpp)
size = grab(ctext, r"kSize\s*=\s*([0-9.]+)f\s*;", float, "drawDiagnostic's kSize", cpp)
ch = grab(htext, r"kCanvasHeight\s*=\s*([0-9.]+)f\s*;", float, "kCanvasHeight", hdr)

# THE IMPORT. mdk_render_canvas_width's own return expression, evaluated against the
# constants it names. Nothing here spells the ratio.
body = re.search(r"mdk_render_canvas_width\s*\([^)]*\)\s*\{(.*?)\n\}", ctext, re.S)
if not body:
    sys.exit("derive: cannot read mdk_render_canvas_width's body in %s -- the "
             "derivation is stale, not the rect" % cpp)
ret = re.search(r"return\s+(.+?);", body.group(1), re.S)
if not ret:
    sys.exit("derive: mdk_render_canvas_width has no return this file can read")
expr = re.sub(r"\(\s*float\s*\)", "", ret.group(1)).replace("mdk::", "")
expr = re.sub(r"//[^\n]*", "", expr)          # the return may carry a trailing comment
for name, val in (("kCanvasHeight", ch), ("kScreenWidth", float(sw)),
                  ("kScreenHeight", float(sh))):
    expr = re.sub(r"\b%s\b" % name, repr(val), expr)
if re.search(r"[A-Za-z_]", expr):
    sys.exit("derive: canvasWidth expression %r names something this derivation "
             "cannot resolve -- re-derive the rect by hand, once, deliberately" % expr)
cw = eval(expr, {"__builtins__": {}}, {})     # noqa: S307 -- digits and operators only

# The marker's far corner, per axis. crop_x = py = x/canvasWidth * kScreenHeight and
# crop_y = 240 - px = y/kCanvasHeight * kScreenWidth, so the two axes are DIFFERENT
# expressions and are asserted separately (review-1 F1): a canvasWidth error moves one
# and not the other, and a single shared `far` would hide that.
far_x = size * sh / cw
far_y = size * sw / ch
print("%d %d %d %d" % (math.ceil(far_x), math.ceil(far_y),
                       math.floor(far_x), math.floor(far_y)))
PY
}
DERIVED="$(derive_rect "$ROOT")"
check "the marker rect is re-derived from the shipped renderer and agrees" \
      "$DERIVED" "$CANARY_X1 $CANARY_Y1 $CANARY_REACH_X $CANARY_REACH_Y"
check "and the derivation is not a constant (it would then certify nothing)" "1" \
      "$(awk '{print ($1 > 0 && $1 < 400 && $3 > 0 && $3 < 400) ? 1 : 0}' <<<$DERIVED)"

# AND THE IMPORT IS PROVEN TO BE AN IMPORT. A derivation that only LOOKS like it reads
# the renderer is the exact failure this block exists to prevent, so mutate a COPY of
# mdk_render_canvas_width -- swap the two screen axes, the very mistake that function's
# own comment warns turns 600 into 216 -- and require the derivation to MOVE. Had this
# file still been spelling `ch*sh/sw` by hand, the mutant would derive the same rect as
# the real tree and this row would pass while certifying nothing.
MUTROOT="$(mktemp -d)"
mkdir -p "$MUTROOT/mdk_3ds/src/render" "$MUTROOT/mdk_core/src/render"
cp "$ROOT/mdk_3ds/src/render/renderer_c3d.cpp" "$MUTROOT/mdk_3ds/src/render/"
cp "$ROOT/mdk_core/src/render/render_types.h" "$MUTROOT/mdk_core/src/render/"
python3 - "$MUTROOT/mdk_3ds/src/render/renderer_c3d.cpp" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
old = "kCanvasHeight * (float)kScreenHeight / (float)kScreenWidth"
if s.count(old) != 1:
    sys.exit("mutate: mdk_render_canvas_width's return is not what this mutation "
             "expects -- update the mutation, do not delete it")
open(p, "w", encoding="utf-8").write(
    s.replace(old, "kCanvasHeight * (float)kScreenWidth / (float)kScreenHeight"))
PY
MUT_DERIVED="$(derive_rect "$MUTROOT")"
rm -rf "$MUTROOT"
check "a mutated mdk_render_canvas_width MOVES the derivation (so it is imported)" "1" \
      "$([ -n "$MUT_DERIVED" ] && [ "$MUT_DERIVED" != "$DERIVED" ] && echo 1 || echo 0)"
check "and it moves the axis a canvasWidth error moves, and only that one" "1" \
      "$(awk -v a="$DERIVED" -v b="$MUT_DERIVED" \
            'BEGIN{split(a,x," ");split(b,y," ");print (y[1]!=x[1] && y[2]==x[2] && y[4]==x[4]) ? 1 : 0}')"

# The lifted block must contain the POSITIONAL half, or every check below is a
# presence test on a fragment that lost it.
check "the lift contains canary_stats, so the rect check cannot be a copy" "1" \
      "$(printf '%s\n' "$EXPECT_FN" | grep -q 'canary_stats()' && echo 1 || echo 0)"

stats_of() { # <shot> -> the harness's own "<total> <inside> <bbox...>" line
    ( eval "$EXPECT_FN"; canary_stats "$1" "$CANARY_X1" "$CANARY_Y1" ) 2>/dev/null
}

expect_rc() { # <log content> <name> [shot] -> the adjudication's exit code
    local content="$1" name="${2:-hud}" shot
    # `"$#"` and not `${3:-}`: an explicitly empty shot is a CASE here ("the run
    # produced no picture"), and `${3:-$CAP_YES}` would silently substitute the
    # good capture for it.
    if [ "$#" -ge 3 ]; then shot="$3"; else shot="$CAP_YES"; fi
    local log; log="$(mktemp)"
    printf '%s\n' "$content" > "$log"
    ( eval "$EXPECT_FN"; expect_check "$name" "$log" "$shot" ) >/dev/null 2>&1
    local rc=$?
    rm -f "$log"
    printf '%s' "$rc"
}

check "hud: a canvas line with quads and draws above zero passes" "0" \
      "$(expect_rc 'canvas: quads=12 draws=12 mesh=3 corners=72 eyes=1 (canvas frame 1)')"
check "hud: no canvas line at all fails" "1" \
      "$(expect_rc 'frame 60 elapsed=1.0000 steps=60')"
check "hud: quads=0 fails -- the frontend asked for nothing" "1" \
      "$(expect_rc 'canvas: quads=0 draws=0 mesh=-1 corners=0 eyes=1 (canvas frame 1)')"
check "hud: queued but never drawn fails (quads 12, draws 0)" "1" \
      "$(expect_rc 'canvas: quads=12 draws=0 mesh=3 corners=72 eyes=1 (canvas frame 1)')"
check "hud: a canvas line with no numbers fails, not a silent pass" "1" \
      "$(expect_rc 'canvas: drew something')"
check "hud: a later frame's line counts, so one good frame is enough" "0" \
      "$(expect_rc 'frame 1 elapsed=0.0166 steps=1
canvas: quads=4 draws=4 mesh=3 corners=24 eyes=1 (canvas frame 1)')"
check "hud: an unrelated line sharing the 'canvas: ' prefix does not shadow the evidence" "0" \
      "$(expect_rc 'canvas: stream mesh 384 corners = 7 KB linear
canvas: quads=12 draws=12 mesh=3 corners=72 eyes=1 (canvas frame 1)')"
check "hud: and the mesh note on its own is NOT the evidence" "1" \
      "$(expect_rc 'canvas: stream mesh 384 corners = 7 KB linear')"
check "hud: the LAST canvas line decides, so a HUD that stopped fails" "1" \
      "$(expect_rc 'canvas: quads=12 draws=12 mesh=3 corners=72 eyes=1 (canvas frame 1)
canvas: quads=0 draws=0 mesh=3 corners=0 eyes=1 (canvas frame 61)')"
check "an unknown expectation is a usage error (2), not a failed run (7)" "2" \
      "$(expect_rc 'canvas: quads=12 draws=12' menu)"

# THE HOLE review-1 F1 was about: the emitted line is green while nothing paints.
# These two rows are the difference between an acceptance that means "the HUD is on
# screen" and one that means "the frontend asked for a HUD".
check "hud: emitted with no canary pixels FAILS -- the review-1 F1 hole" "1" \
      "$(expect_rc 'canvas: quads=12 draws=12 mesh=3 corners=72 eyes=1 (canvas frame 1)' hud "$CAP_NO")"
check "hud-visible: the canary alone is enough (no log line needed)" "0" \
      "$(expect_rc 'frame 60 elapsed=1.0000 steps=60' hud-visible "$CAP_YES")"
check "hud-visible: no canary fails" "1" \
      "$(expect_rc 'canvas: quads=12 draws=12 mesh=3 corners=72 eyes=1 (canvas frame 1)' hud-visible "$CAP_NO")"
check "hud-visible: with no screenshot at all it cannot claim a verdict" "1" \
      "$(expect_rc 'canvas: quads=12 draws=12 mesh=3 corners=72 eyes=1 (canvas frame 1)' hud-visible "")"

# --- the SIGN of the canvas transform, and its EXTENT (reviews 4 F1 and 1 F1) -------
#
# pica::canvasMatrix turns the landscape canvas into the portrait target with
# out col0 = m col1, out col1 = -m col0. Flip either sign and the determinant flips:
# the marker still paints -- same size, same colour, same pixel count -- it just paints
# flush against a different edge. `--expect hud` was green over that build because the
# only pixel question it asked was "how many". And a rect tested only from the inside
# has the same blind spot one size larger: a marker that spills past the rect still
# puts plenty of pixels inside it.
#
# THE FIRST ROWS OF EACH GROUP ARE THE NON-VACUITY: they say the bad capture would have
# satisfied the rule it replaces, so every rejection below is the new rule's work and
# not the capture being bad in some other way.

# Non-vacuity for review-4 F1: a wrong-corner marker that a COUNT alone would pass.
TURN_STATS="$(stats_of "$CAP_TURN")"
check "the turned capture carries a full marker, so a count alone would pass it" "1" \
      "$(awk -v min="$CANARY_MIN" 'NF && $1 ~ /^[0-9]+$/ && $1 >= min {print 1}' <<<"$TURN_STATS")"
check "and it carries NONE of its pixels inside the marker's rect" "0" \
      "$(awk '{print $2}' <<<"$TURN_STATS")"
check "and its pixels are flush against the far edge, not merely scattered" "1" \
      "$(awk '{print ($3 > 200) ? 1 : 0}' <<<"$TURN_STATS")"

# All four dispositions of the quarter turn, each built from its own (a,b) sign pair.
for d in turn flip rot180; do
    case "$d" in
        turn) shot="$CAP_TURN" ;;
        flip) shot="$CAP_FLIP" ;;
        rot180) shot="$CAP_ROT180" ;;
    esac
    check "hud: the $d disposition of the panel turn fails -- review-4 F1" "1" \
          "$(expect_rc 'canvas: quads=12 draws=12 mesh=3 corners=72 eyes=1 (canvas frame 1)' hud "$shot")"
    check "hud-visible: and $d fails the pixel verdict on its own too" "1" \
          "$(expect_rc 'frame 60 elapsed=1.0000 steps=60' hud-visible "$shot")"
done

# Non-vacuity for review-1 F1: the wide marker really does clear the floor INSIDE the
# rect, which is exactly why the inside-count rule let it through.
WIDE_STATS="$(stats_of "$CAP_WIDE")"
check "the wide marker clears the floor INSIDE the rect (so an inside-count passes it)" "1" \
      "$(awk -v min="$CANARY_MIN" 'NF && $2 ~ /^[0-9]+$/ && $2 >= min {print 1}' <<<"$WIDE_STATS")"
check "and it is genuinely wider than the rect it is judged against" "1" \
      "$(awk -v x1="$CANARY_X1" 'NF && $5 > x1 {print 1}' <<<"$WIDE_STATS")"
check "hud: a marker that spills outside its rect fails -- review-1 F1" "1" \
      "$(expect_rc 'canvas: quads=12 draws=12 mesh=3 corners=72 eyes=1 (canvas frame 1)' hud "$CAP_WIDE")"

# The mirror-image extent defect: wholly inside the rect, and too small to be the
# marker the shipped constants produce.
SMALL_STATS="$(stats_of "$CAP_SMALL")"
check "the small marker sits wholly inside the rect and clears the floor" "1" \
      "$(awk -v min="$CANARY_MIN" 'NF && $2 == $1 && $1 >= min {print 1}' <<<"$SMALL_STATS")"
check "and it stops short of the derived reach" "1" \
      "$(awk -v r="$CANARY_REACH_X" 'NF && $5 < r {print 1}' <<<"$SMALL_STATS")"
check "hud: a marker that stops short of its derived reach fails -- review-1 F1" "1" \
      "$(expect_rc 'canvas: quads=12 draws=12 mesh=3 corners=72 eyes=1 (canvas frame 1)' hud "$CAP_SMALL")"

# The rule must not be always-red, and a failure must say where.
check "hud: the marker in its own rect passes, so the rule is not just always-red" "0" \
      "$(expect_rc 'canvas: quads=12 draws=12 mesh=3 corners=72 eyes=1 (canvas frame 1)' hud "$CAP_YES")"
check "and the shipped disposition's bbox is exactly the derived rect" "1" \
      "$(stats_of "$CAP_YES" | awk -v x1="$CANARY_X1" -v y1="$CANARY_Y1" \
              -v rx="$CANARY_REACH_X" -v ry="$CANARY_REACH_Y" \
              'NF == 6 && $3 == 0 && $4 == 0 && $5 == rx && $6 == ry && rx <= x1 && ry <= y1 {print 1}')"
check "and the stats line names the bbox, so a failure says WHERE" "1" \
      "$(awk 'NF == 6 && $3 != "-" {print 1}' <<<"$TURN_STATS")"

# --- review-1 F5: malformed evidence is a FAILED verdict, never a green one ---------
#
# The guard used to test the CONCATENATION of two fields, so a stats line whose first
# field was blank read as "all digits", both comparisons then died on "integer
# expression expected", no branch was taken, and the function FELL THROUGH TO `HUD
# VISIBLE`. These rows stub canary_stats, because the point is the guard, not the
# Python that today always prints six fields.
for bad in " 121 0 0 10 10" "" "x y - - - -" "121 121 0 0 10"; do
    check "verdict: a malformed stats line [$bad] is refused, not certified" "1" \
          "$( ( eval "$EXPECT_FN"; canary_stats() { printf '%s\n' "$bad"; }
               canary_verdict "$CAP_YES" ) >/dev/null 2>&1; printf '%s' "$?")"
done

if [ "$fails" -eq 0 ]; then
    echo "PASS EmurunHarness ($checks checks: config placement + mount ownership + the HUD canary)"
    exit 0
fi
echo "EMURUN HARNESS: $fails FAILED"
exit 1

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

if [ "$fails" -eq 0 ]; then
    echo "PASS EmurunHarness ($checks checks: config placement + mount ownership)"
    exit 0
fi
echo "EMURUN HARNESS: $fails FAILED"
exit 1

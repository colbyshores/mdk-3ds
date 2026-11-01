#!/usr/bin/env bash
# test_review_state.sh — assertions on the loop's review-record state machine.
#
#   bash tools/runner/test_review_state.sh
#
# Each case below is either a failure that actually happened in this loop
# (external review 2026-11-01: review-2.md F1/F2/F5, review-1.md H1/M1/L1) or an
# anticipated misconfiguration asserted against aliases that exist on THIS proxy
# (the independence cases — see their own header). The point is that the three
# bad outcomes — "the reviewer died", "no reviewer could be dispatched" and "the
# loop was killed before it could collect the review" — must never reach the same
# record as "the diff was reviewed".
#
# NOT WIRED INTO tools/check.sh: check.sh is owner-protected and adding a gate
# line is an owner-authorized change (docs/CONTINUATION.md, owner-authorized
# tasks). Run it with the gates by hand until then.
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
# shellcheck source=review_state.sh
. "$ROOT/tools/runner/review_state.sh"

fails=0
LOOP="$ROOT/ralph-pi.sh"
IDENT_LOG="$(mktemp)"
# Match CODE, not prose: ralph-pi.sh documents its own bugs in comments to stop
# them being "tidied" back, and a naive grep reads that documentation as code
# (the same trap audit_c_with_classes.py had to solve).
LOOP_CODE="$(mktemp)"
grep -vE '^[[:space:]]*#' "$LOOP" >"$LOOP_CODE"
REPORT=$(mktemp -d)/report.md
printf 'VERDICT: PASS\n' >"$REPORT"
: >"$REPORT.empty"
cleanup() { rm -rf "$(dirname "$REPORT")" "$IDENT_LOG" "$LOOP_CODE" "${SD:-}" 2>/dev/null || true; }
trap cleanup EXIT
check() { # check <label> <expected> <actual>
    if [ "$2" = "$3" ]; then
        printf 'PASS %s\n' "$1"
    else
        printf 'FAIL %s — expected %s, got %s\n' "$1" "'$2'" "'$3'"
        fails=$((fails + 1))
    fi
}

# --- review_rc_normalise ------------------------------------------------------
check "rc 0 stays 0"                 "0"   "$(review_rc_normalise 0)"
check "rc 124 (timeout) stays 124"   "124" "$(review_rc_normalise 124)"
check "an empty .rc is a failure"    "1"   "$(review_rc_normalise '')"
check "a corrupt .rc is a failure"   "1"   "$(review_rc_normalise 'not-a-number')"
check "a truncated .rc is a failure" "1"   "$(review_rc_normalise '12x')"

# --- review_outcome: F1's exact scenario -------------------------------------
# The reviewer was dispatched, timed out (rc 124) and left a partial report that
# still had text in it. The old code read that as success and wrote
# REVIEW_STATUS=done.
check "timeout with a partial report is FAILED" "failed" "$(review_outcome 124 "$REPORT")"
check "empty rc with a full report is FAILED"   "failed" "$(review_outcome '' "$REPORT")"
check "rc 0 with no report at all is FAILED"    "failed" "$(review_outcome 0 "$REPORT.empty")"
check "rc 0 with a missing report is FAILED"    "failed" "$(review_outcome 0 "$REPORT.nope")"
check "rc 0 with a report is DONE"              "done"   "$(review_outcome 0 "$REPORT")"
check "an unusable rc cannot be talked into DONE" "failed" "$(review_outcome 'x' "$REPORT")"

# --- review_int: a counter that comes back empty must not disable a limit ------
# The shipped guard is `[ "$REVIEW_FAILS" -ge "$REVIEW_FAIL_LIMIT" ]`, and `[` on
# an empty operand errors with status 2, which `&&` reads as FALSE. A state file
# written before these keys existed therefore made the reviewer-failure limit
# silently never fire — F1's shape again, one variable over.
check "an absent counter is 0, not empty"        "0" "$(review_int '')"
check "a corrupt counter is 0, not the garbage"  "0" "$(review_int 'abc')"
check "a real counter survives"                  "7" "$(review_int 7)"
check "a zero counter stays zero (not defaulted)" "0" "$(review_int 0)"
check "an explicit default is honoured"          "3" "$(review_int '' 3)"
# The exact bug, spelled out: the old line tested the DEFAULTED value and assigned
# to the original, so the default never landed.
old_shape() { local v=""; case "${v:-0}" in ''|*[!0-9]*) v=0 ;; esac; printf '%s\n' "$v"; }
check "the old case-with-:-0 shape left it EMPTY (why review_int exists)" "" "$(old_shape)"
check "review_int fixes exactly that case"       "0" "$(review_int '')"

# --- review_owed: F2's exact scenario ----------------------------------------
# A blocked or failed review must leave the unreviewed commit behind, and only a
# completed review may clear it.
check "a completed review owes nothing"          ""      "$(review_owed done '' 1111)"
check "a blocked review owes its own base"       "1111"  "$(review_owed blocked '' 1111)"
check "a failed review owes its own base"        "1111"  "$(review_owed failed '' 1111)"
check "a second failure keeps the EARLIEST base" "1111"  "$(review_owed failed 1111 2222)"
check "the owed base survives a restart"         "1111"  "$(review_owed failed "$(review_owed failed '' 1111)" 2222)"
check "and a later success clears it"            ""      "$(review_owed done 1111 2222)"

# --- review_inflight_owed: H1's exact scenario --------------------------------
# The reviewer was dispatched, the loop was killed before its collector ran (the
# gates phase sits between dispatch and collection), and the state file still
# says `invoked`. The obligation must survive the process that was supposed to
# record it, so the range is persisted at dispatch and folded in here at startup.
check "nothing in flight changes nothing"          ""      "$(review_inflight_owed '' '')"
check "an uncollected review owes its own base"   "1111"  "$(review_inflight_owed '' 1111)"
check "it keeps the EARLIEST base, like a failure" "1111" "$(review_inflight_owed 1111 2222)"
check "an owed base with nothing in flight survives" "1111" "$(review_inflight_owed 1111 '')"
check "and the next completed review still clears it" "" "$(review_owed done "$(review_inflight_owed '' 1111)" 2222)"

# --- the independence guard (F3) ---------------------------------------------
# reviewer_reject_reason lives in ralph-pi.sh, which cannot be sourced without
# starting the loop, so it is lifted out verbatim and run against the same
# resolver the loop uses. Every case below is a pair of aliases on THIS proxy,
# checked with tools/runner/model_identity.py -- not a made-up name.
eval "$(awk '/^reviewer_reject_reason\(\) \{/,/^\}/' "$ROOT/ralph-pi.sh")"
eval "$(grep -E '^model_(upstream|family)\(\) ' "$ROOT/ralph-pi.sh")"
if ! declare -f reviewer_reject_reason model_upstream model_family >/dev/null; then
    echo "FAIL reviewer_reject_reason could not be lifted out of ralph-pi.sh"
    fails=$((fails + 1))
else
    identity() { python3 "$ROOT/tools/runner/model_identity.py" "$1" "$2" 2>/dev/null; }
    set_builder() { RUN_MODEL="$1"; RUN_UPSTREAM="$(identity upstream "$1")"; RUN_FAMILY="$(identity family "$1")"; }
    probe() { # probe <builder> <candidate> -> ACCEPT or the printed reason
        set_builder "$1"
        local reason; reason="$(reviewer_reject_reason "$2")"
        [ -z "$reason" ] && printf 'ACCEPT\n' || printf 'REJECT\n'
    }
    stale_probe() { # stale_probe <abandoned-route> <route-that-answered> <candidate>
        # Reproduces what the loop did BEFORE the M1 fix: RUN_MODEL is the route
        # that answered, but the identity still belongs to the route that was
        # abandoned. F6's point is that this, not `probe`, is the case that could
        # fail, so it is the case that gets asserted.
        RUN_MODEL="$2"
        RUN_UPSTREAM="$(identity upstream "$1")"
        RUN_FAMILY="$(identity family "$1")"
        local reason; reason="$(reviewer_reject_reason "$3")"
        [ -z "$reason" ] && printf 'ACCEPT\n' || printf 'REJECT\n'
    }
    check "qwen builder, deepseek reviewer: accepted"        "ACCEPT" "$(probe litellm/qwen litellm/deepseek-v4.1-flash)"
    check "same alias twice: refused"                       "REJECT" "$(probe litellm/qwen litellm/qwen)"
    check "qwen and swarm-worker are one upstream: refused" "REJECT" "$(probe litellm/qwen litellm/swarm-worker)"
    check "luna and luna-compaction: one upstream, two aliases, refused" \
          "REJECT" "$(probe litellm/luna litellm/luna-compaction)"
    check "the two DeepSeek hosts are one family: refused"  "REJECT" \
          "$(probe litellm/deepseek-v4.1-flash litellm/deepseek-v4.1-flash-nvidia)"
    check "an alias nobody can resolve is refused"          "REJECT" "$(probe litellm/qwen litellm/not-a-model)"
    check "an unresolvable BUILDER refuses every reviewer"  "REJECT" "$(probe litellm/not-a-model litellm/deepseek-v4.1-flash)"
    check "qwen builder, minimax reviewer: accepted"        "ACCEPT" "$(probe litellm/qwen litellm/minimax-m3.1-flash-nvidia)"
    # M1: when the WORKER falls back to the DeepSeek seat, that is the route that
    # wrote the batch, and the primary reviewer is its own family. NOTE (F6): this
    # case hands the guard a CORRECT identity, so it passes on the pre-fix loop
    # too — it pins the guard's answer, not the loop's re-resolution. The pin for
    # the re-resolution is the pair below plus the structural count further down.
    check "M1: a builder that fell back to DeepSeek cannot have a DeepSeek reviewer" \
          "REJECT" "$(probe litellm/deepseek-v4.1-flash-nvidia litellm/deepseek-v4.1-flash)"
    # F6: the behavioural half. Same builder route, same reviewer, judged twice.
    # With the ABANDONED route's family still in RUN_FAMILY the guard reads ACCEPT
    # — that is the self-review the fix exists to stop, and it is what the pre-fix
    # loop produced. Re-resolved, it refuses. If someone re-implements the fallback
    # so that one identity field stays stale, this pair fails.
    check "F6: with the abandoned route's identity the same reviewer would be ACCEPTED" \
          "ACCEPT" "$(stale_probe litellm/qwen litellm/deepseek-v4.1-flash-nvidia litellm/deepseek-v4.1-flash)"
    check "F6: re-resolved to the route that answered, it is REFUSED" \
          "REJECT" "$(probe litellm/deepseek-v4.1-flash-nvidia litellm/deepseek-v4.1-flash)"
    # L3: the contract is in the name AND in the status. The answer is the text;
    # the status is 0 on both arms, so a future caller cannot branch on it and
    # silently refuse (or accept) every candidate.
    check "L3: exits 0 when it ACCEPTS a reviewer"  "0" "$(set_builder litellm/qwen; reviewer_reject_reason litellm/deepseek-v4.1-flash >/dev/null; echo $?)"
    check "L3: exits 0 when it REFUSES (the answer is in the text)" \
          "0" "$(set_builder litellm/qwen; reviewer_reject_reason litellm/qwen >/dev/null; echo $?)"
fi

# --- H1 end to end: the shipped startup block, lifted and run -------------------
# The cases above prove the arithmetic; these prove the WIRING — that the startup
# really folds an uncollected review into the owed set, counts it, clears the
# in-flight marker, and refuses when it cannot reconstruct the obligation. The
# block is lifted out of ralph-pi.sh rather than retyped here, so a rewrite that
# drops the call fails (the same trick the independence guard above uses).
STARTUP_BLOCK="$(awk '/^if \[ -n "\$REVIEW_INFLIGHT_BASE" \]; then$/{f=1} f{print} f&&/^fi$/{exit}' "$LOOP")"
if [ -z "$STARTUP_BLOCK" ]; then
    check "H1: the startup block could be lifted from ralph-pi.sh" "lifted" "NOT FOUND"
else
    SD="$(mktemp -d)"
    # The loop's own state primitives, pointed at a scratch file.
    state_get() { sed -n "s/^$1=//p" "$SD/state" 2>/dev/null | tail -1; }
    state_set() { local kv="$1=$2" k="${1%%=*}"; grep -v "^$k=" "$SD/state" 2>/dev/null >"$SD/state.t" || true; echo "$kv" >>"$SD/state.t"; mv "$SD/state.t" "$SD/state"; }
    log() { printf '%s\n' "$*" >>"$SD/log"; }
    run_startup() { # run_startup <state-file-body> -> "rc|owed|fails"
        printf '%b' "$1" >"$SD/state"; : >"$SD/log"
        REVIEW_OWED_BASE="$(state_get REVIEW_OWED_BASE)"
        REVIEW_FAILS="$(review_int "$(state_get REVIEW_FAILS)")"
        REVIEW_INFLIGHT_BASE="$(state_get REVIEW_INFLIGHT_BASE)"
        REVIEW_INFLIGHT_HEAD="$(state_get REVIEW_INFLIGHT_HEAD)"
        REVIEW_LEFTOVER="$(state_get REVIEW_STATUS)"
        REVIEW_FAIL_LIMIT=2
        STATE="$SD/state"   # named in the refusal message; set -u would catch a rename
        local rc
        ( eval "$STARTUP_BLOCK" ); rc=$?
        printf '%s|%s|%s\n' "$rc" "$(state_get REVIEW_OWED_BASE)" "$(state_get REVIEW_FAILS)"
    }
    check "H1: a clean restart owes nothing"                 "0||0" \
          "$(run_startup 'REVIEW_STATUS=done\nREVIEW_FAILS=0\nREVIEW_OWED_BASE=\nREVIEW_INFLIGHT_BASE=\n')"
    check "H1: an uncollected review is owed at startup"      "0|aaaa1111|1" \
          "$(run_startup 'REVIEW_STATUS=invoked\nREVIEW_FAILS=0\nREVIEW_OWED_BASE=\nREVIEW_INFLIGHT_BASE=aaaa1111\n')"
    run_startup 'REVIEW_STATUS=invoked\nREVIEW_FAILS=0\nREVIEW_OWED_BASE=\nREVIEW_INFLIGHT_BASE=aaaa1111\n' >/dev/null
    check "H1: and the in-flight marker is cleared, not left to fire twice" "" "$(state_get REVIEW_INFLIGHT_BASE)"
    # F2: the fold used to rewrite everything EXCEPT the status, so it left the
    # file saying `invoked` with nothing in flight — the state the refusal below
    # exists for. A second kill in the long window before the end-of-iteration
    # rewrite then turned a recorded obligation into a halt the loop could not
    # restart from. The fold must land on a state the state machine accepts.
    check "F2: the fold does not leave the state its own refusal rejects" "failed" "$(state_get REVIEW_STATUS)"
    check "F2: a second start on the state the fold produced continues" "0|aaaa1111|1" \
          "$(run_startup "$(cat "$SD/state")")"
    check "H1: a second lost review stops the loop"           "7|bbbb2222|2" \
          "$(run_startup 'REVIEW_STATUS=invoked\nREVIEW_FAILS=1\nREVIEW_OWED_BASE=\nREVIEW_INFLIGHT_BASE=bbbb2222\n')"
    check "H1: an owed base is not overwritten by a newer loss" "0|aaaa1111|1" \
          "$(run_startup 'REVIEW_STATUS=skipped\nREVIEW_FAILS=0\nREVIEW_OWED_BASE=aaaa1111\nREVIEW_INFLIGHT_BASE=bbbb2222\n')"
    # F1: `invoked` with no range but WITH an owed base is not an unreconstructible
    # obligation — the owed base is the obligation, and it is already on disk. The
    # refusal used to fire regardless, which halted the loop on the very state the
    # fold above produces, and its printed repair ("delete $STATE") discards the
    # obligation. That is a fail-open escape hatch inside a fail-closed check.
    check "F1: invoked with an owed base continues and still owes it" "0|aaaa1111|0" \
          "$(run_startup 'REVIEW_STATUS=invoked\nREVIEW_FAILS=0\nREVIEW_OWED_BASE=aaaa1111\nREVIEW_INFLIGHT_BASE=\n')"
    check "F1: and the stale invoked marker is rewritten to the truth" "failed" "$(state_get REVIEW_STATUS)"
    # F3: with nothing owed and no range, the HEAD line is a SOUND base, not a
    # guess. It is written at the END of an iteration, so it names the previous
    # iteration's commit — the base that iteration dispatched from, and therefore
    # an ancestor of the unreviewed commit. The range it builds over-covers, which
    # is the trade the in-flight fold already accepts; the old comment claimed the
    # opposite and the branch halted for hand surgery on that false premise.
    REAL_HEAD_SHORT="$(git -C "$ROOT" rev-parse --short HEAD)"
    REAL_HEAD_FULL="$(git -C "$ROOT" rev-parse HEAD)"
    check "F3: invoked with no range reconstructs the obligation from HEAD" "0|$REAL_HEAD_FULL|1" \
          "$(run_startup "REVIEW_STATUS=invoked\nREVIEW_FAILS=0\nREVIEW_OWED_BASE=\nREVIEW_INFLIGHT_BASE=\nHEAD=$REAL_HEAD_SHORT\n")"
    check "F3: the reconstructed state is coherent, not 'invoked'" "failed" "$(state_get REVIEW_STATUS)"
    check "F3: a HEAD line that names no commit is refused"    "7||0" \
          "$(run_startup 'REVIEW_STATUS=invoked\nREVIEW_FAILS=0\nREVIEW_OWED_BASE=\nREVIEW_INFLIGHT_BASE=\nHEAD=not-a-commit\n')"
    check "H1: invoked with no range AND no HEAD line is the only refusal" "7||0" \
          "$(run_startup 'REVIEW_STATUS=invoked\nREVIEW_FAILS=0\nREVIEW_OWED_BASE=\nREVIEW_INFLIGHT_BASE=\n')"
fi

# --- the resolver's own claims -----------------------------------------------
# reviewer_rejected is only as good as the table it reads, and the table is a
# transcription of what the proxy said. Assert the resolver's claims here too, so
# a hand-edited STATIC_UPSTREAMS cannot keep the suite green.
if python3 "$ROOT/tools/runner/model_identity.py" selftest >"$IDENT_LOG" 2>&1; then
    check "model_identity.py selftest is clean" "0" "0"
else
    check "model_identity.py selftest is clean" "0" "1"
    sed 's/^/    /' "$IDENT_LOG"
fi

# --- decay guards on ralph-pi.sh ---------------------------------------------
# These are the exact spellings the findings were. A rewrite that reintroduces
# one re-opens the finding, and no behavioural test above would notice, because
# they are all about a file the test cannot execute.
guard_absent() { # guard_absent <label> <fixed-string> -- the string must be GONE
    if grep -qF "$2" "$LOOP_CODE"; then
        printf 'FAIL %s — %s is back in ralph-pi.sh\n' "$1" "$2"
        fails=$((fails + 1))
    else
        printf 'PASS %s\n' "$1"
    fi
}
guard_present() { # guard_present <label> <fixed-string> -- the string must be THERE
    if grep -qF "$2" "$LOOP_CODE"; then
        printf 'PASS %s\n' "$1"
    else
        printf 'FAIL %s — %s is missing from ralph-pi.sh\n' "$1" "$2"
        fails=$((fails + 1))
    fi
}
# Presence cannot see an ORDER, and a guard helper that is called before it is
# defined fails with "command not found" on stderr WITHOUT incrementing `fails` —
# a guard that silently does not run, which is the exact defect this suite exists
# to catch. So it is defined here, before any call, and a missing helper is a
# counted failure rather than a line on stderr.
order_before() { # order_before <label> <earlier fixed string> <later fixed string>
    local a b
    a="$(grep -nF "$2" "$LOOP_CODE" | head -1 | cut -d: -f1)"
    b="$(grep -nF "$3" "$LOOP_CODE" | head -1 | cut -d: -f1)"
    if [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]; then
        printf 'PASS %s\n' "$1"
    else
        printf 'FAIL %s — %s (line %s) is not before %s (line %s)\n' "$1" "$2" "${a:-none}" "$3" "${b:-none}"
        fails=$((fails + 1))
    fi
}
# A whole-file `grep` cannot see WHICH arm a line belongs to, and that is not a
# pedantic objection: `state_set REVIEW_STATUS "failed"` now appears in two arms
# (the in-flight fold and the owed-base arm), so a file-wide presence/order guard
# on it stays green when the fold's copy is deleted. That is a pin that does not
# pin, which is the defect this suite exists to catch. So the fold is lifted as its
# own body and the F2 guards are asserted INSIDE it.
fold_body() { # the in-flight fold arm: owed computation .. in-flight clear
    awk '/review_inflight_owed/,/REVIEW_INFLIGHT_HEAD=""/' "$LOOP_CODE"
}
FOLD_LINES="$(fold_body | wc -l)"
if [ "$FOLD_LINES" -gt 5 ]; then
    printf 'PASS the fold body was lifted (%s lines), so its guards are not vacuous\n' "$FOLD_LINES"
else
    printf 'FAIL the fold body lifted %s lines — every scoped F2 guard would pass vacuously\n' "$FOLD_LINES"
    fails=$((fails + 1))
fi
guard_in_fold() { # guard_in_fold <label> <fixed-string> -- must be inside the fold
    if fold_body | grep -qF "$2"; then
        printf 'PASS %s\n' "$1"
    else
        printf 'FAIL %s — %s is missing from the in-flight fold\n' "$1" "$2"
        fails=$((fails + 1))
    fi
}
order_in_fold() { # order_in_fold <label> <earlier> <later> -- order inside the fold
    local a b
    a="$(fold_body | grep -nF "$2" | head -1 | cut -d: -f1)"
    b="$(fold_body | grep -nF "$3" | head -1 | cut -d: -f1)"
    if [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]; then
        printf 'PASS %s\n' "$1"
    else
        printf 'FAIL %s — %s (line %s) is not before %s (line %s) inside the fold\n' "$1" "$2" "${a:-none}" "$3" "${b:-none}"
        fails=$((fails + 1))
    fi
}
for helper in guard_absent guard_present order_before guard_in_fold order_in_fold; do
    if declare -f "$helper" >/dev/null; then
        printf 'PASS guard helper %s is defined before it is called\n' "$helper"
    else
        printf 'FAIL guard helper %s is not defined — every guard that uses it silently did not run\n' "$helper"
        fails=$((fails + 1))
    fi
done
# F1: the non-numeric-rc comparison, and the unconditional all-clear.
guard_absent "F1: the raw numeric test on \$RRC is gone"        '[ "$RRC" -ne 0 ]'
guard_present "F1: the outcome comes from review_outcome"      'OUTCOME="$(review_outcome "$RRC" "$RUN_DIR/review-$ITER.md")'
guard_present "F1: REVIEW_STATUS records the outcome, not 'done" 'state_set REVIEW_STATUS "$OUTCOME"'
guard_absent  "F1: no unconditional REVIEW_STATUS=done"        'state_set REVIEW_STATUS "done"'
# F2: the obligation survives the process, and the owed base reaches the record.
guard_present "F2: REVIEW_FAILS is read back at startup"       'REVIEW_FAILS="$(review_int "$(state_get REVIEW_FAILS)")"'
guard_present "F2: the owed base is read back at startup"      'REVIEW_OWED_BASE="$(state_get REVIEW_OWED_BASE)"'
guard_present "F2: both reach the persisted state file"        'echo "REVIEW_FAILS=$REVIEW_FAILS"; echo "REVIEW_OWED_BASE=$REVIEW_OWED_BASE"'
guard_present "F2: the counter is read through review_int"     'review_int "$(state_get REVIEW_FAILS)"'
guard_absent  "F2: no case-with-:-0 that tests the wrong value" 'case "${REVIEW_FAILS:-0}" in'
# F3: independence is resolved, not string-compared.
guard_present "F3: the reviewer chain asks the reject-reason guard" 'REJECT="$(reviewer_reject_reason "$REVIEWER_MODEL")"'
guard_present "F3: the fallback chain asks it too"             'REJECT="$(reviewer_reject_reason "$candidate")"'
guard_present "F3: the identity of both sides is recorded"     'review-$ITER.identity.txt'
# H1: the obligation is written where it cannot be lost with the collector.
guard_present "H1: the range is persisted at dispatch"          'state_set REVIEW_INFLIGHT_BASE "$REVIEW_INFLIGHT_BASE"'
guard_present "H1: startup folds an uncollected review into owed" 'REVIEW_OWED_BASE="$(review_inflight_owed "$REVIEW_OWED_BASE" "$REVIEW_INFLIGHT_BASE")"'
guard_present "H1: the in-flight range reaches the state file"  'echo "REVIEW_INFLIGHT_BASE=$REVIEW_INFLIGHT_BASE"'
guard_present "H1: an invoked review with no range is refused, not guessed" 'so the obligation cannot be reconstructed at all'
# F2: the fold rewrites the status as well as the range, so it cannot manufacture
# the `invoked`-with-no-range state its own refusal rejects. Presence alone is not
# enough here (the F1 arm writes the same line), so the ORDER inside the fold is
# pinned: the status must be rewritten before the in-flight markers go, because a
# kill between the two must land on `failed`, never on `invoked` with no range.
guard_in_fold "F2: the fold rewrites REVIEW_STATUS, not only the range" 'state_set REVIEW_STATUS "failed"'
order_in_fold "F2: and it rewrites the status BEFORE clearing the in-flight range" \
             'state_set REVIEW_STATUS "failed"' 'state_set REVIEW_INFLIGHT_BASE ""'
# F4: at dispatch the RANGE is persisted before the status derived from it, so a
# kill between the two leaves an obligation, not a refusal.
guard_present "F4: the range is still persisted at dispatch" 'state_set REVIEW_INFLIGHT_HEAD "$REVIEW_INFLIGHT_HEAD"'
order_before "F4: the in-flight range is written before the invoked marker" \
             'state_set REVIEW_INFLIGHT_BASE "$REVIEW_INFLIGHT_BASE"' 'state_set REVIEW_STATUS "invoked"'
# M1: a builder that fell back is judged by the route that actually answered.
guard_present "M1: the fallback re-resolves the family too"     'RUN_FAMILY="$(model_family "$RUN_MODEL")"'
IDENTITY_READS="$(grep -cF 'RUN_UPSTREAM="$(model_upstream "$RUN_MODEL")"' "$LOOP_CODE")"
check "M1: identity is resolved on the first route AND the fallback (2 reads)" "2" "$IDENTITY_READS"
# L3: the guard is named for what it returns, so a predicate-shaped call is a
# compile-time-looking mistake instead of a silent always-true.
guard_absent  "L3: the predicate-shaped name is gone"           'REJECT="$(reviewer_rejected '
guard_present "L3: the name says it returns a reason"           'reviewer_reject_reason'

if [ "$fails" -eq 0 ]; then
    echo "REVIEW STATE: ALL CASES PASS"
    exit 0
fi
echo "REVIEW STATE: $fails FAILED"
exit 1
#!/usr/bin/env bash
# test_review_state.sh — assertions on the loop's review-record state machine.
#
#   bash tools/runner/test_review_state.sh
#
# Each case below is a failure that actually happened in the loop, taken from
# external review 2026-11-01 (review-2.md F1/F2), not a hypothetical. The point
# is that the two bad outcomes — "the reviewer died" and "no reviewer could be
# dispatched" — must never reach the same record as "the diff was reviewed".
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
cleanup() { rm -rf "$(dirname "$REPORT")" "$IDENT_LOG" "$LOOP_CODE" 2>/dev/null || true; }
trap cleanup EXIT
check() { # check <label> <expected> <actual>
    if [ "$2" = "$3" ]; then
        printf 'PASS %s\n' "$1"
    else
        printf 'FAIL %s — expected %s, got %s\n' "$1" "'$2'" "'$3'"
        fails=$((fails + 1))
    fi
}

# --- review_rc_normalise -----------------------------------------------------check "rc 0 stays 0"                 "0"   "$(review_rc_normalise 0)"
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

# --- the independence guard (F3) ---------------------------------------------
# reviewer_rejected lives in ralph-pi.sh, which cannot be sourced without
# starting the loop, so it is lifted out verbatim and run against the same
# resolver the loop uses. Every case below is a pair of aliases on THIS proxy,
# checked with tools/runner/model_identity.py -- not a made-up name.
eval "$(awk '/^reviewer_rejected\(\) \{/,/^\}/' "$ROOT/ralph-pi.sh")"
eval "$(grep -E '^model_(upstream|family)\(\) ' "$ROOT/ralph-pi.sh")"
if ! declare -f reviewer_rejected model_upstream model_family >/dev/null; then
    echo "FAIL reviewer_rejected could not be lifted out of ralph-pi.sh"
    fails=$((fails + 1))
else
    identity() { python3 "$ROOT/tools/runner/model_identity.py" "$1" "$2" 2>/dev/null; }
    set_builder() { RUN_MODEL="$1"; RUN_UPSTREAM="$(identity upstream "$1")"; RUN_FAMILY="$(identity family "$1")"; }
    probe() { # probe <builder> <candidate> -> ACCEPT or the printed reason
        set_builder "$1"
        local reason; reason="$(reviewer_rejected "$2")"
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
guard_present "F3: the reviewer chain asks reviewer_rejected"  'REJECT="$(reviewer_rejected "$REVIEWER_MODEL")"'
guard_present "F3: the fallback chain asks it too"             'REJECT="$(reviewer_rejected "$candidate")"'
guard_present "F3: the identity of both sides is recorded"     'review-$ITER.identity.txt'

if [ "$fails" -eq 0 ]; then
    echo "REVIEW STATE: ALL CASES PASS"
    exit 0
fi
echo "REVIEW STATE: $fails FAILED"
exit 1
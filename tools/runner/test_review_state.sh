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
cleanup() { rm -rf "$(dirname "$REPORT")" "$IDENT_LOG" "$LOOP_CODE" "${SD:-}" "${SA_DIR:-}" "${DD:-}" 2>/dev/null || true; }
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

# --- state_apply: the atomic write the review record now depends on -----------
# The kill experiment below replaces the writer, so it proves the FOLD makes one
# call — it cannot see whether that call is one RENAME. `mv` is shadowed here (a
# function beats PATH lookup even inside another function), so the renames the
# shipped state_apply performs are counted directly. One rename is what makes a
# kill land on either a whole old state or a whole new one; a rename per key is
# the AR-2 bug back again with its old shape hidden inside a helper.
SA_DIR="$(mktemp -d)"
printf 'KEEP=1\nA=old\nB=old\n' >"$SA_DIR/state"
SA_RENAMES="$( ( RENAMES=0; mv() { RENAMES=$((RENAMES + 1)); command mv "$@"; }
                 STATE="$SA_DIR/state"; state_apply A=new B=new C=new
                 printf '%s\n' "$RENAMES" ) )"
check "AR-2: state_apply performs exactly ONE rename for three keys" "1" "$SA_RENAMES"
check "AR-2: and the file it leaves is complete, not half a transition" "KEEP=1 C=new" \
      "$(grep -E '^(KEEP|C)=' "$SA_DIR/state" | tr '\n' ' ' | sed 's/ $//')"
check "AR-2: keys it was not given survive the rewrite" "1" "$(grep -c '^KEEP=1$' "$SA_DIR/state")"
check "AR-2: a key it WAS given is replaced, not duplicated" "1" "$(grep -c '^A=' "$SA_DIR/state")"
check "AR-2: and it can create the file when there is nothing to rewrite" "0" \
      "$( STATE="$SA_DIR/missing"; state_apply A=1; [ -f "$SA_DIR/missing" ] && echo 0 || echo 1 )"
# AR6-4: the status is part of the contract, so the primitive must be able to
# produce a bad one. `mv -f tmp somedir` SUCCEEDS by moving the temp file INTO a
# directory, so a $STATE that had become a directory reported a committed
# transition while the record stayed exactly as it was (external review
# 2026-11-01, run-20261101-062333/review-2.md AR6-4).
mkdir -p "$SA_DIR/adir"
check "AR6-4: a state path that is a directory is refused, not renamed into" "1" \
      "$( STATE="$SA_DIR/adir"; state_apply A=1 >/dev/null 2>&1; printf '%s' "$?" )"
check "AR6-4: and the refused write left nothing behind in it" "0" "$(find "$SA_DIR/adir" -type f | wc -l | tr -d ' ')"
check "AR6-4: a state path in an unwriteable place is refused too" "1" \
      "$( STATE="$SA_DIR/no-such-dir/state"; state_apply A=1 >/dev/null 2>&1; printf '%s' "$?" )"

# --- every atomic transition names its KEYS (AR6-2) ---------------------------
# A `guard_present` pins the WORDS of one call, not the KEYS it writes, and that
# difference is the finding: deleting `REVIEW_STATUS="$REVIEW_STATUS"` from the
# collector's write, or `REVIEW_OWED_BASE="$REVIEW_OWED_BASE"` from the BLOCKED
# arm's, left this suite green at 110 PASS, because the substring each guard
# matched did not name the key that vanished (external review 2026-11-01,
# run-20261101-062333/review-2.md AR6-2, both reproduced). So parse the call
# sites instead: join each `state_write` call's continuation lines, take the KEY
# of every KEY=value argument, and assert the SET each arm must write. A new
# transition that nobody pinned also fails the count.
APPLY_SIGS="$(awk '
{
    idx = index($0, "state_write ")
    if (idx == 0) next
    call = substr($0, idx + length("state_write "))
    while (call ~ /\\[[:space:]]*$/) {
        sub(/\\[[:space:]]*$/, "", call)
        if ((getline nxt) > 0) call = call " " nxt
    }
    n = split(call, tok, /[[:space:]]+/); m = 0; delete keys
    for (i = 1; i <= n; i++) {
        t = tok[i]; sub(/^"/, "", t)
        if (t ~ /^[A-Za-z_][A-Za-z_0-9]*=/) { m++; keys[m] = substr(t, 1, index(t, "=") - 1) }
    }
    for (i = 2; i <= m; i++) { v = keys[i]; for (j = i - 1; j >= 1 && keys[j] > v; j--) keys[j+1] = keys[j]; keys[j+1] = v }
    out = ""
    for (i = 1; i <= m; i++) out = out (i > 1 ? " " : "") keys[i]
    print out
}' "$LOOP_CODE")"
sig_count() { printf '%s\n' "$APPLY_SIGS" | grep -Fxc "$1"; }
check "AR6-2: the loop has exactly six atomic transitions" "6" "$(printf '%s\n' "$APPLY_SIGS" | grep -c .)"
check "AR6-2: the startup fold AND the collector each write all five obligation keys" "2" \
      "$(sig_count 'REVIEW_FAILS REVIEW_INFLIGHT_BASE REVIEW_INFLIGHT_HEAD REVIEW_OWED_BASE REVIEW_STATUS')"
check "AR6-2: the HEAD reconstruction writes owed base, counter and status" "1" \
      "$(sig_count 'REVIEW_FAILS REVIEW_OWED_BASE REVIEW_STATUS')"
check "AR6-2: dispatch writes the range and the status together" "1" \
      "$(sig_count 'REVIEW_INFLIGHT_BASE REVIEW_INFLIGHT_HEAD REVIEW_STATUS')"
check "AR6-2: the BLOCKED arm writes its obligation, its counter, its status and its reason" "1" \
      "$(sig_count 'LAST_REVIEW_DECISION REVIEW_FAILS REVIEW_OWED_BASE REVIEW_STATUS')"
check "AR6-3: the end-of-iteration rewrite re-emits the three cross-iteration signals" "1" \
      "$(sig_count 'FAILS HEAD ITER ITERS_SINCE_REVIEW LAST_GATES LAST_RC LAST_REVIEW_DECISION LAST_REVIEW_ITER LAST_RISK REVIEW_CALLS REVIEW_FAILS REVIEW_INFLIGHT_BASE REVIEW_INFLIGHT_HEAD REVIEW_OWED_BASE REVIEW_SKIP_COUNT REVIEW_STATUS STALL')"
check "AR6-2: no atomic transition omits the status it is transitioning" "0" \
      "$(printf '%s\n' "$APPLY_SIGS" | grep -vc 'REVIEW_STATUS')"

# --- H1 end to end: the shipped startup block, lifted and run -------------------
# The cases above prove the arithmetic; these prove the WIRING — that the startup
# really folds an uncollected review into the owed set, counts it, clears the
# in-flight marker, and refuses when it cannot reconstruct the obligation. The
# block is lifted out of ralph-pi.sh rather than retyped here, so a rewrite that
# drops the call fails (the same trick the independence guard above uses).
STARTUP_BLOCK="$(awk '/^if \[ -n "\$REVIEW_INFLIGHT_BASE" \]; then$/{f=1} f{print} f&&/^fi$/{exit}' "$LOOP")"
# The loop's checked writer, lifted for the same reason: the arms call it, so the
# test runs the shipped one rather than a copy that can drift from it (AR6-4).
STATE_WRITE_FN="$(awk '/^state_write\(\) \{/{f=1} f{print} f&&/^\}$/{exit}' "$LOOP")"
if [ -z "$STARTUP_BLOCK" ]; then
    check "H1: the startup block could be lifted from ralph-pi.sh" "lifted" "NOT FOUND"
else
    SD="$(mktemp -d)"
    # The loop's own state primitives, pointed at a scratch file.
    state_get() { sed -n "s/^$1=//p" "$SD/state" 2>/dev/null | tail -1; }
    state_set() { local kv="$1=$2" k="${1%%=*}"; grep -v "^$k=" "$SD/state" 2>/dev/null >"$SD/state.t" || true; echo "$kv" >>"$SD/state.t"; mv "$SD/state.t" "$SD/state"; }
    log() { printf '%s\n' "$*" >>"$SD/log"; }
    eval "$STATE_WRITE_FN"
    run_startup() { # run_startup <state-file-body> [repo-root] -> "rc|owed|fails"
        printf '%b' "$1" >"$SD/state"; : >"$SD/log"
        REVIEW_OWED_BASE="$(state_get REVIEW_OWED_BASE)"
        REVIEW_FAILS="$(review_int "$(state_get REVIEW_FAILS)")"
        REVIEW_INFLIGHT_BASE="$(state_get REVIEW_INFLIGHT_BASE)"
        REVIEW_INFLIGHT_HEAD="$(state_get REVIEW_INFLIGHT_HEAD)"
        REVIEW_LEFTOVER="$(state_get REVIEW_STATUS)"
        REVIEW_FAIL_LIMIT=2
        STATE="$SD/state"   # named in the refusal message; set -u would catch a rename
        local rc root="${2:-$ROOT}"
        ( ROOT="$root"; eval "$STARTUP_BLOCK" ); rc=$?
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
    # AR-4: existence is not the same as ancestry. `rev-parse --verify` accepts any
    # commit in the object database, including one a builder `reset` or `rebase`
    # abandoned, and `git diff A..B` is a tree diff with no ancestry requirement,
    # so a base off to one side of HEAD would be reviewed as a REVERT of that
    # commit rather than as the commit the lost review was covering —
    # under-coverage dressed as over-coverage. Driven against a scratch repo
    # because the real one has no abandoned branch to borrow.
    FAKE="$SD/repo"; mkdir -p "$FAKE"
    if git -C "$FAKE" init -q >/dev/null 2>&1 \
       && git -C "$FAKE" -c user.email=t@t -c user.name=t commit -q --allow-empty -m one >/dev/null 2>&1 \
       && MAIN_SHA="$(git -C "$FAKE" rev-parse HEAD)" \
       && MAIN_BRANCH="$(git -C "$FAKE" rev-parse --abbrev-ref HEAD)" \
       && git -C "$FAKE" checkout -q -b side >/dev/null 2>&1 \
       && git -C "$FAKE" -c user.email=t@t -c user.name=t commit -q --allow-empty -m two >/dev/null 2>&1 \
       && SIDE_SHA="$(git -C "$FAKE" rev-parse HEAD)" \
       && git -C "$FAKE" checkout -q "$MAIN_BRANCH" >/dev/null 2>&1; then
        check "AR-4: a HEAD line naming a NON-ancestor is refused, not widened to" "7||0" \
              "$(run_startup "REVIEW_STATUS=invoked\nREVIEW_FAILS=0\nREVIEW_OWED_BASE=\nREVIEW_INFLIGHT_BASE=\nHEAD=$SIDE_SHA\n" "$FAKE")"
        check "AR-4: and the refusal records nothing, so the operator names the base" "" "$(state_get REVIEW_OWED_BASE)"
        check "AR-4: an ancestor HEAD line still reconstructs the obligation" "0|$MAIN_SHA|1" \
              "$(run_startup "REVIEW_STATUS=invoked\nREVIEW_FAILS=0\nREVIEW_OWED_BASE=\nREVIEW_INFLIGHT_BASE=\nHEAD=$MAIN_SHA\n" "$FAKE")"
    else
        check "AR-4: a scratch repo could be built for the ancestry cases" "created" "FAILED"
    fi
    # AR-2: the fold must be ONE write, and "it writes the status first" is not the
    # same property. A transition written key by key is a transition a SIGKILL can
    # interrupt between keys, and the shipped interruption is exactly that: a
    # watchdog, an OOM kill or a reboot in the fold's window. This drives the real
    # startup block with a writer that dies before its n-th write, then restarts on
    # whatever survived, and asserts the lost review was counted EXACTLY ONCE.
    # Before the fix the fold wrote five keys, so a death before write 4 or 5 left
    # `failed` plus a still-set in-flight marker: the restart folded the same review
    # a second time, REVIEW_FAILS hit the limit of 2, and the loop exited 7 — an
    # exit code the systemd unit deliberately does not restart.
    #
    # AR6-6, said plainly: against the SHIPPED one-write fold only kill_at=1 ever
    # reaches the fake writer's death, so cases 2..6 assert a state no death of the
    # current code can produce. They are kept because they are the write boundaries
    # of the pre-fix five-write sequence, which is what the reversion experiment
    # fails on, and a re-spelled fold that went back to one state_set per key dies
    # on them. They are regression pins for the OLD shape, not six live windows.
    kill_fold_at() { # kill_fold_at <n> — die before the n-th state write, then restart
        local n="$1"
        printf 'REVIEW_STATUS=invoked\nREVIEW_FAILS=0\nREVIEW_OWED_BASE=\nREVIEW_INFLIGHT_BASE=aaaa1111\n' >"$SD/state"
        : >"$SD/log"
        (
            WRITES=0
            _die_before() { WRITES=$((WRITES + 1)); if [ "$WRITES" -ge "$n" ]; then exit 9; fi; }
            state_set() { _die_before; local kv="$1=$2" k="${1%%=*}"; grep -v "^$k=" "$SD/state" 2>/dev/null >"$SD/state.t" || true; echo "$kv" >>"$SD/state.t"; mv "$SD/state.t" "$SD/state"; }
            state_apply() { _die_before; local kv; for kv in "$@"; do printf '%s\n' "$kv" >>"$SD/state"; done; }
            eval "$STARTUP_BLOCK"
        )
        run_startup "$(cat "$SD/state")"
    }
    for kill_at in 1 2 3 4 5 6; do
        check "AR-2: a death at write $kill_at of the fold counts the loss exactly once" "0|aaaa1111|1" \
              "$(kill_fold_at "$kill_at")"
    done
    check "AR-2: and the fold's own death leaves the obligation on disk" "aaaa1111" \
          "$(kill_fold_at 1 >/dev/null; state_get REVIEW_OWED_BASE)"
fi

# --- AR6-1: dispatch records the obligation BEFORE the reviewer exists ---------
# Every other arm has been pinned behaviourally; the arm that CREATES the
# obligation was pinned only by a string naming the non-atomic form it shipped in
# (external review 2026-11-01, run-20261101-062333/review-2.md AR6-1): the
# reviewer was spawned first and the range that IS the obligation was written
# after it, in three separate renames, so a kill in that window left nothing on
# disk and a restart owed nothing. The arm is lifted and run with the spawn
# stubbed, so what is asserted is the ORDER, not the spelling: a death injected
# right after the obligation's rename must leave the obligation recorded and the
# reviewer never started. Under the shipped order that is the worst case there is
# (owed, counted, re-dispatched); under the old order it is a silent all-clear.
DISPATCH_BLOCK="$(awk '/^[[:space:]]*phase review$/{f=1} f{print} f&&/^[[:space:]]*REVIEW_PID=\$!$/{exit}' "$LOOP")"
if [ -z "$DISPATCH_BLOCK" ]; then
    check "AR6-1: the dispatch arm could be lifted from ralph-pi.sh" "lifted" "NOT FOUND"
elif [ -z "$STATE_WRITE_FN" ]; then
    check "AR6-1: state_write could be lifted from ralph-pi.sh" "lifted" "NOT FOUND"
else
    DD="$(mktemp -d)"
    printf 'the review prompt\n' >"$DD/prompt"
    dstate_get() { sed -n "s/^$1=//p" "$DD/state" 2>/dev/null | tail -1; }
    spawn_count() { if [ -f "$DD/spawn" ]; then wc -l <"$DD/spawn" | tr -d ' '; else echo 0; fi; }
    run_dispatch() { # run_dispatch <1 to die right after the obligation's write, 0 to run>
        printf 'REVIEW_STATUS=done\nREVIEW_INFLIGHT_BASE=\nREVIEW_INFLIGHT_HEAD=\nREVIEW_OWED_BASE=\n' >"$DD/state"
        : >"$DD/log"; rm -f "$DD/spawn"
        (
            STATE="$DD/state"
            phase() { :; }
            log() { printf '%s\n' "$*" >>"$DD/log"; }
            timeout() { printf 'spawn\n' >>"$DD/spawn"; }
            eval "$STATE_WRITE_FN"
            if [ "$1" != "0" ]; then
                state_write() { state_apply "$@"; exit 9; }   # a SIGKILL in the window
            fi
            REVIEW_MODEL="litellm/reviewer"
            REVIEW_TIMEOUT=1
            PI_CMD="pi"
            RUN_DIR="$DD"
            ITER=1
            RP="$DD/prompt"
            REVIEW_BASE="aaaa1111"
            HEAD_AFTER="bbbb2222"
            REVIEW_DECISION_REASON="high-risk batch"
            eval "$DISPATCH_BLOCK"
            wait 2>/dev/null || true
        )
    }
    run_dispatch 0
    check "AR6-1: a dispatched review records the obligation" "invoked" "$(dstate_get REVIEW_STATUS)"
    check "AR6-1: and the range it covers" "aaaa1111" "$(dstate_get REVIEW_INFLIGHT_BASE)"
    check "AR6-1: and the reviewer really was started" "1" "$(spawn_count)"
    run_dispatch 1
    check "AR6-1: a death right after the obligation's write still leaves the range owed" "aaaa1111" "$(dstate_get REVIEW_INFLIGHT_BASE)"
    check "AR6-1: and the status it died on is 'invoked', not the previous review's 'done'" "invoked" "$(dstate_get REVIEW_STATUS)"
    check "AR6-1: and the reviewer was NEVER spawned, so nothing claims a running review" "0" "$(spawn_count)"
    # AR6-4: the wrapper must stop the loop when the record could not be written,
    # not log the transition as done and carry on with a stale one.
    mkdir -p "$DD/adir"
    ( STATE="$DD/adir"; eval "$STATE_WRITE_FN"; log() { :; }; state_write A=1 ) 2>/dev/null
    check "AR6-4: a record that could not be written exits the arm non-zero" "9" "$?"
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
fold_body() { # the in-flight fold arm: owed computation .. the fold's own log line
    # The end marker used to be `REVIEW_INFLIGHT_HEAD=""`, which the fold now also
    # matches INSIDE its single state_apply — so the range would stop before the
    # write every guard below is about, and they would pass vacuously. The log line
    # is the arm's last statement and awk's range ends at the first match after the
    # start, which is this arm's (the F3 arm's log comes later).
    awk '/review_inflight_owed/,/reviews lost without a verdict are now/' "$LOOP_CODE"
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
guard_absent_in_fold() { # guard_absent_in_fold <label> <fixed-string> -- must NOT be in the fold
    if fold_body | grep -qF "$2"; then
        printf 'FAIL %s — %s is back inside the in-flight fold\n' "$1" "$2"
        fails=$((fails + 1))
    else
        printf 'PASS %s\n' "$1"
    fi
}
for helper in guard_absent guard_present order_before guard_in_fold order_in_fold guard_absent_in_fold; do
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
guard_present "F1: the persisted status is the computed outcome, not a literal 'done'" 'REVIEW_STATUS="$OUTCOME"'
guard_absent  "F1: no unconditional REVIEW_STATUS=done"        'state_set REVIEW_STATUS "done"'
# F2: the obligation survives the process, and the owed base reaches the record.
guard_present "F2: REVIEW_FAILS is read back at startup"       'REVIEW_FAILS="$(review_int "$(state_get REVIEW_FAILS)")"'
guard_present "F2: the owed base is read back at startup"      'REVIEW_OWED_BASE="$(state_get REVIEW_OWED_BASE)"'
guard_present "F2: both reach the persisted state file"        '"REVIEW_FAILS=$REVIEW_FAILS" "REVIEW_OWED_BASE=$REVIEW_OWED_BASE"'
guard_present "F2: the counter is read through review_int"     'review_int "$(state_get REVIEW_FAILS)"'
guard_absent  "F2: no case-with-:-0 that tests the wrong value" 'case "${REVIEW_FAILS:-0}" in'
# F3: independence is resolved, not string-compared.
guard_present "F3: the reviewer chain asks the reject-reason guard" 'REJECT="$(reviewer_reject_reason "$REVIEWER_MODEL")"'
guard_present "F3: the fallback chain asks it too"             'REJECT="$(reviewer_reject_reason "$candidate")"'
guard_present "F3: the identity of both sides is recorded"     'review-$ITER.identity.txt'
# H1: the obligation is written where it cannot be lost with the collector.
guard_present "H1: the range is persisted at dispatch"          'state_write REVIEW_INFLIGHT_BASE="$REVIEW_INFLIGHT_BASE"'
guard_present "H1: startup folds an uncollected review into owed" 'REVIEW_OWED_BASE="$(review_inflight_owed "$REVIEW_OWED_BASE" "$REVIEW_INFLIGHT_BASE")"'
guard_present "H1: the in-flight range reaches the state file"  '"REVIEW_INFLIGHT_BASE=$REVIEW_INFLIGHT_BASE" "REVIEW_INFLIGHT_HEAD=$REVIEW_INFLIGHT_HEAD"'
guard_present "H1: an invoked review with no range is refused, not guessed" 'so the obligation cannot be reconstructed at all'
# F2: the fold rewrites the status as well as the range, so it cannot manufacture
# the `invoked`-with-no-range state its own refusal rejects. AR-2 narrowed what
# "rewrites" has to mean: the whole transition must land in ONE rename. Five
# ordered `state_set` calls also put `failed` before the in-flight clear and still
# left a window in which a kill re-folded the same review, so the guard is now
# ONENESS of the write rather than the order of its parts (the reviewer's own
# remediation wording), and a `state_set` anywhere in the fold is a failure by
# itself. The behavioural version of this is the kill-at-every-write case above;
# these are the source-level pins that name the spelling.
guard_in_fold "F2: the fold rewrites REVIEW_STATUS, not only the range" 'state_write REVIEW_OWED_BASE='
guard_in_fold "AR-2: and it writes the status, the owed base and both markers together" \
              'REVIEW_STATUS="$REVIEW_STATUS" REVIEW_INFLIGHT_BASE="" REVIEW_INFLIGHT_HEAD=""'
guard_absent_in_fold "AR-2: the fold writes no key with a separate state_set" 'state_set '
order_in_fold "AR-2: the status variable is set BEFORE the atomic write reads it" \
             'REVIEW_STATUS="failed"' 'state_write REVIEW_OWED_BASE='
# AR-2 applied to the other multi-key transitions: the verdict and the BLOCKED
# record are one write each, and the end-of-iteration rewrite no longer truncates
# the state file before filling it back in.
guard_present "AR-2: the verdict reaches the file through the atomic write" 'REVIEW_OWED_BASE="$REVIEW_OWED_BASE" REVIEW_INFLIGHT_BASE="" REVIEW_INFLIGHT_HEAD=""'
guard_present "AR-2: the BLOCKED arm's obligation and its record are one write" '"LAST_REVIEW_DECISION=$LAST_REVIEW_DECISION"'
guard_present "AR-2/AR6-3: the end-of-iteration rewrite goes through the atomic writer" 'state_write "ITER=$ITER"'
guard_absent  "AR-2: the whole-file rewrite no longer truncates the live state file" '; } >"$STATE"'
guard_absent  "AR-2: and it no longer hand-rolls its own temp file and rename" '} >"${STATE}.t.$$"'
guard_absent  "AR-2: state_apply is defined once, in the sourced library, not forked here" 'state_apply()'
guard_present "AR6-4: the loop calls the CHECKED writer, not the raw primitive" 'if ! state_apply "$@"; then'
guard_present "AR6-4: and a record that could not be written stops the loop" 'exit 9'
guard_present "AR-4: the reconstructed base must be an ancestor of HEAD" 'merge-base --is-ancestor "$STATE_HEAD_SHA" HEAD'
# F4/AR6-1: the obligation is on disk before the reviewer exists, so a kill in
# either window leaves an obligation rather than an all-clear. The order used to
# be the reverse and the guard pinned the reverse: the spawn came first and the
# range was written after it in three renames (external review 2026-11-01,
# run-20261101-062333/review-2.md AR6-1). The behavioural version is the
# kill-after-the-write case above; these are the source-level pins.
guard_present "F4: the dispatch transition is ONE write, not a rename per key" \
              'REVIEW_INFLIGHT_HEAD="$REVIEW_INFLIGHT_HEAD" REVIEW_STATUS="invoked"'
guard_absent  "F4/AR6-1: dispatch writes no key with a separate state_set" 'state_set REVIEW_INFLIGHT'
order_before "AR6-1: the obligation is written BEFORE the reviewer is spawned" \
             'state_write REVIEW_INFLIGHT_BASE=' '( timeout -k 30 "$REVIEW_TIMEOUT"'
order_before "AR6-1: and the spawn is what the collector waits on" \
             '( timeout -k 30 "$REVIEW_TIMEOUT"' 'REVIEW_PID=$!'
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
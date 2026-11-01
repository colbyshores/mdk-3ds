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
# Counting record writes, and counting them HONESTLY. A write may sit after a `;` on
# a line that starts with something else — the gates arm used
# `LAST_GATES="PASS"; state_set LAST_GATES PASS` — so a pattern anchored to the line
# start counted ZERO writes in an arm that was writing the record twice, which is a
# pass generator wearing a guard's clothes. So the match is anywhere in the line.
#
# And it counts OCCURRENCES, not lines (external review 2026-11-01,
# run-20261101-062333/review-6.md F3). The number this produces is used as the
# bound of the collector death loop, which injects its failure at a write CALL:
# `state_set() { _die; …; }` counts a call, so two writes coalesced onto one line
# are two boundaries. A line count would have derived 3 from a 4-call arm, the
# loop would have covered three of four, and the label would still have claimed it
# covered all of them — R5-4's PASS generator again, entered through the counter
# instead of through a literal. Measured on itself below.
count_writes() { printf '%s\n' "$1" | grep -oE '(^|[;&|([:space:]])state_(set|write) ' | wc -l | tr -d ' '; }
# Lift ONE function out of the comment-stripped copy of the loop, bounded by the
# definition's own shape rather than by a brace that happens to own a line. Two
# spellings are legitimate in ralph-pi.sh today — `state_set` closes on its opening
# line, `state_write` closes with `}` in column 0, and `reviewer_reject_reason`
# contains `{ …; return 0; }` compounds whose closing braces are indented — and a
# terminator that accepts only one of them fails silently on the other: a bare `^\}`
# never matches a one-line definition and runs on for 73 lines (review-5 R5-1) or
# 53 (review-4 F2), while "the line ends in `}`" truncates `reviewer_reject_reason`
# at its first guard and swallows `pi_run` (review-6 F2). Both directions are
# measured by guard_fragment, never trusted.
# The doubled brackets in the start pattern are load-bearing, and the reason is POSIX
# bracket-class semantics, not an awk defect: `[()]` is ONE character class matching
# `(`-or-`)`, so `[()][[:space:]]` reads "a paren, then whitespace" and can never match
# the two characters `()`. `[(][)]` is two single-character classes in sequence and does
# match — measured on mawk 1.3.4, on busybox awk, and on every `/usr/bin/awk` here (all
# three of awk/nawk/mawk are the same mawk binary on this host, so that name is not an
# independent check). A pattern that matches nothing lifts nothing, which guard_fragment
# reports as zero definitions — loudly, but only because it is measured. Both directions
# are pinned by R7-4 below, because a rationale that blames the tool invites exactly the
# "simplification" it forbids: on a gawk host the next reader tries `[()]`, watches it
# fail there too, and concludes the comment is stale.
lift_fn() { # lift_fn <name> -> that function's text, or nothing
    awk -v fn="$1" '
        !f && $0 ~ "^"fn"[(][)][[:space:]]*[{]" { f=1; print; if ($0 ~ /\}[[:space:]]*$/) exit; next }
        f  { print; if ($0 ~ /^\}[[:space:]]*$/) exit }
    ' "$LOOP_CODE"
}
REPORT=$(mktemp -d)/report.md
printf 'VERDICT: PASS\n' >"$REPORT"
: >"$REPORT.empty"
cleanup() { rm -rf "$(dirname "$REPORT")" "$IDENT_LOG" "$LOOP_CODE" "${SD:-}" "${SA_DIR:-}" "${DD:-}" "${SS:-}" "${CD:-}" "${GR:-}" 2>/dev/null || true; }
trap cleanup EXIT
check() { # check <label> <expected> <actual>
    if [ "$2" = "$3" ]; then
        printf 'PASS %s\n' "$1"
    else
        printf 'FAIL %s — expected %s, got %s\n' "$1" "'$2'" "'$3'"
        fails=$((fails + 1))
    fi
}
# Every lifted fragment gets the SAME three measurements (review-6 F2: the claim
# that all the scoping lifts were measured was true of one of them, and a fourth
# lift of the identical shape had no measurement at all). An over-run turns every
# pin below into a grep over unrelated code AND over prose — ralph-pi.sh documents
# its own bugs, so a fragment that swallows the comment block quotes the strings
# its pins forbid — and a truncation evals a partial function, which fails the
# behavioural cases for the wrong reason. The first check is also the non-vacuity
# guard: an empty fragment holds zero definitions. The second is a guard on the
# SOURCE of the lift, because LOOP_CODE is comment-stripped: it can only fire by
# pointing a lift back at the raw file.
guard_fragment() { # guard_fragment <label> <fragment>
    check "$1 is scoped to one definition, so its pins cannot read a neighbour" "1" \
          "$(printf '%s\n' "$2" | grep -cE '^[A-Za-z_][A-Za-z_0-9]*\(\)')"
    check "$1 carries no comment line, so its pins cannot fire on prose" "0" \
          "$(printf '%s\n' "$2" | grep -cE '^[[:space:]]*#')"
    check "$1 is a syntactically complete fragment, not a truncation of it" "0" \
          "$(printf '%s\n' "$2" | bash -n 2>/dev/null; printf '%s' "$?")"
}

# R7-4 (external review 2026-11-01, run-20261101-062333/review-7.md F4): the comment above
# blamed mawk 1.3.4 for what is POSIX bracket-class semantics. The observed failure was
# real — the first `lift_fn` lifted nothing — but an attribution to a tool bug is a
# rationale no one can act on portably, so the mechanism is measured here instead of
# asserted in prose, on whichever awk runs this suite.
check "R7-4: the doubled-bracket start pattern matches a one-line definition" "1" \
      "$(printf 'state_write() {\n' | awk -v fn=state_write '$0 ~ "^"fn"[(][)][[:space:]]*[{]"{n++} END{print n+0}')"
check "R7-4: and the single-class [()] spelling matches nothing, so the doubling is load-bearing" "0" \
      "$(printf 'state_write() {\n' | awk -v fn=state_write '$0 ~ "^"fn"[()][[:space:]]*[{]"{n++} END{print n+0}')"

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
#
# From the CODE copy, with the shared bounded lift (external review 2026-11-01,
# run-20261101-062333/review-6.md F2). This was the fourth lift of the shape
# review-4 F2 and review-5 R5-1 fixed, and the only one with no measurement: it
# read the RAW file with a `/^\}/` terminator, so collapsing the definition to one
# line — the formatting change R5-1 said must not matter — ran the lift on through
# `pi_run`, eval'd a fragment that is not valid shell, and killed the identity block
# (a false FAIL, but a red suite is a red suite). The two resolvers it depends on
# are lifted the same way now, so no fragment in this file is trusted.
REJECT_FN="$(lift_fn reviewer_reject_reason)"
UPSTREAM_FN="$(lift_fn model_upstream)"
FAMILY_FN="$(lift_fn model_family)"
eval "$REJECT_FN"; eval "$UPSTREAM_FN"; eval "$FAMILY_FN"
guard_fragment "R6-2: the reviewer_reject_reason lift" "$REJECT_FN"
guard_fragment "R6-2: the model_upstream lift"        "$UPSTREAM_FN"
guard_fragment "R6-2: the model_family lift"         "$FAMILY_FN"
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
    if ($0 ~ /^state_set\(\)/) next   # the single-key writer delegates; it is not a transition
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
# Bounded, and from the CODE copy (external review 2026-11-01,
# run-20261101-062333/review-5.md R5-1). This was the identical defect review-4 F2
# found in the `state_set` lift, one function up: a terminator that is exactly `}`.
# Written on one line, `state_write` is never matched by it, the fragment ran on for
# 73 lines over the model-identity block, and the AR6-4 pin below matched the PROSE
# at lift-line 17 (`# This does widen exit 9 to every record write …`) and reported
# PASS with ZERO `exit 9` occurrences left in code. Measured below rather than
# trusted, for the same reason the `state_set` fragment is.
STATE_WRITE_FN="$(lift_fn state_write)"
# The three measurements live in guard_fragment, which every lifted fragment now
# goes through (review-6 F2) — one spelling of the rule, not five copies of it.
guard_fragment "R5-1: the state_write lift" "$STATE_WRITE_FN"
# The single-key writer, lifted for the same reason (review-3 F3): the arms call
# it, so the test must run the shipped one rather than a copy that can drift.
#
# Bounded, and from the CODE copy rather than the raw file (external review
# 2026-11-01, run-20261101-062333/review-4.md F2). `state_set` is ONE line, so the
# terminator this lift used — a line that is exactly `}` — never matched it, and
# the fragment ran on for 53 lines: the whole model-identity comment block,
# model_upstream, model_family and the tail of reviewer_reject_reason. The pin
# below was therefore a grep over three unrelated functions and over prose that
# QUOTES the commands the pin forbids, so moving a comment — a pure documentation
# edit — failed it, and `eval` silently redefined those functions in this process.
# The terminator now accepts a one-line definition or a closing brace, and the
# fragment is measured below instead of trusted: a lift that truncates early makes
# the eval incomplete, which fails the behavioural cases loudly rather than
# vacuously.
STATE_SET_FN="$(lift_fn state_set)"
if [ -z "$STARTUP_BLOCK" ]; then
    check "H1: the startup block could be lifted from ralph-pi.sh" "lifted" "NOT FOUND"
else
    SD="$(mktemp -d)"
    # The loop's own state primitives, pointed at a scratch file. `state_set` is
    # NOT retyped here any more: a copy of the writer living in the test is a copy
    # that can keep passing after the shipped one regressed (review-3 F3).
    STATE="$SD/state"
    state_get() { sed -n "s/^$1=//p" "$STATE" 2>/dev/null | tail -1; }
    log() { printf '%s\n' "$*" >>"$SD/log"; }
    eval "$STATE_WRITE_FN"
    eval "$STATE_SET_FN"
    run_startup() { # run_startup <state-file-body> [repo-root] -> "rc|owed|fails"
        printf '%b' "$1" >"$SD/state"; : >"$SD/log"
        REVIEW_OWED_BASE="$(state_get REVIEW_OWED_BASE)"
        REVIEW_FAILS="$(review_int "$(state_get REVIEW_FAILS)")"
        REVIEW_INFLIGHT_BASE="$(state_get REVIEW_INFLIGHT_BASE)"
        REVIEW_INFLIGHT_HEAD="$(state_get REVIEW_INFLIGHT_HEAD)"
        REVIEW_LEFTOVER="$(state_get REVIEW_STATUS)"
        REVIEW_FAIL_LIMIT=2
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

# --- the single-key writer is the SAME checked writer (review-3 F3) -----------
# `state_set` used to be its own `grep -v > tmp; echo >> tmp; mv tmp $STATE` with
# the `mv` status discarded, so AR6-4's refusal reached only the multi-key arm and
# every single-key write kept a silent no-op (measured: eleven call sites over
# eight distinct keys). A `$STATE` that had become a directory made `mv -f tmp
# somedir` SUCCEED by moving the temp file INTO the directory, so the collector
# could print "REVIEW CALL #N" while REVIEW_CALLS never landed. It now delegates
# to the checked writer, so the refusal, the temp-file check and the status are one
# implementation. These cases prove the delegation is real and not a comment that
# says so: the lifted body must refuse a directory exactly like state_apply does.
if [ -z "$STATE_SET_FN" ]; then
    check "F3: state_set could not be lifted from ralph-pi.sh" "lifted" "NOT FOUND"
else
    SS="$(mktemp -d)"
    mkdir -p "$SS/adir"
    ss_rc() { ( STATE="$1"; log() { :; }; eval "$STATE_WRITE_FN"; eval "$STATE_SET_FN"; state_set REVIEW_CALLS 7 ) 2>/dev/null; printf '%s' "$?"; }
    check "F3: state_set writes a single key" "0" "$(ss_rc "$SS/state")"
    check "F3: and the key landed on disk" "7" "$(sed -n 's/^REVIEW_CALLS=//p' "$SS/state" | tail -1)"
    check "F3: a state path that is a directory stops the arm, not just the announcement" "9" "$(ss_rc "$SS/adir")"
    check "F3: and the refused write left nothing behind in it" "0" "$(find "$SS/adir" -type f | wc -l | tr -d ' ')"
    check "F3: state_set hand-rolls no temp file, grep or rename of its own" "0" \
          "$(printf '%s\n' "$STATE_SET_FN" | grep -cE 'mv |grep -v|\.t')"
    # The two ways the old lift could be wrong, pinned instead of trusted
    # (review-4 F2), plus the third it never had: review-6 F2 counted this fragment
    # as measured and found it short of the `bash -n` completeness check the
    # CONTINUATION claimed for all of them.
    guard_fragment "F2: the state_set lift" "$STATE_SET_FN"
fi

# --- review-4 F1: nothing may sit between the verdict and the in-flight clear ---
# The collector is the only place a review becomes `done`, and the record stops
# claiming it is in flight in the SAME transition. Any record write placed between
# "the verdict is known" and "that transition lands" is a stop boundary inside the
# window, and batch 17 had just made every record write able to stop the loop at
# exit 9. Three counter writes above the transition therefore meant a full disk
# there left the durable record saying `invoked`, with the in-flight markers set,
# and REVIEW_CALLS already advanced — a review the counter says happened and the
# status says did not. The startup fold reads exactly that state as a lost review,
# so a COMPLETED review was re-owed, charged against REVIEW_FAIL_LIMIT and billed
# a second time (external review 2026-11-01, run-20261101-062333/review-4.md F1).
#
# The window is lifted as a body so the guard is about the window and not the file
# — the same scoping the batch that made these writes fatal argued for.
COLLECT_WINDOW="$(awk -v end='REVIEW_OWED_BASE="$REVIEW_OWED_BASE" REVIEW_INFLIGHT_BASE="" REVIEW_INFLIGHT_HEAD=""' \
    '/^[[:space:]]*RRC="\$\(review_rc_normalise/{f=1} f{print} f&&index($0,end){exit}' "$LOOP_CODE")"
# The same arm, lifted to its end, so the behavioural cases below can actually
# reach the counter writes rather than asserting about a region they never ran.
COLLECT_ARM="$(awk -v end='[ "$REVIEW_FAILS" -ge "$REVIEW_FAIL_LIMIT" ]' \
    '/^[[:space:]]*RRC="\$\(review_rc_normalise/{f=1} f{print} f&&index($0,end){exit}' "$LOOP_CODE")"
WINDOW_LINES="$(printf '%s\n' "$COLLECT_WINDOW" | wc -l | tr -d ' ')"
ARM_LINES="$(printf '%s\n' "$COLLECT_ARM" | wc -l | tr -d ' ')"
if [ "${WINDOW_LINES:-0}" -gt 20 ] && [ "$ARM_LINES" -gt "$WINDOW_LINES" ]; then
    printf 'PASS the collect window (%s lines) and the full arm (%s) were lifted, so their guards are not vacuous\n' "$WINDOW_LINES" "$ARM_LINES"
else
    printf 'FAIL the collect window lifted %s lines and the arm %s — every guard below would pass vacuously\n' "${WINDOW_LINES:-0}" "$ARM_LINES"
    fails=$((fails + 1))
fi
check "F1: the window holds exactly one record write, the transition itself" "1" \
      "$(count_writes "$COLLECT_WINDOW")"
# The death loop below must cover EVERY write boundary in the arm, so the bound is
# DERIVED from the lifted arm rather than guessed (external review 2026-11-01,
# run-20261101-062333/review-5.md R5-4). It used to be a literal `for cdie in 1 2 3 4
# 5 6`: the arm has four write sites, so a death injected at "write 5" or "write 6"
# never reached the kill, the arm ran to completion, and four of those fourteen cases
# asserted the uninterrupted outcome while claiming to assert a death. A label that
# cannot inject its own failure is a PASS generator.
COLLECT_WRITES="$(count_writes "$COLLECT_ARM")"
check "F1: the collector arm has four record-write calls, so the loop below covers all of them" "4" \
      "$COLLECT_WRITES"
# The counter is the instrument that DERIVES that bound, so it is measured on
# itself (external review 2026-11-01, run-20261101-062333/review-6.md F3). The
# death loop injects its failure at a write CALL — `state_set() { _die; …; }` — so
# a counter that counts LINES is the wrong unit: coalescing two writes onto one line
# would have derived 3 from a 4-call arm, the loop would have covered three of the
# four boundaries, and the label above would still have claimed it covered all of
# them. Same defect as R5-4, entered through the counter instead of a literal.
check "R6-3: the write counter counts calls, not lines (two on one line count twice)" "2" \
      "$(count_writes 'LAST_GATES="PASS"; state_set LAST_GATES PASS; state_set LAST_REVIEW_ITER 9')"
check "R6-3: and a fragment that writes nothing counts zero" "0" \
      "$(count_writes 'LAST_GATES="PASS"; FAILS=0')"

# The behavioural half: kill the shipped collector at every write boundary in that
# window and assert the counters can never get ahead of the verdict. Under the
# shipped order a death at write 1 loses the transition itself (the honest loss:
# invoked, counters untouched) and every later death has nothing left to die in
# front of the verdict. Under the old order a death at write 2, 3 or 4 printed
# `invoked|4` here, which is the state the fold charges as a lost review.
CD="$(mktemp -d)"
printf '0\n' >"$CD/review-9.rc"
printf 'VERDICT: PASS\n' >"$CD/review-9.md"
cdstate_get() { sed -n "s/^$1=//p" "$CD/state" 2>/dev/null | tail -1; }
run_collect() { # run_collect <n> — the shipped success arm, killed before its n-th rename
    printf 'REVIEW_STATUS=invoked\nREVIEW_FAILS=0\nREVIEW_OWED_BASE=\nREVIEW_CALLS=3\nLAST_REVIEW_ITER=2\nITERS_SINCE_REVIEW=5\nREVIEW_INFLIGHT_BASE=aaaa1111\nREVIEW_INFLIGHT_HEAD=bbbb2222\n' >"$CD/state"
    (
        STATE="$CD/state" ITER=9 HEAD_BEFORE=bbbb2222 REVIEW_MODEL=x REVIEW_FAIL_LIMIT=2
        RUN_DIR="$CD" STALL=1 KILL_AT="$1" WRITES=0
        REVIEW_STATUS=invoked REVIEW_FAILS=0 REVIEW_OWED_BASE= REVIEW_CALLS=3
        LAST_REVIEW_ITER=2 ITERS_SINCE_REVIEW=5
        REVIEW_INFLIGHT_BASE=aaaa1111 REVIEW_INFLIGHT_HEAD=bbbb2222
        _die() { WRITES=$((WRITES + 1)); if [ "$WRITES" -ge "$KILL_AT" ]; then exit 9; fi; }
        log() { :; }
        eval "$STATE_WRITE_FN"
        state_set() { _die; state_apply "$1=$2"; }
        state_write() { _die; state_apply "$@"; }
        eval "$COLLECT_ARM"
    )
    printf '%s|%s\n' "$(cdstate_get REVIEW_STATUS)" "$(cdstate_get REVIEW_CALLS)"
}
for cdie in $(seq 1 "$COLLECT_WRITES"); do
    out="$(run_collect "$cdie")"
    st="${out%%|*}"; calls="${out##*|}"
    # Two things are asserted, and the first is the ordering: write 1 IS the
    # transition, so losing it is the honest loss (`invoked`, counters untouched);
    # every later write sits AFTER it, so the verdict has already landed. Under the
    # old order writes 2-4 were the counters, and they printed `invoked`.
    if [ "$cdie" -eq 1 ]; then want="invoked"; else want="done"; fi
    check "F1: a death at write $cdie leaves the verdict as the first thing that landed" "$want" "$st"
    # And the harm itself: a record that still says `invoked` must never show a
    # review counter that advanced, because that is the state the startup fold
    # charges as a lost review. A stale counter AFTER `done` is harmless — the
    # end-of-iteration rewrite re-emits it from the in-process value.
    if [ "$st" = "invoked" ] && [ "$calls" != "3" ]; then harm="counters ahead of the verdict"; else harm="consistent"; fi
    check "F1: and a death at write $cdie cannot put the counters ahead of the verdict" "consistent" "$harm"
done
check "F1: with no interruption the verdict and its counters both land" "done|4" "$(run_collect 99)"

# --- review-5 R5-2: the gates phase must perform no record write ----------------
# Batch 18 moved the three collector counters out of the window between "the verdict
# is known" and "the transition records it", and left a SECOND stop boundary in that
# window two screens earlier: the gates phase wrote LAST_GATES while the durable
# record still said `invoked` with both in-flight markers set, after a reviewer that
# may already have finished and written its report. A full disk on that one write
# produced the batch-18 harm from a different site — a COMPLETED review the startup
# fold reads as a lost one (owed again, REVIEW_FAILS + 1, a second paid call, and two
# of them reach the restart-prevented exit 7). The fix is that the gates phase writes
# nothing and the end-of-iteration rewrite carries LAST_GATES. Pinned structurally
# (the lifted region holds no write) AND behaviourally (the shipped region, run
# against a record that cannot be written, must not stop the loop).
GATES_REGION="$(awk '/^[[:space:]]*phase gates$/{f=1} f{print} f&&/^[[:space:]]*phase review-collect$/{exit}' "$LOOP_CODE")"
GATES_REGION_LINES="$(printf '%s\n' "$GATES_REGION" | wc -l | tr -d ' ')"
if [ "${GATES_REGION_LINES:-0}" -gt 5 ]; then
    printf 'PASS the gates phase (%s lines) was lifted, so its guards below are not vacuous\n' "$GATES_REGION_LINES"
else
    printf 'FAIL the gates phase lifted %s lines — every guard below would pass vacuously\n' "${GATES_REGION_LINES:-0}"
    fails=$((fails + 1))
fi
check "R5-2: the gates phase performs no record write, so no stop boundary sits in the invoked window" "0" \
      "$(count_writes "$GATES_REGION")"
# Written as a count rather than with `guard_absent`, which is defined further down
# and would not exist yet at this point in the file.
check "R5-2: the LAST_GATES write is gone from the loop, not hidden in another arm" "0" \
      "$(grep -cF 'state_set LAST_GATES' "$LOOP_CODE")"
# The behavioural half: run the SHIPPED gates region against a record that cannot be
# written — a $STATE that is a directory, the shipped refusal, the same stand-in for
# a full disk the AR6-4 cases use. Pre-fix this exits 9 and leaves the record saying
# `invoked` with the in-flight markers set while the report sits in the run dir, which
# is the state the fold charges as a lost review.
GR="$(mktemp -d)"; mkdir -p "$GR/adir"
gr_rc() { # gr_rc <state-path> — the shipped gates phase, reviewer already finished
    (
        STATE="$1" ITER=9 FAILS=7 RUN_DIR="$GR" LAST_GATES=""
        log() { :; }; phase() { :; }
        run_gates() { printf 'gates ran\n' >"${1:-/dev/null}"; return 0; }
        eval "$STATE_WRITE_FN"; eval "$STATE_SET_FN"
        eval "$GATES_REGION"
    )
    printf '%s' "$?"
}
printf 'REVIEW_STATUS=invoked\nREVIEW_INFLIGHT_BASE=aaaa1111\nREVIEW_INFLIGHT_HEAD=bbbb2222\nREVIEW_CALLS=3\n' >"$GR/state"
check "R5-2: and an unwritable record cannot stop the gates phase" "0" "$(gr_rc "$GR/adir")"
check "R5-2: and the gates phase left the review record exactly as it found it" "invoked" \
      "$(gr_rc "$GR/state" >/dev/null; sed -n 's/^REVIEW_STATUS=//p' "$GR/state")"
check "R5-2: and it wrote no key of its own, so the rewrite owns LAST_GATES" "0" \
      "$(grep -c 'LAST_GATES' "$GR/state")"

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
# The writer's own body, scoped (external review 2026-11-01,
# run-20261101-062333/review-3.md F5). A file-wide `guard_present … 'exit 9'` is
# satisfied by an `exit 9` in ANY arm, which is the exact shape this suite spent a
# whole batch arguing against: the AR6-2 finding was that a substring pin stays
# green when the property is deleted from the arm that needs it, and this pin was
# that shape two screens below the argument. The function is already lifted, so
# there is no reason to match the file instead of it.
guard_in_state_write() { # guard_in_state_write <label> <fixed-string> -- must be in state_write
    if printf '%s\n' "$STATE_WRITE_FN" | grep -qF "$2"; then
        printf 'PASS %s\n' "$1"
    else
        printf 'FAIL %s — %s is missing from state_write itself\n' "$1" "$2"
        fails=$((fails + 1))
    fi
}
absent_in_state_write() { # <label> <fixed-string> -- must NOT be in state_write
    if printf '%s\n' "$STATE_WRITE_FN" | grep -qF "$2"; then
        printf 'FAIL %s — %s is back inside state_write\n' "$1" "$2"
        fails=$((fails + 1))
    else
        printf 'PASS %s\n' "$1"
    fi
}
for helper in guard_absent guard_present order_before guard_in_fold order_in_fold guard_absent_in_fold guard_in_state_write absent_in_state_write; do
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
guard_in_state_write "AR6-4: the writer checks the primitive's status, it does not discard it" 'if ! state_apply "$@"; then'
guard_in_state_write "AR6-4: and a record that could not be written stops the loop" 'exit 9'
absent_in_state_write "AR6-4: the stop is not spelled so a caller could swallow it" 'state_apply "$@" || true'
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
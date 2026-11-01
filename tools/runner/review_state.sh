# review_state.sh — the loop's review-record state machine, as functions.
#
# Sourced by ralph-pi.sh and by tools/runner/test_review_state.sh. It lives in
# its own file for one reason: the review record is the only part of the loop
# whose failure is invisible — a review that silently reads "done" is not a
# crash, it is a commit nobody checked — and a state machine you cannot run
# without starting the loop is a state machine you cannot test.
#
# External review 2026-11-01 (run-20261101-002752/review-2.md) found both halves
# of that in the previous revision: a dispatched reviewer that died was recorded
# as REVIEW_STATUS=done (F1), and a BLOCKED review left nothing behind, so a
# restart forgot the obligation (F2). The fix for F2 wrote the obligation in the
# collector, and run-20261101-025705/review-1.md H1 found the third way: a loop
# that is killed between dispatch and collect never reaches the collector, so it
# never writes the obligation either. Hence review_inflight_owed below, and the
# rule that the range is recorded at DISPATCH, not at collection.

# review_rc_normalise <raw> — a reviewer's exit code, or 1 for "no usable code".
# The old test was `[ "$RRC" -ne 0 ]`, and on an empty or corrupt value `[`
# itself fails with status 2, which `if` reads as FALSE: a killed subshell that
# left an empty .rc file took the SUCCESS branch. A missing answer is a failure,
# never a pass.
review_rc_normalise() {
    case "$1" in
        ''|*[!0-9]*) printf '1\n' ;;
        *) printf '%s\n' "$1" ;;
    esac
}

# review_int <raw> [default] — a counter read back from the state file, or the
# default (0) when the file did not have it or held something that is not a
# number. This looks like a nicety and is not: the shipped guard is
# `[ "$REVIEW_FAILS" -ge "$REVIEW_FAIL_LIMIT" ]`, and `[` on an empty operand
# errors with status 2, which `&&` reads as FALSE — so a counter that comes back
# empty from a state file written by an older revision makes the reviewer-failure
# limit silently never fire. That is the same shape as F1: the check that cannot
# answer must not be read as "all clear".
review_int() {
    case "$1" in
        ''|*[!0-9]*) printf '%s\n' "${2:-0}" ;;
        *) printf '%s\n' "$1" ;;
    esac
}

# review_outcome <rc> <report-file> — done | failed.
# A report must EXIST, BE NON-EMPTY and come from a zero exit code. A reviewer
# that timed out after writing half a report is a failed review: the partial
# text is not a review of the diff.
review_outcome() {
    local rc report
    rc="$(review_rc_normalise "$1")"
    report="$2"
    if [ "$rc" -eq 0 ] && [ -s "$report" ]; then
        printf 'done\n'
    else
        printf 'failed\n'
    fi
}

# review_owed <outcome> <owed-so-far> <head-before> — the base commit that is
# still owed a review, or empty when nothing is.
#
# A review that did not complete leaves the FIRST unreviewed commit behind, and
# keeps it: if an earlier iteration already owed one, the earlier one wins, so a
# run of failures cannot shrink the range that must be re-reviewed. Only a
# completed review clears it. The next dispatch then covers owed..HEAD, which is
# how a blocked commit gets its review after the route is fixed instead of
# being skipped forever.
review_owed() {
    local outcome="$1" owed="$2" head_before="$3"
    if [ "$outcome" = "done" ]; then
        printf '\n'
    elif [ -n "$owed" ]; then
        printf '%s\n' "$owed"
    else
        printf '%s\n' "$head_before"
    fi
}

# review_inflight_owed <owed-so-far> <inflight-base> — fold a DISPATCHED-but-
# UNCOLLECTED review into the owed set, or pass the owed set through untouched
# when nothing is in flight.
#
# This is the third way a required review fails to happen (external review
# 2026-11-01, finding H1). The collector is what turns `invoked` into `done` or
# `failed`, and the collector runs after `wait`. Between dispatch and collection
# the loop spends its time in the gates phase — minutes to hours — and a systemd
# restart, a watchdog phase limit, an operator stop, an OOM kill or a host reboot
# in that window means the collector never runs at all. The reviewer may or may
# not have finished; nobody recorded it, so nobody knows. That is a review that
# did not happen, and the commit under it is unreviewed by anyone.
#
# So the range is persisted at dispatch (REVIEW_INFLIGHT_BASE) and a startup that
# finds it non-empty treats the review as lost: the obligation is folded in
# through the same earliest-base rule as a failed review, and only a completed
# review clears it. The direction is deliberate: the worst case is one redundant
# review of an already-reviewed commit, where the alternative is a high-risk
# batch that no model ever looked at.
review_inflight_owed() {
    local owed="$1" inflight="$2"
    if [ -z "$inflight" ]; then
        printf '%s\n' "$owed"
    else
        review_owed failed "$owed" "$inflight"
    fi
}

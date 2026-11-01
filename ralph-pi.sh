#!/usr/bin/env bash
# ralph-pi.sh — the MDK port loop on the Pi harness.
#
# Roles: the worker is pi (the builder) as piagent; the reviewer is a second pi
# model, read-only by instruction, over each iteration's diff; this script is the
# orchestrator — gates, progress/stall detection and review dispatch — and the
# builder can never influence them.
#
# Each committed batch must include docs/frames/*.png and its .txt measurement;
# tools/runner/gates_batch_integrity.py enforces --base/--head and --staged
# boundaries; the published image is produced by tools/publish_frame.sh.
# Usage:
#   tools/install_ralph_pi_service.sh    # systemd unit + tools/ralph_pi_watchdog.sh
#   systemctl --user start ralph-pi-mdk
#   systemctl --user stop ralph-pi-mdk   # kill switch (kills the cgroup, and
#                                        # piagent's children with it)
set -u
set -o pipefail
umask 002
ROOT="$(cd "$(dirname "$0")" && pwd)"
LOGDIR="$ROOT/logs/ralph-pi"
STATE="$LOGDIR/state"
SUMMARY="$LOGDIR/summary.log"
PIDFILE="$LOGDIR/loop.pid"
PHASEFILE="$LOGDIR/phase"
HEARTBEAT="$LOGDIR/heartbeat"
mkdir -p "$LOGDIR"

log() {
    local line="[$(date -Is)] $*"
    printf '%s\n' "$line" | tee -a "$SUMMARY"
}
phase() {
    {
        printf 'epoch=%s\n' "$(date +%s)"
        printf 'phase=%s\niteration=%s\nmodel=%s\npid=%s\n' \
            "$1" "${ITER:-0}" "${RUN_MODEL:-none}" "$$"
    } >"$PHASEFILE"
    log "phase=$1 iteration=${ITER:-0} model=${RUN_MODEL:-none}"
}
phase_limit() {
    case "$1" in
        startup) printf 1800 ;;
        route-health|verify) printf 900 ;;
        worker) printf 7800 ;;
        worker-fallback) printf 7500 ;;
        gates) printf 14400 ;;
        review-collect) printf 5700 ;;
        route-backoff) printf 600 ;;
        idle) printf 1800 ;;
        *) printf 1800 ;;
    esac
}
heartbeat_start() {
    local main_pid="$$"
    (
        while :; do
            sleep 30
            [ -r "$PHASEFILE" ] || continue
            phase_name="$(sed -n 's/^phase=//p' "$PHASEFILE" | head -1)"
            phase_epoch="$(sed -n 's/^epoch=//p' "$PHASEFILE" | head -1)"
            [[ "$phase_epoch" =~ ^[0-9]+$ ]] || continue
            now="$(date +%s)"
            age=$((now - phase_epoch))
            limit="$(phase_limit "$phase_name")"
            touch "$HEARTBEAT"
            if [ "$age" -gt "$limit" ]; then
                log "watchdog: phase=$phase_name stalled for ${age}s (limit ${limit}s); requesting systemd restart"
                kill -TERM "$main_pid"
                exit 0
            fi
        done
    ) &
    HEARTBEAT_PID=$!
}
cleanup() {
    if [ -n "${HEARTBEAT_PID:-}" ]; then
        kill "$HEARTBEAT_PID" 2>/dev/null || true
        wait "$HEARTBEAT_PID" 2>/dev/null || true
    fi
    if [ -f "$PIDFILE" ] && [ "$(cat "$PIDFILE" 2>/dev/null)" = "$$" ]; then
        rm -f "$PIDFILE"
    fi
    rm -f "$PHASEFILE" "$HEARTBEAT"
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT

# Architecture (owner directive 2026-10-31): Qwen implements and remediates; an
# INDEPENDENT model reviews adversarially; the harness (gates) is the objective
# verification authority. The implementation agent is never the sole authority on
# its own correctness.
# Builder: LOCAL qwen (litellm/qwen -> openai/qwen38-flash-next). First in the
# chain so the builder is the free local seat; hosted routes are fallbacks.
WORKER_MODELS="${WORKER_MODELS:-litellm/qwen litellm/minimax-m3.1-flash-nvidia litellm/glm-5.3-nvidia litellm/glm-5.3-flash-nvidia litellm/luna}"
# Reviewer: DeepSeek V4.1 Flash. Verified 2026-10-31 against the live proxy:
# alias `deepseek-v4.1-flash` resolves to upstream `deepseek/deepseek-flash`
# at api.deepseek.com (official DeepSeek API — a genuinely different family
# from the qwen builder) and answers a live PING through the piagent+pi harness
# path. Model identity is the upstream string, never the alias
# (vault: model-identity-is-the-upstream-string-not-the-alias).
REVIEWER_MODEL="${REVIEWER_MODEL:-litellm/deepseek-v4.1-flash}"
# Reviewer fallbacks MUST be a different upstream family than the builder
# (qwen38-flash-next), or the "independent" review is fake. `swarm-worker` is
# deliberately excluded: it is the SAME upstream as qwen. Order prefers the
# DeepSeek mirror first, then cross-family: grok=xai/grok-4.6,
# minimax=minimaxai/*, luna=gpt-6-luna.
REVIEWER_FALLBACKS="${REVIEWER_FALLBACKS:-litellm/deepseek-v4.1-flash-nvidia litellm/grok litellm/minimax-m3.1-flash-nvidia litellm/luna}"
ITER_TIMEOUT="${ITER_TIMEOUT:-7200}"
REVIEW_TIMEOUT="${REVIEW_TIMEOUT:-5400}"
REVIEW_FAIL_LIMIT="${REVIEW_FAIL_LIMIT:-2}"
MAX_ITER="${MAX_ITER:-30}"
STALL_LIMIT="${STALL_LIMIT:-3}"
FAIL_LIMIT="${FAIL_LIMIT:-3}"
PROBE_TIMEOUT="${PROBE_TIMEOUT:-300}"
HEALTH_TIMEOUT="${HEALTH_TIMEOUT:-90}"
PI_CMD="${PI_CMD:-pi}"
BUILDER_FILE="$ROOT/ralph/builder.md"
PROMPT_FILE="$ROOT/ralph/PROMPT.md"
VALIDATOR_FILE="$ROOT/ralph/validator.md"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITARM="${DEVKITARM:-/opt/devkitpro/devkitARM}"

state_get() { sed -n "s/^$1=//p" "$STATE" 2>/dev/null | tail -1; }
state_set() { local kv="$1=$2" k="${1%%=*}"; grep -v "^$k=" "$STATE" 2>/dev/null >"$STATE.t" || true; echo "$kv" >>"$STATE.t"; mv "$STATE.t" "$STATE"; }

# The review-record state machine lives in its own file so that it can be run
# without starting the loop -- see tools/runner/test_review_state.sh, which
# asserts it. A record whose failure mode is invisible (a review that reads
# "done" and checked nothing) is a record that must be testable.
# shellcheck source=tools/runner/review_state.sh
. "$ROOT/tools/runner/review_state.sh"

# --- model identity: the upstream string, never the alias --------------------
# (vault: model-identity-is-the-upstream-string-not-the-alias). On a shared
# litellm proxy two aliases can be ONE upstream -- on this proxy today `qwen`
# and `swarm-worker` are both openai/qwen38-flash-next, and `luna` and
# `luna-compaction` are both openai/responses/gpt-6-luna -- and two hosts can
# carry ONE model family (`deepseek-v4.1-flash` at api.deepseek.com and
# `deepseek-v4.1-flash-nvidia` at integrate.api.nvidia.com are the same weights
# twice, which is exactly the pair this script's own comment calls "the DeepSeek
# mirror"). Comparing alias strings passes every one of those, which is what
# external review 2026-11-01 finding F3 found. Resolution is the proxy's
# /model/info when it answers, the committed map in tools/runner/
# model_identity.py otherwise, and NOTHING if neither knows the alias -- and an
# unresolved alias is refused, because "could not tell" must not reach the same
# path as "proved different".
model_upstream() { python3 "$ROOT/tools/runner/model_identity.py" upstream "$1" 2>/dev/null || true; }
model_family()   { python3 "$ROOT/tools/runner/model_identity.py" family   "$1" 2>/dev/null || true; }

# reviewer_reject_reason <candidate> — prints WHY this route may NOT review the
# builder's work, or nothing when it may. RUN_MODEL / RUN_UPSTREAM / RUN_FAMILY
# are the current iteration's builder route, resolved below (and re-resolved if
# the worker falls back to another route).
#
# The answer is in the TEXT, and the exit status is always 0 by design: a reason
# is not a failure, and a caller that wants to branch on it branches on `-n`.
# The function used to be called `reviewer_rejected`, which reads like a
# predicate while always exiting 0 — so `if reviewer_rejected x; then` or
# `reviewer_rejected x >/dev/null && continue` would have silently refused (or
# accepted) every candidate. The name now says what it returns, and
# tools/runner/test_review_state.sh pins the status on both arms.
reviewer_reject_reason() {
    local cand="$1" cand_upstream cand_family
    [ "$cand" = "$RUN_MODEL" ] && { printf 'it is the builder route itself\n'; return 0; }
    if [ -z "${RUN_FAMILY:-}" ]; then
        printf 'the builder route %s resolves to no upstream, so no reviewer can be proved different from it\n' "$RUN_MODEL"
        return 0
    fi
    cand_upstream="$(model_upstream "$cand")"
    cand_family="$(model_family "$cand")"
    if [ -z "$cand_family" ]; then
        printf 'it resolves to no upstream, so it cannot be proved independent\n'
        return 0
    fi
    if [ -n "$cand_upstream" ] && [ "$cand_upstream" = "$RUN_UPSTREAM" ]; then
        printf 'it is the builder upstream %s under a different alias\n' "$cand_upstream"
        return 0
    fi
    if [ "$cand_family" = "$RUN_FAMILY" ]; then
        printf 'it is the builder family %s (%s and %s are one model on two hosts)\n' "$cand_family" "$cand" "$RUN_MODEL"
        return 0
    fi
    return 0
}

pi_run() {
    ( cd "$ROOT" && timeout -k 30 "$ITER_TIMEOUT" sudo -n -H -u piagent \
        "$PI_CMD" --model "$1" -p "$(cat "$2")" --no-session ) >"$3" 2>"$4"
}

model_alive() {
    timeout "$HEALTH_TIMEOUT" sudo -n -H -u piagent "$PI_CMD" --model "$1" \
        -p "Reply exactly PING_OK" --no-session 2>/dev/null | grep -q PING_OK
}

recover_session() {
    local d="/home/piagent/.pi/agent/sessions/--mnt-nas-Development-mdk-sdl--"
    local latest
    latest="$(sudo -n ls -t "$d" 2>/dev/null | head -1)"
    [ -n "$latest" ] && sudo -n cp "$d/$latest" "$RUN_DIR/iter-$1.session.jsonl" 2>/dev/null
}

run_gates() {
    local out="$1" rc=0
    {
        echo "== gates $(date -Is) =="
        sudo -n -H -u piagent env DEVKITPRO="$DEVKITPRO" DEVKITARM="$DEVKITARM" \
            make -C "$ROOT/tests/scripts" 2>&1 | tail -20
        [ "${PIPESTATUS[0]}" -ne 0 ] && { echo "GATE FAIL: tests/scripts"; rc=1; }
        sudo -n -H -u piagent bash "$ROOT/tools/check.sh" 2>&1 | tail -10
        [ "${PIPESTATUS[0]}" -ne 0 ] && { echo "GATE FAIL: tools/check.sh"; rc=1; }
        sudo -n -H -u piagent env DEVKITPRO="$DEVKITPRO" DEVKITARM="$DEVKITARM" \
            make -C "$ROOT/mdk_3ds" 2>&1 | tail -20
        [ "${PIPESTATUS[0]}" -ne 0 ] && { echo "GATE FAIL: mdk_3ds"; rc=1; }
        [ "$rc" -eq 0 ] && echo "GATES: PASS"
    } >"$out" 2>&1
    return "$rc"
}

guard() {
    local muse_pid
    muse_pid="$(cat "$ROOT/logs/ralph/loop.pid" 2>/dev/null)"
    if [ -n "$muse_pid" ] && kill -0 "$muse_pid" 2>/dev/null; then
        echo "FATAL: Muse ralph.sh (pid $muse_pid) is alive. Stop it first." >&2
        exit 2
    fi
    touch "$ROOT/logs/ralph/disabled"
    echo "$$" >"$PIDFILE"
    local probe_ok=0 model
    for model in $WORKER_MODELS; do
        if timeout "$PROBE_TIMEOUT" sudo -n -H -u piagent "$PI_CMD" --model "$model" \
            -p "Using your bash tool, run exactly: touch '$ROOT/.ralph-pi-wtest' && stat -c '%U:%G %a %n' '$ROOT/.ralph-pi-wtest' && rm '$ROOT/.ralph-pi-wtest' && echo WTEST_OK. Reply with exactly WTEST_OK, or WTEST_FAIL plus the error." \
            --no-session 2>&1 | grep -q WTEST_OK; then
            probe_ok=1
            log "write probe passed on $model"
            break
        fi
        log "write probe failed on $model"
    done
    if [ "$probe_ok" -ne 1 ]; then
        log "FATAL: pi write probe failed on every route"
        exit 3
    fi
}

status() {
    echo "== ralph-pi status =="
    if [ -f "$PHASEFILE" ]; then
        printf 'phase: '; tr '\n' ' ' <"$PHASEFILE"; printf '\n'
        local epoch phase_pid
        epoch="$(sed -n 's/^epoch=//p' "$PHASEFILE")"
        phase_pid="$(sed -n 's/^pid=//p' "$PHASEFILE")"
        [ -n "$epoch" ] && printf 'phase age: %ss\n' "$(( $(date +%s) - epoch ))"
        if [ -n "$phase_pid" ] && kill -0 "$phase_pid" 2>/dev/null; then
            echo "process: active (pid $phase_pid)"
        else
            echo "process: stale phase marker"
        fi
    else
        echo "phase: not running"
    fi
    [ -f "$STATE" ] && cat "$STATE"
    # Review cost guardrail summary
    _rc="$(state_get REVIEW_CALLS)"; _rc="${_rc:-0}"
    _est="$(python3 -c "print('%.4f' % ($_rc * 0.003))" 2>/dev/null || echo n/a)"
    echo "REVIEW_COST_EST_USD: ~\$${_est} ($_rc calls @ ~\$0.003/call)"
    if command -v systemctl >/dev/null 2>&1; then
        systemctl --no-pager --plain show -p ActiveState -p MainPID -p NRestarts -p Result ralph-pi-mdk.service 2>/dev/null || true
    fi
    echo "--- last summary ---"
    tail -8 "$SUMMARY" 2>/dev/null || true
}

# --- Risk-based review helpers -------------------------------------------------
# Extract the builder-declared risk level from the committed state.
# Source of truth: docs/CONTINUATION.md "Risk: low|medium|high" line; fallback to
# a [risk: ...] tag in the latest commit message; default medium (conservative).
extract_risk() {
    local risk=""
    if [ -f "$ROOT/docs/CONTINUATION.md" ]; then
        risk="$(grep -iE '^[[:space:]]*Risk[[:space:]]*:' "$ROOT/docs/CONTINUATION.md" 2>/dev/null | head -1 | sed -E 's/^[^:]*:[[:space:]]*//; s/[[:space:]].*//' | tr '[:upper:]' '[:lower:]')"
    fi
    if [ -z "$risk" ]; then
        risk="$(git -C "$ROOT" log -1 --pretty=%B 2>/dev/null | grep -oiE '\[risk:[[:space:]]*(low|medium|high)\]' | head -1 | sed -E 's/.*:[[:space:]]*//; s/\].*//' | tr '[:upper:]' '[:lower:]')"
    fi
    case "$risk" in
        low|medium|high) printf '%s' "$risk" ;;
        *) printf 'medium' ;;
    esac
}

# Safety-net: force high risk when the diff touches protected/structural surfaces
# or is broad, or when a failure persists. Returns 0 (true) if forced high.
risk_force_high() {
    local base="$1" head="$2"
    # protected contract files changed
    local pc
    pc="$(git -C "$ROOT" diff --name-only "$base".."$head" 2>/dev/null | grep -E '^(AGENTS\.md|SPEC\.md|SPEC-COMPLETION\.md|tools/check\.sh|ralph-pi\.sh|ralph/builder\.md|ralph/validator\.md|tools/runner/gates_batch_integrity\.py)$' || true)"
    [ -n "$pc" ] && return 0
    # broad diff
    local nfiles
    nfiles="$(git -C "$ROOT" diff --name-only "$base".."$head" 2>/dev/null | wc -l)"
    [ "${nfiles:-0}" -gt 6 ] && return 0
    local nlines
    nlines="$(git -C "$ROOT" diff --numstat "$base".."$head" 2>/dev/null | awk '{a+=$1; d+=$2} END{print a+d+0}')"
    [ "${nlines:-0}" -gt 500 ] && return 0
    # test weakening suspicion: net deletions in test files
    local tdel
    tdel="$(git -C "$ROOT" diff --numstat "$base".."$head" 2>/dev/null | grep -iE 'test|golden' | awk '{d+=$2} END{print d+0}')"
    [ "${tdel:-0}" -gt 30 ] && return 0
    # persistent failure
    [ "${FAILS:-0}" -ge 2 ] && return 0
    # milestone marker
    if grep -qiE 'Milestone[[:space:]]*:[[:space:]]*(complete|done|shipped)' "$ROOT/docs/CONTINUATION.md" 2>/dev/null; then
        return 0
    fi
    return 1
}

case "${1:-}" in
    status) status; exit 0 ;;
esac

if ! command -v flock >/dev/null 2>&1; then
    echo "FATAL: flock is required for single-instance protection." >&2
    exit 8
fi
exec 9>"$LOGDIR/loop.lock"
if ! flock -n 9; then
    echo "FATAL: another ralph-pi process holds $LOGDIR/loop.lock" >&2
    exit 2
fi

ITER=0; STALL=0; FAILS=0
# Risk-based review state (persisted across restarts).
REVIEW_CALLS="$(state_get REVIEW_CALLS)"; REVIEW_CALLS="${REVIEW_CALLS:-0}"
ITERS_SINCE_REVIEW="$(state_get ITERS_SINCE_REVIEW)"; ITERS_SINCE_REVIEW="${ITERS_SINCE_REVIEW:-0}"
REVIEW_SKIP_COUNT="$(state_get REVIEW_SKIP_COUNT)"; REVIEW_SKIP_COUNT="${REVIEW_SKIP_COUNT:-0}"
LAST_REVIEW_ITER="$(state_get LAST_REVIEW_ITER)"; LAST_REVIEW_ITER="${LAST_REVIEW_ITER:-0}"
# REVIEW_FAILS and the owed range are persisted too. They used to be reset here
# on every start, so the commit a BLOCKED review stopped the loop on was owed
# nothing: the next run reviewed only its own new commit and the blocked one was
# never reviewed by anyone (external review 2026-11-01, finding F2). Both go
# through review_int / an explicit read rather than a bare `:-0`, because a state
# file written by an older revision simply lacks the key, and an empty
# REVIEW_FAILS makes the `-ge` limit below error and read as FALSE.
REVIEW_FAILS="$(review_int "$(state_get REVIEW_FAILS)")"
REVIEW_OWED_BASE="$(state_get REVIEW_OWED_BASE)"
# The third way a required review fails to happen (external review 2026-11-01,
# finding H1): the collector is the only thing that turns `invoked` into an
# outcome, and it runs after `wait` — with the whole gates phase in between. A
# restart, watchdog kill, operator stop, OOM or reboot in that window means the
# collector never runs, so the obligation is lost with it. The range is therefore
# persisted at DISPATCH and read back here.
REVIEW_INFLIGHT_BASE="$(state_get REVIEW_INFLIGHT_BASE)"
REVIEW_INFLIGHT_HEAD="$(state_get REVIEW_INFLIGHT_HEAD)"
REVIEW_LEFTOVER="$(state_get REVIEW_STATUS)"
guard
heartbeat_start
RUN_ID="$(date -u +%Y%m%d-%H%M%S)"
RUN_DIR="$LOGDIR/run-$RUN_ID"
mkdir -p "$RUN_DIR"
log "loop start worker_chain=[$WORKER_MODELS] reviewer=$REVIEWER_MODEL reviewer_fallbacks=[$REVIEWER_FALLBACKS] pid=$$ run=$RUN_ID"
if [ -n "$REVIEW_OWED_BASE" ]; then
    log "a review is OWED from an earlier run: ${REVIEW_OWED_BASE:0:8} is unreviewed; the next mandatory review covers ${REVIEW_OWED_BASE:0:8}..HEAD, not just the new commit"
fi
if [ -n "$REVIEW_INFLIGHT_BASE" ]; then
    # Dispatched, never collected: nobody knows whether the reviewer finished,
    # so nobody may claim it did. Fold it into the owed set through the same
    # earliest-base rule as a failed review, and count it as a review that did
    # not happen — the counter's meaning is "consecutive reviews that never
    # reached a verdict", which an interrupted one is.
    log "a review was DISPATCHED and never COLLECTED: ${REVIEW_INFLIGHT_BASE:0:8}..${REVIEW_INFLIGHT_HEAD:0:8} was left marked 'invoked' and the collector never ran (restart, watchdog phase limit, operator stop, OOM or reboot — the gates phase sits in this window)"
    REVIEW_OWED_BASE="$(review_inflight_owed "$REVIEW_OWED_BASE" "$REVIEW_INFLIGHT_BASE")"
    REVIEW_FAILS=$((REVIEW_FAILS + 1))
    state_set REVIEW_OWED_BASE "$REVIEW_OWED_BASE"
    state_set REVIEW_FAILS "$REVIEW_FAILS"
    state_set REVIEW_INFLIGHT_BASE ""
    state_set REVIEW_INFLIGHT_HEAD ""
    REVIEW_INFLIGHT_BASE=""
    REVIEW_INFLIGHT_HEAD=""
    log "that review is recorded as NOT done: ${REVIEW_OWED_BASE:0:8} is owed, and reviews lost without a verdict are now $REVIEW_FAILS of $REVIEW_FAIL_LIMIT"
    if [ "$REVIEW_FAILS" -ge "$REVIEW_FAIL_LIMIT" ]; then
        log "STOP: $REVIEW_FAILS reviews have been lost without a verdict — the harness is being restarted faster than reviews can be collected, which is a phase-limit or reviewer-route problem a model cannot fix from inside the loop"
        exit 7
    fi
elif [ "$REVIEW_LEFTOVER" = "invoked" ]; then
    # `invoked` with no recorded range: a state file written before the range was
    # persisted at dispatch. The obligation cannot be reconstructed from what is
    # in the file — the HEAD line records the PREVIOUS completed iteration, so
    # pointing the owed range at it would mark the wrong commit and hand the real
    # one a false all-clear. Refuse rather than guess.
    log "FATAL: the state file says a review was invoked but records no range to review, so the obligation cannot be reconstructed. Either write 'REVIEW_OWED_BASE=<sha of the unreviewed commit>' into $STATE, or delete $STATE to start clean; then restart the loop."
    exit 7
fi
phase startup
while :; do
    phase route-health
    RUN_MODEL=""
    for candidate in $WORKER_MODELS; do
        if model_alive "$candidate"; then
            RUN_MODEL="$candidate"
            [ "$candidate" != "${WORKER_MODELS%% *}" ] && log "worker route degraded to $candidate"
            break
        else
            log "worker route $candidate UNHEALTHY"
        fi
    done
    if [ -z "$RUN_MODEL" ]; then
        phase route-backoff
        log "no healthy worker route — sleeping 300s"
        sleep 300
        continue
    fi
    # Resolve the builder's identity once, before any reviewer is considered.
    RUN_UPSTREAM="$(model_upstream "$RUN_MODEL")"
    RUN_FAMILY="$(model_family "$RUN_MODEL")"
    log "iter candidate worker=$RUN_MODEL upstream=${RUN_UPSTREAM:-UNRESOLVED} family=${RUN_FAMILY:-UNRESOLVED}"
    ITER=$((ITER+1))
    ILOG="$RUN_DIR/iter-$ITER.log"
    HEAD_BEFORE="$(git -C "$ROOT" rev-parse HEAD)"
    {
        cat "$BUILDER_FILE"
        printf '\n---\n\n'
        cat "$PROMPT_FILE"
        # docs/KILO_RALPH_LOOP_POLICY.md is the owner's GENERIC loop policy. It is
        # not this project's contract, and two of its clauses are looser than the
        # project's (it allows a same-model review with an isolated context, and it
        # makes external review escalation-only). Where they differ the project
        # wins, and the two overrides below are enforced in this script, not just
        # asserted here.
        printf '\n--- LOOP POLICY (generic; overridden by this project, see below) ---\n\n'
        cat <<'POLICY_PRECEDENCE'
This project's own contract — `ralph/builder.md`, `ralph/validator.md` and
`.kilo/agent/adversarial-reviewer.md` — OVERRIDES the generic policy below wherever
they differ. Two overrides are load-bearing and are enforced by the loop script
itself, not left to the reader:

1. INDEPENDENCE. A reviewer dispatched on the builder's own upstream is NOT
   INDEPENDENT and is not dispatched at all; isolating the review context is not
   a substitute (vault: model-identity-is-the-upstream-string-not-the-alias).
   Identity is the resolved upstream and model family, never the alias string:
   two aliases can be one upstream and two hosts can be one family. The reviewer
   chain in `ralph-pi.sh` skips any candidate that resolves to the builder's
   upstream or family, and skips any candidate it cannot resolve at all.
2. FAIL CLOSED. A mandatory review that cannot be dispatched is recorded as
   BLOCKED, counted in REVIEW_FAILS, and STOPS the loop (exit 8). It is never
   logged as a cost skip. A review that WAS dispatched and then died is recorded
   as FAILED, not done, and its commit stays OWED: the range is persisted AT
   DISPATCH, so a restart cannot forget it — even one that kills the loop before
   its own collector runs — and the next mandatory review covers the unreviewed
   commits instead of only the newest one. "Could not check" must not reach the
   same path as "all clear" (vault: a-gate-that-skips-on-missing-input-is-a-pass-generator).

POLICY_PRECEDENCE
        cat "$ROOT/docs/KILO_RALPH_LOOP_POLICY.md" 2>/dev/null || true
        printf '\n\n---\n\n# Loop status block (machine-generated)\n\n'
        echo "- iteration: $ITER (worker $RUN_MODEL, reviewer $REVIEWER_MODEL)"
        echo "- HEAD: $(git -C "$ROOT" log --oneline -1)"
        echo "- dirty tree: $(git -C "$ROOT" status --porcelain | wc -l) file(s) — if any, finish and commit THAT work first (builder rule)."
        echo "- review policy: risk-based (low=skip, medium=batch every 5 iters or milestone, high=immediate). You MUST declare Risk: low|medium|high in docs/CONTINUATION.md (first 20 lines) for this batch, with a one-line reason. Default if omitted: medium."
        NEWEST_REVIEW="$(ls -t "$LOGDIR"/run-*/review-*.md 2>/dev/null | head -1)"
        [ -n "$NEWEST_REVIEW" ] && echo "- newest external review: $NEWEST_REVIEW — investigate and remediate its Critical/High findings (or rebut each with evidence)."
        PREVG="$(state_get LAST_GATES)"
        [ -n "$PREVG" ] && [ "$PREVG" != PASS ] && echo "- WARNING: previous iteration left gates $PREVG — root-cause them before new work."
    } >"$RUN_DIR/.prompt-$ITER.md"
    if ! sudo -n -H -u piagent python3 "$ROOT/tools/runner/gates_batch_integrity.py" \
            >"$RUN_DIR/invariants-$ITER.log" 2>&1; then
        log "iter=$ITER PROJECT INVARIANTS FAILED before worker"
        cat "$RUN_DIR/invariants-$ITER.log" >>"$SUMMARY"
        exit 6
    fi
    log "iter=$ITER start worker=$RUN_MODEL"
    phase worker
    pi_run "$RUN_MODEL" "$RUN_DIR/.prompt-$ITER.md" "$ILOG" "$ILOG.err"
    RC=$?
    [ "$RC" -eq 124 ] && recover_session "$ITER"
    if [ "$RC" -ne 0 ]; then
        NEXT_MODEL=""
        seen=0
        for candidate in $WORKER_MODELS; do
            if [ "$seen" -eq 1 ] && model_alive "$candidate"; then
                NEXT_MODEL="$candidate"
                break
            fi
            [ "$candidate" = "$RUN_MODEL" ] && seen=1
        done
        if [ -n "$NEXT_MODEL" ]; then
            log "iter=$ITER worker rc=$RC on $RUN_MODEL — retrying once on $NEXT_MODEL"
            phase worker-fallback
            RUN_MODEL="$NEXT_MODEL"
            # Re-resolve the builder's identity for the route that ACTUALLY
            # answered (external review 2026-11-01, finding M1). The identity is
            # resolved above, before the worker runs; if the fallback is what
            # wrote the batch and the guard still compares against the abandoned
            # route's family, a reviewer in the new builder's own family passes
            # as independent and reviews its own weights. That is exactly the F3
            # scenario, re-opened through the fallback arm instead of the chain.
            RUN_UPSTREAM="$(model_upstream "$RUN_MODEL")"
            RUN_FAMILY="$(model_family "$RUN_MODEL")"
            log "iter=$ITER builder identity re-resolved for the route that answered: $RUN_MODEL upstream=${RUN_UPSTREAM:-UNRESOLVED} family=${RUN_FAMILY:-UNRESOLVED}"
            pi_run "$NEXT_MODEL" "$RUN_DIR/.prompt-$ITER.md" "$ILOG.fb" "$ILOG.fb.err"
            RC=$?
            [ "$RC" -eq 124 ] && recover_session "$ITER.fb"
            [ "$RC" -eq 0 ] && log "iter=$ITER fallback $NEXT_MODEL completed"
        fi
    fi
    log "iter=$ITER worker rc=$RC session-log=$ILOG"
    HEAD_AFTER="$(git -C "$ROOT" rev-parse HEAD)"
    if [ "$HEAD_AFTER" != "$HEAD_BEFORE" ]; then
        if ! sudo -n -H -u piagent python3 "$ROOT/tools/runner/gates_batch_integrity.py" \
                --base "$HEAD_BEFORE" --head "$HEAD_AFTER" \
                >"$RUN_DIR/integrity-$ITER.log" 2>&1; then
            log "iter=$ITER INTEGRITY GUARD FAILED; stopping"
            cat "$RUN_DIR/integrity-$ITER.log" >>"$SUMMARY"
            exit 6
        fi
        log "iter=$ITER integrity guard PASS"
    fi

    REVIEW_PID=""
    REVIEW_SKIPPED_REASON=""
    REVIEW_BLOCKED=0
    REVIEW_STATUS="none"
    REVIEW_INFLIGHT_BASE=""
    REVIEW_INFLIGHT_HEAD=""
    if [ "$HEAD_AFTER" != "$HEAD_BEFORE" ]; then
        # --- Risk classification -------------------------------------------------
        RISK="$(extract_risk)"
        if risk_force_high "$HEAD_BEFORE" "$HEAD_AFTER"; then
            RISK="high"
            RISK_OVERRIDE=" (safety-net override to high)"
        else
            RISK_OVERRIDE=""
        fi
        log "iter=$ITER risk=$RISK$RISK_OVERRIDE"

        # --- Review decision (risk-based, cost-guarded) --------------------------
        REVIEW_NEEDED=0
        REVIEW_DECISION_REASON=""
        # Count every committed iteration toward the batch checkpoint.
        ITERS_SINCE_REVIEW=$((ITERS_SINCE_REVIEW + 1))
        state_set ITERS_SINCE_REVIEW "$ITERS_SINCE_REVIEW"
        # An owed review outranks the risk policy: a review that was blocked or
        # that died is not made optional by the next commit being small.
        if [ -n "$REVIEW_OWED_BASE" ]; then
            REVIEW_NEEDED=1
            REVIEW_DECISION_REASON="carried forward: ${REVIEW_OWED_BASE:0:8} is still unreviewed from an earlier iteration"
        elif [ "${FAILS:-0}" -ge 2 ]; then
            # Failure-triggered: same failure persisted after remediation attempt.
            REVIEW_NEEDED=1
            REVIEW_DECISION_REASON="failure-triggered: $FAILS consecutive gate failures"
        elif [ "$RISK" = "high" ]; then
            REVIEW_NEEDED=1
            REVIEW_DECISION_REASON="high-risk change"
        elif [ "$RISK" = "medium" ]; then
            # Milestone marker in CONTINUATION.md
            if grep -qiE 'Milestone[[:space:]]*:[[:space:]]*(complete|done|shipped)' "$ROOT/docs/CONTINUATION.md" 2>/dev/null; then
                REVIEW_NEEDED=1
                REVIEW_DECISION_REASON="milestone checkpoint (medium-risk batch)"
            elif [ "$ITERS_SINCE_REVIEW" -ge 5 ]; then
                REVIEW_NEEDED=1
                REVIEW_DECISION_REASON="5-iteration batch checkpoint (medium-risk)"
            else
                REVIEW_NEEDED=0
                REVIEW_DECISION_REASON="medium-risk batched ($ITERS_SINCE_REVIEW/5 iterations)"
            fi
        else
            # low risk
            REVIEW_NEEDED=0
            REVIEW_DECISION_REASON="low-risk routine change (skipped)"
        fi

        state_set LAST_RISK "$RISK"
        state_set LAST_REVIEW_DECISION "$REVIEW_DECISION_REASON"

        if [ "$REVIEW_NEEDED" -eq 1 ]; then
            RP="$RUN_DIR/.review-prompt-$ITER.md"
            REVIEW_BASE="$HEAD_BEFORE"
            [ -n "$REVIEW_OWED_BASE" ] && REVIEW_BASE="$REVIEW_OWED_BASE"
            {
                cat "$VALIDATOR_FILE"
                printf '\n\n--- REVIEW SCOPE ---\nReview ONLY commits %s..%s plus uncommitted work. Do not edit files.\n' "${REVIEW_BASE:0:8}" "${HEAD_AFTER:0:8}"
                if [ "$REVIEW_BASE" != "$HEAD_BEFORE" ]; then
                    printf 'This range is wider than this iteration because an earlier review could not be dispatched or died; %s..%s was never reviewed by anyone.\n' "${REVIEW_BASE:0:8}" "${HEAD_BEFORE:0:8}"
                fi
                printf '\n--- RISK CONTEXT ---\nDeclared risk: %s. Decision: %s.\n' "$RISK" "$REVIEW_DECISION_REASON"
            } >"$RP"
            REVIEW_MODEL=""
            REJECT="$(reviewer_reject_reason "$REVIEWER_MODEL")"
            if [ -n "$REJECT" ]; then
                # A reviewer on the builder's own upstream (or family, or one
                # whose identity cannot be resolved at all) is not an independent
                # review, so it is not dispatched -- but the review is still OWED.
                # Try the cross-family fallbacks instead of dropping it.
                log "iter=$ITER reviewer $REVIEWER_MODEL refused: $REJECT — trying the fallbacks"
            elif model_alive "$REVIEWER_MODEL"; then
                REVIEW_MODEL="$REVIEWER_MODEL"
            else
                log "iter=$ITER reviewer route $REVIEWER_MODEL UNHEALTHY"
            fi
            if [ -z "$REVIEW_MODEL" ]; then
                for candidate in $REVIEWER_FALLBACKS; do
                    REJECT="$(reviewer_reject_reason "$candidate")"
                    if [ -n "$REJECT" ]; then
                        log "iter=$ITER reviewer fallback $candidate refused: $REJECT"
                        continue
                    fi
                    if model_alive "$candidate"; then
                        REVIEW_MODEL="$candidate"
                        log "iter=$ITER reviewer falling back to $candidate"
                        break
                    fi
                    log "iter=$ITER reviewer fallback $candidate UNHEALTHY"
                done
            fi
            # Record who was actually on each side, by upstream and family, so a
            # later reader can check the independence claim instead of trusting
            # the alias in the log line.
            {
                echo "builder:  $RUN_MODEL upstream=${RUN_UPSTREAM:-UNRESOLVED} family=${RUN_FAMILY:-UNRESOLVED}"
                if [ -n "$REVIEW_MODEL" ]; then
                    echo "reviewer: $REVIEW_MODEL upstream=$(model_upstream "$REVIEW_MODEL") family=$(model_family "$REVIEW_MODEL")"
                else
                    echo "reviewer: none dispatched"
                fi
                echo "scope:    ${REVIEW_BASE:0:8}..${HEAD_AFTER:0:8}"
                echo "rule:     independence is the resolved upstream and model family, never the alias string."
                echo "source:   the proxy's /model/info when it answers, else the committed map in tools/runner/model_identity.py; an unresolved alias is refused."
            } >"$RUN_DIR/review-$ITER.identity.txt"
            if [ -n "$REVIEW_MODEL" ]; then
                phase review
                ( timeout -k 30 "$REVIEW_TIMEOUT" sudo -n -H -u piagent "$PI_CMD" \
                    --model "$REVIEW_MODEL" -p "$(cat "$RP")" --no-session \
                    >"$RUN_DIR/review-$ITER.md" 2>"$RUN_DIR/review-$ITER.err"
                  echo $? >"$RUN_DIR/review-$ITER.rc" ) &
                REVIEW_PID=$!
                REVIEW_STATUS="invoked"
                state_set REVIEW_STATUS "invoked"
                # Persist the range NOW, at dispatch, not in the collector: the
                # collector runs after `wait`, with the gates phase in between,
                # and a loop killed in that window would otherwise leave an
                # unreviewed commit and no record of it (H1).
                REVIEW_INFLIGHT_BASE="$REVIEW_BASE"
                REVIEW_INFLIGHT_HEAD="$HEAD_AFTER"
                state_set REVIEW_INFLIGHT_BASE "$REVIEW_INFLIGHT_BASE"
                state_set REVIEW_INFLIGHT_HEAD "$REVIEW_INFLIGHT_HEAD"
                log "iter=$ITER review INVOKED on $REVIEW_MODEL — $REVIEW_DECISION_REASON"
            else
                # FAIL CLOSED. A mandatory review with no eligible reviewer is a
                # BLOCKED review, not a skipped one. Recording it as a cost skip
                # would let a high-risk batch advance unreviewed with REVIEW_FAILS
                # still at zero — the same defect as a gate that skips on missing
                # input and prints the success banner.
                REVIEW_BLOCKED=1
                REVIEW_STATUS="blocked"
                REVIEW_SKIPPED_REASON="no eligible reviewer route for a mandatory review"
                {
                    echo "REVIEW BLOCKED — NOT REVIEWED, NOT SKIPPED."
                    echo "Reason the review was mandatory: $REVIEW_DECISION_REASON"
                    echo "Builder route: $RUN_MODEL (upstream ${RUN_UPSTREAM:-UNRESOLVED}, family ${RUN_FAMILY:-UNRESOLVED})"
                    echo "Configured reviewer: $REVIEWER_MODEL (must not be the builder's route)"
                    echo "Fallbacks tried: $REVIEWER_FALLBACKS"
                    echo "Every candidate was either unhealthy, refused as the builder's own"
                    echo "upstream/family, or unresolvable, so no independent review could be"
                    echo "dispatched. The loop stops here rather than advancing an unreviewed"
                    echo "commit as if the review had been optional."
                } >"$RUN_DIR/review-$ITER.md"
                REVIEW_FAILS=$((REVIEW_FAILS+1))
                state_set REVIEW_STATUS "blocked"
                state_set REVIEW_FAILS "$REVIEW_FAILS"
                REVIEW_OWED_BASE="$(review_owed blocked "$REVIEW_OWED_BASE" "$HEAD_BEFORE")"
                state_set REVIEW_OWED_BASE "$REVIEW_OWED_BASE"
                state_set LAST_REVIEW_DECISION "BLOCKED: $REVIEW_DECISION_REASON"
                log "iter=$ITER review BLOCKED — $REVIEW_DECISION_REASON: no eligible reviewer route"
            fi
        else
            log "iter=$ITER review SKIPPED — $REVIEW_DECISION_REASON"
            REVIEW_SKIPPED_REASON="$REVIEW_DECISION_REASON"
            REVIEW_STATUS="skipped"
            state_set REVIEW_STATUS "skipped"
            REVIEW_SKIP_COUNT=$((REVIEW_SKIP_COUNT + 1))
            state_set REVIEW_SKIP_COUNT "$REVIEW_SKIP_COUNT"
        fi
    fi

    phase gates
    GF="$RUN_DIR/gates-$ITER.log"
    if run_gates "$GF"; then
        state_set LAST_GATES PASS
        FAILS=0
    else
        state_set LAST_GATES FAIL
        FAILS=$((FAILS+1))
        log "iter=$ITER GATES FAILED (see $GF) fail_count=$FAILS"
    fi

    phase review-collect
    if [ "$HEAD_AFTER" != "$HEAD_BEFORE" ]; then
        if [ -n "$REVIEW_PID" ]; then
            wait "$REVIEW_PID"
            # A missing, empty or corrupt .rc is a failure, not a zero. The old
            # test was `[ "$RRC" -ne 0 ]`, and `[` itself fails on a non-numeric
            # operand, which `if` reads as FALSE: a killed subshell that left an
            # empty .rc took the success branch (external review 2026-11-01 F1).
            RRC="$(review_rc_normalise "$(cat "$RUN_DIR/review-$ITER.rc" 2>/dev/null)")"
            OUTCOME="$(review_outcome "$RRC" "$RUN_DIR/review-$ITER.md")"
            if [ "$OUTCOME" != "done" ]; then
                echo "REVIEWER FAILED model=$REVIEW_MODEL rc=$RRC" >>"$RUN_DIR/review-$ITER.md"
                log "iter=$ITER REVIEWER FAILED rc=$RRC — the commit is still unreviewed and is now owed"
                REVIEW_FAILS=$((REVIEW_FAILS+1))
            else
                log "iter=$ITER reviewed by $REVIEW_MODEL -> $RUN_DIR/review-$ITER.md"
                REVIEW_FAILS=0
                REVIEW_CALLS=$((REVIEW_CALLS + 1))
                state_set REVIEW_CALLS "$REVIEW_CALLS"
                state_set LAST_REVIEW_ITER "$ITER"
                ITERS_SINCE_REVIEW=0
                state_set ITERS_SINCE_REVIEW "0"
                # Cost guardrail report
                EST_COST="$(python3 -c "print('%.4f' % ($REVIEW_CALLS * 0.003))" 2>/dev/null || echo "n/a")"
                log "iter=$ITER REVIEW CALL #$REVIEW_CALLS — estimated cumulative cost ~\$${EST_COST} USD (rough; ~\$0.003/call)"
            fi
            STALL=0
            # The persisted record says which of the two happened. Writing
            # REVIEW_STATUS=done on both arms is how a reviewer that died became
            # an all-clear in `ralph-pi.sh status`, and the only place
            # REVIEW_FAILS was reset (external review 2026-11-01 F1).
            REVIEW_STATUS="$OUTCOME"
            state_set REVIEW_STATUS "$OUTCOME"
            state_set REVIEW_FAILS "$REVIEW_FAILS"
            REVIEW_OWED_BASE="$(review_owed "$OUTCOME" "$REVIEW_OWED_BASE" "$HEAD_BEFORE")"
            state_set REVIEW_OWED_BASE "$REVIEW_OWED_BASE"
            # The review reached a verdict, so the in-flight marker goes. The
            # outcome is written BEFORE this on purpose: a kill between the two
            # leaves the range marked as owed (one redundant review) rather than
            # `invoked` with no range (which the startup can only refuse).
            REVIEW_INFLIGHT_BASE=""
            REVIEW_INFLIGHT_HEAD=""
            state_set REVIEW_INFLIGHT_BASE ""
            state_set REVIEW_INFLIGHT_HEAD ""
            [ "$REVIEW_FAILS" -ge "$REVIEW_FAIL_LIMIT" ] && { log "STOP: $REVIEW_FAILS consecutive reviewer failures"; exit 7; }
        elif [ "$REVIEW_BLOCKED" -eq 1 ]; then
            # A mandatory review that could not be dispatched. This is NOT a cost
            # skip and must not read as one: stop the loop and say why.
            log "iter=$ITER review BLOCKED (not skipped) — $REVIEW_SKIPPED_REASON; stopping, a mandatory review cannot be waived"
            echo "REVIEW BLOCKED at iter $ITER: $REVIEW_SKIPPED_REASON (see $RUN_DIR/review-$ITER.md)" >>"$SUMMARY"
            exit 8
        else
            # Review not required by the risk policy. Say the reason that made it
            # optional; do not call every non-invoked review a cost skip.
            STALL=0
            log "iter=$ITER review not required — ${REVIEW_SKIPPED_REASON:-no reason recorded} — cumulative review calls: $REVIEW_CALLS, skipped: $REVIEW_SKIP_COUNT"
        fi
    else
        STALL=$((STALL+1))
        log "iter=$ITER no commit — stall_count=$STALL"
    fi

    { echo "ITER=$ITER"; echo "LAST_RC=$RC"; echo "HEAD=$(git -C "$ROOT" rev-parse --short HEAD)"; echo "STALL=$STALL"; echo "FAILS=$FAILS"; echo "REVIEW_CALLS=$REVIEW_CALLS"; echo "ITERS_SINCE_REVIEW=$ITERS_SINCE_REVIEW"; echo "REVIEW_SKIP_COUNT=$REVIEW_SKIP_COUNT"; echo "LAST_REVIEW_ITER=$LAST_REVIEW_ITER"; echo "REVIEW_STATUS=$REVIEW_STATUS"; echo "REVIEW_FAILS=$REVIEW_FAILS"; echo "REVIEW_OWED_BASE=$REVIEW_OWED_BASE"; echo "REVIEW_INFLIGHT_BASE=$REVIEW_INFLIGHT_BASE"; echo "REVIEW_INFLIGHT_HEAD=$REVIEW_INFLIGHT_HEAD"; } >"$STATE"
    phase idle
    [ "$STALL" -ge "$STALL_LIMIT" ] && { log "STOP: $STALL consecutive stalled iterations"; exit 5; }
    [ "$FAILS" -ge "$FAIL_LIMIT" ] && { log "STOP: $FAILS consecutive gate failures"; exit 4; }
    [ "$ITER" -ge "$MAX_ITER" ] && { log "iteration cap ($MAX_ITER) reached"; exit 0; }
done

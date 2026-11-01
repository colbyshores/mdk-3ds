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

ITER=0; STALL=0; FAILS=0; REVIEW_FAILS=0
# Risk-based review state (persisted across restarts).
REVIEW_CALLS="$(state_get REVIEW_CALLS)"; REVIEW_CALLS="${REVIEW_CALLS:-0}"
ITERS_SINCE_REVIEW="$(state_get ITERS_SINCE_REVIEW)"; ITERS_SINCE_REVIEW="${ITERS_SINCE_REVIEW:-0}"
REVIEW_SKIP_COUNT="$(state_get REVIEW_SKIP_COUNT)"; REVIEW_SKIP_COUNT="${REVIEW_SKIP_COUNT:-0}"
LAST_REVIEW_ITER="$(state_get LAST_REVIEW_ITER)"; LAST_REVIEW_ITER="${LAST_REVIEW_ITER:-0}"
guard
heartbeat_start
RUN_ID="$(date -u +%Y%m%d-%H%M%S)"
RUN_DIR="$LOGDIR/run-$RUN_ID"
mkdir -p "$RUN_DIR"
log "loop start worker_chain=[$WORKER_MODELS] reviewer=$REVIEWER_MODEL reviewer_fallbacks=[$REVIEWER_FALLBACKS] pid=$$ run=$RUN_ID"
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
    ITER=$((ITER+1))
    ILOG="$RUN_DIR/iter-$ITER.log"
    HEAD_BEFORE="$(git -C "$ROOT" rev-parse HEAD)"
    {
        cat "$BUILDER_FILE"
        printf '\n---\n\n'
        cat "$PROMPT_FILE"
        printf '\n---\n\n# Loop status block (machine-generated)\n\n'
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
        # Failure-triggered: same failure persisted after remediation attempt.
        if [ "${FAILS:-0}" -ge 2 ]; then
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
            {
                cat "$VALIDATOR_FILE"
                printf '\n\n--- REVIEW SCOPE ---\nReview ONLY commits %s..%s plus uncommitted work. Do not edit files.\n' "${HEAD_BEFORE:0:8}" "${HEAD_AFTER:0:8}"
                printf '\n--- RISK CONTEXT ---\nDeclared risk: %s. Decision: %s.\n' "$RISK" "$REVIEW_DECISION_REASON"
            } >"$RP"
            REVIEW_MODEL=""
            if [ "$REVIEWER_MODEL" = "$RUN_MODEL" ]; then
                log "iter=$ITER reviewer $REVIEWER_MODEL equals worker — skipping self-review"
                REVIEW_SKIPPED_REASON="reviewer equals worker"
            elif model_alive "$REVIEWER_MODEL"; then
                REVIEW_MODEL="$REVIEWER_MODEL"
            else
                log "iter=$ITER reviewer route $REVIEWER_MODEL UNHEALTHY"
                for candidate in $REVIEWER_FALLBACKS; do
                    [ "$candidate" = "$RUN_MODEL" ] && continue
                    if model_alive "$candidate"; then
                        REVIEW_MODEL="$candidate"
                        log "iter=$ITER reviewer falling back to $candidate"
                        break
                    fi
                done
            fi
            if [ -n "$REVIEW_MODEL" ]; then
                phase review
                ( timeout -k 30 "$REVIEW_TIMEOUT" sudo -n -H -u piagent "$PI_CMD" \
                    --model "$REVIEW_MODEL" -p "$(cat "$RP")" --no-session \
                    >"$RUN_DIR/review-$ITER.md" 2>"$RUN_DIR/review-$ITER.err"
                  echo $? >"$RUN_DIR/review-$ITER.rc" ) &
                REVIEW_PID=$!
                log "iter=$ITER review INVOKED on $REVIEW_MODEL — $REVIEW_DECISION_REASON"
            fi
        else
            log "iter=$ITER review SKIPPED — $REVIEW_DECISION_REASON"
            REVIEW_SKIPPED_REASON="$REVIEW_DECISION_REASON"
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
            RRC="$(cat "$RUN_DIR/review-$ITER.rc" 2>/dev/null || echo 1)"
            if [ "$RRC" -ne 0 ] || [ ! -s "$RUN_DIR/review-$ITER.md" ]; then
                echo "REVIEWER FAILED model=$REVIEW_MODEL rc=$RRC" >>"$RUN_DIR/review-$ITER.md"
                log "iter=$ITER REVIEWER FAILED rc=$RRC"
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
            [ "$REVIEW_FAILS" -ge "$REVIEW_FAIL_LIMIT" ] && { log "STOP: $REVIEW_FAILS consecutive reviewer failures"; exit 7; }
        else
            # Review skipped this iteration.
            STALL=0
            log "iter=$ITER review skipped (cost guardrail) — cumulative review calls: $REVIEW_CALLS, skipped: $REVIEW_SKIP_COUNT"
        fi
    else
        STALL=$((STALL+1))
        log "iter=$ITER no commit — stall_count=$STALL"
    fi

    { echo "ITER=$ITER"; echo "LAST_RC=$RC"; echo "HEAD=$(git -C "$ROOT" rev-parse --short HEAD)"; echo "STALL=$STALL"; echo "FAILS=$FAILS"; echo "REVIEW_CALLS=$REVIEW_CALLS"; echo "ITERS_SINCE_REVIEW=$ITERS_SINCE_REVIEW"; echo "REVIEW_SKIP_COUNT=$REVIEW_SKIP_COUNT"; echo "LAST_REVIEW_ITER=$LAST_REVIEW_ITER"; } >"$STATE"
    phase idle
    [ "$STALL" -ge "$STALL_LIMIT" ] && { log "STOP: $STALL consecutive stalled iterations"; exit 5; }
    [ "$FAILS" -ge "$FAIL_LIMIT" ] && { log "STOP: $FAILS consecutive gate failures"; exit 4; }
    [ "$ITER" -ge "$MAX_ITER" ] && { log "iteration cap ($MAX_ITER) reached"; exit 0; }
done

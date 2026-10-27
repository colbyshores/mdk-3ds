#!/usr/bin/env bash
# ralph-pi.sh — the MDK port loop on the Pi harness (shape proven by
# perfect_dark's tools/ralph-pi.sh, mechanics adapted to THIS repo's gates).
#
# Swarm roles (owner directive 2026-10-26, while the Muse subscription is
# out of credits until 2026-11-01):
#   worker     pi as piagent, model litellm/glm-5.3-flash-nvidia
#              (fallback litellm/luna on fast-fail)
#   reviewer   pi as piagent, model litellm/minimax-m3.1-flash-nvidia,
#              read-only by instruction, over each iteration's diff
#   orchestrator  this script: gates, progress/stall detection, review
#              dispatch — the worker can never influence them
# The Muse loop (ralph.sh) stays parked: this script creates
# logs/ralph/disabled so the crontab watchdog cannot resurrect it into a
# second writer. Delete that file to resume Muse on Nov 1.
#
# Usage:
#   systemd-run --user --unit=ralph-pi-mdk --property=Restart=on-failure \
#       --property=RestartSec=120 ./ralph-pi.sh
#   ./ralph-pi.sh status
#   systemctl --user stop ralph-pi-mdk        # kill switch (kills the cgroup,
#                                             # including piagent's children)
set -u
umask 002   # sudo preserves the caller's umask; piagent's files stay group-writable
ROOT="$(cd "$(dirname "$0")" && pwd)"
LOGDIR="$ROOT/logs/ralph-pi"
STATE="$LOGDIR/state"
SUMMARY="$LOGDIR/summary.log"
PIDFILE="$LOGDIR/loop.pid"
mkdir -p "$LOGDIR"

WORKER_MODEL="${WORKER_MODEL:-litellm/glm-5.3-flash-nvidia}"
FALLBACK_MODEL="${FALLBACK_MODEL:-litellm/luna}"
REVIEWER_MODEL="${REVIEWER_MODEL:-litellm/minimax-m3.1-flash-nvidia}"
ITER_TIMEOUT="${ITER_TIMEOUT:-5400}"
REVIEW_TIMEOUT="${REVIEW_TIMEOUT:-5400}"
MAX_ITER="${MAX_ITER:-30}"
STALL_LIMIT="${STALL_LIMIT:-3}"
FAIL_LIMIT="${FAIL_LIMIT:-3}"
PROBE_TIMEOUT="${PROBE_TIMEOUT:-600}"
HEALTH_TIMEOUT="${HEALTH_TIMEOUT:-90}"
PI_CMD="${PI_CMD:-pi}"
BUILDER_FILE="$ROOT/ralph/builder.md"
PROMPT_FILE="$ROOT/ralph/PROMPT.md"
VALIDATOR_FILE="$ROOT/ralph/validator.md"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITARM="${DEVKITARM:-/opt/devkitpro/devkitARM}"

log() { echo "[$(date -Is)] $*" >>"$SUMMARY"; }

state_get() { sed -n "s/^$1=//p" "$STATE" 2>/dev/null | tail -1; }
state_set() { local kv="$1=$2" k="${1%%=*}"; grep -v "^$k=" "$STATE" 2>/dev/null >"$STATE.t" || true; echo "$kv" >>"$STATE.t"; mv "$STATE.t" "$STATE"; }

pi_run() { # $1=model $2=prompt-file $3=out $4=err
    ( cd "$ROOT" && timeout -k 30 "$ITER_TIMEOUT" sudo -n -H -u piagent \
        "$PI_CMD" --model "$1" -p "$(cat "$2")" --no-session ) >"$3" 2>"$4"
}

# Model health probe: a route can die mid-project (measured 2026-10-26:
# litellm/glm-5.3-flash-nvidia hung for hours while luna/minimax stayed up).
# Without this check every iteration burns its wall clock on a dead route.
model_alive() { # $1=model -> 0 alive
    timeout "$HEALTH_TIMEOUT" sudo -n -H -u piagent "$PI_CMD" --model "$1" \
        -p "Reply exactly PING_OK" --no-session 2>/dev/null | grep -q PING_OK
}

# pi buffers its transcript and loses it when killed; recover the session file
# so a timed-out iteration is at least diagnosable.
recover_session() { # $1=iteration
    local d="/home/piagent/.pi/agent/sessions/--mnt-nas-Development-mdk-sdl--"
    local latest
    latest="$(sudo -n ls -t "$d" 2>/dev/null | head -1)"
    [ -n "$latest" ] && sudo -n cp "$d/$latest" "$RUN_DIR/iter-$1.session.jsonl" 2>/dev/null
}

run_gates() { # independent verification; never delegated to the worker.
    # Runs as piagent in a FRESH process (the worker cannot influence it).
    # NOT as coleshores: this NFS mount does not honor setgid group
    # inheritance (measured 2026-10-26), so worker-created build artifacts
    # are piagent:piagent and a coleshores rebuild fails on them.
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
    # never two writers: the Muse loop must be parked
    local mp; mp="$(cat "$ROOT/logs/ralph/loop.pid" 2>/dev/null)"
    if [ -n "${mp:-}" ] && kill -0 "$mp" 2>/dev/null; then
        echo "FATAL: Muse ralph.sh (pid $mp) is alive. Stop it first — one writer." >&2; exit 2
    fi
    touch "$ROOT/logs/ralph/disabled"   # block the crontab watchdog
    # one ralph-pi at a time
    local op; op="$(cat "$PIDFILE" 2>/dev/null)"
    if [ -n "${op:-}" ] && [ "$op" != "$$" ] && kill -0 "$op" 2>/dev/null; then
        echo "FATAL: ralph-pi already running (pid $op)" >&2; exit 2
    fi
    echo "$$" >"$PIDFILE"
    # full-chain probe: sudo rule, model chain, pi bash tool, repo writes
    if ! timeout "$PROBE_TIMEOUT" sudo -n -H -u piagent "$PI_CMD" --model "$WORKER_MODEL" \
        -p "Using your bash tool, run exactly: touch '$ROOT/.ralph-pi-wtest' && stat -c '%A %G' '$ROOT/.ralph-pi-wtest' && rm '$ROOT/.ralph-pi-wtest' && echo WTEST_OK. Reply with exactly WTEST_OK, or WTEST_FAIL plus the error." \
        --no-session 2>&1 | grep -q WTEST_OK; then
        echo "FATAL: pi write probe failed (sudo rule, model chain, or repo writes)." >&2; exit 3
    fi
}

status() {
    echo "== ralph-pi status =="
    [ -f "$STATE" ] && cat "$STATE"
    echo "--- last summary ---"; tail -8 "$SUMMARY" 2>/dev/null
}

case "${1:-}" in
    status) status; exit 0 ;;
esac

guard
RUN_ID="$(date -u +%Y%m%d-%H%M%S)"
RUN_DIR="$LOGDIR/run-$RUN_ID"
mkdir -p "$RUN_DIR"
log "loop start worker=$WORKER_MODEL fallback=$FALLBACK_MODEL reviewer=$REVIEWER_MODEL pid=$$ run=$RUN_ID"
ITER=0; STALL=0; FAILS=0
while :; do
    # Route health FIRST: a dead route must not consume the iteration cap.
    RUN_MODEL="$WORKER_MODEL"
    if ! model_alive "$RUN_MODEL"; then
        log "worker route $RUN_MODEL UNHEALTHY"
        if model_alive "$FALLBACK_MODEL"; then
            RUN_MODEL="$FALLBACK_MODEL"
            log "falling back to $RUN_MODEL"
        else
            log "no healthy worker route — sleeping 300s"
            sleep 300
            continue
        fi
    fi
    ITER=$((ITER+1))
    ILOG="$RUN_DIR/iter-$ITER.log"
    HEAD_BEFORE="$(git -C "$ROOT" rev-parse HEAD)"
    {
        cat "$BUILDER_FILE"
        printf '\n---\n\n'
        cat "$PROMPT_FILE"
        printf '\n---\n\n# Loop status block (machine-generated)\n\n'
        echo "- iteration: $ITER (worker $WORKER_MODEL, reviewer $REVIEWER_MODEL)"
        echo "- HEAD: $(git -C "$ROOT" log --oneline -1)"
        echo "- dirty tree: $(git -C "$ROOT" status --porcelain | wc -l) file(s) — if any, finish and commit THAT work first (builder rule)."
        NEWEST_REVIEW="$(ls -t "$LOGDIR"/run-*/review-*.md 2>/dev/null | head -1)"
        [ -n "$NEWEST_REVIEW" ] && echo "- newest external review: $NEWEST_REVIEW — address its BLOCKER/MAJOR findings."
        PREVG="$(state_get LAST_GATES)"
        [ -n "$PREVG" ] && [ "$PREVG" != "PASS" ] && echo "- WARNING: previous iteration left gates $PREVG — root-cause them before new work."
    } >"$RUN_DIR/.prompt-$ITER.md"
    log "iter=$ITER start worker=$RUN_MODEL"
    pi_run "$RUN_MODEL" "$RUN_DIR/.prompt-$ITER.md" "$ILOG" "$ILOG.err"
    RC=$?
    [ "$RC" -eq 124 ] && recover_session "$ITER"
    if [ "$RC" -ne 0 ] && [ "$RUN_MODEL" != "$FALLBACK_MODEL" ] && model_alive "$FALLBACK_MODEL"; then
        log "iter=$ITER worker rc=$RC on $RUN_MODEL — retrying once on fallback $FALLBACK_MODEL"
        pi_run "$FALLBACK_MODEL" "$RUN_DIR/.prompt-$ITER.md" "$ILOG.fb" "$ILOG.fb.err"
        RC=$?
        [ "$RC" -eq 124 ] && recover_session "$ITER.fb"
        [ "$RC" -eq 0 ] && log "iter=$ITER fallback $FALLBACK_MODEL completed"
    fi
    log "iter=$ITER worker rc=$RC session-log=$ILOG"

    # gates: independent, after every iteration
    GF="$RUN_DIR/gates-$ITER.log"
    if run_gates "$GF"; then
        state_set LAST_GATES PASS; FAILS=0
    else
        state_set LAST_GATES FAIL; FAILS=$((FAILS+1))
        log "iter=$ITER GATES FAILED (see $GF) fail_count=$FAILS"
    fi

    HEAD_AFTER="$(git -C "$ROOT" rev-parse HEAD)"
    if [ "$HEAD_AFTER" != "$HEAD_BEFORE" ]; then
        STALL=0
        # adversarial review of the batch — read-only by instruction
        RP="$LOGDIR/.review-prompt-$ITER.md"
        {
            cat "$VALIDATOR_FILE"
            printf '\n\n--- REVIEW SCOPE ---\nReview ONLY the changes in commits %s..%s plus any uncommitted work (bound the scope with git log/diff first; do not read unrelated files). You must NOT edit any file; read-only inspection only. Work efficiently: a review that times out is a failed review.\n' "${HEAD_BEFORE:0:8}" "${HEAD_AFTER:0:8}"
        } >"$RP"
        REVIEW_MODEL="$REVIEWER_MODEL"
        if ! model_alive "$REVIEW_MODEL"; then
            log "iter=$ITER reviewer route $REVIEW_MODEL UNHEALTHY"
            if model_alive "$FALLBACK_MODEL"; then
                REVIEW_MODEL="$FALLBACK_MODEL"
                log "iter=$ITER reviewer falling back to $REVIEW_MODEL"
            else
                REVIEW_MODEL=""
            fi
        fi
        if [ -n "$REVIEW_MODEL" ]; then
            timeout -k 30 "$REVIEW_TIMEOUT" sudo -n -H -u piagent "$PI_CMD" \
                --model "$REVIEW_MODEL" -p "$(cat "$RP")" --no-session \
                >"$RUN_DIR/review-$ITER.md" 2>"$RUN_DIR/review-$ITER.err"
            RRC=$?
        else
            RRC=127
        fi
        if [ "$RRC" -ne 0 ] || [ ! -s "$RUN_DIR/review-$ITER.md" ]; then
            echo "REVIEWER FAILED model=$REVIEW_MODEL rc=$RRC" >>"$RUN_DIR/review-$ITER.md"
            log "iter=$ITER REVIEWER FAILED rc=$RRC (recorded, not silently skipped)"
        else
            log "iter=$ITER reviewed by $REVIEW_MODEL -> $RUN_DIR/review-$ITER.md"
        fi
    else
        STALL=$((STALL+1))
        log "iter=$ITER no commit — stall_count=$STALL"
    fi

    { echo "ITER=$ITER"; echo "LAST_RC=$RC"; echo "HEAD=$(git -C "$ROOT" rev-parse --short HEAD)"; echo "STALL=$STALL"; echo "FAILS=$FAILS"; } >"$STATE"
    [ "$STALL" -ge "$STALL_LIMIT" ] && { log "STOP: $STALL consecutive stalled iterations"; exit 4; }
    [ "$FAILS" -ge "$FAIL_LIMIT" ] && { log "STOP: $FAILS consecutive gate failures"; exit 5; }
    [ "$ITER" -ge "$MAX_ITER" ] && { log "iteration cap ($MAX_ITER) reached"; exit 0; }
done

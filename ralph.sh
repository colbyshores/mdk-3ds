#!/usr/bin/env bash
# ralph.sh — autonomous mission loop for the MDK Phase 2 port (Muse swarm).
#
# Each iteration starts a FRESH coding-agent context (no session memory),
# feeds it ralph/builder.md + ralph/PROMPT.md + docs/CONTINUATION.md, and
# lets it complete one verified batch. The loop itself independently re-runs
# the gates and an external adversarial reviewer — a worker's completion
# claim is never sufficient evidence on its own.
#
# Both swarm roles run on the Muse subscription via `muse exec`:
# the builder is muse-spark-1.3 (high effort, writes allowed) and the
# adversarial reviewer is muse-wonder-1.0 (max effort, --disable-write).
# There is no Kilo/litellm path; do not re-add one.
#
# Usage:
#   ./ralph.sh start [--max N]   start detached (pid file, logs/ralph/)
#   ./ralph.sh once              run one iteration in the foreground
#   ./ralph.sh smoke             one iteration with a no-op prompt (mechanics)
#   ./ralph.sh status            loop state + last summary lines
#   ./ralph.sh stop              terminate a detached loop
#
# Config (env overrides): MUSE_BIN WORKER_MODEL WORKER_EFFORT REVIEWER_MODEL
# REVIEWER_EFFORT MAX_ITERATIONS ITER_TIMEOUT REVIEW_TIMEOUT STALL_LIMIT
# FAIL_LIMIT PROMPT_FILE BUILDER_FILE VALIDATOR_FILE
#
# Exit codes: 0 complete (independently verified) | 3 stalled | 4 repeated
# failures | 5 iteration budget exhausted | 6 already running / bad state.
#
# NOTE: ralph.sh must run from a normal operator shell, not inside another
# agent's sandbox: `muse exec` needs read access to ~/.config/muse (auth)
# and write access to the session store.

set -u -o pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO" || exit 6

# ---- configuration ----------------------------------------------------------
find_muse() {
    if [ -n "${MUSE_BIN:-}" ] && [ -x "$MUSE_BIN" ]; then echo "$MUSE_BIN"; return; fi
    command -v muse
}
MUSE="$(find_muse)" || { echo "FATAL: muse CLI not found (set MUSE_BIN)"; exit 6; }

WORKER_MODEL="${WORKER_MODEL:-muse-spark-1.3}"
WORKER_EFFORT="${WORKER_EFFORT:-high}"
WORKER_STEPS="${WORKER_STEPS:-80}"
REVIEWER_MODEL="${REVIEWER_MODEL:-muse-wonder-1.0}"
REVIEWER_EFFORT="${REVIEWER_EFFORT:-max}"
REVIEWER_STEPS="${REVIEWER_STEPS:-60}"
MAX_ITERATIONS="${MAX_ITERATIONS:-30}"
ITER_TIMEOUT="${ITER_TIMEOUT:-3600}"
REVIEW_TIMEOUT="${REVIEW_TIMEOUT:-3600}"
STALL_LIMIT="${STALL_LIMIT:-4}"
FAIL_LIMIT="${FAIL_LIMIT:-3}"
PROMPT_FILE="${PROMPT_FILE:-ralph/PROMPT.md}"
BUILDER_FILE="${BUILDER_FILE:-ralph/builder.md}"
VALIDATOR_FILE="${VALIDATOR_FILE:-ralph/validator.md}"

LOGDIR="$REPO/logs/ralph"
STATE="$LOGDIR/state"
SUMMARY="$LOGDIR/summary.log"
LOCK="$LOGDIR/lock"
PIDFILE="$LOGDIR/loop.pid"

mkdir -p "$LOGDIR"

# ---- muse exec helpers ------------------------------------------------------
# muse exec --json emits one JSON object per line; the final answer is the
# text of the run.terminal.completed event. Anything else (non-zero exit, a
# failed terminal, no terminal event) is a FAILED run, never a result.
muse_final_text() { # $1 = jsonl file
    jq -rs '[.[] | select(.payload_type=="run.terminal.completed") | .payload.text] | last // ""' "$1" 2>/dev/null
}
muse_terminal_ok() { # $1 = jsonl file
    jq -rs '[.[] | select(.payload_type=="run.terminal.completed") | .payload.terminal] | last // ""' "$1" 2>/dev/null | grep -qx completed
}
muse_session_id() { # $1 = jsonl file
    jq -rs '[.[] | select(.stream.kind=="session") | .stream.id] | first // "none"' "$1" 2>/dev/null
}

run_builder() { # $1 = prompt file, $2 = jsonl out, $3 = err out
    timeout "$ITER_TIMEOUT" "$MUSE" exec --json --no-session-log \
        --trust-workspace --approval-mode never \
        --model "$WORKER_MODEL" --reasoning-effort "$WORKER_EFFORT" \
        --max-model-steps "$WORKER_STEPS" \
        --prompt-file "$1" \
        9>&- >"$2" 2>"$3"
}

run_reviewer_exec() { # $1 = prompt file, $2 = jsonl out, $3 = err out
    timeout "$REVIEW_TIMEOUT" "$MUSE" exec --json --no-session-log \
        --trust-workspace --approval-mode never --disable-write \
        --model "$REVIEWER_MODEL" --reasoning-effort "$REVIEWER_EFFORT" \
        --max-model-steps "$REVIEWER_STEPS" \
        --prompt-file "$1" \
        9>&- >"$2" 2>"$3"
}

# ---- gates (independent verification, never delegated to the worker) --------
run_gates() { # -> sets GATES_OK, appends gate lines to $1
    # The 3DS gate needs the devkitPro toolchain paths; builders export them
    # per-command, so default them here or the loop's own gate run fails
    # environmentally on every iteration.
    if [ -d /opt/devkitpro/devkitARM ]; then
        export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
        export DEVKITARM="${DEVKITARM:-/opt/devkitpro/devkitARM}"
    fi
    local log="$1" ok=1
    local -a names=("tests/scripts" "tools/check.sh" "mdk_3ds build")
    local -a cmds=("make -C tests/scripts" "bash tools/check.sh" "make -C mdk_3ds")
    for i in 0 1 2; do
        if timeout 1800 bash -c "${cmds[$i]}" 9>&- >>"$log" 2>&1; then
            echo "GATE ${names[$i]}: PASS" | tee -a "$log"
        else
            echo "GATE ${names[$i]}: FAIL" | tee -a "$log"
            ok=0
        fi
    done
    GATES_OK=$ok
}

tree_hash() {
    { git -C "$REPO" rev-parse HEAD; git -C "$REPO" status --porcelain; git -C "$REPO" diff | sha256sum; } | sha256sum | cut -d' ' -f1
}

# ---- external adversarial reviewer ------------------------------------------
# The reviewer is a separate fresh muse exec run on the stronger model with
# writes disabled (ralph/validator.md is its instructions). A failed run
# (provider error, timeout, empty reply) is a FAILED review, never a verdict;
# it is retried once on the same route and then recorded as failed.
review_attempt() { # $1=out file  $2=scope text
    local out="$1" scope="$2"
    local prompt="$LOGDIR/.review-prompt.md"
    { cat "$VALIDATOR_FILE"; printf '\n\n--- REVIEW SCOPE ---\n%s\n' "$scope"; } >"$prompt"
    run_reviewer_exec "$prompt" "$out.jsonl.attempt" "$out.err"
    local rc=$?
    local texts
    texts="$(muse_final_text "$out.jsonl.attempt")"
    if [ "$rc" -ne 0 ] || ! muse_terminal_ok "$out.jsonl.attempt" || [ -z "$texts" ]; then
        { echo "REVIEWER FAILED model=$REVIEWER_MODEL rc=$rc"; head -c 800 "$out.err" 2>/dev/null; } >"$out"
        return 1
    fi
    { echo "<!-- reviewer=$REVIEWER_MODEL effort=$REVIEWER_EFFORT rc=$rc -->"; printf '%s\n' "$texts"; } >"$out"
    cp "$out.jsonl.attempt" "$out.jsonl"
    return 0
}

run_reviewer() { # $1 = review output file, $2 = scope: latest|full
    local out="$1" scope_arg="${2:-latest}" scope
    if [ "$scope_arg" = "full" ]; then
        scope="the ENTIRE current C++ tree against the SPEC.md acceptance criteria and the C# oracle (src/Mdk.Game/**). This is the completion-gate review: check every acceptance criterion you can and list every unmet one."
    else
        scope="the changes in the commits since $(state_get HEAD_AT_START HEAD) plus any uncommitted work — use git log/diff to bound it; do not re-review older code."
    fi
    if review_attempt "$out" "$scope"; then
        return 0
    fi
    echo "[$(date -Is)] reviewer $REVIEWER_MODEL FAILED — retrying once" >>"$SUMMARY"
    if review_attempt "$out" "$scope"; then
        echo "[$(date -Is)] WARNING: review produced on retry (route $REVIEWER_MODEL)" >>"$SUMMARY"
        return 0
    fi
    echo "[$(date -Is)] REVIEWER FAILED TWICE (see $out)" >>"$SUMMARY"
    return 1
}

watchdog_case() {
    # Installed via crontab: restarts the loop after a SILENT death
    # (OOM, stray kill). Never resurrects an intentional stop/exit.
    [ -f "$LOGDIR/disabled" ] && return 0
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        return 0
    fi
    echo "[$(date -Is)] WATCHDOG: loop dead, restarting" >>"$SUMMARY"
    # Crash-loop guard: a loop that dies again within 3 minutes of its
    # restart is failing at startup, not working. Stop after 3 tries and
    # leave a diagnostic instead of spinning every 5 minutes forever.
    local last_restart now age quick
    last_restart="$(state_get LAST_RESTART_TS 0)"
    now="$(date +%s)"
    age=$(( now - last_restart ))
    quick="$(state_get QUICK_DEATHS 0)"
    if [ "${last_restart:-0}" -gt 0 ] && [ "$age" -lt 180 ]; then
        quick=$(( quick + 1 ))
        state_set QUICK_DEATHS "$quick"
        echo "[$(date -Is)] WATCHDOG: loop died ${age}s after restart (quick deaths: $quick)" >>"$SUMMARY"
        if [ "$quick" -ge 3 ]; then
            echo "[$(date -Is)] WATCHDOG: crash loop detected — restarts disabled" >>"$SUMMARY"
            echo "crash-loop at $(date -Is)" >"$LOGDIR/disabled"
            return 0
        fi
    else
        state_set QUICK_DEATHS 0
    fi
    state_set LAST_RESTART_TS "$now"
    { echo "[$(date -Is)] memory at restart:"; free -h | head -2; } >>"$LOGDIR/watchdog.log" 2>&1
    "$0" start >>"$LOGDIR/watchdog.log" 2>&1
}

# Kill a process that inherited the lock fd from a dead loop: it would hold
# the flock forever and make every restart fail with "lock held".
kill_stale_lock_holder() {
    local holder f
    for holder in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
        for f in /proc/"$holder"/fd/*; do
            if [ "$(readlink -f "$f" 2>/dev/null)" = "$LOCK" ] && \
               ! grep -q "ralph.sh __run" /proc/"$holder"/cmdline 2>/dev/null; then
                kill -9 "$holder" 2>/dev/null
            fi
        done
    done
}

# Kill orphaned muse exec children of a dead/stopped loop so two workers
# never edit the tree at once. Children are identified by the prompt files
# this script generates under $LOGDIR.
kill_orphan_workers() {
    pkill -f -- "--prompt-file $LOGDIR/.prompt-" 2>/dev/null
    pkill -f -- "--prompt-file $LOGDIR/.review-prompt" 2>/dev/null
    true
}

# ---- one iteration ----------------------------------------------------------
state_get() {
    local v
    v="$(sed -n "s/^$1=//p" "$STATE" 2>/dev/null | tail -1)"
    echo "${v:-${2:-}}"
}
state_set() {
    grep -v "^$1=" "$STATE" 2>/dev/null >"$STATE.tmp" || true
    echo "$1=$2" >>"$STATE.tmp"
    mv "$STATE.tmp" "$STATE"
}

iterate() {
    local iter prompt_file="$PROMPT_FILE"
    iter=$(( $(state_get ITER 0) + 1 ))
    state_set ITER "$iter"
    state_set HEAD_AT_START "$(git -C "$REPO" rev-parse HEAD)"
    local itlog="$LOGDIR/iter-$iter"
    local status_block
    status_block="$(printf '\n\n--- LOOP STATUS (machine-generated) ---\niteration: %s of max %s\ngates from previous iteration: %s\nlast progress: iteration %s\nIf logs/ralph/review-*.md exists and is newer than your last change, address its findings first.\n' "$iter" "$MAX_ITERATIONS" "$(state_get LAST_GATES n/a)" "$(state_get LAST_PROGRESS none)")"
    local full_prompt="$LOGDIR/.prompt-iter-$iter.md"
    { cat "$BUILDER_FILE"; printf '\n\n--- ITERATION MESSAGE ---\n'; cat "$prompt_file"; printf '%s' "$status_block"; } >"$full_prompt"

    echo "[$(date -Is)] iter=$iter start builder=$WORKER_MODEL effort=$WORKER_EFFORT" >>"$SUMMARY"
    run_builder "$full_prompt" "$itlog.jsonl" "$itlog.err"
    local rc=$?
    local final_text
    final_text="$(muse_final_text "$itlog.jsonl")"
    [ -n "$final_text" ] || final_text="(no text)"
    local session
    session="$(muse_session_id "$itlog.jsonl")"
    echo "[$(date -Is)] iter=$iter worker rc=$rc session=$session" >>"$SUMMARY"
    printf '%s\n' "$final_text" >"$itlog.txt"
    if [ "$rc" -ne 0 ] || ! muse_terminal_ok "$itlog.jsonl"; then
        # A timeout that still banked commits is progress, not failure.
        if [ "$rc" -eq 124 ] && [ "$(git -C "$REPO" rev-parse HEAD)" != "$(state_get HEAD_AT_START "$(git -C "$REPO" rev-parse HEAD)")" ]; then
            echo "[$(date -Is)] iter=$iter worker timed out AFTER banking commits — not counted as failure" >>"$SUMMARY"
            state_set FAILS 0
            state_set LAST_PROGRESS "$iter"
        else
            local fails=$(( $(state_get FAILS 0) + 1 ))
            state_set FAILS "$fails"
            echo "[$(date -Is)] iter=$iter WORKER FAILED rc=$rc (consecutive: $fails)" >>"$SUMMARY"
            if [ "$fails" -ge "$FAIL_LIMIT" ]; then
                echo "[$(date -Is)] STOP: $fails consecutive worker failures" >>"$SUMMARY"
                return 4
            fi
        fi
        return 0
    fi
    state_set FAILS 0

    # Independent gate run.
    run_gates "$itlog.gates"
    state_set LAST_GATES "$([ "$GATES_OK" = 1 ] && echo PASS || echo FAIL)"

    # Progress detection.
    local h; h="$(tree_hash)"
    local last_h; last_h="$(state_get LAST_HASH || echo)"
    if [ "$h" != "$last_h" ]; then
        state_set LAST_HASH "$h"
        state_set LAST_PROGRESS "$iter"
        state_set STALL 0
    else
        local stall=$(( $(state_get STALL 0) + 1 ))
        state_set STALL "$stall"
        echo "[$(date -Is)] iter=$iter NO PROGRESS (stall=$stall)" >>"$SUMMARY"
    fi

    # External adversarial review after any productive iteration.
    if [ "$(state_get LAST_PROGRESS)" = "$iter" ]; then
        echo "[$(date -Is)] iter=$iter external review start reviewer=$REVIEWER_MODEL effort=$REVIEWER_EFFORT" >>"$SUMMARY"
        if run_reviewer "$LOGDIR/review-$iter.md" latest; then
            grep -q '^VERDICT: COMPLETE' "$LOGDIR/review-$iter.md" \
                && echo "[$(date -Is)] iter=$iter review verdict COMPLETE" >>"$SUMMARY" \
                || echo "[$(date -Is)] iter=$iter review verdict INCOMPLETE" >>"$SUMMARY"
        else
            echo "[$(date -Is)] iter=$iter reviewer ERROR (see review-$iter.md)" >>"$SUMMARY"
        fi
    fi

    # Completion: completion marker + all gates green + reviewer COMPLETE.
    if [ -f docs/PHASE2_COMPLETE ] && [ "$GATES_OK" = 1 ]; then
        # The acceptance gate is a FULL-TREE review: a per-batch scoped review
        # cannot establish that every SPEC criterion is met.
        if run_reviewer "$LOGDIR/review-final.md" full && grep -q '^VERDICT: COMPLETE' "$LOGDIR/review-final.md"; then
            echo "[$(date -Is)] COMPLETE: gates green + full-tree review verdict COMPLETE" >>"$SUMMARY"
            return 0
        fi
        echo "[$(date -Is)] completion claim REJECTED (full-tree review verdict not COMPLETE)" >>"$SUMMARY"
    fi

    if [ "$(state_get STALL)" -ge "$STALL_LIMIT" ]; then
        echo "[$(date -Is)] STOP: stalled $STALL iterations with no tree change" >>"$SUMMARY"
        return 3
    fi
    return 0
}

loop() {
    # The lock fd must NOT be inherited by the worker/reviewer children: an
    # orphaned child that keeps fd 9 open holds the flock after the loop dies,
    # and every later start then fails with "lock held" forever. Each child
    # command closes it explicitly (9>&-).
    exec 9>"$LOCK"
    flock -n 9 || { echo "ralph loop already running (lock held)"; exit 6; }
    [ -f "$STATE" ] || { echo "ITER=0"; echo "FAILS=0"; echo "STALL=0"; } >"$STATE"
    [ -n "$(state_get LAST_HASH)" ] || state_set LAST_HASH "$(tree_hash)"
    echo "[$(date -Is)] LOOP START builder=$WORKER_MODEL/$WORKER_EFFORT reviewer=$REVIEWER_MODEL/$REVIEWER_EFFORT max=$MAX_ITERATIONS" >>"$SUMMARY"
    # Route preflight: a dead model route must be reported now, not
    # discovered as a silently useless review later.
    local m
    for m in "$WORKER_MODEL" "$REVIEWER_MODEL"; do
        if timeout 180 "$MUSE" exec --json --no-session-log --approval-mode never --model "$m" "Reply with exactly: OK" 9>&- 2>/dev/null | grep -q '"terminal":"completed"'; then
            echo "[$(date -Is)] preflight OK: $m" >>"$SUMMARY"
        else
            echo "[$(date -Is)] PREFLIGHT FAILED: $m" >>"$SUMMARY"
        fi
    done
    local rc=0
    while :; do
        local iter; iter="$(state_get ITER 0)"
        if [ "$iter" -ge "$MAX_ITERATIONS" ]; then
            echo "[$(date -Is)] STOP: iteration budget ($MAX_ITERATIONS) exhausted" >>"$SUMMARY"
            rc=5; break
        fi
        iterate
        rc=$?
        [ "$rc" -ne 0 ] && break
    done
    echo "[$(date -Is)] LOOP EXIT rc=$rc iter=$(state_get ITER)" >>"$SUMMARY"
    # Intentional exit (complete, budget, stall, repeated failures): mark
    # disabled so the watchdog does not resurrect the loop. A silent death
    # (OOM, stray kill) never reaches this line, so the watchdog restarts.
    echo "rc=$rc at $(date -Is)" >"$LOGDIR/disabled"
    rm -f "$PIDFILE"
    exit "$rc"
}

# ---- subcommands ------------------------------------------------------------
case "${1:-}" in
    start)
        if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
            echo "already running pid $(cat "$PIDFILE")"; exit 6
        fi
        # The loop died (or was stopped): kill any orphaned worker/reviewer it
        # left behind so two workers never edit the tree at once. An orphan
        # that inherited the lock fd would also block every restart.
        kill_orphan_workers
        kill_stale_lock_holder
        while [ $# -gt 1 ]; do
            case "$2" in
                --max) MAX_ITERATIONS="$3"; shift 2 ;;
                *) echo "unknown option $2"; exit 6 ;;
            esac
        done
        export MAX_ITERATIONS
        rm -f "$LOGDIR/disabled"
        nohup "$0" __run >>"$LOGDIR/loop.log" 2>&1 &
        echo $! >"$PIDFILE"
        echo "started pid $(cat "$PIDFILE") — logs: $LOGDIR"
        ;;
    __run) loop ;;
    once)
        exec 9>"$LOCK"; flock -n 9 || { echo "loop already running"; exit 6; }
        [ -f "$STATE" ] || { echo "ITER=0"; echo "FAILS=0"; echo "STALL=0"; } >"$STATE"
        [ -n "$(state_get LAST_HASH)" ] || state_set LAST_HASH "$(tree_hash)"
        iterate
        exit $?
        ;;
    smoke)
        PROMPT_FILE="ralph/SMOKE.md" BUILDER_FILE="ralph/SMOKE-BUILDER.md" exec "$0" once
        ;;
    status)
        if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
            echo "RUNNING pid $(cat "$PIDFILE")"
        else
            echo "not running"
        fi
        [ -f "$STATE" ] && { echo "--- state ---"; cat "$STATE"; }
        [ -f "$SUMMARY" ] && { echo "--- last summary ---"; tail -15 "$SUMMARY"; }
        ;;
    stop)
        if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
            kill "$(cat "$PIDFILE")" && echo "stopped pid $(cat "$PIDFILE")"
            rm -f "$PIDFILE"
        else
            echo "not running"
        fi
        kill_orphan_workers
        echo "stopped by user at $(date -Is)" >"$LOGDIR/disabled"
        ;;
    watchdog)
        watchdog_case
        ;;
    *)
        sed -n '2,27p' "$0"
        exit 6
        ;;
esac

#!/usr/bin/env bash
# ralph.sh — autonomous mission loop for the MDK Phase 2 port.
#
# Each iteration starts a FRESH coding-agent context (no session memory),
# feeds it ralph/PROMPT.md + docs/CONTINUATION.md, and lets it complete one
# verified batch. The loop itself independently re-runs the gates and an
# external adversarial reviewer — a worker's completion claim is never
# sufficient evidence on its own.
#
# Usage:
#   ./ralph.sh start [--max N]   start detached (pid file, logs/ralph/)
#   ./ralph.sh once              run one iteration in the foreground
#   ./ralph.sh smoke             one iteration with a no-op prompt (mechanics)
#   ./ralph.sh status            loop state + last summary lines
#   ./ralph.sh stop              terminate a detached loop
#
# Config (env overrides): KILO_BIN WORKER_MODEL REVIEWER_MODEL MAX_ITERATIONS
# ITER_TIMEOUT REVIEW_TIMEOUT STALL_LIMIT FAIL_LIMIT PROMPT_FILE
#
# Exit codes: 0 complete (independently verified) | 3 stalled | 4 repeated
# failures | 5 iteration budget exhausted | 6 already running / bad state.

set -u -o pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO" || exit 6

# ---- configuration ----------------------------------------------------------
find_kilo() {
    # The VS Code host may export a stale KILO_BIN (old extension version);
    # only honour it if it is actually executable.
    if [ -n "${KILO_BIN:-}" ] && [ -x "$KILO_BIN" ]; then echo "$KILO_BIN"; return; fi
    local c
    c="$(ls -d "$HOME"/.vscode/extensions/kilocode.kilo-code-*/bin/kilo 2>/dev/null | sort -V | tail -1)"
    [ -n "$c" ] && [ -x "$c" ] && { echo "$c"; return; }
    command -v kilo
}
KILO="$(find_kilo)" || { echo "FATAL: kilo CLI not found (set KILO_BIN)"; exit 6; }

WORKER_MODEL="${WORKER_MODEL:-litellm/qwen}"
REVIEWER_MODEL="${REVIEWER_MODEL:-litellm/kimi-k3-nvidia}"
MAX_ITERATIONS="${MAX_ITERATIONS:-30}"
ITER_TIMEOUT="${ITER_TIMEOUT:-3600}"
REVIEW_TIMEOUT="${REVIEW_TIMEOUT:-3600}"
STALL_LIMIT="${STALL_LIMIT:-4}"
FAIL_LIMIT="${FAIL_LIMIT:-3}"
PROMPT_FILE="${PROMPT_FILE:-ralph/PROMPT.md}"

LOGDIR="$REPO/logs/ralph"
STATE="$LOGDIR/state"
SUMMARY="$LOGDIR/summary.log"
LOCK="$LOGDIR/lock"
PIDFILE="$LOGDIR/loop.pid"

mkdir -p "$LOGDIR"

# ---- gates (independent verification, never delegated to the worker) --------
run_gates() { # -> sets GATES_OK, appends gate lines to $1
    local log="$1" ok=1
    local -a names=("tests/scripts" "tools/check.sh" "mdk_3ds build")
    local -a cmds=("make -C tests/scripts" "bash tools/check.sh" "make -C mdk_3ds")
    for i in 0 1 2; do
        if timeout 1800 bash -c "${cmds[$i]}" >>"$log" 2>&1; then
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
# The project validator agent (.kilo/agent/adversarial-validator.md) is a
# subagent-mode agent; it is run here as instructions for the reviewer model
# so the reviewer is provably the stronger model. The loop captures its
# output (the reviewer has no write permission by design).
run_reviewer() { # $1 = review output file
    local out="$1"
    timeout "$REVIEW_TIMEOUT" "$KILO" run -m "$REVIEWER_MODEL" --format json \
        --title "ralph-review" \
        "Read .kilo/agent/adversarial-validator.md and act exactly as that adversarial validator (you may not edit anything, and do not run builds — review by reading code and the C# oracle). SCOPE: the changes in the most recent commits since $(state_get HEAD_AT_START HEAD) plus any uncommitted work — use git log/diff to bound your review; do not re-review older code. Verify the claims in docs/CONTINUATION.md about these changes. Be concise and concrete. End your reply with exactly one line: 'VERDICT: COMPLETE' only if every SPEC acceptance criterion is met with evidence and nothing actionable remains except hardware-only verification; otherwise 'VERDICT: INCOMPLETE' followed by the BLOCKER/MAJOR findings that must be fixed." \
        >"$out.jsonl" 2>"$out.err"
    local rc=$?
    jq -rs '[.[] | select(.type=="text") | .part.text] | join("\n\n")' "$out.jsonl" >"$out" 2>/dev/null
    if [ ! -s "$out" ]; then
        echo "REVIEWER ERROR rc=$rc (see $out.err)" >"$out"
        return 1
    fi
    echo "" >>"$out"
    echo "<!-- reviewer=$REVIEWER_MODEL rc=$rc -->" >>"$out"
    return 0
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
    local full_prompt
    full_prompt="$(cat "$prompt_file")$(printf '%s' "$status_block")"

    echo "[$(date -Is)] iter=$iter start model=$WORKER_MODEL" >>"$SUMMARY"
    timeout "$ITER_TIMEOUT" "$KILO" run -m "$WORKER_MODEL" --auto --format json \
        --title "ralph-iter-$iter" "$full_prompt" \
        >"$itlog.jsonl" 2>"$itlog.err"
    local rc=$?
    local final_text
    final_text="$(jq -rs '[.[] | select(.type=="text") | .part.text] | last // "(no text)"' "$itlog.jsonl" 2>/dev/null)"
    local session
    session="$(jq -rs '[.[] | .sessionID? // empty][0] // "none"' "$itlog.jsonl" 2>/dev/null)"
    echo "[$(date -Is)] iter=$iter worker rc=$rc session=$session" >>"$SUMMARY"
    printf '%s\n' "$final_text" >"$itlog.txt"
    if [ "$rc" -ne 0 ]; then
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
        echo "[$(date -Is)] iter=$iter external review start model=$REVIEWER_MODEL" >>"$SUMMARY"
        if run_reviewer "$LOGDIR/review-$iter.md"; then
            grep -q '^VERDICT: COMPLETE' "$LOGDIR/review-$iter.md" \
                && echo "[$(date -Is)] iter=$iter review verdict COMPLETE" >>"$SUMMARY" \
                || echo "[$(date -Is)] iter=$iter review verdict INCOMPLETE" >>"$SUMMARY"
        else
            echo "[$(date -Is)] iter=$iter reviewer ERROR (see review-$iter.md)" >>"$SUMMARY"
        fi
    fi

    # Completion: worker marker + all gates green + reviewer COMPLETE.
    if [ -f docs/PHASE2_COMPLETE ] && [ "$GATES_OK" = 1 ]; then
        local rv; rv="$(ls -t "$LOGDIR"/review-*.md 2>/dev/null | head -1)"
        if [ -n "$rv" ] && grep -q '^VERDICT: COMPLETE' "$rv"; then
            echo "[$(date -Is)] COMPLETE: gates green + reviewer verdict at $rv" >>"$SUMMARY"
            return 0
        fi
        # Marker without a fresh COMPLETE verdict: re-review right now.
        if run_reviewer "$LOGDIR/review-final.md" && grep -q '^VERDICT: COMPLETE' "$LOGDIR/review-final.md"; then
            echo "[$(date -Is)] COMPLETE: verified by fresh final review" >>"$SUMMARY"
            return 0
        fi
        echo "[$(date -Is)] completion claim REJECTED (reviewer verdict not COMPLETE)" >>"$SUMMARY"
    fi

    if [ "$(state_get STALL)" -ge "$STALL_LIMIT" ]; then
        echo "[$(date -Is)] STOP: stalled $STALL iterations with no tree change" >>"$SUMMARY"
        return 3
    fi
    return 0
}

loop() {
    exec 9>"$LOCK"
    flock -n 9 || { echo "ralph loop already running (lock held)"; exit 6; }
    [ -f "$STATE" ] || { echo "ITER=0"; echo "FAILS=0"; echo "STALL=0"; } >"$STATE"
    [ -n "$(state_get LAST_HASH)" ] || state_set LAST_HASH "$(tree_hash)"
    echo "[$(date -Is)] LOOP START worker=$WORKER_MODEL reviewer=$REVIEWER_MODEL max=$MAX_ITERATIONS" >>"$SUMMARY"
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
        # The loop died (or was stopped): kill any orphaned worker it left
        # behind so two workers never edit the tree at once.
        pkill -f -- "--title ralph-iter-" 2>/dev/null
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
        PROMPT_FILE="ralph/SMOKE.md" exec "$0" once
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
        pkill -f -- "--title ralph-iter-" 2>/dev/null
        echo "stopped by user at $(date -Is)" >"$LOGDIR/disabled"
        ;;
    watchdog)
        # Installed via crontab: restarts the loop after a SILENT death
        # (OOM, stray kill). Never resurrects an intentional stop/exit.
        [ -f "$LOGDIR/disabled" ] && exit 0
        if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
            exit 0
        fi
        echo "[$(date -Is)] WATCHDOG: loop dead, restarting" >>"$SUMMARY"
        { echo "[$(date -Is)] memory at restart:"; free -h | head -2; } >>"$LOGDIR/watchdog.log" 2>&1
        "$0" start >>"$LOGDIR/watchdog.log" 2>&1
        ;;
    *)
        sed -n '2,20p' "$0"
        exit 6
        ;;
esac

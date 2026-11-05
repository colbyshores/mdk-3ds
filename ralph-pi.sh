#!/usr/bin/env bash
# ralph-pi.sh — the MDK port loop on the Pi harness.
#
# Roles: the worker is pi (the builder) as piagent; the reviewer is a second pi
# model, read-only by instruction, over each iteration's diff; this script is the
# orchestrator — gates, progress/stall detection and review dispatch — and the
# builder can never influence them.
#
# Each commit must include docs/frames/*.png and its .txt measurement;
# tools/runner/gates_batch_integrity.py validates per-commit --base/--head and
# --staged boundaries; the published image is produced by tools/publish_frame.sh.
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
        worker|worker-fallback)
            local worker_limit=$((ITER_TIMEOUT - 300))
            [ "$worker_limit" -gt 0 ] || worker_limit="$ITER_TIMEOUT"
            printf '%s' "$worker_limit"
            ;;
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
WORKER_MODELS="${WORKER_MODELS:-litellm/qwen litellm/deepseek-v4.1-flash-th}"
# Reviewer: DeepSeek V4.1 Flash. Verified 2026-10-31 against the live proxy:
# alias `deepseek-v4.1-flash` resolves to upstream `deepseek/deepseek-flash`
# at api.deepseek.com (official DeepSeek API — a genuinely different family
# from the qwen builder) and answers a live PING through the piagent+pi harness
# path. Model identity is the upstream string, never the alias
# (vault: model-identity-is-the-upstream-string-not-the-alias).
REVIEWER_MODEL="${REVIEWER_MODEL:-litellm/deepseek-v4.1-flash}"
# Reviewer fallbacks MUST be a different upstream family than the builder
# (qwen38-flash-next), or the "independent" review is fake. `swarm-worker` is
# deliberately excluded: it is the SAME upstream as qwen.
#
# The owner directive of 2026-11-05 names the reviewer chain:
#
#   REVIEWER_FALLBACKS="litellm/deepseek-v4.1-flash-nvidia
#                       litellm/minimax-m3.1-flash-nvidia litellm/luna"
#
# and states the behaviour the chain exists to produce: "when the builder falls
# back to deepseek-v4.1-flash-th, the reviewer's own family rule skips both
# DeepSeek variants, so review lands on minimax or luna". Two of the three are
# shipped here; the third is not, with a reason, so nobody re-derives it:
#
#   * minimax-m3.1-flash-nvidia (family minimax) and luna (family luna) both
#     resolve on the LIVE proxy (model_identity.py report --live, 2026-11-05)
#     and both are a different family from every builder route in WORKER_MODELS.
#   * deepseek-v4.1-flash-nvidia is NOT re-added. It folds to family `deepseek`,
#     so the family rule refuses it in exactly the case a fallback is needed for
#     (a -th builder), and it is absent from the live proxy table — the static
#     map only — so it would burn a health probe to arrive at a refusal. The
#     directive's line predates that retirement; what it asks for is a
#     cross-family reviewer when the builder degrades, and that is what this
#     line delivers.
#
# `swarm-worker` stays excluded: same upstream as qwen. grok stays retired by
# operator decision and must not return to any chain. If BOTH routes below ever
# go unhealthy the answer is BLOCKED and a stopped loop, not a same-family
# review — see the note above reviewer_reject_reason.
REVIEWER_FALLBACKS="${REVIEWER_FALLBACKS:-litellm/minimax-m3.1-flash-nvidia litellm/luna}"
ITER_TIMEOUT="${ITER_TIMEOUT:-7200}"
REVIEW_TIMEOUT="${REVIEW_TIMEOUT:-5400}"
REVIEW_FAIL_LIMIT="${REVIEW_FAIL_LIMIT:-2}"
MAX_ITER="${MAX_ITER:-80}"
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
export PI_KNOWLEDGE_EMBEDDING="${PI_KNOWLEDGE_EMBEDDING:-openai:qwen3-embedding-0.6b}"
export PI_KNOWLEDGE_EMBEDDING_BASE_URL="${PI_KNOWLEDGE_EMBEDDING_BASE_URL:-http://127.0.0.1:8731/v1}"
export PI_KNOWLEDGE_EMBEDDING_PREFIXES="${PI_KNOWLEDGE_EMBEDDING_PREFIXES:-off}"
export PI_KNOWLEDGE_EMBEDDING_MAX_CHARS="${PI_KNOWLEDGE_EMBEDDING_MAX_CHARS:-3000}"

state_get() { sed -n "s/^$1=//p" "$STATE" 2>/dev/null | tail -1; }

# The review-record state machine lives in its own file so that it can be run
# without starting the loop -- see tools/runner/test_review_state.sh, which
# asserts it. A record whose failure mode is invisible (a review that reads
# "done" and checked nothing) is a record that must be testable.
# shellcheck source=tools/runner/review_state.sh
. "$ROOT/tools/runner/review_state.sh"

# state_write <key=value> [...] — state_apply, and STOP if it fails.
#
# `set -e` is deliberately not in effect here (only -u and pipefail), so a
# multi-key transition whose one rename failed was silently reverted in full
# while the log line printed right after it announced the transition as done
# (external review 2026-11-01, run-20261101-062333/review-2.md AR6-4). A full
# disk, a read-only $LOGDIR or a $STATE that is not a regular file therefore
# left the obligation bookkeeping stale on disk while the log claimed otherwise
# — and a stale in-flight marker is a review that gets folded twice, which is the
# AR-2 stop re-entered through an ignored error instead of a torn write.
#
# The loop stops rather than continuing, because every decision past this point
# would be made from a record the loop does not have: "could not write" must not
# reach the same path as "recorded". Exit 9 is new and is NOT in the unit's
# RestartPreventExitStatus, because a full disk is often transient and
# StartLimitBurst caps the retry at 3 per 30 minutes. Read that list as it is
# shipped (tools/install_ralph_pi_service.sh: RestartPreventExitStatus=2 6 7):
# a reviewer that keeps dying (7) is prevented and is not restarted; a review
# that cannot be dispatched (8) is NOT in the list, so systemd does retry it,
# which is intended because a blocked reviewer route can recover when a worker
# route comes back — it lands in `failed` after StartLimitBurst, not silently.
# 9 joins neither list deliberately, for the same transient-disk reason above.
state_write() {
    if ! state_apply "$@"; then
        log "STOP: could not write the review record at $STATE — the transition was NOT recorded, so every decision after this point would be made from a record the loop does not have (disk full, read-only log dir, or $STATE is not a regular file)"
        exit 9
    fi
}

# state_set <key> <value> — the single-key spelling of the SAME checked writer.
#
# It used to be its own hand-rolled `grep -v > tmp; echo >> tmp; mv tmp $STATE`,
# and `mv`'s status was discarded (external review 2026-11-01,
# run-20261101-062333/review-3.md F3). AR6-4 put the refusal on `state_apply` and
# the stop on `state_write`, but the property it bought — "a write that did not
# happen is never announced as done" — stopped at the multi-key arm, so every
# single-key write kept a silent no-op (eleven call sites over eight distinct
# keys, counted): a `$STATE` that had become a directory made `mv` succeed by
# moving the temp file INTO it, the collector printed "REVIEW CALL #N", and
# `REVIEW_CALLS` never landed. One writer with a checked status is the whole fix;
# a second unchecked one is a second thing to forget. Delegation, not a copy: the
# refusal, the temp-file check and the mv status now live in exactly one place.
#
# This does widen exit 9 to every record write, which is the intent and not an
# accident: a full disk stops the loop (restartable, burst-capped) instead of
# running an iteration on a record it did not write.
state_set() { state_write "$1=$2"; }

# --- model identity: the upstream string, never the alias --------------------
# (vault: model-identity-is-the-upstream-string-not-the-alias). On a shared
# litellm proxy two aliases can be ONE upstream -- on this proxy today `qwen`
# and `swarm-worker` are both openai/qwen38-flash-next, and `luna` and
# `luna-compaction` are both openai/responses/gpt-6-luna -- and two hosts can
# carry ONE model family (`deepseek-v4.1-flash` at api.deepseek.com and the now
# retired `deepseek-v4.1-flash-nvidia` at integrate.api.nvidia.com were the same
# weights twice, and `deepseek-v4.1-flash` and `deepseek-v4.1-flash-th` are still
# one family on two routes). Comparing alias strings passes every one of those,
# which is what external review 2026-11-01 finding F3 found. Resolution is the
# proxy's /model/info when it answers, the committed map in tools/runner/
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
# THERE IS NO INDEPENDENCE EXCEPTION HERE, AND ONE MUST NOT BE ADDED.
#
# A batch put a literal `-th` -> `flash` allowlist at this spot on 2026-11-05 and
# called it owner-directed. It was not, and both owner-authored texts in this
# repository say the opposite:
#
#   * SPEC-COMPLETION.md, harness rule 4 (PROTECTED): the reviewer "must stay a
#     different model family from the builder — identity is the resolved
#     upstream, never the alias — and a reviewer that resolves to the builder's
#     family is skipped rather than dispatched. If no eligible reviewer resolves,
#     the loop records BLOCKED and stops; it never reviews with the builder's own
#     model." Editing that sentence is an owner act on a protected file
#     (gates_batch_integrity.py --owner-approved); a batch may not author its own
#     exception, and this loop will not accept one that it did.
#   * docs/OWNER_DIRECTIVE_2026-11-05.md, for this exact case: "the reviewer's
#     own family rule skips both DeepSeek variants, so review lands on minimax
#     or luna".
#
# The stated reason for the hole — no eligible candidate when the builder falls
# back — was also measured false: the two routes in REVIEWER_FALLBACKS above
# resolve live and are cross-family, which is what the owner directive intends
# for that case. A same-family review is not an adversarial check; a green
# artifact that claims one is worse than a stopped loop, because the next
# iteration reads it as evidence. tools/runner/test_review_state.sh pins that
# this mechanism stays absent and that the `-th`/`flash` pair is REFUSED.

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
    ( cd "$ROOT" && timeout -k 30 "$ITER_TIMEOUT" sudo -n -H -u piagent env \
        NODE_EXTRA_CA_CERTS=/home/piagent/.pi/agent/ca-bundle.crt \
        PI_KNOWLEDGE_EMBEDDING="$PI_KNOWLEDGE_EMBEDDING" \
        PI_KNOWLEDGE_EMBEDDING_BASE_URL="$PI_KNOWLEDGE_EMBEDDING_BASE_URL" \
        PI_KNOWLEDGE_EMBEDDING_PREFIXES="$PI_KNOWLEDGE_EMBEDDING_PREFIXES" \
        PI_KNOWLEDGE_EMBEDDING_MAX_CHARS="$PI_KNOWLEDGE_EMBEDDING_MAX_CHARS" \
        "$PI_CMD" --model "$1" -p "$(cat "$2")" --no-session ) >"$3" 2>"$4"
}

model_alive() {
    timeout "$HEALTH_TIMEOUT" sudo -n -H -u piagent env \
        NODE_EXTRA_CA_CERTS=/home/piagent/.pi/agent/ca-bundle.crt \
        PI_KNOWLEDGE_EMBEDDING="$PI_KNOWLEDGE_EMBEDDING" \
        PI_KNOWLEDGE_EMBEDDING_BASE_URL="$PI_KNOWLEDGE_EMBEDDING_BASE_URL" \
        PI_KNOWLEDGE_EMBEDDING_PREFIXES="$PI_KNOWLEDGE_EMBEDDING_PREFIXES" \
        PI_KNOWLEDGE_EMBEDDING_MAX_CHARS="$PI_KNOWLEDGE_EMBEDDING_MAX_CHARS" \
        "$PI_CMD" --model "$1" \
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
    local other_loop_pid
    other_loop_pid="$(cat "$ROOT/logs/ralph/loop.pid" 2>/dev/null)"
    if [ -n "$other_loop_pid" ] && kill -0 "$other_loop_pid" 2>/dev/null; then
        echo "FATAL: another ralph loop (pid $other_loop_pid) is alive. Stop it first." >&2
        exit 2
    fi
    touch "$ROOT/logs/ralph/disabled"
    echo "$$" >"$PIDFILE"
    local probe_ok=0 model
    for model in $WORKER_MODELS; do
        if timeout "$PROBE_TIMEOUT" sudo -n -H -u piagent env \
            NODE_EXTRA_CA_CERTS=/home/piagent/.pi/agent/ca-bundle.crt \
            PI_KNOWLEDGE_EMBEDDING="$PI_KNOWLEDGE_EMBEDDING" \
            PI_KNOWLEDGE_EMBEDDING_BASE_URL="$PI_KNOWLEDGE_EMBEDDING_BASE_URL" \
            PI_KNOWLEDGE_EMBEDDING_PREFIXES="$PI_KNOWLEDGE_EMBEDDING_PREFIXES" \
            PI_KNOWLEDGE_EMBEDDING_MAX_CHARS="$PI_KNOWLEDGE_EMBEDDING_MAX_CHARS" \
            "$PI_CMD" --model "$model" \
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
# REVIEW_INFLIGHT_HEAD is an AUDIT field and nothing more: it names the far end of
# the lost range in the log line below, and no decision reads it. The owed set is
# keyed on the BASE alone, because the earliest base wins and the far end of a lost
# range is always at or before the current HEAD.
REVIEW_INFLIGHT_HEAD="$(state_get REVIEW_INFLIGHT_HEAD)"
REVIEW_LEFTOVER="$(state_get REVIEW_STATUS)"
# The three cross-iteration signals are read back so the end-of-iteration rewrite
# can re-emit them (external review 2026-11-01, run-20261101-062333/review-2.md
# AR6-3). They used to be written by `state_set` and then dropped by that rewrite,
# which emitted a fixed list of fourteen keys and never named these three, so the
# durable record never contained them: the "previous iteration left gates FAIL"
# warning could only fire if the previous process died between its own
# `state_set LAST_GATES FAIL` and its rewrite, and an operator reading `status`
# saw no gate result, no declared risk and no review decision at all.
#
# Iteration 47 (review-5 R5-2) removed the gates arm's mid-iteration write, because
# it sat inside the invoked-review window, so this rewrite is now the ONLY place
# LAST_GATES lands. Do not put a `state_set LAST_GATES` back here to make the warning
# "more current": the warning still fires, because the rewrite runs at the end of
# iteration N and the next prompt reads it at the start of iteration N+1.
LAST_GATES="$(state_get LAST_GATES)"
LAST_RISK="$(state_get LAST_RISK)"
LAST_REVIEW_DECISION="$(state_get LAST_REVIEW_DECISION)"
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
    # The status goes with the obligation (external review 2026-11-01,
    # run-20261101-025705/review-3.md F2) AND the whole transition lands in one
    # rename (same review cycle, run-20261101-062333/review-1.md AR-2). F2 needed
    # `failed` instead of the stale `invoked`, because the fold used to rewrite
    # everything except the status and so manufactured the `invoked`-with-no-range
    # state the arm below exists to refuse. AR-2 showed the fix was still written
    # as five `state_set` calls — five renames — so a kill between the status and
    # the in-flight clear left `failed` plus a still-set in-flight marker, the next
    # start folded the SAME review again, and one lost review reached the exit-7
    # limit. `failed` is the truth and one write is the delivery: a kill now leaves
    # either the whole old state (the fold runs once on restart) or the whole new
    # one (it does not run again), so a review cannot be lost twice.
    REVIEW_STATUS="failed"
    REVIEW_INFLIGHT_BASE=""
    REVIEW_INFLIGHT_HEAD=""
    state_write REVIEW_OWED_BASE="$REVIEW_OWED_BASE" REVIEW_FAILS="$REVIEW_FAILS" \
        REVIEW_STATUS="$REVIEW_STATUS" REVIEW_INFLIGHT_BASE="" REVIEW_INFLIGHT_HEAD=""
    log "that review is recorded as NOT done: ${REVIEW_OWED_BASE:0:8} is owed, and reviews lost without a verdict are now $REVIEW_FAILS of $REVIEW_FAIL_LIMIT"
    if [ "$REVIEW_FAILS" -ge "$REVIEW_FAIL_LIMIT" ]; then
        log "STOP: $REVIEW_FAILS reviews have been lost without a verdict — the harness is being restarted faster than reviews can be collected, which is a phase-limit or reviewer-route problem a model cannot fix from inside the loop"
        exit 7
    fi
elif [ "$REVIEW_LEFTOVER" = "invoked" ]; then
    # `invoked` with no in-flight range. Two different states wear this face, and
    # only one of them is unreconstructible (external review 2026-11-01,
    # run-20261101-025705/review-3.md F1 and F3).
    if [ -n "$REVIEW_OWED_BASE" ]; then
        # The obligation is ALREADY on disk: an earlier loss recorded it, and the
        # base it recorded is at or before whatever this `invoked` was covering,
        # because the owed set only ever moves to the earliest base. Halting here
        # stopped the loop on a state that had already done the right thing, and
        # the printed repair ("delete $STATE") throws the obligation away — a
        # fail-open escape hatch inside a fail-closed check. Normalise the stale
        # marker and continue; the next mandatory review covers this range.
        # It is NOT counted as a second lost review: the owed base already carries
        # the count of the loss that created it, and a review cannot be lost twice.
        REVIEW_STATUS="failed"
        state_set REVIEW_STATUS "failed"
        log "the state file says a review was invoked with nothing in flight, but ${REVIEW_OWED_BASE:0:8} is already recorded as owed, so the obligation survived; the stale 'invoked' marker is rewritten to 'failed' and the next mandatory review covers ${REVIEW_OWED_BASE:0:8}..HEAD"
    else
        # Nothing in flight and nothing owed: the range has to come from the HEAD
        # line. That is SOUND, not a guess (review-3 F3 corrected the rationale
        # this branch used to give, which was inverted): the HEAD line is written
        # at the END of an iteration, so it names the previous iteration's commit,
        # which is that iteration's HEAD_BEFORE and therefore the base the lost
        # review was dispatched from. It is an ancestor of the unreviewed commit,
        # so the range it builds OVER-covers — the unreviewed commit is inside it.
        # Over-coverage is what this branch must buy; under-coverage is what it must
        # not produce.
        #
        # The price is NOT "one redundant review", and this branch used to claim it
        # was (external review 2026-11-01, run-20261101-062333/review-1.md AR-1).
        # A widened range only costs extra scrutiny if the review that covers it can
        # discharge it, and `review_outcome` clears the owed base on rc 0 plus a
        # non-empty report WITHOUT reading the report's VERDICT — so a report that
        # says INCOMPLETE with a High finding still clears every commit the widening
        # swept in. Until the owner decides whether a verdict should gate clearing
        # (docs/CONTINUATION.md, owner-authorized task 3), the honest statement of
        # the cost is "an obligation that a non-verdict can discharge", not "one
        # redundant review". The widening is still right: an over-wide range is a
        # review that may be wasted, an under-wide one is a commit nobody sees.
        STATE_HEAD="$(state_get HEAD)"
        STATE_HEAD_SHA=""
        if [ -n "$STATE_HEAD" ]; then
            STATE_HEAD_SHA="$(git -C "$ROOT" rev-parse --verify --quiet "${STATE_HEAD}^{commit}" 2>/dev/null)"
        fi
        # Existence is not enough (external review 2026-11-01, run-20261101-062333
        # review-1.md AR-4). `rev-parse --verify` accepts ANY commit in the object
        # database, including one a builder `reset` or `rebase` abandoned, and
        # `git diff A..B` is a tree diff with no ancestry requirement: a base that
        # is not an ancestor of HEAD does not contain the unreviewed commit, it
        # removes it, so the reconstruction would be reviewed as a revert of that
        # commit instead of as the commit the lost review was covering. That is
        # under-coverage wearing over-coverage's clothes, which is the one thing
        # this branch is allowed not to do. Require the ancestry or refuse.
        if [ -n "$STATE_HEAD_SHA" ] && ! git -C "$ROOT" merge-base --is-ancestor "$STATE_HEAD_SHA" HEAD 2>/dev/null; then
            log "the state file's HEAD (${STATE_HEAD:0:8}) names a commit that is NOT an ancestor of the current HEAD, so it cannot be the base a lost review was dispatched from; refusing to build a range from it"
            STATE_HEAD_SHA=""
        fi
        if [ -n "$STATE_HEAD_SHA" ]; then
            REVIEW_OWED_BASE="$STATE_HEAD_SHA"
            REVIEW_FAILS=$((REVIEW_FAILS + 1))
            REVIEW_STATUS="failed"
            state_write REVIEW_OWED_BASE="$REVIEW_OWED_BASE" REVIEW_FAILS="$REVIEW_FAILS" REVIEW_STATUS="$REVIEW_STATUS"
            log "a review was left marked 'invoked' with no range recorded; the range is widened defensively to the state file's HEAD (${STATE_HEAD:0:8}), which is the PREVIOUS iteration's commit and therefore an ancestor of the unreviewed one, so the obligation is covered by over-coverage — and the cost is NOT bounded at one redundant review, because review_outcome clears an owed base on rc 0 plus a non-empty report without reading its VERDICT (owner decision: docs/CONTINUATION.md owner-authorized task 3); ${REVIEW_OWED_BASE:0:8} is owed and reviews lost without a verdict are now $REVIEW_FAILS of $REVIEW_FAIL_LIMIT"
            if [ "$REVIEW_FAILS" -ge "$REVIEW_FAIL_LIMIT" ]; then
                log "STOP: $REVIEW_FAILS reviews have been lost without a verdict — the harness is being restarted faster than reviews can be collected, which is a phase-limit or reviewer-route problem a model cannot fix from inside the loop"
                exit 7
            fi
        else
            # `invoked`, no range, nothing owed, and no HEAD line that names a
            # commit: there is genuinely nothing to reconstruct from, and guessing
            # would hand an unreviewed commit a false all-clear. Refuse.
            log "FATAL: the state file says a review was invoked but records no in-flight range, no owed base and no HEAD line that names a commit, so the obligation cannot be reconstructed at all. Repair it by writing 'REVIEW_OWED_BASE=<sha of the oldest unreviewed commit>' into $STATE and restarting. Deleting $STATE instead starts clean and DISCARDS the obligation, so the unreviewed commit is never reviewed by anyone; do that only if you have checked the log and know which commits were reviewed."
            exit 7
        fi
    fi
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

Search scope is strict: search only within this repository, scoped to the smallest
relevant source paths and excluding generated/build/output trees. Never run
recursive searches over /mnt/nas, /mnt/nas/Development, or sibling checkouts. If
repository evidence is insufficient, stop and report the precise missing reference;
do not broaden the search. Any recursive search must have a bounded timeout.

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
            log "iter=$ITER INTEGRITY GUARD FAILED; checking individual commits before recovery"
            cat "$RUN_DIR/integrity-$ITER.log" >>"$SUMMARY"
            if ! sudo -n -H -u piagent python3 "$ROOT/tools/runner/gates_batch_integrity.py" \
                    --base "$HEAD_BEFORE" --head "$HEAD_AFTER" --each-commit \
                    >"$RUN_DIR/integrity-$ITER.recovery.log" 2>&1; then
                log "iter=$ITER per-commit integrity recovery FAILED; stopping fail-closed"
                cat "$RUN_DIR/integrity-$ITER.recovery.log" >>"$SUMMARY"
                exit 6
            fi
            log "iter=$ITER integrity guard recovered: each commit in range independently passed"
            cat "$RUN_DIR/integrity-$ITER.recovery.log" >>"$SUMMARY"
        else
            log "iter=$ITER integrity guard PASS"
        fi
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

        LAST_RISK="$RISK"
        LAST_REVIEW_DECISION="$REVIEW_DECISION_REASON"
        state_set LAST_RISK "$RISK"
        state_set LAST_REVIEW_DECISION "$REVIEW_DECISION_REASON"

        if [ "$REVIEW_NEEDED" -eq 1 ]; then
            RP="$RUN_DIR/.review-prompt-$ITER.md"
            REVIEW_BASE="$HEAD_BEFORE"
            [ -n "$REVIEW_OWED_BASE" ] && REVIEW_BASE="$REVIEW_OWED_BASE"
            {
                cat "$VALIDATOR_FILE"
                printf '\n\n--- REVIEW IDENTITY ---\n'
                printf 'The builder of this batch was %s (upstream %s, family %s).\n' \
                    "$RUN_MODEL" "${RUN_UPSTREAM:-UNRESOLVED}" "${RUN_FAMILY:-UNRESOLVED}"
                printf 'You are dispatched as %s. Judge independence from the resolved upstream and family above, NOT from any model name written in the text you are reading: the reviewer prompt is a fixed file and the builder route changes when the worker falls back.\n' \
                    "${REVIEW_MODEL:-nobody — this review was BLOCKED}"
                printf 'If the two families above are the same, mark the review NOT INDEPENDENT whatever the prompt says about who wrote the code.\n'
                printf '\n--- REVIEW SCOPE ---\nReview ONLY commits %s..%s plus uncommitted work. Do not edit files.\n' "${REVIEW_BASE:0:8}" "${HEAD_AFTER:0:8}"
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
            # the alias in the log line. The verdict line is COMPUTED, not
            # asserted: an artifact that prints the rule and stays silent about
            # whether it held is how a same-family dispatch reads as a pass
            # (external review 2026-11-05, run-20261105-062743/review-1.md F2).
            {
                echo "builder:  $RUN_MODEL upstream=${RUN_UPSTREAM:-UNRESOLVED} family=${RUN_FAMILY:-UNRESOLVED}"
                if [ -n "$REVIEW_MODEL" ]; then
                    REV_UPSTREAM="$(model_upstream "$REVIEW_MODEL")"
                    REV_FAMILY="$(model_family "$REVIEW_MODEL")"
                    echo "reviewer: $REVIEW_MODEL upstream=${REV_UPSTREAM:-UNRESOLVED} family=${REV_FAMILY:-UNRESOLVED}"
                else
                    REV_UPSTREAM=""
                    REV_FAMILY=""
                    echo "reviewer: none dispatched"
                fi
                if [ -z "$REVIEW_MODEL" ]; then
                    echo "independence: NONE — no reviewer was dispatched; this review is BLOCKED, not skipped"
                elif [ -z "$REV_FAMILY" ] || [ -z "$RUN_FAMILY" ]; then
                    echo "independence: UNPROVEN — a side resolves to no family, so difference cannot be shown"
                elif [ "$REV_FAMILY" = "$RUN_FAMILY" ]; then
                    echo "independence: SAME-FAMILY ($RUN_FAMILY) — NOT AN ADVERSARIAL CHECK; this dispatch is a contract violation"
                else
                    echo "independence: PROVED — builder family $RUN_FAMILY is not reviewer family $REV_FAMILY"
                fi
                echo "scope:    ${REVIEW_BASE:0:8}..${HEAD_AFTER:0:8}"
                echo "rule:     independence is the resolved upstream and model family, never the alias string."
                echo "source:   the proxy's /model/info when it answers, else the committed map in tools/runner/model_identity.py; an unresolved alias is refused."
            } >"$RUN_DIR/review-$ITER.identity.txt"
            if [ -n "$REVIEW_MODEL" ] && [ "$(model_family "$REVIEW_MODEL")" = "$RUN_FAMILY" ]; then
                # Unreachable through the chain above (reviewer_reject_reason
                # refuses it), so this is a tripwire, not a branch: if it ever
                # prints, something dispatches reviewers without asking the guard.
                log "iter=$ITER BUG: dispatched $REVIEW_MODEL, which is the builder family ${RUN_FAMILY}"
            fi
            if [ -n "$REVIEW_MODEL" ]; then
                phase review
                # The obligation is recorded BEFORE the reviewer exists, as ONE
                # write (external review 2026-11-01, run-20261101-062333
                # /review-2.md AR6-1). The order used to be spawn-then-record, and
                # the comment here argued that "the range IS the obligation … so
                # the range is what has to survive a kill" while writing the range
                # AFTER the thing it had to survive: a kill between `REVIEW_PID=$!`
                # and the first rename left NOTHING on disk — no in-flight marker,
                # and a REVIEW_STATUS still reading `done` from the last collected
                # review — so a restart owed nothing and the commit under review
                # was never reviewed by anyone. That is worse than a lost review:
                # the reviewer's `timeout`/`sudo` child survives the parent, so a
                # complete report could sit in the run dir that no code ever reads.
                # One write also retires the ordering rule run-20261101-025705
                # review-3.md F4 asked for (range before the status derived from
                # it): there is no longer a between.
                #
                # Recording first inverts the failure. A kill between this rename
                # and the spawn leaves `invoked` with a range and no reviewer, which
                # startup folds as a lost review: owed, counted, and re-dispatched.
                # The worst case is one review of a range nobody looked at, which is
                # the same direction as every other arm here, and the state_write
                # itself stops the loop (AR6-4) rather than spawning a reviewer
                # whose verdict would have nowhere to be recorded.
                REVIEW_INFLIGHT_BASE="$REVIEW_BASE"
                REVIEW_INFLIGHT_HEAD="$HEAD_AFTER"
                REVIEW_STATUS="invoked"
                state_write REVIEW_INFLIGHT_BASE="$REVIEW_INFLIGHT_BASE" \
                    REVIEW_INFLIGHT_HEAD="$REVIEW_INFLIGHT_HEAD" REVIEW_STATUS="invoked"
                log "iter=$ITER review INVOKED on $REVIEW_MODEL — $REVIEW_DECISION_REASON (obligation recorded before the spawn)"
                ( timeout -k 30 "$REVIEW_TIMEOUT" sudo -n -H -u piagent env \
                    NODE_EXTRA_CA_CERTS=/home/piagent/.pi/agent/ca-bundle.crt \
                    PI_KNOWLEDGE_EMBEDDING="$PI_KNOWLEDGE_EMBEDDING" \
                    PI_KNOWLEDGE_EMBEDDING_BASE_URL="$PI_KNOWLEDGE_EMBEDDING_BASE_URL" \
                    PI_KNOWLEDGE_EMBEDDING_PREFIXES="$PI_KNOWLEDGE_EMBEDDING_PREFIXES" \
                    PI_KNOWLEDGE_EMBEDDING_MAX_CHARS="$PI_KNOWLEDGE_EMBEDDING_MAX_CHARS" \
                    "$PI_CMD" \
                    --model "$REVIEW_MODEL" -p "$(cat "$RP")" --no-session \
                    >"$RUN_DIR/review-$ITER.md" 2>"$RUN_DIR/review-$ITER.err"
                  echo $? >"$RUN_DIR/review-$ITER.rc" ) &
                REVIEW_PID=$!
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
                REVIEW_OWED_BASE="$(review_owed blocked "$REVIEW_OWED_BASE" "$HEAD_BEFORE")"
                # One rename for the whole record of the failure, for the AR-2
                # reason: written key by key, a kill between the status and the
                # owed base left `blocked` on disk with NOTHING owed, and a restart
                # that finds neither an in-flight range nor an owed base has no
                # place to carry the unreviewed commit — the obligation is simply
                # gone, which is the fail-open this arm exists to prevent. The
                # gates phase sits after this and takes minutes.
                REVIEW_STATUS="blocked"
                LAST_REVIEW_DECISION="BLOCKED: $REVIEW_DECISION_REASON"
                state_write REVIEW_STATUS="$REVIEW_STATUS" REVIEW_FAILS="$REVIEW_FAILS" \
                    REVIEW_OWED_BASE="$REVIEW_OWED_BASE" "LAST_REVIEW_DECISION=$LAST_REVIEW_DECISION"
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

    # The gates phase performs NO record write (external review 2026-11-01,
    # run-20261101-062333/review-5.md R5-2). It used to write LAST_GATES here, and
    # batch 17 had just made every record write able to stop the loop at exit 9, so
    # that write sat INSIDE the window the record reports: the reviewer may already
    # have finished and written review-$ITER.md/.rc, the durable record still said
    # `invoked` with both in-flight markers set, and a full disk on this one write
    # left exactly the state batch 18 removed from the collector — a COMPLETED
    # review that the startup fold reads as a lost one (owed again, REVIEW_FAILS + 1,
    # a second paid call for the same range, and two of them reach the
    # restart-prevented exit 7). This loop's own rule is that nothing may sit between
    # a verdict and the transition that records it; this was a second stop boundary
    # sitting in that window, at a site the batch-18 fix never looked at.
    #
    # The verdict is held in the shell and lands with the end-of-iteration rewrite,
    # which already re-emits LAST_GATES (AR6-3 counts it among the cross-iteration
    # signals the rewrite must re-emit). A stop AFTER the collector's transition is
    # the harmless class the three review counters already live in: the fold does not
    # fire on `done`, and the rewrite re-emits the key from the in-process value.
    # The cost is honest and small: if the loop dies between the gates phase and the
    # end of the iteration, `status` and the next prompt's PREVG warning can show the
    # PREVIOUS iteration's gates verdict. Nothing decides anything from LAST_GATES —
    # the gate-failure counter that stops the loop is FAILS, and it was already
    # deferred to this same rewrite — and the gates output itself is in
    # $RUN_DIR/gates-$ITER.log. Pinned by the gates-region guards in
    # tools/runner/test_review_state.sh (R5-2).
    phase gates
    GF="$RUN_DIR/gates-$ITER.log"
    if run_gates "$GF"; then
        LAST_GATES="PASS"
        FAILS=0
    else
        LAST_GATES="FAIL"
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
                ITERS_SINCE_REVIEW=0
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
            REVIEW_OWED_BASE="$(review_owed "$OUTCOME" "$REVIEW_OWED_BASE" "$HEAD_BEFORE")"
            # The verdict, the counter, the owed base and the two in-flight markers
            # are ONE transition and are written as one (external review
            # 2026-11-01, run-20261101-062333/review-1.md AR-2, applied to this
            # arm by the same argument as the startup fold). Written separately the
            # outcome landed before the in-flight markers went, so a kill in that
            # window left a COMPLETED review still marked in flight — and the next
            # start folded a review that had actually happened in as a second lost
            # one, which is a stop issued against a loop that was working.
            REVIEW_INFLIGHT_BASE=""
            REVIEW_INFLIGHT_HEAD=""
            state_write REVIEW_STATUS="$REVIEW_STATUS" REVIEW_FAILS="$REVIEW_FAILS" \
                REVIEW_OWED_BASE="$REVIEW_OWED_BASE" REVIEW_INFLIGHT_BASE="" REVIEW_INFLIGHT_HEAD=""
            # The three counters describe the review the transition above just
            # recorded, so they land AFTER it and never before it (external review
            # 2026-11-01, run-20261101-062333/review-4.md F1). They used to be here,
            # three `state_set`s above the transition — and batch 17 had just made
            # every record write able to stop the loop at exit 9, so a full disk
            # between them and the transition left the durable record saying
            # `invoked`, with the in-flight markers still set, AND `REVIEW_CALLS`
            # already advanced: a review the counter says happened and the status
            # says did not. The next start read that as a lost review and folded a
            # COMPLETED one in as a second loss — owed again, REVIEW_FAILS + 1, and
            # a second paid call for the same range — which is the exact failure
            # the AR-2 comment above exists to prevent, re-opened by a counter.
            # Measured against the shipped collector arm: a death before write 2, 3
            # or 4 of the old sequence left `status=invoked calls=4`; the window now
            # contains exactly ONE write, so the only way to lose the verdict is to
            # lose the transition itself. A death between the transition and these
            # three keys costs nothing: the fold does not fire on `done`, and the
            # end-of-iteration rewrite re-emits all three from the in-process
            # values, so the worst case is a cost counter one short and a checkpoint
            # that fires one iteration early — over-review, never under-review.
            if [ "$OUTCOME" = "done" ]; then
                state_set REVIEW_CALLS "$REVIEW_CALLS"
                state_set LAST_REVIEW_ITER "$ITER"
                state_set ITERS_SINCE_REVIEW "0"
            fi
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

    # The end-of-iteration rewrite goes through the same atomic writer, for the
    # same AR-2 reason — `>"$STATE"` TRUNCATES first, so a kill in the middle of
    # the hand-written list left a partial state file, potentially without the owed
    # base or the in-flight marker, which is the obligation disappearing rather than
    # merely being rewritten twice — and routing it through `state_write` fixes the
    # drift AR6-3 found in the list itself: a hand-written key list silently DROPS
    # any key the loop writes elsewhere, so these three vanished from the durable
    # record every single iteration. Now an unnamed key survives the rewrite instead
    # of disappearing from it, and the writer's status is checked (AR6-4).
    state_write "ITER=$ITER" "LAST_RC=$RC" "HEAD=$(git -C "$ROOT" rev-parse --short HEAD)" \
        "STALL=$STALL" "FAILS=$FAILS" "REVIEW_CALLS=$REVIEW_CALLS" \
        "ITERS_SINCE_REVIEW=$ITERS_SINCE_REVIEW" "REVIEW_SKIP_COUNT=$REVIEW_SKIP_COUNT" \
        "LAST_REVIEW_ITER=$LAST_REVIEW_ITER" "REVIEW_STATUS=$REVIEW_STATUS" \
        "REVIEW_FAILS=$REVIEW_FAILS" "REVIEW_OWED_BASE=$REVIEW_OWED_BASE" \
        "REVIEW_INFLIGHT_BASE=$REVIEW_INFLIGHT_BASE" "REVIEW_INFLIGHT_HEAD=$REVIEW_INFLIGHT_HEAD" \
        "LAST_GATES=$LAST_GATES" "LAST_RISK=$LAST_RISK" "LAST_REVIEW_DECISION=$LAST_REVIEW_DECISION"
    phase idle
    [ "$STALL" -ge "$STALL_LIMIT" ] && { log "STOP: $STALL consecutive stalled iterations"; exit 5; }
    [ "$FAILS" -ge "$FAIL_LIMIT" ] && { log "STOP: $FAILS consecutive gate failures"; exit 4; }
    [ "$ITER" -ge "$MAX_ITER" ] && { log "iteration cap ($MAX_ITER) reached"; exit 0; }
done

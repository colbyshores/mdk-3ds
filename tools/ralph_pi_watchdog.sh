#!/usr/bin/env bash
set -euo pipefail

ROOT="${RALPH_ROOT:-/mnt/nas/Development/mdk-sdl}"
SERVICE="${RALPH_SERVICE:-ralph-pi-mdk.service}"
PHASEFILE="$ROOT/logs/ralph-pi/phase"

# Two autonomous jobs:
#  1. heal a FAILED unit (crash, or a crash-loop that exhausted StartLimit);
#  2. restart the unit when a phase has outlived its limit.
# An explicit `systemctl stop` is honored: it leaves ActiveState=inactive with
# Result=success, and this timer never starts an inactive unit. A completed loop
# (exit 0 / DONE) is likewise inactive and stays stopped.
if ! systemctl is-active --quiet "$SERVICE"; then
    state="$(systemctl show -p ActiveState --value "$SERVICE" 2>/dev/null || true)"
    if [ "$state" = "failed" ]; then
        # A park code is a DECISION, not a crash. The unit sets
        # RestartPreventExitStatus=6 7 8 so that a blocked review (7), a failed
        # review dispatch (8) or an exhausted ledger row (6) stops for an operator.
        # Reviving those unconditionally is what turned one parked row into a
        # restart storm: the loop exited 6 in 30s, the watchdog restarted it, and
        # the attempt counter climbed to 79 without doing any work.
        park_code="$(systemctl show -p ExecMainStatus --value "$SERVICE" 2>/dev/null)"
        case "$park_code" in
            6|7|8)
                logger -t ralph-pi-watchdog "service parked (exit $park_code); NOT restarting — this needs an operator or a ledger change"
                exit 0 ;;
        esac
        logger -t ralph-pi-watchdog "service is failed; reset-failed + start"
        systemctl reset-failed "$SERVICE" >/dev/null 2>&1 || true
        systemctl start "$SERVICE" >/dev/null 2>&1 || true
    fi
    exit 0
fi
[ -r "$PHASEFILE" ] || exit 0

phase="$(sed -n 's/^phase=//p' "$PHASEFILE" | head -1)"
epoch="$(sed -n 's/^epoch=//p' "$PHASEFILE" | head -1)"
[[ "$epoch" =~ ^[0-9]+$ ]] || exit 0
age=$(( $(date +%s) - epoch ))

# Mirrors ralph-pi.sh phase_limit: this timer is the out-of-process backstop and
# must never be TIGHTER than the in-process watchdog, or it pre-empts it.
# The in-process limits are worker = ITER_TIMEOUT - 300 (14100 with a 4h wall),
# gates = 14400, review-collect = REVIEW_TIMEOUT + 600 (11400 with a 3h review).
# Every external limit therefore sits ABOVE its internal counterpart plus one
# 5-minute timer period.
case "$phase" in
    worker) limit=15000 ;;
    worker-fallback) limit=14700 ;;
    gates) limit=15000 ;;
    review-collect) limit=12000 ;;
    route-health|startup|verify) limit=900 ;;
    route-backoff) limit=600 ;;
    idle) limit=1800 ;;
    *) limit=1800 ;;
esac

if [ "$age" -gt "$limit" ]; then
    logger -t ralph-pi-watchdog "phase=$phase age=${age}s stalled > ${limit}s; restarting $SERVICE"
    systemctl restart "$SERVICE"
fi

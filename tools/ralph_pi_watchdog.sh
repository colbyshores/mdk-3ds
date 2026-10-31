#!/usr/bin/env bash
set -euo pipefail

ROOT="${RALPH_ROOT:-/mnt/nas/Development/mdk-sdl}"
SERVICE="${RALPH_SERVICE:-ralph-pi-mdk.service}"
PHASEFILE="$ROOT/logs/ralph-pi/phase"

systemctl is-active --quiet "$SERVICE" || exit 0
[ -r "$PHASEFILE" ] || exit 0

phase="$(sed -n 's/^phase=//p' "$PHASEFILE" | head -1)"
epoch="$(sed -n 's/^epoch=//p' "$PHASEFILE" | head -1)"
[[ "$epoch" =~ ^[0-9]+$ ]] || exit 0
age=$(( $(date +%s) - epoch ))

case "$phase" in
    worker) limit=7800 ;;
    worker-fallback) limit=7500 ;;
    gates) limit=14400 ;;
    review-collect) limit=5700 ;;
    route-health|startup|verify) limit=900 ;;
    route-backoff) limit=600 ;;
    idle) limit=1800 ;;
    *) limit=1800 ;;
esac

if [ "$age" -gt "$limit" ]; then
    logger -t ralph-pi-watchdog "phase=$phase age=${age}s limit=${limit}s; restarting $SERVICE"
    systemctl restart "$SERVICE"
fi

#!/usr/bin/env bash
# ============================================================================
# hw_cycle.sh -- one autonomous hardware cycle (ledger hw.* rows).
#
# The loop (Obsidian 20-runbooks/3ds-autonomous-hardware-profiling-loop.md):
# 3dslink pushes the harness .3dsx (built with `make -C mdk_3ds AUTOQUIT=N`),
# the game runs N seconds and quits by itself, chain-loading netload_stub;
# this script then pulls the guest log over the ATM sysmodule's HTTP (:8080)
# and refuses to call a stale log a result (freshness is the md5, never the
# push's exit code).
#
#   hw_cycle.sh --once [--ip <console-ip>] [--out <log>]
#
# The console address comes from --ip or $MDK_3DS_IP. The other modes the
# ledger names (--profile, --capture, --install, --sleep) are hardware
# steps of their own rows; they print what they need and exit 3 until
# their row wires them, rather than pretending to have run.
# ============================================================================
set -uo pipefail

MODE=""
IP="${MDK_3DS_IP:-}"
OUT="/tmp/mdk-hw-cycle.log"
while [ $# -gt 0 ]; do
    case "$1" in
        --once|--profile|--capture|--install|--sleep) MODE="$1"; shift ;;
        --ip) IP="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "hw_cycle: unknown argument: $1" >&2; exit 2 ;;
    esac
done

case "$MODE" in
    --once) ;;
    --profile|--capture|--install|--sleep)
        echo "hw_cycle: $MODE is a hardware step of its own ledger row and is not wired yet." >&2
        echo "          --once (push, self-quit, pull the log) is the wired cycle." >&2
        exit 3 ;;
    *) echo "usage: hw_cycle.sh --once [--ip <console-ip>] [--out <log>]" >&2; exit 2 ;;
esac

if [ -z "$IP" ]; then
    echo "hw_cycle: no console address (pass --ip or set MDK_3DS_IP)." >&2
    exit 2
fi

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DSX="$ROOT/mdk_3ds/mdk.3dsx"
if [ ! -f "$DSX" ]; then
    echo "hw_cycle: $DSX is not built (make -C mdk_3ds AUTOQUIT=90 first)." >&2
    exit 1
fi

PREV=""
[ -f "$OUT" ] && PREV=$(md5sum "$OUT" | cut -d' ' -f1)
T0=$(date +%s)
echo "hw_cycle: pushing $DSX to $IP"
3dslink -a "$IP" "$DSX" >/dev/null 2>&1 || { echo "hw_cycle: 3dslink FAILED" >&2; exit 1; }

# The run-complete signal is the guest log itself: poll for it to exist,
# then for its md5 to settle (the game appends frame lines while it runs).
LOG_URL="http://$IP:8080/3ds/mdk/mdk.log"
LAST=""
for i in $(seq 1 60); do
    if curl -s --max-time 10 -o /tmp/mdk-hw-cycle.pull "$LOG_URL" 2>/dev/null; then
        NOW=$(md5sum /tmp/mdk-hw-cycle.pull | cut -d' ' -f1)
        if [ -n "$LAST" ] && [ "$NOW" = "$LAST" ]; then
            break
        fi
        LAST="$NOW"
    fi
    if [ $(( $(date +%s) - T0 )) -gt 300 ]; then
        echo "hw_cycle: TIMEOUT waiting for the guest log" >&2
        exit 1
    fi
    sleep 3
done

cp /tmp/mdk-hw-cycle.pull "$OUT"
NOW=$(md5sum "$OUT" | cut -d' ' -f1)
echo "hw_cycle: cycle $(( $(date +%s) - T0 ))s, log $OUT"
if [ "$NOW" = "$PREV" ]; then
    echo "hw_cycle: STALE -- the run wrote nothing (same md5 as before)" >&2
    exit 1
fi
echo "hw_cycle: FRESH ($NOW)"

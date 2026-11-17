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

# The baseline must be the DEVICE log as it stands before this push, never the
# local output file. mdk.log is append-only across boots, so a card that has
# already run the game carries `--- session end ---` markers from earlier
# sessions; baselining on a missing local file counts 0 and the first pull then
# looks like a new run. Measured on the fake console: `cycle 0s, FRESH`.
PREV=""
PREV_END=0
T0=$(date +%s)

# ---- preflight: never push into a dead listener -----------------------------
# A push into a wedged or absent HTTP listener "succeeds" from 3dslink's point
# of view (it only fills a socket nobody drains) and then looks like a bad
# build. Probe the log endpoint first, recover the console if it is wedged, and
# refuse to continue if it never comes back.
LOG_URL="http://$IP:8080/3ds/mdk/mdk.log"
ATMCTL=/mnt/nas/Development/netload_stub/tools/atmctl.py
probe_log() {
    [ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$LOG_URL")" = "200" ]
}
if ! probe_log; then
    echo "hw_cycle: preflight -- $LOG_URL is not answering; attempting recovery" >&2
    if ping -c 1 -W 2 "$IP" >/dev/null 2>&1; then
        [ -f "$ATMCTL" ] && python3 "$ATMCTL" --ip "$IP" cmd reboot >/dev/null 2>&1
        for i in $(seq 1 45); do probe_log && break; sleep 4; done
    else
        echo "hw_cycle: console is ARP-dead (off, or not at network init yet); waiting up to 300s" >&2
        for i in $(seq 1 60); do
            ping -c 1 -W 2 "$IP" >/dev/null 2>&1 && probe_log && break
            sleep 5
        done
    fi
    if ! probe_log; then
        echo "hw_cycle: console unavailable -- $LOG_URL still not 200. Not pushing." >&2
        echo "hw_cycle: if ARP is dead too, the console is off; otherwise recover with:" >&2
        echo "    python3 $ATMCTL --ip $IP cmd reboot" >&2
        exit 1
    fi
    echo "hw_cycle: preflight recovered -- :8080 answers again"
fi

# Snapshot the device log BEFORE the push, so a marker written by an earlier
# session can never be read as this run's completion.
PREV_LOG="$OUT.prev"
if ! curl -s --max-time 10 -o "$PREV_LOG" "$LOG_URL" 2>/dev/null; then
    if [ -f "$OUT" ]; then
        cp "$OUT" "$PREV_LOG"
        echo "hw_cycle: WARNING -- could not snapshot the device log; baselining on the local copy $OUT" >&2
    else
        echo "hw_cycle: cannot establish the pre-push baseline ($LOG_URL unreadable, no local $OUT)." >&2
        echo "hw_cycle: not pushing -- a run whose completion cannot be told apart from an older one is not evidence." >&2
        exit 1
    fi
fi
PREV=$(md5sum "$PREV_LOG" | cut -d' ' -f1)
PREV_END=$(grep -c '^--- session end ---$' "$PREV_LOG" 2>/dev/null || true)
: "${PREV_END:=0}"
echo "hw_cycle: baseline $PREV_END session-end marker(s), md5 $PREV"

echo "hw_cycle: pushing $DSX to $IP"
3dslink -a "$IP" "$DSX" >/dev/null 2>&1 || { echo "hw_cycle: 3dslink FAILED" >&2; exit 1; }
push_done=$(date +%s)
echo "hw_cycle: push returned after $(( push_done - T0 ))s"

# ---- verify the push by the device's own ledger -----------------------------
# 3dslink's exit code is 0 even for a stalled transfer. transfers.log is the
# only evidence that bytes reached the card.
SIZE=$(stat -c %s "$DSX")
LEDGER_OK=0
for attempt in 1 2 3 4 5; do
    if LEDGER=$(curl -s --max-time 10 "http://$IP:8080/3ds/netload_stub/transfers.log" 2>/dev/null) \
       && [ -n "$LEDGER" ]; then
        LAST=$(printf '%s\n' "$LEDGER" | tail -1)
        case "$LAST" in
            *status=ok*bytes_out=$SIZE*) LEDGER_OK=1; break ;;
        esac
    fi
    sleep 2
done
if [ "$LEDGER_OK" = "1" ]; then
    echo "hw_cycle: push verified by transfers.log (bytes_out=$SIZE)"
else
    # A run of a build that is not this build is not evidence, and 3dslink
    # exits 0 even when the transfer lands nowhere (its progress line is stale
    # when stdout is a pipe). Refuse rather than report on an unknown binary.
    echo "hw_cycle: push UNVERIFIED -- the device ledger has no status=ok line with bytes_out=$SIZE." >&2
    [ -n "${LAST:-}" ] && echo "    last line: $LAST" >&2
    echo "hw_cycle: not measuring a run whose binary cannot be confirmed; check free SD space and :17491, then retry." >&2
    exit 1
fi


# The run-complete signal is a NEW `--- session end ---` marker in the guest
# log, not a settled md5. Every line is fflushed, so the marker is complete the
# moment it is served. md5 equality is unusable as the stop condition: the log
# is unchanged between the push and the game's first line, so two consecutive
# pulls match immediately and a partial log looks finished.
#
# A pull can also fail outright without meaning the run failed: the console's
# sysmodule can wedge on HTTP right after a clean exit, and a failed curl is
# then the wedge, not the build. Keep polling through failed pulls.
# The wait budget must not include the push: a 119 MB .3dsx has taken 160-650 s
# on this card (transfers.log: net_wait_ms alone has been 118 s), so a budget
# started before the push expires while the game is still running and reads
# exactly like a failed run.
WAIT=${MDK_HW_WAIT:-600}
PULL=/tmp/mdk-hw-cycle.pull
rm -f "$PULL"
PULL_FAILS=0
GOT=0
ENDS=$PREV_END
for i in $(seq 1 $(( WAIT / 3 ))); do
    # Remove the file before every attempt: curl truncates on success, but a
    # failed attempt can leave the PREVIOUS pull's bytes in place, and a stale
    # pull file is a ready-made `FRESH` (measured on the wedge fixture).
    rm -f "$PULL"
    if curl -s --max-time 10 -o "$PULL" "$LOG_URL" 2>/dev/null; then
        PULL_FAILS=0
        GOT=1
        ENDS=$(grep -c '^--- session end ---$' "$PULL" 2>/dev/null || true)
        if [ "${ENDS:-0}" -gt "$PREV_END" ]; then
            break
        fi
    else
        PULL_FAILS=$(( PULL_FAILS + 1 ))
        GOT=0
    fi
    if [ $(( $(date +%s) - push_done )) -gt "$WAIT" ]; then
        echo "hw_cycle: TIMEOUT after ${WAIT}s of polling (push was $(( push_done - T0 ))s)" >&2
        echo "hw_cycle: session-end markers seen=$ENDS (baseline $PREV_END), consecutive failed pulls=$PULL_FAILS" >&2
        if [ -s "$PULL" ]; then
            cp "$PULL" "$OUT.partial"
            echo "hw_cycle: kept the last pull as $OUT.partial (append-only log on SD survives the wedge)" >&2
        fi
        if [ "$PULL_FAILS" -gt 2 ]; then
            echo "hw_cycle: HTTP is not answering -- likely the post-exit wedge, NOT the build. Recover with:" >&2
            echo "    python3 /mnt/nas/Development/netload_stub/tools/atmctl.py --ip $IP cmd reboot" >&2
        else
            echo "hw_cycle: HTTP answers but no new session end -- raise MDK_HW_WAIT or check the run on SD." >&2
        fi
        exit 1
    fi
    sleep 3
done

if [ "$GOT" != "1" ]; then
    echo "hw_cycle: TIMEOUT -- no successful pull after $(( $(date +%s) - push_done ))s of polling." >&2
    echo "hw_cycle: HTTP stopped answering mid-run; that is the post-exit wedge, not the build. Recover with:" >&2
    echo "    python3 /mnt/nas/Development/netload_stub/tools/atmctl.py --ip $IP cmd reboot" >&2
    echo "hw_cycle: then pull /3ds/mdk/mdk.log -- the SD log survives the wedge, so the run is still on the card." >&2
    exit 1
fi
cp "$PULL" "$OUT"
ENDS=$(grep -c '^--- session end ---$' "$OUT" 2>/dev/null || true)
if [ "${ENDS:-0}" -le "$PREV_END" ]; then
    echo "hw_cycle: TIMEOUT waiting for a new session end marker (no marker after $(( $(date +%s) - T0 ))s)" >&2
    cp "$PULL" "$OUT.partial"
    exit 1
fi
NOW=$(md5sum "$OUT" | cut -d' ' -f1)
echo "hw_cycle: cycle $(( $(date +%s) - T0 ))s, log $OUT"
if [ "$NOW" = "$PREV" ]; then
    echo "hw_cycle: STALE -- the run wrote nothing (same md5 as before)" >&2
    exit 1
fi
echo "hw_cycle: FRESH ($NOW)"

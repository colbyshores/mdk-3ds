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
#   hw_cycle.sh --capture [--ip <console-ip>] [--out <log>]
#   hw_cycle.sh --profile [--ip <console-ip>] [--out <log>]
#
# --profile is --once plus the OG frame-period profile (ledger hw.profile-og):
# the payload must be built `make AUTOQUIT=90 PROFILE=1`, THIS session must log
# its `profile:` summary, and the console must report the Old-3DS family -- a
# New 3DS run is refused even at 268 MHz (AGENTS.md: not an OG pass).
#
# --capture is --once plus the hardware picture (ledger hw.pixels-rgba8): the
# payload must be built `make AUTOQUIT=90 CAPTURE=<frame>`, the run must log
# that it wrote sdmc:/3ds/mdk/hwcap.bmp.log in THIS session, and the BMP is
# pulled to <out>.bmp and checked against its own header.
#
# The console address comes from --ip or $MDK_3DS_IP. --sleep (hw.sleep-resume)
# is not wired: it prints why and exits 3 rather than pretending to have run.
# There is NO --install: this loop and netload_stub are .3dsx-only by owner
# decision (2026-11-17). CIA testing is a separate final step
# (docs/OWNER_BLOCKS.md, hw.cia-boot), never a hardware-loop mode.
# ============================================================================
set -uo pipefail

MODE=""
IP="${MDK_3DS_IP:-}"
OUT="/tmp/mdk-hw-cycle.log"
while [ $# -gt 0 ]; do
    case "$1" in
        --once|--profile|--capture|--sleep) MODE="$1"; shift ;;
        --ip) IP="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "hw_cycle: unknown argument: $1" >&2; exit 2 ;;
    esac
done

case "$MODE" in
    --once|--capture|--profile) ;;
    --sleep)
        echo "hw_cycle: $MODE is a hardware step of its own ledger row and is not wired yet." >&2
        echo "          --once (push, self-quit, pull the log) is the wired cycle." >&2
        exit 3 ;;
    *) echo "usage: hw_cycle.sh --once|--capture|--profile [--ip <console-ip>] [--out <log>]" >&2; exit 2 ;;
esac

if [ -z "$IP" ]; then
    echo "hw_cycle: no console address (pass --ip or set MDK_3DS_IP)." >&2
    exit 2
fi

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# MDK_HW_DSX is the self-test's seam for feeding a payload that is not the
# tree's build (the "payload cannot end a run" case).
DSX="${MDK_HW_DSX:-$ROOT/mdk_3ds/mdk.3dsx}"
if [ ! -f "$DSX" ]; then
    echo "hw_cycle: $DSX is not built (make -C mdk_3ds AUTOQUIT=90 first)." >&2
    exit 1
fi

# REFUSE A PAYLOAD THAT CANNOT END ITS RUN. Without -DMDK_AUTOQUIT,
# harness_3ds.cpp links the no-op answers: harnessAutoquitDue() is a constant
# false and harnessChainloadStub() does nothing, so the game runs until someone
# stops it, no `--- session end ---` can appear, and the only reachable verdict
# is a timeout on a console still sitting in the game. The `harness autoquit`
# STRING is not evidence -- it is a literal in main_3ds.cpp in every build; the
# chainload path is, because netload_stub only exists under the macro.
# grep reads the .3dsx directly (`-a`) rather than piping `strings`: the script
# is `set -uo pipefail`, and `grep -q` closing the pipe early would make
# `strings` die of SIGPIPE and report a FALSE refusal on a good payload.
if ! grep -aqF -- '/3ds/netload_stub.3dsx' "$DSX"; then
    echo "hw_cycle: $DSX has no harness chainload -- it was built WITHOUT -DMDK_AUTOQUIT." >&2
    echo "hw_cycle: that payload cannot end a run, so this cycle could only ever time out. Rebuild:" >&2
    echo "    make -C mdk_3ds -j\$(nproc) AUTOQUIT=90" >&2
    exit 1
fi
# --capture needs a payload that writes the picture; the path literal exists
# only under -DMDK_HW_CAPTURE (harness_3ds.cpp), so it is the evidence.
CAP_NAME=hwcap.bmp.log
if [ "$MODE" = "--capture" ] && ! grep -aqF -- "sdmc:/3ds/mdk/$CAP_NAME" "$DSX"; then
    echo "hw_cycle: $DSX has no harness capture -- it was built WITHOUT CAPTURE=<frame>. Rebuild:" >&2
    echo "    make -C mdk_3ds clean && make -C mdk_3ds -j\$(nproc) AUTOQUIT=90 CAPTURE=600" >&2
    exit 1
fi
# --profile likewise: the console-model line's format string exists only under
# -DMDK_HW_PROFILE (harness_3ds.cpp).
if [ "$MODE" = "--profile" ] && ! grep -aqF -- "profile: console model=" "$DSX"; then
    echo "hw_cycle: $DSX has no harness profile -- it was built WITHOUT PROFILE=1. Rebuild:" >&2
    echo "    make -C mdk_3ds clean && make -C mdk_3ds -j\$(nproc) AUTOQUIT=90 PROFILE=1" >&2
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
# WAKE THE ATM SERVICE LOOP BEFORE EVERY HTTP READ. On firmware 9f3632fe047c
# the ATM thread parks on its discovery socket (socPoll(...,0) is wait-for-
# event on SOC), so :8080 ACCEPTS a TCP connection and then sends 0 bytes --
# the state AGENTS.md trap 8 had been calling a sysmodule wedge and "fixing"
# with reboots that did not help. A DIRECTED discovery probe wakes the loop and
# HTTP answers at once (Obsidian 10-projects/netload_stub/atm/troubleshooting.md,
# row "service loop parked"; measured 2026-11-17: two reboots, boot_count
# 655->657, left :8080 at 0 bytes; one `atmctl --ip discover` restored a full
# status.txt). Best-effort and bounded: a failed wake is not a verdict, the
# HTTP read after it is. MDK_HW_WAKE=0 disables it (the self-test's fake
# console has no discovery responder).
# THE LOG READ NEEDS A REAL BUDGET. mdk.log is append-only across sessions, so
# it only grows (725 KB on 2026-11-17), and the ATM HTTP server serves it at
# ~28 KB/s with up to 40 s before the first byte; it ignores Range. A 10 s
# --max-time therefore cut every read short and read as an unreadable console
# (measured: 654,672 of 724,968 B at 60 s, full reads in 26-52 s).
HTTP_T=${MDK_HW_HTTP_TIMEOUT:-180}
wake() {
    [ "${MDK_HW_WAKE:-1}" = "1" ] && [ -f "$ATMCTL" ] || return 0
    timeout 8 python3 "$ATMCTL" --ip "$IP" discover >/dev/null 2>&1 || true
}
# The preflight asks only "does the HTTP server answer at all" -- any status
# line, on a path that costs nothing to serve. Downloading the whole log here
# with the long budget made the 45/60-try recovery loops worth hours.
probe_log() {
    wake
    [ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 20 "http://$IP:8080/luma/atm/status.txt")" != "000" ]
}
if ! probe_log; then
    echo "hw_cycle: preflight -- $LOG_URL is not answering; attempting recovery" >&2
    if ping -c 1 -W 2 "$IP" >/dev/null 2>&1; then
        # probe_log already woke the loop once; a reboot is the fallback for a
        # console that a directed discovery could not wake.
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
        echo "hw_cycle: if ARP is dead too, the console is off; otherwise try a directed discovery, then:" >&2
        echo "    python3 $ATMCTL --ip $IP cmd reboot" >&2
        exit 1
    fi
    echo "hw_cycle: preflight recovered -- :8080 answers again"
fi

# Snapshot the device log BEFORE the push, so a marker written by an earlier
# session can never be read as this run's completion.
PREV_LOG="$OUT.prev"
# Rotation can only be judged against the DEVICE's own snapshot: a local copy
# from an earlier cycle may predate a rotation the card has already done, and
# then every later pull would "not begin with the snapshot".
ROTATION_OK=1
wake
# --fail: a 403/404 error body must not become the baseline.
if ! curl -s --fail --max-time "$HTTP_T" -o "$PREV_LOG" "$LOG_URL" 2>/dev/null; then
    if [ -f "$OUT" ]; then
        ROTATION_OK=0
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
# The stub may not be listening YET: the previous run's chain-load relaunches
# it only after that run's teardown, and a heavy level tears down slowly.
# Measured 2026-11-17: level 3 exited cleanly, and the next cycle's 3dslink was
# refused, along with four more pushes in the following two minutes; minutes
# later the same push went through. So a refused push is retried, bounded:
# MDK_HW_PUSH_TRIES attempts (default 12), MDK_HW_PUSH_GAP seconds apart (10).
# ALWAYS ARRIVE AS mdk.3dsx. netload_stub saves a push under the file's own
# basename, so a payload named mdk-L4.3dsx is a NEW 119 MB file on the card,
# not an overwrite. Measured 2026-11-17: per-level payloads filled the SD card
# and every later push ended `status=disk_full_or_io` in transfers.log. A
# differently named payload is pushed through a symlink named mdk.3dsx.
PUSH_FILE="$DSX"
if [ "$(basename "$DSX")" != "mdk.3dsx" ]; then
    PUSH_DIR=$(mktemp -d)
    trap 'rm -rf "$PUSH_DIR"' EXIT
    ln -s "$(readlink -f "$DSX")" "$PUSH_DIR/mdk.3dsx"
    PUSH_FILE="$PUSH_DIR/mdk.3dsx"
    echo "hw_cycle: pushing $(basename "$DSX") under the name mdk.3dsx (the stub keeps one file per name)"
fi
PUSH_TRIES=${MDK_HW_PUSH_TRIES:-12}
PUSH_GAP=${MDK_HW_PUSH_GAP:-10}
# THE STUB CAN BE GONE, not just late. Measured 2026-11-17: after a clean
# level-5 exit the chain-load did not happen, the console sat at HOME with no
# 3dsx_app process (GDB `info os processes`), and nothing listened on 17491.
# ATM's `launch` cannot start a stub it did not stage, but a reboot autoboots
# it. So after MDK_HW_REBOOT_AFTER refused pushes (default 3) the harness
# reboots the console ONCE over UDP 8082 and keeps retrying.
REBOOT_AFTER=${MDK_HW_REBOOT_AFTER:-3}
REBOOT_WAIT=${MDK_HW_REBOOT_WAIT:-90}
rebooted=0
pushed=0
for attempt in $(seq 1 "$PUSH_TRIES"); do
    if 3dslink -a "$IP" "$PUSH_FILE" >/dev/null 2>&1; then pushed=1; break; fi
    echo "hw_cycle: 3dslink refused (attempt $attempt/$PUSH_TRIES); the stub may still be relaunching" >&2
    if [ "$rebooted" = "0" ] && [ "$attempt" -ge "$REBOOT_AFTER" ]; then
        rebooted=1
        echo "hw_cycle: rebooting the console once so it autoboots the stub (UDP cmd reboot), waiting ${REBOOT_WAIT}s" >&2
        [ -f "$ATMCTL" ] && python3 "$ATMCTL" --ip "$IP" cmd reboot >/dev/null 2>&1
        sleep "$REBOOT_WAIT"
    fi
    [ "$attempt" -lt "$PUSH_TRIES" ] && { sleep "$PUSH_GAP"; wake; }
done
[ "$pushed" = "1" ] || { echo "hw_cycle: 3dslink FAILED after $PUSH_TRIES attempts" >&2; exit 1; }
push_done=$(date +%s)
echo "hw_cycle: push returned after $(( push_done - T0 ))s"

# ---- verify the push by the device's own ledger -----------------------------
# 3dslink's exit code is 0 even for a stalled transfer. transfers.log is the
# only evidence that bytes reached the card.
SIZE=$(stat -c %s "$DSX")
LEDGER_OK=0
for attempt in 1 2 3 4 5; do
    wake
    if LEDGER=$(curl -s --fail --max-time "$HTTP_T" "http://$IP:8080/3ds/netload_stub/transfers.log" 2>/dev/null) \
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
# BASE is the marker count a pull must beat. It starts as the snapshot's, and
# drops to 0 when the game has ROTATED the log at boot (log_3ds.cpp moves a log
# over 128 KiB to mdk.prev.log): the new file then holds only this session, so
# its count can be BELOW the snapshot's and a count-only verdict would time out
# on a run that finished. Rotation is recognised by the pull no longer
# beginning with the snapshot's bytes AND by the game's own statement of it: the
# new log's first line is the session header and its next names the rotated
# size, which is the snapshot's size (nothing is appended between the snapshot
# and this run's boot). A pull that merely differs -- a truncated or foreign
# body -- is NOT a rotation, and the baseline stays where it was.
BASE=$PREV_END
ROTATED=0
PREV_SIZE=$(stat -c %s "$PREV_LOG")
for i in $(seq 1 $(( WAIT / 3 ))); do
    # Remove the file before every attempt: curl truncates on success, but a
    # failed attempt can leave the PREVIOUS pull's bytes in place, and a stale
    # pull file is a ready-made `FRESH` (measured on the wedge fixture).
    rm -f "$PULL"
    wake
    if curl -s --fail --max-time "$HTTP_T" -o "$PULL" "$LOG_URL" 2>/dev/null; then
        PULL_FAILS=0
        GOT=1
        ENDS=$(grep -c '^--- session end ---$' "$PULL" 2>/dev/null || true)
        if [ "$ROTATED" = "0" ] && [ "$ROTATION_OK" = "1" ] && [ "$PREV_SIZE" -gt 0 ] \
           && ! cmp -s -n "$PREV_SIZE" "$PREV_LOG" "$PULL" \
           && [ "$(head -1 "$PULL")" = "--- mdk 3ds session ---" ] \
           && sed -n 2p "$PULL" | grep -qxF "log: rotated the previous $PREV_SIZE-byte log to mdk.prev.log"; then
            ROTATED=1
            BASE=0
            echo "hw_cycle: log rotated by the game at boot (it no longer begins with the snapshot); baseline is now 0"
        fi
        if [ "${ENDS:-0}" -gt "$BASE" ]; then
            break
        fi
    else
        PULL_FAILS=$(( PULL_FAILS + 1 ))
        GOT=0
    fi
    if [ $(( $(date +%s) - push_done )) -gt "$WAIT" ]; then
        echo "hw_cycle: TIMEOUT after ${WAIT}s of polling (push was $(( push_done - T0 ))s)" >&2
        echo "hw_cycle: session-end markers seen=$ENDS (baseline $BASE), consecutive failed pulls=$PULL_FAILS" >&2
        if [ -s "$PULL" ]; then
            cp "$PULL" "$OUT.partial"
            echo "hw_cycle: kept the last pull as $OUT.partial (append-only log on SD survives the wedge)" >&2
        fi
        if [ "$PULL_FAILS" -gt 2 ]; then
            echo "hw_cycle: HTTP is not answering -- likely the post-exit wedge, NOT the build. Recover with a directed discovery first, reboot only if that fails:" >&2
            echo "    python3 $ATMCTL --ip $IP discover   # then: cmd reboot" >&2
        else
            echo "hw_cycle: HTTP answers but no new session end -- raise MDK_HW_WAIT or check the run on SD." >&2
        fi
        exit 1
    fi
    sleep 3
done

if [ "$GOT" != "1" ]; then
    echo "hw_cycle: TIMEOUT -- no successful pull after $(( $(date +%s) - push_done ))s of polling." >&2
    echo "hw_cycle: HTTP stopped answering mid-run; that is the post-exit wedge, not the build. Recover with a directed discovery first, reboot only if that fails:" >&2
    echo "    python3 $ATMCTL --ip $IP discover   # then: cmd reboot" >&2
    echo "hw_cycle: then pull /3ds/mdk/mdk.log -- the SD log survives the wedge, so the run is still on the card." >&2
    exit 1
fi
cp "$PULL" "$OUT"
ENDS=$(grep -c '^--- session end ---$' "$OUT" 2>/dev/null || true)
if [ "${ENDS:-0}" -le "$BASE" ]; then
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

# The newest session: everything after the last `--- mdk 3ds session ---`.
SESSION=$(awk '/^--- mdk 3ds session ---$/{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' "$OUT")

if [ "$MODE" = "--profile" ]; then
    PROF=$(printf '%s' "$SESSION" | grep -F 'profile: frames=' | tail -1)
    CONS=$(printf '%s' "$SESSION" | grep -F 'profile: console model=' | tail -1)
    if [ -z "$PROF" ] || [ -z "$CONS" ]; then
        echo "hw_cycle: PROFILE MISSING -- this session logged no profile summary." >&2
        exit 1
    fi
    echo "hw_cycle: $PROF"
    # The steady-state line (frames 3 on, and the vblank spread), when the
    # payload logs one: the p50 alone is quantised by the 60 Hz refresh.
    STEADY=$(printf '%s' "$SESSION" | grep -F 'profile: steady ' | tail -1)
    [ -n "$STEADY" ] && echo "hw_cycle: $STEADY"
    SPANS=$(printf '%s' "$SESSION" | grep -F 'profile: spans ' | tail -1)
    [ -n "$SPANS" ] && echo "hw_cycle: $SPANS"
    STEPS=$(printf '%s' "$SESSION" | grep -F 'profile: step ' | tail -1)
    [ -n "$STEPS" ] && echo "hw_cycle: $STEPS"
    # A WALK payload's segment starts and the arena-entry table (harness_3ds.h),
    # when the payload logs them: printed at exit, so they are this session's.
    printf '%s' "$SESSION" | grep -E '^(walk: frame |profile: arena)' | sed 's/^/hw_cycle: /' || true
    echo "hw_cycle: $CONS"
    case "$CONS" in
        *family=old3ds*) echo "hw_cycle: PROFILE OG (Old-3DS family)" ;;
        *) echo "hw_cycle: NOT AN OG RESULT -- the console is not the Old-3DS family." >&2; exit 1 ;;
    esac
    exit 0
fi

[ "$MODE" = "--capture" ] || exit 0

# ---- the picture -------------------------------------------------------------
# The file on the card is overwritten by every capture run, so its presence
# proves nothing about THIS run. The proof is THIS session's own line: the
# renderer logs `screenshot: wrote <path>` only after a complete write. The
# session is everything after the last `--- mdk 3ds session ---` header.
if ! printf '%s' "$SESSION" | grep -qF "screenshot: wrote sdmc:/3ds/mdk/$CAP_NAME"; then
    echo "hw_cycle: CAPTURE MISSING -- this session did not log a completed write of $CAP_NAME." >&2
    printf '%s' "$SESSION" | grep -F 'screenshot:' | sed 's/^/    /' >&2
    exit 1
fi
CAP_OUT="$OUT.bmp"
rm -f "$CAP_OUT"
wake
if ! curl -s --fail --max-time "$HTTP_T" -o "$CAP_OUT" "http://$IP:8080/3ds/mdk/$CAP_NAME" 2>/dev/null; then
    echo "hw_cycle: CAPTURE UNREADABLE -- the run wrote it but the pull failed; it is still on the card." >&2
    exit 1
fi
# A BMP states its own size (bytes 2-5, little-endian); a short pull cannot
# pass for a picture.
CAP_SIZE=$(stat -c %s "$CAP_OUT")
read -r MAGIC DECL W H < <(python3 -c '
import struct, sys
b = open(sys.argv[1], "rb").read(26)
if len(b) < 26:
    print("-- 0 0 0")
else:
    print(b[:2].decode("latin1"), struct.unpack("<I", b[2:6])[0], *struct.unpack("<ii", b[18:26]))
' "$CAP_OUT")
if [ "$MAGIC" != "BM" ] || [ "$DECL" != "$CAP_SIZE" ]; then
    echo "hw_cycle: CAPTURE MALFORMED -- magic '$MAGIC', header says $DECL bytes, pulled $CAP_SIZE." >&2
    exit 1
fi
echo "hw_cycle: CAPTURE ${W}x${H} $CAP_SIZE bytes -> $CAP_OUT ($(md5sum "$CAP_OUT" | cut -d' ' -f1))"

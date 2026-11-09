#!/usr/bin/env bash
# reclaim_qwen_slots.sh -- root-only recovery for the qwen admission gate.
#
# Finds who is actually holding each slot lock, shows the evidence (pid, ppid,
# age, cgroup, cmdline, ancestor chain), and -- only with --kill -- terminates
# holders that are NOT a live turn. Then optionally unlinks stuck slot files
# and restarts the MDK loop.
#
# /proc/locks is deliberately not used to pick victims: its PID is the transient
# `flock` utility, which always exits immediately, so a "dead owner" there is
# normal and says nothing about whether the slot is leaked.
#
# Usage:
#   sudo tools/reclaim_qwen_slots.sh                 # report only
#   sudo tools/reclaim_qwen_slots.sh --kill          # kill orphan/leftover holders
#   sudo tools/reclaim_qwen_slots.sh --kill --unlink --restart
#   sudo tools/reclaim_qwen_slots.sh --kill --kill-live   # also kill live turns (DANGEROUS)
# Env: RECLAIM_ALLOW_NONROOT=1 (tests only)
set -euo pipefail

SLOT_DIR=${QWEN_SLOT_DIR:-/dev/shm/qwen-slots}
SERVICE=${RALPH_SERVICE:-ralph-pi-mdk.service}
DO_KILL=0
KILL_LIVE=0
DO_UNLINK=0
DO_RESTART=0

while [ $# -gt 0 ]; do
    case $1 in
        --kill) DO_KILL=1 ;;
        --kill-live) DO_KILL=1; KILL_LIVE=1 ;;
        --unlink) DO_UNLINK=1 ;;
        --restart) DO_RESTART=1 ;;
        --service) SERVICE=${2:?--service needs a value}; shift ;;
        --slot-dir) SLOT_DIR=${2:?--slot-dir needs a value}; shift ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "reclaim: unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

if [ "${RECLAIM_ALLOW_NONROOT:-0}" != "1" ] && [ "$(id -u)" -ne 0 ]; then
    echo "reclaim: must run as root: sudo $0" >&2
    exit 1
fi
[ -d "$SLOT_DIR" ] || { echo "reclaim: $SLOT_DIR does not exist; nothing to do."; exit 0; }

# returns 0 when the slot is LOCKED, 1 when free
slot_locked() {
    local file=$1
    ( exec 9>>"$file" 2>/dev/null || exit 0
      if flock -n 9 2>/dev/null; then exit 1; else exit 0; fi )
}

fd_holders() {
    local want f di pid
    want=$(stat -c '%d:%i' "$1" 2>/dev/null) || return 0
    for f in /proc/[0-9]*/fd/*; do
        [ -L "$f" ] || continue
        di=$(stat -L -c '%d:%i' "$f" 2>/dev/null) || continue
        [ "$di" = "$want" ] || continue
        pid=${f#/proc/}
        echo "${pid%%/*}"
    done | sort -un
}

# A process is part of a live turn if it, or any ancestor, is the slot wrapper
# or the pi agent. A leftover MCP/tool child of a killed turn has neither.
is_live_turn() {
    local pid=$1 depth=0 cmd
    while [ -n "$pid" ] && [ "$pid" != "0" ] && [ "$depth" -lt 12 ]; do
        cmd=$(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null | cut -c1-200 || true)
        case $cmd in
            *qwen-slot-pi.sh*|*/pi|*/pi\ *|pi|pi\ *) return 0 ;;
        esac
        pid=$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ' || true)
        [ -n "$pid" ] || return 1
        depth=$((depth + 1))
    done
    return 1
}

describe() {
    local pid=$1 ppid etime cg cmd
    ppid=$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ' || true)
    etime=$(ps -o etime= -p "$pid" 2>/dev/null | tr -d ' ' || true)
    cg=$(sed -n 's/^0:://p' "/proc/$pid/cgroup" 2>/dev/null | head -1 || true)
    cmd=$(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null | cut -c1-120 || true)
    printf '  holder pid=%s ppid=%s age=%s cgroup=%s\n    %s\n' \
        "$pid" "${ppid:-?}" "${etime:-?}" "${cg:-?}" "${cmd:-<unreadable>}"
}

kill_targets=()
unlink_targets=()

for file in "$SLOT_DIR"/slot-*; do
    [ -e "$file" ] || continue
    case $file in *.owner) continue ;; esac
    echo "== $file"
    if slot_locked "$file"; then
        echo "  LOCKED"
    else
        echo "  free"
        continue
    fi

    holders=$(fd_holders "$file")
    if [ -z "$holders" ]; then
        echo "  locked, but no live process holds the inode (stale entry)"
        unlink_targets+=("$file")
        continue
    fi

    for pid in $holders; do
        [ "$pid" = "$$" ] && continue
        describe "$pid"
        if is_live_turn "$pid"; then
            echo "    => LIVE TURN: will not kill"
        else
            echo "    => ORPHAN/LEFTOVER"
            kill_targets+=("$pid")
            unlink_targets+=("$file")
        fi
    done
done

echo
if [ "${#kill_targets[@]}" -eq 0 ] && [ "${#unlink_targets[@]}" -eq 0 ]; then
    echo "reclaim: every locked slot is held by a live turn; nothing to reclaim."
    exit 0
fi

if [ "$DO_KILL" -eq 0 ]; then
    echo "reclaim: report only. Re-run with --kill (and --unlink --restart) to act."
    printf '  would kill:%s\n' " ${kill_targets[*]:- none}"
    exit 0
fi

if [ "${#kill_targets[@]}" -gt 0 ]; then
    if [ "$KILL_LIVE" -eq 0 ]; then
        echo "reclaim: TERM ${kill_targets[*]}"
        kill -TERM "${kill_targets[@]}" 2>/dev/null || true
        sleep 3
        for pid in "${kill_targets[@]}"; do
            [ -d "/proc/$pid" ] && { echo "  KILL $pid"; kill -KILL "$pid" 2>/dev/null || true; }
        done
        sleep 1
    else
        echo "reclaim: KILL-LIVE requested; terminating every holder: ${kill_targets[*]}"
        kill -KILL "${kill_targets[@]}" 2>/dev/null || true
        sleep 1
    fi
fi

if [ "$DO_UNLINK" -eq 1 ]; then
    for file in "${unlink_targets[@]}"; do
        if slot_locked "$file"; then
            echo "reclaim: $file still locked; unlinking it (waiters remain on the old inode)"
            rm -f "$file" "$file".owner
        fi
    done
fi

if [ "$DO_RESTART" -eq 1 ]; then
    echo "reclaim: restarting $SERVICE"
    systemctl restart "$SERVICE"
fi

echo "== after =="
for file in "$SLOT_DIR"/slot-*; do
    [ -e "$file" ] || continue
    case $file in *.owner) continue ;; esac
    if slot_locked "$file"; then echo "  $file: LOCKED"; else echo "  $file: free"; fi
done
systemctl is-active "$SERVICE" 2>/dev/null || true

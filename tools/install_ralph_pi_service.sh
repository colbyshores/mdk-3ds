#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVICE="ralph-pi-mdk.service"
UNIT="/etc/systemd/system/$SERVICE"
RUN_USER="coleshores"
RUN_GROUP="development"
PIDFILE="$ROOT/logs/ralph-pi/loop.pid"
LOCKFILE="$ROOT/logs/ralph-pi/loop.lock"
WATCHDOG_SERVICE="ralph-pi-mdk-watchdog.service"
WATCHDOG_TIMER="ralph-pi-mdk-watchdog.timer"
WATCHDOG_BIN="/usr/local/libexec/ralph-pi-mdk-watchdog"

if [ "$(id -u)" -ne 0 ]; then
    printf 'Run this installer as root.\n' >&2
    exit 1
fi

id "$RUN_USER" >/dev/null
getent group "$RUN_GROUP" >/dev/null
id -nG "$RUN_USER" | tr ' ' '\n' | grep -Fxq "$RUN_GROUP" || {
    printf '%s is not a member of %s. Add membership, then log in again.\n' "$RUN_USER" "$RUN_GROUP" >&2
    exit 1
}

if systemctl is-active --quiet "$SERVICE"; then
    systemctl stop "$SERVICE"
fi

if [ -r "$PIDFILE" ]; then
    OLD_PID="$(tr -cd '0-9' <"$PIDFILE")"
    if [ -n "$OLD_PID" ] && kill -0 "$OLD_PID" 2>/dev/null; then
        OLD_CMD="$(ps -p "$OLD_PID" -o args= 2>/dev/null || true)"
        case "$OLD_CMD" in
            *ralph-pi.sh*)
                printf 'Standalone Ralph loop PID %s is still active; stop it cleanly and rerun installer.\n' "$OLD_PID" >&2
                exit 1
                ;;
        esac
    fi
fi

if [ -r "$LOCKFILE" ] && command -v flock >/dev/null 2>&1; then
    exec 9<>"$LOCKFILE"
    if ! flock -n 9; then
        printf 'A Ralph loop still holds %s. Stop it cleanly, preserve its dirty work, then rerun installer.\n' "$LOCKFILE" >&2
        exit 1
    fi
    flock -u 9
    exec 9>&-
fi

install -d -o "$RUN_USER" -g "$RUN_GROUP" -m 2775 "$ROOT/logs/ralph-pi"

cat >"$UNIT" <<EOF
[Unit]
Description=MDK Ralph Pi Loop
After=network-online.target
Wants=network-online.target
StartLimitIntervalSec=30min
StartLimitBurst=3

[Service]
Type=simple
User=$RUN_USER
SupplementaryGroups=$RUN_GROUP
WorkingDirectory=$ROOT
Environment=PATH=/home/$RUN_USER/.local/bin:/usr/local/bin:/usr/bin:/bin
UMask=0002
ExecStart=$ROOT/ralph-pi.sh
ExecStopPost=/usr/bin/rm -f $ROOT/logs/ralph-pi/loop.pid $ROOT/logs/ralph-pi/phase $ROOT/logs/ralph-pi/heartbeat
Restart=on-failure
RestartSec=60s
RestartPreventExitStatus=2 6 7
KillMode=control-group
TimeoutStopSec=90s

[Install]
WantedBy=multi-user.target
EOF

install -D -o root -g root -m 0755 "$ROOT/tools/ralph_pi_watchdog.sh" "$WATCHDOG_BIN"

cat >/etc/systemd/system/$WATCHDOG_SERVICE <<EOF
[Unit]
Description=Check MDK Ralph Pi Loop progress

[Service]
Type=oneshot
ExecStart=$WATCHDOG_BIN
EOF

cat >/etc/systemd/system/$WATCHDOG_TIMER <<EOF
[Unit]
Description=Periodic MDK Ralph Pi progress check

[Timer]
OnBootSec=5min
OnUnitActiveSec=5min
AccuracySec=30s
Unit=$WATCHDOG_SERVICE

[Install]
WantedBy=timers.target
EOF

systemctl daemon-reload
systemctl enable "$SERVICE" "$WATCHDOG_TIMER"
systemctl reset-failed "$SERVICE" || true
systemctl start "$SERVICE"
systemctl start "$WATCHDOG_TIMER"
systemctl --no-pager --full status "$SERVICE"
systemctl --no-pager --full status "$WATCHDOG_TIMER"

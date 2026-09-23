#!/bin/sh
set -eu

RACE_BIN_DIR=${RACE_BIN_DIR:-"$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"}
RACE_PREFIX=${RACE_PREFIX:-"$(CDPATH= cd -- "$RACE_BIN_DIR/.." && pwd)"}
RACE_CONFIG_FILE=${RACE_CONFIG_FILE:-"$RACE_PREFIX/config/terminal.conf"}
if [ -r "$RACE_CONFIG_FILE" ]; then
    . "$RACE_CONFIG_FILE"
fi
RACE_RUN_DIR=${RACE_RUN_DIR:-/run/race}
RACE_LOG_DIR=${RACE_LOG_DIR:-/mnt/usb/sda/log/app}
RACE_WATCHDOG_ENABLED=${RACE_WATCHDOG_ENABLED:-1}
RACE_WATCHDOG_TIMEOUT_SEC=${RACE_WATCHDOG_TIMEOUT_SEC:-15}
RACE_HEALTH_INTERVAL_SEC=${RACE_HEALTH_INTERVAL_SEC:-1}
RACE_MIN_FREE_BYTES=${RACE_MIN_FREE_BYTES:-134217728}
RACE_APP_LOG_MAX_BYTES=${RACE_APP_LOG_MAX_BYTES:-4194304}
RACE_HEALTH_MAX_BYTES=${RACE_HEALTH_MAX_BYTES:-1048576}
RACE_AP_ENABLED=${RACE_AP_ENABLED:-1}
if [ "$RACE_LOG_DIR" = /mnt/usb/sda/log/app ] &&
   ! awk '$2 == "/mnt/usb/sda" && $4 !~ /(^|,)ro(,|$)/ {found=1} END {exit found ? 0 : 1}' \
       /proc/mounts 2>/dev/null; then
    RACE_LOG_DIR=${RACE_LOG_DIR_FALLBACK:-/tmp/race}
fi
if ! mkdir -p "$RACE_RUN_DIR" "$RACE_LOG_DIR" 2>/dev/null; then
    RACE_LOG_DIR=${RACE_LOG_DIR_FALLBACK:-/tmp/race}
    mkdir -p "$RACE_RUN_DIR" "$RACE_LOG_DIR"
fi

mkdir -p "$RACE_RUN_DIR" "$RACE_LOG_DIR"

if [ -r "$RACE_RUN_DIR/race_supervisor.pid" ]; then
    pid=$(sed -n '1p' "$RACE_RUN_DIR/race_supervisor.pid")
    if kill -0 "$pid" 2>/dev/null &&
       { [ ! -r "/proc/$pid/cmdline" ] ||
         tr '\000' ' ' <"/proc/$pid/cmdline" 2>/dev/null |
             grep -q race_supervisor; }; then
        echo "race stack is already supervised"
        exit 0
    fi
    rm -f "$RACE_RUN_DIR/race_supervisor.pid"
fi

export RACE_BIN_DIR RACE_PREFIX RACE_RUN_DIR RACE_LOG_DIR \
       RACE_CONFIG_FILE RACE_WATCHDOG_ENABLED RACE_WATCHDOG_TIMEOUT_SEC \
       RACE_HEALTH_INTERVAL_SEC RACE_MIN_FREE_BYTES RACE_AP_ENABLED
export RACE_APP_LOG_MAX_BYTES RACE_HEALTH_MAX_BYTES
if [ "$RACE_AP_ENABLED" = 1 ] && [ -x "$RACE_BIN_DIR/t113_ap.sh" ]; then
    "$RACE_BIN_DIR/t113_ap.sh" start ||
        echo "warning: unable to configure T113 wireless AP" >&2
fi
# Keep the service alive when this launcher is called from an SSH/ADB shell;
# init starts it without a terminal, and nohup makes both paths equivalent.
RACE_BIN_DIR="$RACE_BIN_DIR" RACE_RUN_DIR="$RACE_RUN_DIR" \
RACE_LOG_DIR="$RACE_LOG_DIR" \
    nohup "$RACE_BIN_DIR/race_supervisor.sh" "$@" \
    >"$RACE_LOG_DIR/race_supervisor.log" 2>&1 < /dev/null &
# race_supervisor.sh owns race_supervisor.pid and records its own PID.  Do not
# overwrite it with the shell launcher PID; doing so makes stop/status target a
# process that has already exited and leaves the real supervisor orphaned.
echo "race supervisor started; core waits for CAN and logger readiness"

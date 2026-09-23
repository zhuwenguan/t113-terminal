#!/bin/sh
set -eu

RACE_RUN_DIR=${RACE_RUN_DIR:-/run/race}

pid_is_number() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

stop_daemon() {
    name=$1
    pid_file="$RACE_RUN_DIR/$name.pid"
    [ -r "$pid_file" ] || return 0
    pid=$(sed -n '1p' "$pid_file")
    if pid_is_number "$pid" && kill -0 "$pid" 2>/dev/null; then
        if [ ! -r "/proc/$pid/cmdline" ] ||
           tr '\000' ' ' <"/proc/$pid/cmdline" 2>/dev/null |
               grep -q "$name"; then
            kill -TERM "$pid" 2>/dev/null || true
            i=0
            while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 100 ]; do
                sleep 0.1
                i=$((i + 1))
            done
            if kill -0 "$pid" 2>/dev/null; then
                echo "$name did not stop within 10 seconds" >&2
                return 1
            fi
        fi
    fi
    rm -f "$pid_file"
}

stop_daemon race_supervisor
stop_daemon health_log
stop_daemon t113_watchdog
stop_daemon race_video
stop_daemon webd
stop_daemon race_core
echo "race daemons stopped"

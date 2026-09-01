#!/bin/sh
set -eu
RACE_RUN_DIR=${RACE_RUN_DIR:-/run/race}

stop_daemon() {
    name=$1
    pid_file="$RACE_RUN_DIR/$name.pid"
    [ -f "$pid_file" ] || return 0
    pid=$(cat "$pid_file")
    if kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid"
        i=0
        while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
        if kill -0 "$pid" 2>/dev/null; then
            echo "$name did not stop within 10 seconds" >&2
            return 1
        fi
    fi
    rm -f "$pid_file"
}

stop_daemon telemetryd
stop_daemon webd
stop_daemon race_video
stop_daemon race_logger
stop_daemon race_core
echo "race daemons stopped"

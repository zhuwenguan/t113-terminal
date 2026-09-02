#!/bin/sh
set -eu

RACE_BIN_DIR=${RACE_BIN_DIR:-"$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"}
RACE_RUN_DIR=${RACE_RUN_DIR:-/run/race}
RACE_LOG_DIR=${RACE_LOG_DIR:-/tmp/race}

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

RACE_BIN_DIR="$RACE_BIN_DIR" RACE_RUN_DIR="$RACE_RUN_DIR" \
RACE_LOG_DIR="$RACE_LOG_DIR" \
    "$RACE_BIN_DIR/race_supervisor.sh" "$@" \
    >"$RACE_LOG_DIR/race_supervisor.log" 2>&1 &
echo $! >"$RACE_RUN_DIR/race_supervisor.pid"
echo "race supervisor started; core waits for CAN and logger readiness"

#!/bin/sh
set -eu

RACE_BIN_DIR=${RACE_BIN_DIR:-"$(CDPATH= cd -- "$(dirname -- "$0")/../out/arm-release" && pwd)"}
RACE_RUN_DIR=${RACE_RUN_DIR:-/run/race}
RACE_LOG_DIR=${RACE_LOG_DIR:-/tmp/race}

mkdir -p "$RACE_RUN_DIR" "$RACE_LOG_DIR"

start_daemon() {
    name=$1
    shift
    if [ -f "$RACE_RUN_DIR/$name.pid" ] && kill -0 "$(cat "$RACE_RUN_DIR/$name.pid")" 2>/dev/null; then
        echo "$name is already running"
        return
    fi
    "$RACE_BIN_DIR/$name" "$@" >"$RACE_LOG_DIR/$name.log" 2>&1 &
    echo $! >"$RACE_RUN_DIR/$name.pid"
}

start_daemon race_core "$@"
ready=0
i=0
while [ "$i" -lt 50 ]; do
    if [ -e /dev/shm/race_vehicle_state ] &&
       "$RACE_BIN_DIR/race_state_dump" 2>/dev/null | grep -q 'stale=no'; then
        ready=1
        break
    fi
    if ! kill -0 "$(cat "$RACE_RUN_DIR/race_core.pid")" 2>/dev/null; then
        echo "race_core exited; see $RACE_LOG_DIR/race_core.log" >&2
        exit 1
    fi
    sleep 0.1
    i=$((i + 1))
done
if [ "$ready" -ne 1 ]; then echo "shared state did not become ready" >&2; exit 1; fi
start_daemon race_video
echo "race_core and race_video started"

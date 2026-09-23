#!/bin/sh
set -eu

RACE_BIN_DIR=${RACE_BIN_DIR:-"$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"}
RACE_RUN_DIR=${RACE_RUN_DIR:-/run/race}
RACE_LOG_DIR=${RACE_LOG_DIR:-/mnt/usb/sda/log/app}
RACE_WEBD_CONFIG=${RACE_WEBD_CONFIG:-"$RACE_BIN_DIR/../config/webd.conf"}
RACE_WEB_ROOT=${RACE_WEB_ROOT:-"$RACE_BIN_DIR/../web"}

mkdir -p "$RACE_RUN_DIR" "$RACE_LOG_DIR"
echo $$ >"$RACE_RUN_DIR/race_supervisor.pid"
stop_requested=0
restart_count=0
trap 'stop_requested=1' INT TERM

health_log_running() {
    daemon_running health_log
}

watchdog_running() {
    daemon_running t113_watchdog
}

pid_is_number() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

daemon_running() {
    name=$1
    pid_file="$RACE_RUN_DIR/$name.pid"
    [ -r "$pid_file" ] || return 1
    pid=$(sed -n '1p' "$pid_file")
    pid_is_number "$pid" || return 1
    kill -0 "$pid" 2>/dev/null || return 1
    if [ -r "/proc/$pid/cmdline" ]; then
        tr '\000' ' ' <"/proc/$pid/cmdline" 2>/dev/null |
            grep -q "$name" || return 1
    fi
    return 0
}

start_daemon() {
    name=$1
    shift
    if daemon_running "$name"; then
        return 0
    fi
    rm -f "$RACE_RUN_DIR/$name.pid"
    "$RACE_BIN_DIR/$name" "$@" >"$RACE_LOG_DIR/$name.log" 2>&1 &
    echo $! >"$RACE_RUN_DIR/$name.pid"
}

record_restart() {
    restart_count=$((restart_count + 1))
    printf '%s restart=%s service=%s reason=%s\n' \
        "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$restart_count" "$1" "$2" \
        >>"$RACE_LOG_DIR/supervisor.log"
}

limit_log() {
    path=$1
    max_bytes=${RACE_APP_LOG_MAX_BYTES:-4194304}
    if [ -f "$path" ] && [ "$(wc -c <"$path")" -gt "$max_bytes" ]; then
        # Keep one bounded snapshot, then truncate in place because the daemon
        # keeps its descriptor open and would otherwise write to a renamed inode.
        cp "$path" "$path.1" 2>/dev/null || true
        : >"$path"
        printf '%s log_truncated=%s\n' \
            "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$path" \
            >>"$RACE_LOG_DIR/supervisor.log"
    fi
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
                kill -KILL "$pid" 2>/dev/null || true
            fi
        fi
    fi
    rm -f "$pid_file"
}

core_is_running_state() {
    daemon_running race_core || return 1
    "$RACE_BIN_DIR/race_core_status_dump" 2>/dev/null |
        grep -q 'state=RUNNING.*logger=ready.*can_mask=0x3'
}

backoff=1
while [ "$stop_requested" -eq 0 ]; do
    limit_log "$RACE_LOG_DIR/supervisor.log"
    limit_log "$RACE_LOG_DIR/race_core.log"
    limit_log "$RACE_LOG_DIR/race_video.log"
    limit_log "$RACE_LOG_DIR/webd.log"
    limit_log "$RACE_LOG_DIR/health_log.log"
    if ! health_log_running; then
        start_daemon health_log
    fi
    if [ "${RACE_WATCHDOG_ENABLED:-1}" = 1 ] &&
       [ -c /dev/watchdog ] && [ -x "$RACE_BIN_DIR/t113_watchdog" ] &&
       ! watchdog_running; then
        start_daemon t113_watchdog -s /race_core_status \
            -t "${RACE_WATCHDOG_TIMEOUT_SEC:-15}"
        record_restart watchdog unavailable
    fi
    if ! daemon_running race_core; then
        sleep "$backoff"
        [ "$stop_requested" -ne 0 ] && break
        start_daemon race_core "$@"
        record_restart race_core stopped
        if [ "$backoff" -lt 30 ]; then
            backoff=$((backoff * 2))
            [ "$backoff" -le 30 ] || backoff=30
        fi
    elif core_is_running_state; then
        backoff=1
        start_daemon race_video
        if [ -f "$RACE_WEBD_CONFIG" ]; then
            start_daemon webd --config "$RACE_WEBD_CONFIG" \
                --web-root "$RACE_WEB_ROOT"
        else
            start_daemon webd --web-root "$RACE_WEB_ROOT" --port 80
        fi
    else
        stop_daemon race_video
        stop_daemon webd
    fi
    sleep 1
done

stop_daemon race_video
stop_daemon webd
stop_daemon race_core
    stop_daemon health_log
    stop_daemon t113_watchdog
rm -f "$RACE_RUN_DIR/race_supervisor.pid"

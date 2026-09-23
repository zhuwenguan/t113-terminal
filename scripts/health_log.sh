#!/bin/sh
set -eu

RACE_RUN_DIR=${RACE_RUN_DIR:-/run/race}
RACE_LOG_DIR=${RACE_LOG_DIR:-/mnt/usb/sda/log/app}
RACE_BIN_DIR=${RACE_BIN_DIR:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}
RACE_HEALTH_INTERVAL_SEC=${RACE_HEALTH_INTERVAL_SEC:-1}
mkdir -p "$RACE_RUN_DIR" "$RACE_LOG_DIR"
echo $$ >"$RACE_RUN_DIR/health_log.pid"
trap 'rm -f "$RACE_RUN_DIR/health_log.pid"; exit 0' INT TERM EXIT

path="$RACE_LOG_DIR/health.csv"
if [ ! -s "$path" ]; then
    printf '%s\n' 'epoch,uptime_s,mem_available_kb,disk_available_kb,core_state,can_mask,logger,hmi,lappro,core_stale,can0_rx,can1_rx,can0_error,can1_error,can0_busoff,can1_busoff,core_pid,video_pid,webd_pid,restarts' >"$path"
fi
while :; do
    if [ -f "$path" ] && [ "$(wc -c <"$path")" -gt "${RACE_HEALTH_MAX_BYTES:-1048576}" ]; then
        mv "$path" "$path.1"
        printf '%s\n' 'epoch,uptime_s,mem_available_kb,disk_available_kb,core_state,can_mask,logger,hmi,lappro,core_stale,can0_rx,can1_rx,can0_error,can1_error,can0_busoff,can1_busoff,core_pid,video_pid,webd_pid,restarts' >"$path"
    fi
    epoch=$(date +%s)
    uptime=$(awk '{print $1}' /proc/uptime 2>/dev/null || echo 0)
    mem=$(awk '/MemAvailable:/ {print $2; exit}' /proc/meminfo 2>/dev/null || echo 0)
    disk=$(df -Pk "$RACE_LOG_DIR" 2>/dev/null | awk 'NR==2 {print $4}' || echo 0)
    status=$($RACE_BIN_DIR/race_core_status_dump 2>/dev/null || true)
    state=$(printf '%s\n' "$status" | sed -n 's/.*state=\([^ ]*\).*/\1/p')
    can=$(printf '%s\n' "$status" | sed -n 's/.*can_mask=\([^ ]*\).*/\1/p')
    logger=$(printf '%s\n' "$status" | sed -n 's/.*logger=\([^ ]*\).*/\1/p')
    hmi=$(printf '%s\n' "$status" | sed -n 's/.*hmi=\([^ ]*\).*/\1/p')
    lappro=$(printf '%s\n' "$status" | sed -n 's/.*lappro=\([^ ]*\).*/\1/p')
    stale=$(printf '%s\n' "$status" | sed -n 's/.*stale=\([^ ]*\).*/\1/p')
    rx0=$(printf '%s\n' "$status" | sed -n 's/.*can_rx0=\([^ ]*\).*/\1/p')
    rx1=$(printf '%s\n' "$status" | sed -n 's/.*can_rx1=\([^ ]*\).*/\1/p')
    err0=$(printf '%s\n' "$status" | sed -n 's/.*can_err0=\([^ ]*\).*/\1/p')
    err1=$(printf '%s\n' "$status" | sed -n 's/.*can_err1=\([^ ]*\).*/\1/p')
    boff0=$(printf '%s\n' "$status" | sed -n 's/.*can_busoff0=\([^ ]*\).*/\1/p')
    boff1=$(printf '%s\n' "$status" | sed -n 's/.*can_busoff1=\([^ ]*\).*/\1/p')
    pid_value() { [ -r "$RACE_RUN_DIR/$1.pid" ] && sed -n '1p' "$RACE_RUN_DIR/$1.pid" || echo 0; }
    restarts=$(awk -F'restart=' 'END {if (NF>1) print $2; else print 0}' "$RACE_LOG_DIR/supervisor.log" 2>/dev/null || echo 0)
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$epoch" "$uptime" "${mem:-0}" "${disk:-0}" "${state:-unknown}" \
        "${can:-0}" "${logger:-down}" "${hmi:-down}" "${lappro:-down}" \
        "${stale:-yes}" "${rx0:-0}" "${rx1:-0}" "${err0:-0}" "${err1:-0}" \
        "${boff0:-0}" "${boff1:-0}" "$(pid_value race_core)" "$(pid_value race_video)" \
        "$(pid_value webd)" "${restarts:-0}" >>"$path"
    sleep "$RACE_HEALTH_INTERVAL_SEC"
done

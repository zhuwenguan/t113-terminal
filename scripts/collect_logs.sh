#!/bin/sh
set -eu
RACE_PREFIX=${RACE_PREFIX:-/opt/t113}
RACE_LOG_DIR=${RACE_LOG_DIR:-/mnt/usb/sda/log/app}
RACE_RUN_DIR=${RACE_RUN_DIR:-/run/race}
destination=${1:-"$RACE_LOG_DIR/diagnostic_$(date +%Y%m%d_%H%M%S).tar.gz"}
mkdir -p "$(dirname "$destination")"
if [ -x "$RACE_PREFIX/bin/race_core_status_dump" ]; then
    "$RACE_PREFIX/bin/race_core_status_dump" \
        >"$RACE_LOG_DIR/status_snapshot.txt" 2>&1 || true
fi
temporary_archive="/tmp/t113-diagnostic.$$.tar.gz"
trap 'rm -f "$temporary_archive"' EXIT INT TERM
tar -czf "$temporary_archive" \
    -C "$RACE_LOG_DIR" . -C "$RACE_RUN_DIR" . \
    /proc/uptime /proc/meminfo 2>/dev/null || true
mv "$temporary_archive" "$destination"
echo "$destination"

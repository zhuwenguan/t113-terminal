#!/bin/sh
set -eu

PREFIX=${T113_PREFIX:-/opt/t113}
BACKUP="${PREFIX}.previous"
if [ ! -d "$BACKUP" ]; then
    echo "no previous release: $BACKUP" >&2
    exit 1
fi
"$PREFIX/bin/stop_race.sh" || true
FAILED="${PREFIX}.failed.$(date +%Y%m%d_%H%M%S)"
mv "$PREFIX" "$FAILED"
mv "$BACKUP" "$PREFIX"
"$PREFIX/bin/start_race.sh"
"$PREFIX/bin/status.sh" || true

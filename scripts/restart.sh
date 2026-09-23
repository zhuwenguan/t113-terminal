#!/bin/sh
set -eu
RACE_PREFIX=${RACE_PREFIX:-/opt/t113}
"$RACE_PREFIX/bin/stop_race.sh" || true
exec "$RACE_PREFIX/bin/start_race.sh"

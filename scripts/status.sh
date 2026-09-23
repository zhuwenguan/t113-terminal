#!/bin/sh
set -eu
RACE_PREFIX=${RACE_PREFIX:-/opt/t113}
exec "$RACE_PREFIX/etc/init.d/S95t113-terminal" status

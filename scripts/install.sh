#!/bin/sh
set -eu

PREFIX=${T113_PREFIX:-/opt/t113}
RELEASE=${1:-"$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"}
STAMP=$(date +%Y%m%d_%H%M%S)
BACKUP="${PREFIX}.previous"

if [ ! -d "$RELEASE/bin" ]; then
    echo "release directory has no bin/: $RELEASE" >&2
    exit 1
fi
if [ -x "$PREFIX/bin/stop_race.sh" ]; then
    "$PREFIX/bin/stop_race.sh" || true
fi
if [ -e "$BACKUP" ]; then
    mv "$BACKUP" "${PREFIX}.previous.$STAMP"
fi
if [ -e "$PREFIX" ]; then
    mv "$PREFIX" "$BACKUP"
fi
mkdir -p "$PREFIX"
cp -a "$RELEASE/." "$PREFIX/"
chmod +x "$PREFIX/bin/"*.sh "$PREFIX/etc/init.d/"* 2>/dev/null || true
"$PREFIX/bin/start_race.sh"
sleep 2
"$PREFIX/bin/status.sh" || true

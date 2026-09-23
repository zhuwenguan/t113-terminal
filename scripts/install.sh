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
release_real=$(CDPATH= cd -- "$RELEASE" && pwd -P)
prefix_real=$(CDPATH= cd -- "$(dirname -- "$PREFIX")" && pwd -P)/$(basename -- "$PREFIX")
if [ "$release_real" = "$prefix_real" ]; then
    echo "release directory must be outside the install prefix: $RELEASE" >&2
    exit 1
fi
for required_file in bin/start_race.sh bin/stop_race.sh bin/status.sh \
                     etc/init.d/S95t113-terminal; do
    if [ ! -s "$RELEASE/$required_file" ]; then
        echo "release file is missing or empty: $RELEASE/$required_file" >&2
        exit 1
    fi
done
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
for required_file in bin/start_race.sh bin/stop_race.sh bin/status.sh \
                     etc/init.d/S95t113-terminal; do
    if [ ! -s "$PREFIX/$required_file" ]; then
        echo "installed file is missing or empty: $PREFIX/$required_file" >&2
        exit 1
    fi
done
sync
if [ -d /etc/init.d ]; then
    ln -sfn "$PREFIX/etc/init.d/S95t113-terminal" \
        /etc/init.d/S95t113-terminal
    if [ -L /etc/init.d/S75race ] &&
       [ "$(readlink /etc/init.d/S75race 2>/dev/null || true)" = "$PREFIX/etc/init.d/S75race" ]; then
        rm -f /etc/init.d/S75race
    fi
fi
"$PREFIX/bin/start_race.sh"
sleep 2
"$PREFIX/bin/status.sh" || true

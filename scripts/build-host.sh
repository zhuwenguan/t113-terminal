#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
preset=host-release
mode=core-only

while [ "$#" -gt 0 ]; do
    case "$1" in
        host-debug|host-release|host-sanitize)
            preset=$1
            ;;
        --full|full)
            mode=full
            ;;
        --core-only|core-only)
            mode=core-only
            ;;
        *)
            echo "usage: $0 [host-debug|host-release|host-sanitize] [--full|--core-only]" >&2
            exit 2
            ;;
    esac
    shift
done

cd "$project_root"
if [ "$mode" = full ]; then
    cmake --preset "$preset" \
        -DT113_ENABLE_HUD=ON \
        -DT113_BUILD_VIDEO=ON
else
    cmake --preset "$preset" \
        -DT113_ENABLE_HUD=OFF \
        -DT113_BUILD_VIDEO=OFF
fi
cmake --build --preset "$preset" --parallel

#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
preset=${1:-arm-release}

case "$preset" in
    arm-debug|arm-release)
        ;;
    *)
        echo "usage: $0 [arm-debug|arm-release]" >&2
        exit 2
        ;;
esac

cd "$project_root"
cmake --preset "$preset"
cmake --build --preset "$preset" --parallel

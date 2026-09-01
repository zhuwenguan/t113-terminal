#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
preset=${1:-host-release}

case "$preset" in
    host-debug|host-release|host-sanitize)
        ;;
    *)
        echo "usage: $0 [host-debug|host-release|host-sanitize]" >&2
        exit 2
        ;;
esac

cd "$project_root"
cmake --preset "$preset"
cmake --build --preset "$preset" --parallel

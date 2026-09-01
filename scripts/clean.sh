#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out_dir=$project_root/out

if [ -d "$out_dir" ]; then
    rm -rf -- "$out_dir"
    echo "removed $out_dir"
else
    echo "nothing to clean: $out_dir does not exist"
fi

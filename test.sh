#!/bin/sh
set -eu
cd "$(dirname "$0")"

if ! output=$(meson setup --reconfigure build-all "$@" 2>&1); then
    printf '%s\n' "$output" >&2
    exit 1
fi

meson compile -C build-all
meson test -C build-all --no-rebuild --print-errorlogs

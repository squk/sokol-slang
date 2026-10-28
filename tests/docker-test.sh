#!/bin/sh
set -eu
cd /work
export LIBGL_ALWAYS_SOFTWARE=1

build() {
    directory=$1
    shift
    meson setup --reconfigure "$directory" "$@"
    meson compile -C "$directory" -j "${JOBS:-4}"
}

check() {
    build "$@"
    xvfb-run -a meson test -C "$1" --no-rebuild --print-errorlogs
}

linux() {
    check build-docker-linux
    check build-docker-core -Dslang=false
}

browser() {
    build build-docker-browser --cross-file tests/emscripten.ini -Dbrowser=/usr/local/bin/sokol-chromium
    meson test -C build-docker-browser --suite browser --no-rebuild --print-errorlogs
}

windows() {
    build build-docker-windows --cross-file tests/mingw.ini
    echo 'Windows cross-build passed; run the .exe fixtures on Windows for D3D11 validation.'
}

case "${1:-all}" in
    all) linux; browser; windows ;;
    linux) linux ;;
    browser) browser ;;
    windows) windows ;;
    *) echo 'usage: sokol-test [all|linux|browser|windows]' >&2; exit 2 ;;
esac

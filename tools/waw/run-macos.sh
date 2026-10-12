#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
app="$HOME/Applications/Vita3K-WAW.app"
export VITA3K_PORTABLE_DIR="$repo/build/waw-runtime"
codesign --verify --deep --strict "$app"
cd "$VITA3K_PORTABLE_DIR"
exec "$app/Contents/MacOS/Vita3K" --frontend sdl --load-config --keep-config --config-location "$VITA3K_PORTABLE_DIR/config.yml" -r WAW000001 "$@"

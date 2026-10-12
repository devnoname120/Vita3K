#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
source_app=${1:-"$repo/build/waw-debug/bin/Vita3K.app"}
app="$HOME/Applications/Vita3K-WAW.app"
identity=427EBD8232795EF3576E1E900E376B6A9C7E090C
[ -f "$source_app/Contents/MacOS/Vita3K" ] || { echo "Missing built application: $source_app" >&2; exit 1; }
security find-identity -v -p codesigning | grep -q "$identity" || { echo "The fixed signing identity is unavailable" >&2; exit 1; }
mkdir -p "$HOME/Applications"
rsync -a --delete "$source_app/" "$app/"
/usr/libexec/PlistBuddy -c 'Set :CFBundleIdentifier org.vita3k.WAWLocal' "$app/Contents/Info.plist"
codesign --force --deep --sign "$identity" --options runtime --timestamp=none --entitlements "$repo/tools/waw/macos.entitlements" "$app"
codesign --verify --deep --strict --verbose=2 "$app"
codesign -d -r- "$app" 2>&1
printf '%s\n' "$app"

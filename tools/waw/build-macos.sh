#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
build="$repo/build/macos-xcode"
mkdir -p "$repo/build/waw-checks"
cmake -S "$repo" -B "$build" -G Xcode   -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0   -DCMAKE_OSX_ARCHITECTURES=arm64 -DUSE_LTO=NEVER -DUSE_DISCORD_RICH_PRESENCE=OFF -DVITA3K_BUNDLE_QT=OFF
xcodebuild -project "$build/Vita3K.xcodeproj" -target vita3k   -configuration RelWithDebInfo -jobs "${JOBS:-8}" MACOSX_DEPLOYMENT_TARGET=13.0   ARCHS=arm64 ONLY_ACTIVE_ARCH=YES CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO
python3 "$repo/tools/waw/prepare-local-macos.py"
staged=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["staged_app"])' "$repo/build/waw-checks/local-bundle.json")
exec "$repo/tools/waw/deploy-macos.sh" "$staged"

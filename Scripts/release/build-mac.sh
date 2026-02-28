#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

mkdir -p "$ROOT/dist/archive" "$ROOT/dist/export"

echo "Ensuring signing config exists..."
"$ROOT/Scripts/setup-signing.sh"

echo "Building the driver..."
xcodebuild \
  -project "$ROOT/iMic.xcodeproj" \
  -scheme iMicDriver \
  -configuration Release \
  clean build

echo "Archiving iMicHost..."
xcodebuild \
  -project "$ROOT/iMic.xcodeproj" \
  -scheme iMicHost \
  -configuration Release \
  -archivePath "$ROOT/dist/archive/iMicHost.xcarchive" \
  clean archive

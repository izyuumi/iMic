#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
METADATA_FILE="$ROOT/dist/release-metadata.env"

if [ ! -f "$METADATA_FILE" ]; then
  echo "Missing release metadata."
  exit 1
fi

source "$METADATA_FILE"

if [ -z "${APP_PATH:-}" ]; then
  echo "APP_PATH missing from metadata."
  exit 1
fi

echo "Verifying codesign..."
codesign --verify --deep --strict "$APP_PATH"

echo "Checking Gatekeeper trust..."
spctl --assess --type execute --verbose "$APP_PATH"

RESOURCES_DRIVER="$APP_PATH/Contents/Resources/iMicDriver.driver"
if [ ! -d "$RESOURCES_DRIVER" ]; then
  echo "Embedded driver missing at $RESOURCES_DRIVER"
  exit 1
fi

echo "Driver present."

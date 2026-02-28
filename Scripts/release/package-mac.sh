#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCHIVE_PATH="$ROOT/dist/archive/iMicHost.xcarchive"
EXPORT_DIR="$ROOT/dist/export"
METADATA_FILE="$ROOT/dist/release-metadata.env"

if [ -z "${CODE_SIGN_IDENTITY:-}" ]; then
  echo "CODE_SIGN_IDENTITY must be set before packaging."
  exit 1
fi

if [ -z "${DEVELOPMENT_TEAM:-}" ]; then
  echo "DEVELOPMENT_TEAM must be set before packaging."
  exit 1
fi

mkdir -p "$EXPORT_DIR"

BUILD_SETTINGS="$(xcodebuild -project "$ROOT/iMic.xcodeproj" \
  -scheme iMicHost \
  -configuration Release \
  -showBuildSettings)"

MARKETING_VERSION="$(printf '%s' "$BUILD_SETTINGS" | awk '/MARKETING_VERSION/ {print $3; exit}')"

if [ -z "$MARKETING_VERSION" ]; then
  echo "Unable to read MARKETING_VERSION."
  exit 1
fi

EXPORT_OPTIONS="$EXPORT_DIR/ExportOptions.plist"
cat <<EOF > "$EXPORT_OPTIONS"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>method</key>
  <string>developer-id</string>
  <key>signingStyle</key>
  <string>automatic</string>
  <key>signingCertificate</key>
  <string>${CODE_SIGN_IDENTITY}</string>
  <key>teamID</key>
  <string>${DEVELOPMENT_TEAM}</string>
  <key>stripSwiftSymbols</key>
  <true/>
</dict>
</plist>
EOF

echo "Exporting archive..."
xcodebuild -exportArchive \
  -archivePath "$ARCHIVE_PATH" \
  -exportPath "$EXPORT_DIR" \
  -exportOptionsPlist "$EXPORT_OPTIONS"

APP_PATH="$EXPORT_DIR/iMicHost.app"
ZIP_PATH="$ROOT/dist/iMicHost-${MARKETING_VERSION}-macos.zip"

rm -f "$ZIP_PATH"
ditto -c -k --keepParent "$APP_PATH" "$ZIP_PATH"

ZIP_SHA256="$(shasum -a 256 "$ZIP_PATH" | awk '{print $1}')"
ZIP_FILE_REL="${ZIP_PATH#$ROOT/}"

cat <<EOF > "$METADATA_FILE"
VERSION=${MARKETING_VERSION}
ZIP_FILE=${ZIP_PATH}
ZIP_FILE_REL=${ZIP_FILE_REL}
ZIP_SHA256=${ZIP_SHA256}
APP_PATH=${APP_PATH}
EOF

echo "Packaged ${ZIP_PATH}"

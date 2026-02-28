#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
METADATA_FILE="$ROOT/dist/release-metadata.env"

if [ ! -f "$METADATA_FILE" ]; then
  echo "Missing release metadata at $METADATA_FILE"
  exit 1
fi

source "$METADATA_FILE"

if [ -z "${NOTARY_PROFILE:-}" ]; then
  if [ -z "${NOTARY_APPLE_ID:-}" ] || [ -z "${NOTARY_TEAM_ID:-}" ] || [ -z "${NOTARY_APP_PASSWORD:-}" ]; then
    echo "Notary credentials are required (NOTARY_APPLE_ID, NOTARY_TEAM_ID, NOTARY_APP_PASSWORD)"
    exit 1
  fi
  NOTARY_ARGS=(--apple-id "$NOTARY_APPLE_ID" --team-id "$NOTARY_TEAM_ID" --password "$NOTARY_APP_PASSWORD")
else
  NOTARY_ARGS=(--keychain-profile "$NOTARY_PROFILE")
fi

if [ -z "${ZIP_FILE:-}" ] || [ -z "${APP_PATH:-}" ]; then
  echo "Release metadata is missing ZIP_FILE or APP_PATH."
  exit 1
fi

echo "Submitting $ZIP_FILE for notarization..."
xcrun notarytool submit "${NOTARY_ARGS[@]}" "$ZIP_FILE" --wait

echo "Stapling $APP_PATH..."
xcrun stapler staple "$APP_PATH"

echo "Repacking stapled app..."
rm -f "$ZIP_FILE"
ditto -c -k --keepParent "$APP_PATH" "$ZIP_FILE"

ZIP_SHA256="$(shasum -a 256 "$ZIP_FILE" | awk '{print $1}')"
ZIP_FILE_REL="$(realpath --relative-to="$ROOT" "$ZIP_FILE")"

cat <<EOF > "$METADATA_FILE"
VERSION=${VERSION}
ZIP_FILE=${ZIP_FILE}
ZIP_FILE_REL=${ZIP_FILE_REL}
ZIP_SHA256=${ZIP_SHA256}
APP_PATH=${APP_PATH}
EOF

echo "Notarization complete. ZIP SHA256=${ZIP_SHA256}"

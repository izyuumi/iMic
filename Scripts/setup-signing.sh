#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

TARGET="$ROOT/Config/Signing.xcconfig"
EXAMPLE="$ROOT/Config/Signing.example.xcconfig"

if [ -f "$TARGET" ]; then
  echo "Config/Signing.xcconfig already exists."
  exit 0
fi

if [ ! -f "$EXAMPLE" ]; then
  echo "Missing $EXAMPLE."
  exit 1
fi

cp "$EXAMPLE" "$TARGET"
echo "Created Config/Signing.xcconfig from example. Update it with your credentials before signing."

#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
METADATA_FILE="$ROOT/dist/release-metadata.env"

if [ ! -f "$METADATA_FILE" ]; then
  echo "Release metadata not found at $METADATA_FILE"
  exit 1
fi

source "$METADATA_FILE"

VERSION="${VERSION:?}"
ZIP_SHA256="${ZIP_SHA256:?}"

HOMEBREW_TAP_TOKEN="${HOMEBREW_TAP_TOKEN:-${GH_TOKEN:-}}"
if [ -z "$HOMEBREW_TAP_TOKEN" ]; then
  echo "HOMEBREW_TAP_TOKEN or GH_TOKEN is required to push to the tap."
  exit 1
fi

TAP_URL="${HOMEBREW_TAP_URL:-https://github.com/izyuumi/homebrew-tap}"
TAP_PATH="${TAP_URL#https://github.com/}"
CLONE_URL="https://x-access-token:${HOMEBREW_TAP_TOKEN}@github.com/${TAP_PATH}"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

git clone "$CLONE_URL" "$WORKDIR"
cd "$WORKDIR"

BRANCH="release/imic-host-${VERSION}"
git checkout -b "$BRANCH"
git remote set-url origin "$CLONE_URL"

CASK_FILE="Casks/imic-host.rb"
cat <<EOF > "$CASK_FILE"
cask "imic-host" do
  version "${VERSION}"
  sha256 "${ZIP_SHA256}"

  url "https://github.com/yumiizumi/iMic/releases/download/v${VERSION}/iMicHost-${VERSION}-macos.zip"
  name "iMic Host"
  desc "Stream your iPhone microphone into macOS as a driver-backed virtual input."
  homepage "https://github.com/yumiizumi/iMic"

  depends_on macos: ">= :sonoma"

  app "iMicHost.app"

  caveats <<~EOS
    iMicHost installs iMicDriver to /Library/Audio/Plug-Ins/HAL/ when you grant it administrator privileges on first launch.
    Homebrew only drops the app bundle; the driver installation happens at runtime.
  EOS
end
EOF

python3 - <<PY
from pathlib import Path
path = Path("README.md")
text = path.read_text()
row = "| `imic-host` | Stream your iPhone microphone into macOS as a virtual input. |"
if "`imic-host`" not in text:
    marker = "| `koe` |"
    if marker not in text:
        raise SystemExit("Unexpected README format, cannot insert cask row.")
    text = text.replace(marker, marker + "\n" + row)
snippet = """```sh
brew install --cask izyuumi/tap/koe
```
"""
extra = """```sh
brew install --cask izyuumi/tap/imic-host
```
"""
if extra.strip() not in text:
    if snippet not in text:
        raise SystemExit("Expected existing snippet missing.")
    text = text.replace(snippet, snippet + "\n" + extra)
path.write_text(text)
PY

git add "$CASK_FILE" README.md
git commit -m "Release imic-host ${VERSION}"
git push "$CLONE_URL" "$BRANCH"

if command -v gh >/dev/null 2>&1; then
  gh pr create \
    --title "Release imic-host ${VERSION}" \
    --body "Automated release for imic-host ${VERSION}." \
    --base main \
    --head "$BRANCH" \
    --label "homebrew"
else
  echo "gh CLI not installed; skipping PR creation."
fi

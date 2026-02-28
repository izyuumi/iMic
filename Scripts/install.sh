#!/bin/bash
set -e

DRIVER_NAME="iMicDriver.driver"
DRIVER_SRC="$(dirname "$0")/../build/Release/${DRIVER_NAME}"
DRIVER_DST="/Library/Audio/Plug-Ins/HAL/${DRIVER_NAME}"

if [ ! -d "$DRIVER_SRC" ]; then
    echo "Error: Driver bundle not found at ${DRIVER_SRC}"
    echo "Build the iMicDriver target first in Xcode."
    exit 1
fi

echo "Installing ${DRIVER_NAME}..."
sudo cp -R "$DRIVER_SRC" "$DRIVER_DST"
echo "Restarting coreaudiod..."
sudo kill -9 $(pgrep coreaudiod)
echo "Done. 'iMic' should now appear in System Settings > Sound > Input."

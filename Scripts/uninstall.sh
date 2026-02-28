#!/bin/bash
set -e

DRIVER_NAME="iMicDriver.driver"
DRIVER_DST="/Library/Audio/Plug-Ins/HAL/${DRIVER_NAME}"

if [ ! -d "$DRIVER_DST" ]; then
    echo "Driver not installed at ${DRIVER_DST}"
    exit 0
fi

echo "Removing ${DRIVER_NAME}..."
sudo rm -rf "$DRIVER_DST"
echo "Restarting coreaudiod..."
sudo kill -9 $(pgrep coreaudiod)
echo "Done. 'iMic' has been removed."

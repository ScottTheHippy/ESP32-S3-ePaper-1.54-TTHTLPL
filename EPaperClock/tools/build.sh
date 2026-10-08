#!/bin/sh
# Compile and (optionally) flash the clock.
# usage: tools/build.sh [serial-port]      e.g. tools/build.sh /dev/cu.usbmodemXXXX
set -e
cd "$(dirname "$0")/.."
CLI="${ARDUINO_CLI:-arduino-cli}"
# CDCOnBoot=cdc: without it Serial prints nothing over this board's native USB port.
# huge_app: the default partition is too small once Wi-Fi, the web server and TLS are linked in.
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=huge_app"
if [ -n "$1" ]; then
  "$CLI" compile --upload -p "$1" --fqbn "$FQBN" .
else
  "$CLI" compile --fqbn "$FQBN" .
fi

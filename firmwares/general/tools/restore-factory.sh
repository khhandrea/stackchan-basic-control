#!/usr/bin/env bash
# Write the full factory flash backup back to the StackChan through the Raspberry Pi.
# See documents/factory-firmware.md. Takes a few minutes (16 MB).
set -euo pipefail

PI=${STACKCHAN_PI:-rpi3}
PORT=${STACKCHAN_PORT:-/dev/ttyACM0}
IMAGE=backup/stackchan-factory-20261025.bin
SHA256=e0bd4f7af5074634b84c25dd54d390874a51777547a997f589bfe555978f7337

ssh "$PI" "set -e; cd ~
echo '$SHA256  $IMAGE' | sha256sum -c -
~/.local/bin/esptool.py --chip esp32s3 -p '$PORT' -b 460800 \
    --before default_reset --after hard_reset write_flash 0x0 '$IMAGE'"

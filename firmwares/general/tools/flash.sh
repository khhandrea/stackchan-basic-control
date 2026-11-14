#!/usr/bin/env bash
# Flash the current build to the StackChan through the Raspberry Pi.
# Copies the images listed in build/flash_args to the Pi, then runs esptool there.
#
# Usage: tools/flash.sh           (run idf.py build first)
# Env:   STACKCHAN_PI   ssh host       (default: rpi3)
#        STACKCHAN_PORT serial device  (default: /dev/ttyACM0)
set -euo pipefail

PI=${STACKCHAN_PI:-rpi3}
PORT=${STACKCHAN_PORT:-/dev/ttyACM0}
BUILD="$(cd "$(dirname "$0")/.." && pwd)/build"
REMOTE=stackchan/firmware

[ -f "$BUILD/flash_args" ] || { echo "no $BUILD/flash_args: run idf.py build" >&2; exit 1; }

# flash_args: first line is options, then "<offset> <path>" per image
files=$( { echo flash_args; awk 'NR > 1 { print $2 }' "$BUILD/flash_args"; } )
# tar over ssh (no rsync needed)
tar -C "$BUILD" -cf - $files | ssh "$PI" "rm -rf $REMOTE && mkdir -p $REMOTE && tar -xf - -C $REMOTE"

ssh "$PI" PORT="$PORT" REMOTE="$REMOTE" 'bash -s' <<'REMOTE_SCRIPT'
set -euo pipefail
dev=$(readlink -f "$PORT")
for fd in /proc/[0-9]*/fd/*; do
    if [ "$(readlink "$fd" 2>/dev/null)" = "$dev" ]; then
        pid=${fd#/proc/}; pid=${pid%%/*}
        echo "$PORT is open by pid $pid ($(tr '\0' ' ' < /proc/$pid/cmdline)); stop it first" >&2
        exit 1
    fi
done
cd "$REMOTE"
~/.local/bin/esptool.py --chip esp32s3 -p "$PORT" -b 460800 \
    --before default_reset --after hard_reset write_flash @flash_args
REMOTE_SCRIPT

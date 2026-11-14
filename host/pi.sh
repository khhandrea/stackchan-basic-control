#!/usr/bin/env bash
# Sync this project to the Raspberry Pi and run the stackchan CLI there.
# Usage: ./pi.sh ping | ./pi.sh call sys.info | ./pi.sh listen -s 10 | ./pi.sh serve
set -euo pipefail
PI=${STACKCHAN_PI:-rpi3}
HERE="$(cd "$(dirname "$0")" && pwd)"
# tar over ssh (no rsync needed); keeps the remote .venv between runs
tar -C "$HERE" --exclude .venv --exclude __pycache__ -cf - . |
    ssh "$PI" 'mkdir -p stackchan/host && cd stackchan/host && find . -mindepth 1 -maxdepth 1 ! -name .venv -exec rm -rf {} + && tar -xf -'
# -t from a terminal, so Ctrl-C also stops long-running commands (listen, serve) on the Pi
TTY=()
[ -t 0 ] && TTY=(-t)
ssh "${TTY[@]}" "$PI" "cd stackchan/host && ~/.local/bin/uv run --quiet stackchan $(printf '%q ' "$@")"

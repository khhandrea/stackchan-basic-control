#!/usr/bin/env bash
# Development loop: build on this PC, then flash through the Raspberry Pi.
# The full build log is kept in build/build.log.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
if ! bash -ic 'get_idf >/dev/null 2>&1 && idf.py build' > build/build.log 2>&1; then
    grep -E "error:|FAILED:" build/build.log | cut -c1-300 | head -20 >&2
    echo "build failed (build/build.log)" >&2
    exit 1
fi
grep -E "warning:" build/build.log | grep -v "/esp-idf/\|managed_components" || true
tools/flash.sh 2>&1 | grep -E "Hash of data verified|rror|Hard resetting|stop it first"

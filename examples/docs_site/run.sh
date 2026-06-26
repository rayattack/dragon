#!/usr/bin/env bash
# Build and run the example docs site. Run it from this directory:
#   ./run.sh
# then open http://127.0.0.1:2018/.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DRAGON="${DRAGON:-$HERE/../../build/dragon}"
BIN="${BIN:-/tmp/docs_site}"

cd "$HERE"
"$DRAGON" build main.dr -o "$BIN"
exec "$BIN"

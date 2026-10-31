#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT_DIR/build-release"
WORK=/tmp/eg-quick
rm -rf "$WORK"; mkdir -p "$WORK/root" "$WORK/uploads"
echo hi > "$WORK/root/hi.txt"
"$BIN/eg_demo" --listen 127.0.0.1:9511 --name quick --root "$WORK/root" --uploads "$WORK/uploads" --quiet &
P=$!
trap 'kill $P 2>/dev/null || true' EXIT
sleep 0.3
"$BIN/eg_loadgen" --url http://127.0.0.1:9511 --path /api/info --connections 1 --requests 100
"$BIN/eg_loadgen" --url http://127.0.0.1:9511 --path /api/info --connections 8 --requests 1000

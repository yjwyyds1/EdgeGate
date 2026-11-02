#!/usr/bin/env bash
set -euo pipefail
export no_proxy='*'
export NO_PROXY='*'
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT_DIR/build-release"
WORK=/tmp/eg-bench
rm -rf "$WORK"
mkdir -p "$WORK/root" "$WORK/uploads"
head -c 67108864 /dev/zero > "$WORK/root/bench.bin"
echo "edgegate benchmark payload" > "$WORK/root/small.txt"

"$BIN/eg_demo" --listen 127.0.0.1:9501 --name bench-a --root "$WORK/root" --uploads "$WORK/uploads" --quiet &
P1=$!
"$BIN/eg_demo" --listen 127.0.0.1:9502 --name bench-b --root "$WORK/root" --uploads "$WORK/uploads" --quiet &
P2=$!
"$BIN/eg_demo" --listen 127.0.0.1:9503 --name bench-c --root "$WORK/root" --uploads "$WORK/uploads" --quiet &
P3=$!
cat > "$WORK/edgegate.conf" <<EOF
listen = 127.0.0.1:9600
admin_listen = 127.0.0.1:9601
balancer = least_conn
max_attempts = 2
health_interval_ms = 5000
health_timeout_ms = 1000
healthy_threshold = 1
unhealthy_threshold = 3
max_connections_per_backend = 256
max_idle_connections_per_backend = 64
access_log = off
log_level = warn
[backend bench-a]
url = http://127.0.0.1:9501
[backend bench-b]
url = http://127.0.0.1:9502
[backend bench-c]
url = http://127.0.0.1:9503
EOF
"$BIN/edgegate" --config "$WORK/edgegate.conf" > "$WORK/proxy.log" 2>&1 &
P4=$!
cleanup() { kill "$P1" "$P2" "$P3" "$P4" 2>/dev/null || true; }
trap cleanup EXIT

sleep 1
echo "== environment =="
uname -srm
g++ --version | head -1
cmake --version | head -1
nproc
echo

echo "== small JSON (GET /api/info), 3 rounds, 100000 requests total =="
for conns in 1 8 32; do
  reqs=$((100000 / conns))
  for round in 1 2 3; do
    printf "direct conns=%-2s round=%s " "$conns" "$round"
    "$BIN/eg_loadgen" --url http://127.0.0.1:9501 --path /api/info \
      --connections "$conns" --requests "$reqs" --warmup 1000
  done
  for round in 1 2 3; do
    printf "proxy  conns=%-2s round=%s " "$conns" "$round"
    "$BIN/eg_loadgen" --url http://127.0.0.1:9600 --path /api/info \
      --connections "$conns" --requests "$reqs" --warmup 1000
  done
done

echo
echo "== 64 MiB download, 3 requests, 1 connection =="
for round in 1 2 3; do
  printf "direct round=%s " "$round"
  "$BIN/eg_loadgen" --url http://127.0.0.1:9501 --path /files/bench.bin \
    --connections 1 --requests 3 --warmup 0
done
for round in 1 2 3; do
  printf "proxy  round=%s " "$round"
  "$BIN/eg_loadgen" --url http://127.0.0.1:9600 --path /files/bench.bin \
    --connections 1 --requests 3 --warmup 0
done

echo
echo "== admin metrics snapshot =="
curl -sS http://127.0.0.1:9601/metrics | grep -E \
  'edgegate_(requests_total|responses_total|errors_total|backend_requests_total|backend_latency_ms|buffered_bytes|upstream_read_paused_total)' | head -40

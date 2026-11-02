#!/usr/bin/env bash
set -euo pipefail
# WSL 的 .bashrc 经常导出 http_proxy/https_proxy；本地冒烟测试必须绕过代理，
# 否则 curl 会把 127.0.0.1 也发给 Windows 代理，出现 "via 10.255.255.254"。
export no_proxy='*'
export NO_PROXY='*'
BIN="$(cd "$(dirname "$0")/.." && pwd)/build"
WORK=/tmp/eg-smoke
rm -rf "$WORK"
mkdir -p "$WORK/root" "$WORK/uploads"
echo "hello edgegate" > "$WORK/root/hello.txt"
head -c 262144 /dev/urandom > "$WORK/root/blob.bin"

"$BIN/eg_demo" --listen 127.0.0.1:9101 --name app-a --root "$WORK/root" --uploads "$WORK/uploads" --quiet &
P1=$!
"$BIN/eg_demo" --listen 127.0.0.1:9102 --name app-b --root "$WORK/root" --uploads "$WORK/uploads" --quiet &
P2=$!
cat > "$WORK/edgegate.conf" <<EOF
listen = 127.0.0.1:9200
admin_listen = 127.0.0.1:9201
balancer = round_robin
max_attempts = 2
upstream_response_timeout_ms = 5000
health_interval_ms = 1000
health_timeout_ms = 500
healthy_threshold = 1
unhealthy_threshold = 1
log_level = info
[backend app-a]
url = http://127.0.0.1:9101
[backend app-b]
url = http://127.0.0.1:9102
EOF
"$BIN/edgegate" --config "$WORK/edgegate.conf" --log-level debug > "$WORK/proxy.log" 2>&1 &
P3=$!
cleanup() { kill "$P1" "$P2" "$P3" 2>/dev/null || true; }
trap cleanup EXIT

sleep 0.8
echo "=== info via proxy ==="
curl -sS -i http://127.0.0.1:9200/api/info | head -20
echo "=== info again ==="
curl -sS -i http://127.0.0.1:9200/api/info | head -12
echo "=== download ==="
curl -sS http://127.0.0.1:9200/files/hello.txt
curl -sS http://127.0.0.1:9200/files/blob.bin -o "$WORK/blob.out"
sha256sum "$WORK/root/blob.bin" "$WORK/blob.out"
echo "=== upload ==="
head -c 1048576 /dev/urandom > "$WORK/upload.bin"
curl -sS -X PUT --data-binary @"$WORK/upload.bin" http://127.0.0.1:9200/uploads/up.bin
echo
sha256sum "$WORK/upload.bin"
curl -sS http://127.0.0.1:9200/uploads/up.bin -o "$WORK/up.out"
sha256sum "$WORK/up.out"
echo "=== chunked request ==="
printf 'hello-chunked' > "$WORK/echo.txt"
curl -sS -X POST -H 'Transfer-Encoding: chunked' --data-binary @"$WORK/echo.txt" http://127.0.0.1:9200/api/echo
echo
echo "=== admin ==="
curl -sS http://127.0.0.1:9201/api/status | head -c 400; echo
curl -sS http://127.0.0.1:9201/metrics | grep -E 'edgegate_(requests_total|backend_requests_total|buffered_bytes)' | head
echo "=== proxy log ==="
tail -20 "$WORK/proxy.log"

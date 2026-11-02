#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# 默认优先使用 build-h2，其次使用手册里的 build/
if [ -z "${BIN:-}" ]; then
  if [ -x "$ROOT/build-h2/edgegate" ]; then BIN="$ROOT/build-h2"; else BIN="$ROOT/build"; fi
fi
WORK=/tmp/eg-tls-smoke
rm -rf "$WORK"
mkdir -p "$WORK/root" "$WORK/uploads" "$WORK/certs"
echo "tls hello" > "$WORK/root/hello.txt"

openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -keyout "$WORK/certs/key.pem" -out "$WORK/certs/cert.pem" \
  -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1

cat > "$WORK/edgegate.conf" <<EOF
listen = 127.0.0.1:9210
admin_listen = 127.0.0.1:9212
tls_listen = 127.0.0.1:9211
tls_cert = $WORK/certs/cert.pem
tls_key = $WORK/certs/key.pem
tls_min_version = 1.2
http2 = true
health_interval_ms = 1000
health_timeout_ms = 500
healthy_threshold = 1
unhealthy_threshold = 1
[backend app-a]
url = http://127.0.0.1:9111
EOF

"$BIN/eg_demo" --listen 127.0.0.1:9111 --name app-a --root "$WORK/root" \
  --uploads "$WORK/uploads" --quiet &
P1=$!
"$BIN/edgegate" --config "$WORK/edgegate.conf" --log-level debug > "$WORK/proxy.log" 2>&1 &
P2=$!
cleanup() { kill "$P1" "$P2" 2>/dev/null || true; }
trap cleanup EXIT
sleep 0.8

echo "=== TLS HTTP/1.1 ==="
curl -sk --http1.1 -D - https://127.0.0.1:9211/api/info | head -20
echo
echo "=== TLS download ==="
curl -sk --http1.1 https://127.0.0.1:9211/files/hello.txt
echo "=== TLS HTTP/2 GET ==="
curl -sk --http2 -D - https://127.0.0.1:9211/api/info | head -20
echo
echo "=== TLS HTTP/2 version ==="
curl -sk --http2 -o /dev/null -w "http_version=%{http_version}\n" https://127.0.0.1:9211/api/info
echo "=== TLS HTTP/2 download ==="
curl -sk --http2 https://127.0.0.1:9211/files/hello.txt
echo "=== TLS HTTP/2 POST echo ==="
printf 'h2 body' | curl -sk --http2 -X POST --data-binary @- https://127.0.0.1:9211/api/echo
echo
echo "=== TLS HTTP/2 bulk ==="
curl -sk --http2 https://127.0.0.1:9211/api/bulk?mb=2 -o /tmp/eg-h2-bulk.bin
wc -c /tmp/eg-h2-bulk.bin
echo "=== ALPN / cipher ==="
openssl s_client -connect 127.0.0.1:9211 -alpn http/1.1 -brief </dev/null 2>&1 | head -10
echo "=== proxy log ==="
tail -10 "$WORK/proxy.log"

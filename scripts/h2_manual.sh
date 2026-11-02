#!/usr/bin/env bash
set -euo pipefail
export no_proxy='*'
export NO_PROXY='*'
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# 默认优先使用 build-h2，其次使用手册里的 build/
if [ -z "${BIN:-}" ]; then
  if [ -x "$ROOT/build-h2/edgegate" ]; then BIN="$ROOT/build-h2"; else BIN="$ROOT/build"; fi
fi
WORK=/tmp/eg-h2-manual
rm -rf "$WORK"
mkdir -p "$WORK/root" "$WORK/uploads" "$WORK/certs"
echo "h2 hello" > "$WORK/root/hello.txt"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -keyout "$WORK/certs/key.pem" -out "$WORK/certs/cert.pem" \
  -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1
cat > "$WORK/edgegate.conf" <<EOF
listen = 127.0.0.1:9213
admin_listen = 127.0.0.1:9214
tls_listen = 127.0.0.1:9215
tls_cert = $WORK/certs/cert.pem
tls_key = $WORK/certs/key.pem
http2 = true
health_interval_ms = 1000
health_timeout_ms = 500
healthy_threshold = 1
unhealthy_threshold = 100
upstream_response_timeout_ms = 5000
[backend app-a]
url = http://127.0.0.1:9112
EOF
"$BIN/eg_demo" --listen 127.0.0.1:9112 --name app-a --root "$WORK/root" --uploads "$WORK/uploads" --quiet &
P1=$!
"$BIN/edgegate" --config "$WORK/edgegate.conf" --log-level debug > "$WORK/proxy.log" 2>&1 &
P2=$!
cleanup() { kill "$P1" "$P2" 2>/dev/null || true; }
trap cleanup EXIT
sleep 0.8

echo "=== h2 parallel (should multiplex) ==="
/usr/bin/time -f "wall=%e s" curl -sk --http2 --parallel --parallel-immediate \
  -o /tmp/h2a -o /tmp/h2b \
  -w "%{url_effective} http=%{http_version} time=%{time_total}\n" \
  "https://127.0.0.1:9215/api/delay?ms=500" \
  "https://127.0.0.1:9215/api/info"

echo "=== h2 special responses ==="
printf "chunked: "; curl -sk --http2 "https://127.0.0.1:9215/api/chunked?chunks=3&size=1000" | wc -c
printf "close-delimited: "; curl -sk --http2 "https://127.0.0.1:9215/api/close" | head -c 40; echo
printf "HEAD bulk CL: "; curl -sk --http2 -I "https://127.0.0.1:9215/api/bulk?mb=1" | tr -d '\r' | awk 'tolower($1)=="content-length:"{print $2}'
printf "204: "; curl -sk --http2 -o /dev/null -w "%{http_code}\n" "https://127.0.0.1:9215/api/no-content"
printf "500: "; curl -sk --http2 -o /dev/null -w "%{http_code}\n" "https://127.0.0.1:9215/api/fail?code=500"

echo "=== h2 POST 2 MiB echo ==="
head -c 2097152 /dev/urandom > "$WORK/up.bin"
EXPECT=$(sha256sum "$WORK/up.bin" | awk '{print $1}')
RESP=$(curl -sk --http2 -X POST --data-binary @"$WORK/up.bin" "https://127.0.0.1:9215/api/echo")
echo "response: ${RESP:0:300}"
GOT=$(printf '%s' "$RESP" | python3 -c 'import json,sys;print(json.load(sys.stdin)["sha256"])')
echo "expected=$EXPECT"; echo "got=$GOT"; test "$EXPECT" = "$GOT"

echo "=== h2 PUT upload ==="
GOT2=$(curl -sk --http2 -X PUT --data-binary @"$WORK/up.bin" "https://127.0.0.1:9215/uploads/h2.bin" |
  python3 -c 'import json,sys;print(json.load(sys.stdin)["sha256"])')
echo "upload=$GOT2"; test "$EXPECT" = "$GOT2"

echo "=== h2 slow download (4 MiB, limit 200k) ==="
/usr/bin/time -f "wall=%e s" curl -sk --http2 --limit-rate 200k \
  "https://127.0.0.1:9215/api/bulk?mb=4" -o /tmp/h2slow.bin
wc -c /tmp/h2slow.bin
curl -s http://127.0.0.1:9214/metrics | grep -E "edgegate_(buffered_bytes|upstream_read_paused_total|requests_total|backend_requests_total)" | head

echo "=== log tail ==="
tail -12 "$WORK/proxy.log"

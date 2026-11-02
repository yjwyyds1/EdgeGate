#!/usr/bin/env bash
set -euo pipefail
OUT="${1:-certs}"
mkdir -p "$OUT"
if [ -f "$OUT/cert.pem" ] && [ -f "$OUT/key.pem" ]; then
  echo "certificate already exists: $OUT/cert.pem"
  exit 0
fi
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
  -keyout "$OUT/key.pem" -out "$OUT/cert.pem" \
  -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1
chmod 600 "$OUT/key.pem"
echo "generated $OUT/cert.pem and $OUT/key.pem (self-signed, for local testing only)"

#!/usr/bin/env bash
set -euo pipefail

# docker-entrypoint.sh
# Generates a self-signed certificate if missing and runs the static HTTPS server.

DOCROOT=${DOCROOT:-/var/www}
PORT=${PORT:-8443}
CERT_PATH=${CERT_PATH:-/etc/ssl/certs/test_cert.pem}
KEY_PATH=${KEY_PATH:-/etc/ssl/private/test_key.pem}

mkdir -p "$(dirname "$CERT_PATH")" "$(dirname "$KEY_PATH")" "${DOCROOT}"

if [ ! -f "$CERT_PATH" ] || [ ! -f "$KEY_PATH" ]; then
  echo "Generating self-signed certificate at ${CERT_PATH} and ${KEY_PATH}"
  openssl req -x509 -nodes -days 365 -newkey rsa:2048 \
    -subj "/CN=localhost" \
    -keyout "$KEY_PATH" -out "$CERT_PATH"
fi

echo "Starting static HTTPS server: docroot=${DOCROOT} port=${PORT} cert=${CERT_PATH} key=${KEY_PATH}"
# Ensure the server can find certs by also creating expected filenames in the process CWD
cp -f "$CERT_PATH" /test_cert.pem || true
cp -f "$KEY_PATH" /test_key.pem || true

exec /usr/local/bin/static_https_server "${DOCROOT}" "${PORT}"

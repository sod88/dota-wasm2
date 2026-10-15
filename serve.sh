#!/usr/bin/env bash
# Serve web/ over http://localhost:8080 (browsers will not fetch .wasm from file:// URLs).
#   ./serve.sh [port]
# Uses the first Python 3 that actually runs on this machine (on Apple Silicon Macs an old Intel-only
# /usr/local/bin/python3 can fail with "Bad CPU type"), and falls back to Ruby, which every Mac has.
set -euo pipefail
cd "$(dirname "$0")/web"
PORT="${1:-8080}"
echo "Lane Wars: http://localhost:$PORT   (parity check: http://localhost:$PORT/parity.html)   Ctrl+C to stop"
for py in python3 /usr/bin/python3 /opt/homebrew/bin/python3 python; do
  if command -v "$py" >/dev/null 2>&1 && "$py" -c 'import http.server' >/dev/null 2>&1; then
    exec "$py" -m http.server "$PORT"
  fi
done
if command -v ruby >/dev/null 2>&1; then
  exec ruby -run -ehttpd . -p"$PORT"
fi
echo "Could not find a working python3 or ruby to serve the page." >&2
exit 1

#!/usr/bin/env bash
# Compile the TypeScript parts of the front end (web/ts/*.ts) to JavaScript next to the rest of it
# (web/js/*.js). Needs Node.js (macOS: brew install node); the TypeScript compiler itself is installed
# into node_modules/ on first use. ./serve.sh runs this whenever a .ts file is newer than its output.
#   ./build-web.sh           compile
#   ./build-web.sh --check   type-check only, write nothing
set -euo pipefail
cd "$(dirname "$0")"

if ! command -v npm >/dev/null 2>&1; then
  echo "The web front end needs Node.js to compile its TypeScript (macOS: brew install node)." >&2
  exit 1
fi
[ -x node_modules/.bin/tsc ] || npm ci --no-fund --no-audit

if [ "${1:-}" = "--check" ]; then
  node_modules/.bin/tsc -p web --noEmit
  echo "OK: web/ts type-checks"
else
  node_modules/.bin/tsc -p web
  echo "OK: web/ts -> web/js"
fi

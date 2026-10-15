#!/usr/bin/env bash
# Build the engine to WebAssembly and drop it next to the web front end (web/moba_engine.wasm).
# The crate has no dependencies and talks to JS through a plain C ABI, so wasm-bindgen / wasm-pack
# are NOT needed — only the Rust wasm target.
set -euo pipefail
cd "$(dirname "$0")"

TARGET=wasm32-unknown-unknown
if ! rustup target list --installed 2>/dev/null | grep -qx "$TARGET"; then
  echo "Installing the Rust target $TARGET ..."
  rustup target add "$TARGET"
fi

cargo build --release --target "$TARGET"
OUT="target/$TARGET/release/moba_engine.wasm"
cp "$OUT" web/moba_engine.wasm

# Optional: shrink further if binaryen is installed (brew install binaryen / apt install binaryen).
if command -v wasm-opt >/dev/null 2>&1; then
  wasm-opt -O3 -o web/moba_engine.wasm web/moba_engine.wasm
fi

echo "OK: web/moba_engine.wasm ($(wc -c < web/moba_engine.wasm | tr -d ' ') bytes)"
echo "Run ./serve.sh and open http://localhost:8080"

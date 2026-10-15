#!/usr/bin/env bash
# Build the C++ 3D renderer (cpp/render/) to WebAssembly: web/render3d.wasm.
# It draws with WebGPU and works with either engine. Without it the game still runs, in the 2D view.
#
# The renderer is freestanding C++ (no standard library), so any C++ -> wasm32 compiler will do:
# Emscripten, wasi-sdk (WASI_SDK_PATH), zig, or a clang with the WebAssembly target and wasm-ld
# (macOS: brew install llvm; Apple's own clang has no WebAssembly target).
set -euo pipefail
cd "$(dirname "$0")"

OUT=web/render3d.wasm
SRC=cpp/render/render3d.cpp
FLAGS=(-std=c++17 -O2 -nostdlib -fno-exceptions -fno-rtti -Wall -Wextra)
LINK=(-Wl,--no-entry -Wl,--strip-all)

# A clang that can target wasm32 and has wasm-ld next to it (or on PATH)
plain_clang() {
  for cc in "${CLANGXX:-}" /opt/homebrew/opt/llvm/bin/clang++ /usr/local/opt/llvm/bin/clang++ clang++; do
    [ -n "$cc" ] && command -v "$cc" >/dev/null 2>&1 || continue
    if echo 'int x;' | "$cc" --target=wasm32 -nostdlib -Wl,--no-entry -x c++ - -o /dev/null >/dev/null 2>&1; then
      echo "$cc"; return 0
    fi
  done
  return 1
}

if CC=$(plain_clang); then
  echo "Building the renderer with $CC"
  "$CC" --target=wasm32 "${FLAGS[@]}" "${LINK[@]}" -o "$OUT" "$SRC"
elif command -v em++ >/dev/null 2>&1; then
  echo "Building the renderer with Emscripten ($(em++ --version | head -1))"
  em++ "${FLAGS[@]/-nostdlib}" "$SRC" --no-entry -sSTANDALONE_WASM=1 -sERROR_ON_UNDEFINED_SYMBOLS=0 -sINITIAL_MEMORY=32MB \
    -sEXPORTED_FUNCTIONS=_r3d_terrain_buf,_r3d_frame_buf,_r3d_cam_ptr,_r3d_max_units,_r3d_height,_r3d_init,_r3d_camera,_r3d_draw \
    -o "$OUT"
elif [ -n "${WASI_SDK_PATH:-}" ] && [ -x "$WASI_SDK_PATH/bin/clang++" ]; then
  echo "Building the renderer with wasi-sdk ($WASI_SDK_PATH)"
  "$WASI_SDK_PATH/bin/clang++" --target=wasm32 "${FLAGS[@]}" "${LINK[@]}" -o "$OUT" "$SRC"
elif command -v zig >/dev/null 2>&1; then
  echo "Building the renderer with zig ($(zig version))"
  zig c++ -target wasm32-freestanding "${FLAGS[@]}" "${LINK[@]}" -o "$OUT" "$SRC"
else
  echo "No C++ -> WebAssembly compiler found for the 3D renderer (the game will run in 2D)."
  echo "Install one of: Emscripten (brew install emscripten), LLVM (brew install llvm), or zig; or set WASI_SDK_PATH."
  exit 1
fi
echo "OK: $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)"

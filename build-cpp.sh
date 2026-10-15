#!/usr/bin/env bash
# Build the C++ engine (cpp/src) to WebAssembly: web/moba_engine_cpp.wasm
#
# It exports exactly the same functions as the Rust engine, so the web front end can run either one
# (pick it in the menu, or open index.html?engine=cpp).
#
# Needs one C++ -> WebAssembly toolchain. The first one found is used:
#   1. Emscripten   macOS: brew install emscripten      (any OS: https://emscripten.org)
#   2. wasi-sdk     set WASI_SDK_PATH to the unpacked release (https://github.com/WebAssembly/wasi-sdk)
#   3. zig          macOS: brew install zig
#
# Native C++ builds (tests, headless matches) use CMake instead:
#   cmake -S cpp -B cpp/build && cmake --build cpp/build -j && cpp/build/engine_tests
set -euo pipefail
cd "$(dirname "$0")"

OUT=web/moba_engine_cpp.wasm
SRC=(cpp/src/*.cpp)
# Same floating-point rules as the native build: the simulation must match the Rust engine bit for bit.
FLAGS=(-std=c++17 -O3 -DNDEBUG -ffp-contract=off -fno-fast-math -fno-exceptions -Icpp/src)
FUNCS=(moba_abi_version moba_new moba_step moba_snapshot moba_hash_lo moba_hash_hi
       moba_units_ptr moba_heroes_ptr moba_projs_ptr moba_zones_ptr moba_events_ptr moba_global_ptr
       moba_info_ptr moba_info_len moba_terrain_ptr
       moba_alloc moba_mod_load moba_mod_error_ptr moba_mod_error_len moba_mod_clear moba_mod_count
       moba_content_export moba_content_ptr
       moba_cmd_move moba_cmd_attack_move moba_cmd_attack moba_cmd_stop moba_cmd_hold moba_cmd_cast
       moba_cmd_level_up moba_cmd_buy moba_cmd_sell moba_set_bot)
LINK_EXPORTS=()
for f in "${FUNCS[@]}"; do LINK_EXPORTS+=("-Wl,--export=$f"); done

if command -v em++ >/dev/null 2>&1; then
  echo "Building with Emscripten ($(em++ --version | head -1))"
  EXPORTED=$(printf '_%s,' "${FUNCS[@]}")
  # em++ (not emcc): it links the C++ standard library (operator new/delete, std::nothrow, ...).
  em++ "${SRC[@]}" "${FLAGS[@]}" --no-entry \
    -sSTANDALONE_WASM=1 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=32MB -sSTACK_SIZE=1MB \
    -sEXPORTED_FUNCTIONS="${EXPORTED%,}" -o "$OUT"
elif [ -n "${WASI_SDK_PATH:-}" ] && [ -x "$WASI_SDK_PATH/bin/clang++" ]; then
  echo "Building with wasi-sdk ($WASI_SDK_PATH)"
  TARGET=wasm32-wasip1
  [ -d "$WASI_SDK_PATH/share/wasi-sysroot/lib/$TARGET" ] || TARGET=wasm32-wasi
  "$WASI_SDK_PATH/bin/clang++" --target=$TARGET -mexec-model=reactor --sysroot="$WASI_SDK_PATH/share/wasi-sysroot" \
    "${FLAGS[@]}" "${LINK_EXPORTS[@]}" -Wl,--strip-all -o "$OUT" "${SRC[@]}"
elif command -v zig >/dev/null 2>&1; then
  echo "Building with zig ($(zig version))"
  zig c++ -target wasm32-wasi -mexec-model=reactor "${FLAGS[@]}" "${LINK_EXPORTS[@]}" -o "$OUT" "${SRC[@]}"
else
  echo "No C++ -> WebAssembly compiler found."
  echo "Install Emscripten (macOS: brew install emscripten), or set WASI_SDK_PATH, or install zig, then run this again."
  exit 1
fi

if command -v wasm-opt >/dev/null 2>&1; then
  wasm-opt -O3 --enable-bulk-memory --enable-nontrapping-float-to-int --enable-sign-ext -o "$OUT" "$OUT" || true
fi
echo "OK: $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)"
echo "Run ./serve.sh, open http://localhost:8080 and pick the C++ engine in the menu (or add ?engine=cpp)."

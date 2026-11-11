#!/usr/bin/env bash
# Build the native desktop app (cpp/native/) for macOS or Linux: cpp/build/lanewars
#
# It is the C++ engine plus the C++ 3D renderer in a window, with no browser. It needs CMake, a C++20
# compiler, SDL2 for the window and wgpu-native for drawing (WebGPU on Metal / Vulkan):
#   macOS    brew install cmake sdl2 wgpu-native
#   Linux    sudo apt install cmake g++ libsdl2-dev        (Fedora: sudo dnf install cmake gcc-c++ SDL2-devel)
#            wgpu-native has no distro package, so this script downloads the pinned release below into
#            .deps/ and checks its SHA-256. To use your own copy, set WGPU_NATIVE_DIR to an unpacked release.
#            Running the app needs a Vulkan driver (any recent Mesa or vendor driver).
#
#   ./build-native.sh && cpp/build/lanewars 2 7      # heroes per team, seed
set -euo pipefail
cd "$(dirname "$0")"

WGPU_VERSION=v29.0.1.1
WGPU_SHA256_x86_64=95a4d90c071005a98d03eab348beaa6b07e16eb00d1dcdb9f8348f75eb97ec5a
WGPU_SHA256_aarch64=015fcdf1dbae82e614a783cc38017e5399ae0927a889fe9b69c9b664bc61b47a

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing $1. $2" >&2
    exit 1
  fi
}

case "$(uname -s)" in
  Darwin)
    need cmake "Install it with: brew install cmake"
    need sdl2-config "Install SDL2 with: brew install sdl2"
    if [ -z "${WGPU_NATIVE_DIR:-}" ] && [ ! -d /opt/homebrew/opt/wgpu-native ] && [ ! -d /usr/local/opt/wgpu-native ]; then
      echo "Missing wgpu-native. Install it with: brew install wgpu-native" >&2
      exit 1
    fi
    ;;
  Linux)
    need cmake "Install it with: sudo apt install cmake g++ libsdl2-dev"
    need sdl2-config "Install SDL2's development files: sudo apt install libsdl2-dev   (Fedora: sudo dnf install SDL2-devel)"
    if [ -z "${WGPU_NATIVE_DIR:-}" ]; then
      ARCH=$(uname -m)
      [ "$ARCH" = arm64 ] && ARCH=aarch64
      SUM_VAR="WGPU_SHA256_$ARCH"
      SUM="${!SUM_VAR:-}"
      if [ -z "$SUM" ]; then
        echo "No pinned wgpu-native build for $ARCH. Unpack one yourself and set WGPU_NATIVE_DIR." >&2
        exit 1
      fi
      WGPU_NATIVE_DIR="$PWD/.deps/wgpu-native-$WGPU_VERSION-$ARCH"
      if [ ! -f "$WGPU_NATIVE_DIR/lib/libwgpu_native.so" ]; then
        need curl "It downloads wgpu-native; or unpack a release yourself and set WGPU_NATIVE_DIR."
        need sha256sum "It verifies the wgpu-native download."
        ZIP="wgpu-linux-$ARCH-release.zip"
        URL="https://github.com/gfx-rs/wgpu-native/releases/download/$WGPU_VERSION/$ZIP"
        mkdir -p .deps
        echo "Downloading wgpu-native $WGPU_VERSION ($ARCH)"
        curl --proto '=https' --tlsv1.2 -fsSL -o ".deps/$ZIP" "$URL"
        if ! echo "$SUM  .deps/$ZIP" | sha256sum -c - >/dev/null; then
          echo "Checksum mismatch for $URL; not using it." >&2
          rm -f ".deps/$ZIP"
          exit 1
        fi
        rm -rf "$WGPU_NATIVE_DIR"
        mkdir -p "$WGPU_NATIVE_DIR"
        (cd "$WGPU_NATIVE_DIR" && cmake -E tar xf "../$ZIP")
        rm -f ".deps/$ZIP"
      fi
      export WGPU_NATIVE_DIR
    fi
    ;;
  *)
    echo "build-native.sh covers macOS and Linux. On Windows, run CMake yourself with -DMOBA_NATIVE_APP=ON (untested)." >&2
    exit 1
    ;;
esac

ARGS=(-DMOBA_NATIVE_APP=ON)
[ -n "${WGPU_NATIVE_DIR:-}" ] && ARGS+=("-DWGPU_NATIVE_DIR=$WGPU_NATIVE_DIR")
[ -n "${WERROR:-}" ] && ARGS+=(-DMOBA_WERROR=ON) # CI sets WERROR=1
cmake -S cpp -B cpp/build "${ARGS[@]}" >/dev/null
cmake --build cpp/build -j --target lanewars

echo "OK: cpp/build/lanewars"
echo "Run it: cpp/build/lanewars 2 7     (heroes per team, seed)"

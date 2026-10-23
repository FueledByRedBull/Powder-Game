#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT_DIR/build-win"
DIST_DIR="$ROOT_DIR/dist/win11/PowderGame"
ZIP_PATH="$ROOT_DIR/dist/win11/PowderGame-win11-x64.zip"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++

cmake --build "$BUILD_DIR" -j

# Start from an empty folder so an older bundle's shaders/ and DLLs are not zipped again.
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"
cp "$BUILD_DIR/powder_game.exe" "$DIST_DIR/"

cp "$ROOT_DIR/packaging/README.txt" "$DIST_DIR/README.txt"

mkdir -p "$(dirname "$ZIP_PATH")"
rm -f "$ZIP_PATH"
(
  cd "$(dirname "$DIST_DIR")"
  zip -r "$ZIP_PATH" "$(basename "$DIST_DIR")" >/dev/null
)

echo "Created: $ZIP_PATH"

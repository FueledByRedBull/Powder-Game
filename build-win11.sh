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

cat > "$DIST_DIR/README.txt" <<'TXT'
Powder Game (Windows 11 x64)

Run:
- Double-click powder_game.exe

Controls:
- 1: Sand
- 2: Water
- 3: Solid
- 4: Erase
- 5: Smoke
- 6: Fire
- 7: Wood
- 8: Oil
- [ / ]: Decrease / increase brush radius (1 to 96)
- Left mouse: paint
- H: Show / hide the on-screen help
- F1: Normal view
- F2-F10: Debug views (boundary, water, water velocity, gas velocity,
  gas pressure, smoke, temperature, fuel/reaction, failure counters)
- Esc: quit

The window shows a dark cave. A compact inventory (top left) holds the eight
materials, the selected one's name and the brush radius, with a help panel under
it that hides itself after a few seconds. The brush is a ring and a warm lantern
light over the simulation.

Notes:
- powder_game.exe is self-contained (shaders built in, MinGW runtime linked
  statically); run it from any folder.
- Requires OpenGL 4.3 core with at least 18 image units, 18 compute image
  uniforms, 19 combined shader output resources, 8 SSBO bindings and 16
  fragment texture units; the app checks the first four at startup.
TXT

mkdir -p "$(dirname "$ZIP_PATH")"
rm -f "$ZIP_PATH"
(
  cd "$(dirname "$DIST_DIR")"
  zip -r "$ZIP_PATH" "$(basename "$DIST_DIR")" >/dev/null
)

echo "Created: $ZIP_PATH"

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

mkdir -p "$DIST_DIR"
cp "$BUILD_DIR/powder_game.exe" "$DIST_DIR/"
cp -r "$BUILD_DIR/shaders" "$DIST_DIR/"

cat > "$DIST_DIR/README.txt" <<'TXT'
Powder Game (Windows 11 x64)

Run:
- Double-click powder_game.exe

Controls:
- 1: Sand brush
- 2: Water brush
- 3: Solid brush
- 4: Erase brush
- 5: Smoke brush
- 6: Fire brush
- [ / ]: Decrease / increase brush radius
- Left mouse: paint
- Esc: quit

Notes:
- Keep the shaders/ folder next to powder_game.exe.
- Requires a GPU/driver with OpenGL 4.3+ support.
TXT

mkdir -p "$(dirname "$ZIP_PATH")"
rm -f "$ZIP_PATH"
(
  cd "$(dirname "$DIST_DIR")"
  zip -r "$ZIP_PATH" "$(basename "$DIST_DIR")" >/dev/null
)

echo "Created: $ZIP_PATH"

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
- powder_game.exe is self-contained (shaders built in, C/C++ runtime linked
  statically); run it from any folder.
- Requires OpenGL 4.3 core with at least 18 image units, 18 compute image
  uniforms, 19 combined shader output resources, 8 SSBO bindings and 16
  fragment texture units. The game checks the first four at startup and names
  a missing one in a dialog.
- Source, documentation and measured limitations (VALIDATION.md):
  https://github.com/FueledByRedBull/Powder-Game

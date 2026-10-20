# Powder Game agent instructions

Applies to this repository. Read [README.md](README.md) for setup and controls,
[PLAN.md](PLAN.md) for the implemented architecture, [VALIDATION.md](VALIDATION.md)
for commands and acceptance methodology, and [ROADMAP.md](ROADMAP.md) for current
handoff and historical evidence. Historical checkpoints are not current passes.

## Repository map and runtime

- `src/PowderApp.cpp` / `.hpp`: resource ownership, pass orchestration, input,
  rendering, the HUD pass and replays. `src/Config.hpp` validates environment settings;
  `src/FixedStepClock.hpp` controls elapsed-time scheduling; `src/GLUtil.*` loads
  shaders; `src/HudFont.hpp` holds the HUD bitmap font and layout constants.
- `shaders/`: GLSL compute and render programs. Trace host bindings, formats,
  particle layouts and producer/consumer barriers together when changing a pass.
  `composite.frag` draws the scene; `light_build.comp` builds the small emitted-light
  texture it reads; `hud.frag` is the interactive-only overlay.
- `tests/ConfigTests.cpp`, `GpuTests.cpp`, `SandTests.cpp` and
  `AppLifecycleTests.cpp` back the `powder_config_tests`, `powder_gpu_tests`,
  `powder_sand_tests` and `powder_app_lifecycle_tests` targets respectively.
  CTest names are `config-parsing`, `gpu-<case>`, `sand-<case>`, `app-<case>` and
  `app-shader-package`; `CMakeLists.txt` lists them.
- Rendering split: `Render()` is HUD-free and stretches the scene over the framebuffer,
  which replays and pixel tests rely on. The interactive loop and the `hud` case call
  `Render(true)` (letterboxed square viewport) followed by `DrawHud()`. Keep the HUD out
  of replays and other pixel tests. `composite.frag` paints the whole framebuffer: the
  simulation inside `simRect` and the letterbox margin with the same dark cave-wall
  albedo; it has no time dependence. The scene is albedo times light (moonlit ambient,
  the fire's glow from `light_build.comp` and its mip chain, and a cursor lantern that
  only letterboxed frames with the cursor over the simulation get) plus emissive fire.
  `PowderApp::lighting_enabled_` (uniform `lighting`) switches the light and the lantern
  off, leaving plain albedo: the CPU oracle in `render-modes` and the `hud` pixel tests
  run unlit, and the `lighting` case covers the light. A `composite.frag` albedo change
  also changes that oracle; HUD layout changes belong in `HudFont.hpp` and the `hud` case.
  The HUD stack sits in the left letterbox margin when `hud::FitsInMargin` says it fits
  and overlays the corner otherwise. HUD motion is eased host state (`HudMotion`,
  advanced by `DrawHud(now)`), so tests pass explicit times; the help panel hides itself
  after `hud::kHelpSeconds` only when `help_hide_time_` is armed.
- `CMakeLists.txt`, `cmake/CopyRuntimeLibraries.cmake`, `build-win11.sh` and
  `.github/workflows/` define builds and packaging. `build/`, `build-msvc/`,
  `build-win/` and `dist/` are generated output locations; existing evidence and
  older packages there do not describe the current source automatically.
- CMake 3.24+, C++20, GLFW and GLAD; OpenGL 4.3 core with at least 18 image units,
  18 compute image uniforms, 19 combined shader output resources and 8 SSBO
  binding points. `composite.frag` already uses all 16 fragment texture units the
  GL 4.3 minimum guarantees (units 0..15), so a new composite sampler must reuse one.
  GPU fixtures need a real compatible context, even when hidden.
  CMake can fetch pinned dependencies when system packages are absent.

## Build and checks

Run from the repository root. On Windows use Visual Studio 2022 Developer
PowerShell with CMake and Python available. `PYTHONUTF8` is needed by the pinned
GLAD generator on Windows. The bundled CMake path is documented in VALIDATION.

```powershell
$env:PYTHONUTF8 = '1'
cmake -S . -B build-msvc -A x64
cmake --build build-msvc --config Release --parallel 8
```

For a focused shader change, build the relevant target and select its registered
CTest case. For example, water integration uses:

```powershell
cmake --build build-msvc --config Release --target powder_gpu_tests --parallel 8
ctest --test-dir build-msvc -C Release -R '^gpu-water-integration$' --output-on-failure -j 1
```

Use the matching application fixture for host orchestration changes. For shared
pipeline or deliverable integration, build all targets and run:

```powershell
ctest --test-dir build-msvc -C Release --output-on-failure -j 1
cmake -E env POWDER_REPLAY=all .\build-msvc\Release\powder_game.exe
.\build-msvc\Release\powder_app_lifecycle_tests.exe benchmark-realtime
```

Replay selectors are `water`, `gas`, `sand`, `boundary`, `leakage`, `combined` and
`all`; each scene repeats twice by default. `benchmark-realtime` is a manual
acceptance command, excluded from CTest; the single-tick `benchmark` diagnostic
does not establish the two-tick real-time gate. Run GPU checks serially and keep
other simulations/benchmarks idle. Do not build while another worker uses the
shared build tree or its copied runtime shaders.

## Physics and evidence contracts

- Particle records plus the packed per-cell counts (sleeping sand, packed water) are
  authoritative for water/sand count and mass; census and conservation checks must
  count moving plus packed. Render fields are derived. Preserve quadratic transfer moments, FLIP's pre-force baseline,
  swept wall/domain collision, sand restoring stress and plastic yield.
- Keep divergence, pressure operator and projection consistent at solids/free
  surfaces. Preserve finite-storage diagnostics, elapsed-time dissipation and
  CFL/material-wave/diffusion rejection before unsafe dispatch. Do not hide
  failures by clipping required substeps or weakening residual gates.
  The gas residual threshold is 0.25 /s (was 0.015 when the gas speed cap was 10 cells/s): about one
  half-float step of a face at the 300 cells/s cap, 8e-4 of the cap and so stricter relative to speed than
  before. Keep it tied to the speed cap; a deliberately broken solve (CG update disabled) still reports
  millions of residual failures in the fire scenes (VALIDATION).
- Physical scale (PLAN, Scale and constants): one cell is 5 mm, g = 1962 cells/s^2 for water and
  sand, no uniform velocity damping (only `water_bulk_damping` on pressure-supported water; free fall is
  exempt), speed caps 2400 cells/s (12 m/s, above a full-domain free
  fall). Water substeps follow the fastest particle at pressure CFL 8 (at most 6 solves per tick) with
  swept advection sub-moves of at most 2 cells; sand substeps are the larger of the stress-wave limit
  (CFL 0.4) and the advective limit (1 cell per substep), not their sum. The budgets at the caps are
  checked before any dispatch.
- Regression scenes must contain the intended matter, ignite before testing
  extinction, and reach boundaries before testing leakage. Source-free particle
  mass drift is at most 0.5%; unexpected particle creation/loss, invalid state and
  nonzero failure counters are failures.
- Performance gates remain 60 FPS idle and 30 FPS combined at 1000x1000 with
  100,000 water and 50,000 sand particles plus smoke/fire; gas is 500x500.
  Combined frames use two default 1/60-second ticks. Mean GPU and wall budgets
  must both pass; complete GPU-frame p95 is at most 50 ms. The preferred combined
  target is 60 FPS. Preserve load, physics and correctness gates when optimizing.
- Bind reported results to the exact source, shader and binary hashes, relevant
  configuration, renderer/driver and workload. Keep raw failures and exit codes
  in the existing ignored `build/goal-baseline/` evidence area. Reuse results only
  when those inputs remain unchanged; distinguish static review, GPU execution
  and native interactive observations.

## Runtime packaging

The app loads `shaders/` beside its executable, independent of working directory;
direct GPU fixtures load source shaders. Build the appropriate target after shader
edits so application checks use the updated copy. Stage DLLs from the generated
`runtime-dlls.txt` manifest as well as shaders. The `app-shader-package` CTest
checks ASCII/Unicode relocation and shader selection. Follow VALIDATION for
package verification; do not treat an old `dist/win11` bundle as current evidence.
The tag workflow creates a draft release containing versioned binary/source ZIPs
and `SHA256SUMS.txt`. Verify the downloaded binary, source and checksums before
publishing the draft; a successful hosted build alone does not verify GPU behavior.

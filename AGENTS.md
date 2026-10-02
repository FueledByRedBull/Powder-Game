# Powder Game agent instructions

Applies to this repository. Read [README.md](README.md) for setup and controls,
[PLAN.md](PLAN.md) for the implemented architecture, [VALIDATION.md](VALIDATION.md)
for commands and acceptance methodology, and [ROADMAP.md](ROADMAP.md) for current
handoff and historical evidence. Historical checkpoints are not current passes.

## Repository map and runtime

- `src/PowderApp.cpp` / `.hpp`: resource ownership, pass orchestration, input,
  rendering and replays. `src/Config.hpp` validates environment settings;
  `src/FixedStepClock.hpp` controls elapsed-time scheduling; `src/GLUtil.*` loads shaders.
- `shaders/`: GLSL compute and render programs. Trace host bindings, formats,
  particle layouts and producer/consumer barriers together when changing a pass.
- `tests/ConfigTests.cpp`, `GpuTests.cpp`, `SandTests.cpp` and
  `AppLifecycleTests.cpp` back the `powder_config_tests`, `powder_gpu_tests`,
  `powder_sand_tests` and `powder_app_lifecycle_tests` targets respectively.
- `CMakeLists.txt`, `cmake/CopyRuntimeLibraries.cmake`, `build-win11.sh` and
  `.github/workflows/` define builds and packaging. `build/`, `build-msvc/`,
  `build-win/` and `dist/` are generated output locations; existing evidence and
  older packages there do not describe the current source automatically.
- CMake 3.24+, C++20, GLFW and GLAD; OpenGL 4.3 core with at least 18 image units,
  18 compute image uniforms, 19 combined shader output resources and 8 SSBO
  binding points. GPU fixtures need a real compatible context, even when hidden.
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

- Particle records are authoritative for water/sand count and mass; render fields
  are derived. Preserve quadratic transfer moments, FLIP's pre-force baseline,
  swept wall/domain collision, sand restoring stress and plastic yield.
- Keep divergence, pressure operator and projection consistent at solids/free
  surfaces. Preserve finite-storage diagnostics, elapsed-time dissipation and
  CFL/material-wave/diffusion rejection before unsafe dispatch. Do not hide
  failures by clipping required substeps or weakening residual gates.
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

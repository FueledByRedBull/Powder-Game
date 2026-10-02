# Powder Game Validation

Use these commands from the repository root.

## Current verification status - 2026-10-02

The user requested Computer Use after completing the stabilization goal with an
interactive-check waiver. Native testing exposed short key taps and mouse clicks
being lost between polling and simulation ticks. Brush/view selection and Escape
now use press callbacks; GLFW sticky mouse mode preserves brief clicks until a
simulation tick consumes them. The regression failed before the fix (exit 1,
`brush tap must survive PRESS+RELEASE before polling: key 49`) and passes afterward.
Evidence: `native-input-{red,green}.log` under `build/goal-baseline/`.

The input-fixed Release build passes all 44 native/GPU/application tests in 22.36
seconds, and all six replay scenes pass two repetitions. Evidence:
`native-input-integration-{build,ctest}.log` and
`native-input-replay-{water,gas,sand,boundary,leakage,combined}.log`.
Independent review found no actionable correctness issues in the input diff.

The fresh two-tick benchmark narrowly fails the unchanged 30 FPS mean gate.
The initial run measured GPU/wall 33.330/33.520 ms (29.833 wall FPS), GPU p95
46.248 ms. Three consecutive repeats chosen before execution measured wall
33.607/33.593/33.661 ms (29.756/29.768/29.708 FPS), GPU p95 46.424/46.415/45.722 ms.
All four runs returned exit 1: `FAIL benchmark-realtime: [combined full-frame
acceptance failed]`. All retain zero counters and sampled mass drift, and identical
water iteration/gas cycle signatures. The earlier checkpoint passed at GPU/wall
31.566/31.724 ms (31.522 wall FPS), GPU p95 44.481 ms; that historical pass does
not erase the fresh failures. Evidence: `native-input-realtime.log`,
`native-input-realtime-repeat-{1,2,3}.log`, `native-input-realtime-repeat.json`,
and `final-complete-realtime.log`. These offscreen measurements include two ticks
and rendering but exclude display/vsync latency. No thresholds were relaxed.

Two same-session control runs of the preserved pre-input half-storage executable
also fail: wall 33.975/33.548 ms (29.434/29.808 FPS), GPU p95 46.547/45.994 ms,
zero counters/mass drift and the same iteration signatures. Its benchmark body,
executed physics and all 55 shaders match this workload; it is not the exact
`final-complete` executable. The input delta has no direct timed path: this harness
bypasses Initialize, does not call UpdateInput and uses scripted replay input.
The evidence does not isolate an input-induced slowdown; the session/hardware
cause remains unknown, and current 30 FPS acceptance is still unmet. Logs:
`native-input-realtime-control-{1,2}.log`, provenance:
`native-input-realtime-control.json`. No unrelated desktop applications or driver
settings were changed for these measurements.

Native observations on the input-fixed build: brief brush selection and clicks,
solid painting/erasing, falling sand and water, smoke/fire response, discrete
bracket sizing, debug shortcuts, width resize from 1280 to 920 client pixels,
pointer alignment after resize, Escape/native close and empty default relaunch.
Plain F1 is intercepted by Lightshot on this desktop; Ctrl+F1 reaches the same
composite handler. Some quiet diagnostic fields are visually black, so shortcut
exercise is supplemented by automated mapping and nonzero render-field tests.
The smoke response was small; the sampled clear F10 frame does not establish zero
counters throughout the session. Synthetic drag timing does not establish
continuous stroke coverage. True held-key repeat timing, DPI transitions and
sustained native stress at the benchmark load remain unverified.
Evidence: `native-input-ui-verification.json`.

The original gas replay failed with reaction 22.64: its water stream fell below
a wider, rising flame. Measured cooling matches the independent formula. The
corrected source covers the plume from above with fewer particles (236,676).
Its causal regression ends at reaction 0.00213 versus 139.73 without water, with
unchanged ignition/extinction gates and conservation checks. Original failure and
rejected geometry logs remain preserved.

Shared GLFW/GLAD Release build and clean ASCII/Unicode package relocation pass;
the former exe/shader-only staging fails with missing-library exit `0xc0000135`.
Linux, MinGW and other GPUs remain unverified. The existing
`dist/win11` package contains older `v0.3.0` shaders.

The unpublished Windows candidate is
`build/goal-baseline/PowderGame-win11-x64-candidate-20261002-native.zip`. It includes the
matching executable/shaders, eight runtime DLLs, dependency licenses, first-party
source and `SHA256SUMS.txt`. Fresh Unicode extraction verifies every checksum;
the extracted app passes two water replays from an unrelated working directory.
See `candidate-native-archive-verification.json` and
`candidate-native-extracted-replay.log`. It contains the input-fixed executable;
matching source and shader hashes are checked during packaging. The benchmark
shortfall is disclosed in its notes. Earlier candidates and their evidence are preserved.
An initial staging-check wrapper exited 1 (`Expected two passing replay repetitions`)
because it counted stdout only; the captured stderr contained both passing results
and application exit 0. The corrected check verifies both streams. This wrapper
failure is preserved in `candidate-package-positive-verification.json`.

## Historical baseline and first corrections

Numeric parsing and configuration domains have been tested with MSVC 19.44 using
`/std:c++20 /EHsc /W4 /WX`: 115 top-level checks plus 3 empty-value child checks
passed. Missing numeric variables retain defaults. Present invalid, empty,
nonfinite or out-of-range values fail before graphics startup and identify the
setting. All original defaults are preserved.

Accepted domains:

- Fixed timestep: `1/240` through `1/15` seconds; replay repetitions: `1` through `4`.
- Substep and pressure iteration counts: positive; minimum substeps cannot exceed
  maximum substeps. CFL limits and timing budgets must be positive.
- FLIP blend and sand spawn occupancy limit: `0` through `1`.
- Reaction, cooling, diffusion, dissipation, drag, friction, sand speed/density
  threshold, pressure residual thresholds and replay tolerances: nonnegative.
- Water gravity and gas buoyancy coefficients may be signed.

Invalid settings are rejected instead of silently clamped. Native domain checks
do not establish GPU stability for extreme values or replace runtime validation.
The initial substep arithmetic correction passed 117 native checks plus three
empty-value child checks. The final calculation uses double arithmetic and rejects
requirements above the configured budget before narrowing. Gas characteristic
components and both particle transport samples obey their configured speed caps;
runtime numeric diagnostics still cover unsupported extreme states.

After the normal configure, build and run the native parser test with:

```powershell
cmake --build build-msvc --config Release --target powder_config_tests
ctest --test-dir build-msvc -C Release -R '^config-parsing$' --output-on-failure
```

The parser now also passes through CTest. Both the preserved original application
and the current application built in Release after downloading the pinned GLFW
3.4 and GLAD 0.1.36 dependencies. All runtime shaders compile on the RX 7800 XT.
The first GPU regression run reproduced five correctness failures; all six old
replays still reported success. The five defects are corrected, and the extended
initial CTest suite passes 7/7: configuration, all-shader compilation, water fixed-point
transfer, boundary mask preservation, water projection beside walls, smoke decay
over different substep counts, and nonfinite pressure detection. The current water
replay also passes both repetitions. These checks do not yet meet all acceptance
gates. Baseline failures and verification logs are retained under
`build/goal-baseline/`, including `ctest-shader-fixes.log`.

For reproducible GLAD generation with `-DGLAD_REPRODUCIBLE=ON`, enable Python
UTF-8 mode for the build process on Windows. Otherwise GLAD can misread its packaged
UTF-8 XML specification using the system text encoding:

```powershell
cmake -E env PYTHONUTF8=1 cmake --build build-msvc --config Release --parallel 8
```

The expanded regression checkpoint passed 17/17 tests after rebuilding the app,
including reset/erase, FLIP force transfer, analytic affine transfer/advection,
particle capacity, free-surface translation, source-free water mass and integrated
gas projection around obstacles and a narrow neck. See
`build/goal-baseline/ctest-conservation-batch.log`.

The following full replay attempt failed on the water scene with accumulated
pressure nonconvergence (118,391 reports, exit 1). It stopped before the other scenes.
At that checkpoint this superseded earlier weak replay successes. See
`build/goal-baseline/replay-conservation-batch-all.log`. The corrected final replay
passes; benchmark history and the fresh mean-time shortfall are recorded above.

The next integration checkpoint passed 18/18 tests, including consumer-specific
memory barriers and 12 nonfinite readback rejection cases
(`ctest-synchronization-readback.log`). Subsequent focused gas velocity decay and
sand transfer/integration fixtures reproduced failures and passed after correction;
their shared application integration was pending at that checkpoint. The fixed
water-pressure pool then remained a failing gate while its solver was replaced: half precision
stagnates at maximum residual 0.34375 after 4,096--16,384 iterations, versus
0.000397 for the identical single-precision control. See `water-pressure-red.log`
and `ROADMAP.md` for the subsequent correction and current handoff.

## Earlier integration and performance checkpoints

The later Release checkpoint passes 39/39 cases in 16.01 seconds, including material
physics/configuration, fire resource limits/no-flux diffusion, dense-versus-tiled
coupling, exact wide transfer sums, strengthened replay metrics and ASCII/Greek
standalone packaging. Evidence: `preconditioner-integration-{build,ctest}.log`.
Its source/binaries/shaders/hashes are preserved in `preconditioner-checkpoint/`.
The final 44-case run also covers subsequent pressure/coupling changes. Windows was
locked when interactive checks were attempted; no input was sent to the lock screen.
The user subsequently waived those checks. The later Computer Use follow-up and
remaining limits are recorded in the current status above.

Release build and all 25 then-registered CTest cases passed in 7.82 seconds, including
single-precision CG pressure, elapsed-time gas decay, 36 swept-collision cases,
fixed-step timing and actual particle mass/ignition validation. Water replay passed
twice with identical aggregate signatures and no debug failures. Logs:
`water-cg-integration-build.log`, `water-cg-integration-ctest.log`, `water-cg-replay.log`.
The final integration run supersedes this checkpoint; earlier failures remain
historical evidence rather than the current water-replay status.

The manual complete-frame benchmark commands are:

```powershell
.\build-msvc\Release\powder_app_lifecycle_tests.exe benchmark
.\build-msvc\Release\powder_app_lifecycle_tests.exe benchmark-realtime
```

It renders to a real 1000x1000 framebuffer, with 60 warmup and 240 measured frames.
The combined scene starts with 100,000 water and 50,000 sand particles and injects
smoke/fire. GPU timestamps include all simulation and rendering; wall time
includes CPU submission and waiting for completion. Validation readback is excluded
from both. Mean GPU and wall time must fit 60 FPS idle / 30 FPS combined, GPU p95
must be at most 50 ms, sampled mass drift at most 0.5%, gas must become active and
all failure counters must stay zero. It does not measure display/vsync latency.
`benchmark` uses one 1/60-second tick per rendered frame; `benchmark-realtime` uses
two ticks for combined frames so the 30 FPS budget also advances simulation in real
time. Both use one tick for idle frames. Final acceptance requires the latter.

Default cap 64: idle GPU/wall 11.45/12.91 ms; combined 15.21/16.80 ms, p95 18.95 ms,
zero sampled particle mass drift, but 6,781,044 accumulated pressure failures.
Caps 128/256/512 also failed and incurred substantial idle overhead. No threshold
has been relaxed. See `full-frame-benchmark-initial.log` and
`benchmark-water-caps.json`. These are historical failures. After correcting the
gas coarse-boundary operator and water CG work scheduling, cap 1024 passes the
single-tick benchmark: idle GPU/wall 8.213/8.408 ms; combined 17.839/18.071 ms,
p95 35.377 ms, zero counters and zero sampled mass drift
(`benchmark-pressure-bounds-1024.log`). That result predates the new sand material
model. The first material-model real-time run fails speed: idle GPU/wall
8.622/8.793 ms; combined 61.114/61.337 ms (16.3 FPS), p95 80.331 ms, with zero
counters and sampled mass drift. The diagnostic repeat attributes 19.781 ms/tick
to water, 6.826 to gas and 3.014 to sand, with a mean of 193 iterations in the final
water solve. Evidence: `benchmark-material-realtime{,-profile}.log`. These failed
candidates preserve the original acceptance targets. The original final checkpoint
met them; the fresh timing shortfall is recorded in the current status above.

The subsequent 32-sweep block-preconditioned water solve reduces the combined
real-time GPU/wall means to 39.702/39.864 ms, p95 46.846 ms, with zero counters and
zero sampled mass drift (`pressure-block32-realtime.log`). The mean still fails
30 FPS. Pressure reduction fusion plus direct coupling then measure GPU/wall
38.458/38.628 ms, p95 45.825 ms, with zero counters and mass drift; the mean still
fails (`pressure-fused32-realtime.log`, exit 1). Four focused pressure checks pass.
Offscreen extraction and all ten render modes pass after correcting upper-edge
sampling (`render-modes-{red,green}.log`); interactive controls were unverified at
that checkpoint. The later native observations and their limits are recorded above.
`ROADMAP.md` records the current integration handoff.

## Configure

Windows with a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build-msvc -A x64
```

If `cmake` is not on PATH, use the installed Visual Studio bundled CMake:

```powershell
& 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' -S . -B build-msvc -A x64
```

Linux:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
```

## Build

Windows:

```powershell
cmake -E env PYTHONUTF8=1 cmake --build build-msvc --config Release --parallel 8
ctest --test-dir build-msvc -C Release --output-on-failure -j 1
```

With Visual Studio bundled CMake:

```powershell
$cmake = 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
& $cmake -E env PYTHONUTF8=1 $cmake --build build-msvc --config Release --parallel 8
& (Join-Path (Split-Path $cmake) 'ctest.exe') --test-dir build-msvc -C Release --output-on-failure -j 1
```

Linux:

```bash
cmake --build build --target powder_game -j
```

## Run Interactive App

Windows:

```powershell
.\build-msvc\Release\powder_game.exe
```

If the generator places the executable directly under `build-msvc`, use:

```powershell
.\build-msvc\powder_game.exe
```

Linux:

```bash
./build/powder_game
```

## Run Acceptance Replay

Replay mode compiles all runtime shaders, runs scripted scenes, reads validation aggregates, and returns nonzero on failure.
Each requested scene runs twice by default and compares tolerance-based deterministic signatures.

Run every scene:

```powershell
$env:POWDER_REPLAY = "all"
.\build-msvc\Release\powder_game.exe
Remove-Item Env:\POWDER_REPLAY
```

Run one scene:

```powershell
$env:POWDER_REPLAY = "boundary"
.\build-msvc\Release\powder_game.exe
Remove-Item Env:\POWDER_REPLAY
```

Available scenes:

- `water`
- `gas`
- `sand`
- `boundary`
- `leakage`
- `combined`

Useful replay/config overrides:

- `POWDER_REPLAY_REPEAT`: replay repetitions per scene, default `2`.
- `POWDER_REPLAY_DETERMINISM_REL_TOL`: relative signature tolerance, default `0.15`.
- `POWDER_REPLAY_DETERMINISM_ABS_TOL`: absolute signature tolerance, default `0.05`.
- `POWDER_FIXED_DT`: fixed simulation timestep.
- `POWDER_WATER_PRESSURE_ITERATIONS`, `POWDER_WATER_PRESSURE_RESIDUAL`
- `POWDER_GAS_PRESSURE_ITERATIONS`, `POWDER_GAS_PRESSURE_RESIDUAL`
- `POWDER_GAS_COMBUSTION_RATE`, `POWDER_GAS_WATER_COOLING`, `POWDER_GAS_BUOYANCY_TEMPERATURE`, `POWDER_GAS_BUOYANCY_SMOKE`
- `POWDER_SAND_DRAG`, `POWDER_SAND_MAX_VELOCITY`, `POWDER_SAND_FRICTION`
- `POWDER_SAND_REFERENCE_DENSITY`, `POWDER_SAND_SHEAR_MODULUS`, `POWDER_SAND_LAME_LAMBDA`
- `POWDER_SAND_FRICTION_ALPHA`, `POWDER_SAND_CFL`, `POWDER_SAND_MAX_SUBSTEPS`
- `POWDER_COMBINED_TARGET_FRAME_MS`, `POWDER_COMBINED_WORST_FRAME_MS`

## Expected Replay Checks

Replay fails if:

- any accumulated debug counter is nonzero:
  - `nan_inf_detected`
  - `particle_overflow_blocked`
  - `pressure_nonconverged`
  - reserved counter index 3 (the former `inactive_tile_misses` counter; no longer
    evidence of coupling coverage after removal of tile scheduling)
- timing queries return invalid values
- replay deterministic signatures drift beyond configured tolerance
- water source growth is not monotonic in the source window
- water replay finishes with zero live particles, unexpected count/mass changes,
  or exceeds the configured water particle cap
- gas never ignites or suppression does not extinguish within the fixed window
- sand mass drifts by more than `0.5%`
- boundary/leakage water never reaches its obstacle or crosses the blocker
- combined scene has missing water/sand or inactive gas, violates source-free mass
  conservation, or exceeds the configured GPU-time budget

## Current Environment Note

During the Milestone A audit, `cmake` was not available on PATH in the shell. If that happens, open a Visual Studio Developer PowerShell or add the installed CMake directory to PATH before running the commands above.

# Handoff and history

## Current handoff - v0.7.0, 2026-10-23

This section is the current handoff. Everything below it is history, kept as written;
where an older section says "current", read it as the state on the date in its title.

State: branch `release/v0.7.0`, with the integration in commit `12c1343`. Test counts, timings, hashes and exit codes are in VALIDATION, "Current
status"; this file does not repeat them. The algorithms are documented pass by pass in
`docs/ALGORITHMS.md`.

What v0.7.0 adds over v0.6.0 (design in PLAN):

- Water: a ghost-fluid free surface on a smoothed level set, and a conjugate-gradient
  restart that the GPU arms from the true residual.
- Gas: one pressure solve spread over a pair of ticks.
- Sand: hole collapse, per-grain damp bonds, slow-creep hardening with stochastic
  rounding, and a spawn budget on the sand pool.
- Sparks and embers are GPU particles.
- The benchmarks no longer see the real mouse cursor over their window.

Open items:

1. Native interactive checks: live mouse painting and fast strokes, the H key and help
   timeout, HiDPI and DPI transitions, temporal flicker, and how the gas and sparks look
   in a real window.
2. Other GPUs and drivers. Everything was measured on one machine.
3. The Linux and MinGW builds have not been run locally. Check the hosted CI, and the
   draft release's ZIPs and `SHA256SUMS.txt`, before publishing.
4. `app-sand-sleep-bias` passes its centroid gate with little margin.
5. `app-oil-air` and `app-gas-water` move when the multigrid preconditioner changes. They
   pass now, but re-run them after any change to it.

## Handoff - v0.6.0, 2026-10-21 (historical)

State and results below are as of v0.6.0. They are superseded by the v0.7.0 handoff above.

State: `main` at `c4efd24` (v0.5.0) plus the working tree that is to be committed and
tagged `v0.6.0`. Measured results, hashes and exit codes are in VALIDATION, "Current
status"; raw logs in the ignored `build/goal-baseline/2026-10-20-release-candidate/`.
The algorithms are documented pass by pass in `docs/ALGORITHMS.md`.

What changed since v0.5.0 (design in PLAN): a single self-contained `powder_game.exe`
(embedded shaders, static runtime, `POWDER_SHADER_DIR` override); sealed-pocket mean
removal for water and gas (`pressure_pockets.comp`); a 30 Hz gas step with an R32F
right-hand side and a gas projection consistent with the porous-sand operator; water
to the domain face, binned compaction and shared-memory P2G; sand sleep readiness,
submerged beds and pool-full brush limits; centripetal Catmull-Rom stroke smoothing.

Verified on 2026-10-21 (Windows 11, RX 7800 XT, Unity editor open): Release build exit
0, CTest 93/93, `POWDER_REPLAY=all` exit 0, `benchmark-realtime` exit 0 twice (combined
about 55 FPS, p95 about 28 ms), four `benchmark-profile` scenes exit 0, the exe alone in
a fresh folder, and `check-package.ps1` on a locally built release-shaped zip.

Open items:

1. The preferred 60 FPS combined target (about 55 FPS here) is unmet.
2. Native interactive checks: live mouse painting and fast strokes, the H key and help
   timeout, HiDPI and DPI transitions, temporal flicker, the gas look (scalars step every tick, the pressure solve spans a tick pair).
3. Not run here: Linux and MinGW builds, other GPUs, a machine without the VC++
   redistributable, the hosted CI and release workflows. Verify the draft release's
   ZIPs and `SHA256SUMS.txt` before publishing it.
4. Sand feel: a packed pile has a lower, flatter top than a moving one (not gated).

## Handoff - v0.5.0, 2026-10-20 (historical)

State: `main` at `3f7055d` plus the working tree that is to be committed and tagged
`v0.5.0` (the tag workflow then builds a draft release). Measured results, hashes and
exit codes are in VALIDATION, "Current status"; raw logs are in the ignored
`build/goal-baseline/2026-10-20-v0.5.0-final/`.

What the release contains (design in PLAN):

- Eight materials on keys 1-8: sand, water, solid, erase, smoke, fire, wood and oil.
  Fire burns in ambient oxygen and rises at real speed; water boils to steam where it
  meets flame; oil floats on water and burns as a slick; wood catches, chars and
  crumbles; sand smothers fire. H toggles the help panel, F1 the composite view, F2-F10
  the debug views, Esc quits.
- Real scale: 5 mm cells, g = 9.81 m/s^2, no artificial velocity damping except the
  supported-water bulk term, speed caps 2400 cells/s. Water is APIC/FLIP with an MGPCG
  pressure solve; sand is MPM-style with plastic yield, mu(I), volume correction and wet
  cohesion; the 500x500 gas grid has an MGPCG projection.
- Pools of 2,097,152 water and 1,048,576 sand records. Settled sand sleeps as per-cell
  grain counts and calm deep water packs the same way, so neither stays in the moving
  pool; both wake when disturbed.
- Dark cave composite with fire light (`light_build.comp`) and a cursor lantern, and an
  interactive-only HUD (`hud.frag`, `HudFont.hpp`) in the letterbox margin.

Verified on 2026-10-20 (Windows 11, RX 7800 XT, Unity editor sharing the GPU): Release
build exit 0, CTest 80/80, `POWDER_REPLAY=all` exit 0, a 5 s launch with empty output.

Open items:

1. `benchmark-realtime` exits 1. Its timing gates now pass (combined GPU mean 21.4-22.9 ms,
   p95 26.5-28.3 ms) but `pressure_nonconverged` is 2 from frame 13 on both runs. Find
   which solve misses its gate there and fix it without weakening the residual gate;
   `POWDER_PRESSURE_AUDIT` currently fails to capture the system. Then rerun on an idle
   GPU. The preferred 60 FPS combined target (about 44-47 FPS here) is unmet.
2. Native interactive checks: live mouse painting and fast strokes, the H key and help
   timeout, HiDPI and DPI transitions, temporal flicker on a real monitor.
3. Fast strokes on Windows: `WM_MOUSEMOVE` is coalesced to about one event per poll, and a
   radius 1-3 brush faster than about 64 cells per tick still leaves beaded gaps.
4. Sand feel: measured repose 26.6-30.7 degrees against a target near 32; a packed pile
   has a lower, flatter top than a moving one (not gated).
5. Not run here: Linux and MinGW builds, other GPUs, the hosted CI and release workflows.
   Verify the draft release's ZIPs and `SHA256SUMS.txt` before publishing it.
6. Packaging hygiene: CMake's `copy_directory` keeps deleted shaders in an existing build
   tree; package from a clean build directory.

## Earlier handoff - 2026-10-19 (historical)

This was the handoff on 2026-10-19; the items it lists as open are superseded by the
section above.

State: `main` at `3f7055d` plus uncommitted changes. Modified: `CMakeLists.txt`,
`shaders/composite.frag`, `shaders/sand_particle_clear.comp`,
`shaders/spawn_smoke.comp`, `shaders/water_particle_clear.comp`,
`shaders/water_particle_spawn.comp`, `src/PowderApp.cpp`, `src/PowderApp.hpp`,
`tests/AppLifecycleTests.cpp`, `tests/GpuTests.cpp`. New: `shaders/hud.frag`,
`shaders/light_build.comp`, `src/HudFont.hpp`, `docs/screenshot.png`. The documentation files and the packaged
README text in `build-win11.sh` and both workflows were updated to match. No branches, commits or
publication.

Done in this working tree:

- In-game HUD, interactive only: a compact inventory (six pixel-art material slots
  numbered 1-6, the selected slot outlined, its name and the brush radius) and a help
  panel under it at the top left, in the letterbox margin when it is wide enough;
  the help panel is shown at launch, hides itself after about 8 seconds and toggles with
  `H` (an "H HELP" hint remains when hidden); an anti-aliased brush ring drawn in grid
  space. The window title is
  refreshed about twice a second with brush, radius and FPS. `Render()` stays
  HUD-free so replays and pixel tests are unchanged; only `DrawHud()` draws it.
- Letterboxed, aspect-correct simulation viewport in the interactive window
  (`Render(true)`). Cursor mapping handles the margins and DPI scaling, and painting
  needs the cursor over the simulation with the window hovered.
- New composite look. Water is a cohesive blue body: the "water barely shows up" bug
  came from composite thresholds calibrated above typical free-surface water amounts,
  and thin one- or two-particle water now reads, with depth shading, a surface rim and
  foam only for fast flow (about 35 cells/s and up). Also a background gradient with
  dither, sand grain with a soft edge and top light, a solid bevel, a fire temperature
  glow with soft tonemapping, and a warm smoke tint. The render-modes CPU oracle
  mirrors the new math.
- Dark cave look pass, presentation only (no simulation, particle, solver, spawn or
  replay change; it replaces the earlier cozy toybox direction): a near-black textured
  cave wall that `composite.frag` also paints into the letterbox margin; textured cool
  stone with moss patches and crusty rims; ochre sand; murky teal water with a thin
  surface line; dark smoke; fire as the hero light. The scene is albedo times light
  (dim moonlit ambient with a strong vignette, a wide warm glow built from the fire's
  render fields in `light_build.comp` and a mip chain, and a cursor lantern in interactive
  frames) plus emissive flames and embers. `lighting_enabled_` switches the light off for
  the CPU oracle and the HUD pixel tests; the new `app-lighting` case checks the light.
  The HUD is dark low-opacity pixel-art plates with a warm one-unit edge, a brush ring that
  is thin and reddish for erase, and eased motion (about 150 ms, `HudMotion`, advanced by
  `DrawHud(now)`). Measured cost and verification are in VALIDATION.
- Bug fixes. Erase no longer deletes water particles in sand-covered cells across the
  whole map (`water_particle_clear` was culling every "blocked" cell). The solid brush
  removes sand and water particles inside the painted disc. Held strokes are
  interpolated along the cursor events collected since the last tick, from the press
  (spacing at most max(1, radius/2), at most 64 stamps per tick), so fast strokes leave
  no gaps and curves are followed; sand and water stamps skip the previous stamp's disc,
  so a swept cell gets one held tick of particles. Smoke erase clears the shared right and top velocity
  faces. The interactive water brush has a density gate; scripted and replay brushes
  keep exact emission.
- Sand and sand-water pass (PLAN "Sand and water", VALIDATION): occupancy is mass over
  rest density; sand walls off water above 0.25 and gas above 0.75 (mask values 2 and
  3); sand is a moving, slightly porous wall for the water pressure solve; water caught
  in sand is carried out instead of freezing; submerged sand is buoyed and dragged and
  water feels the equal drag; the sand grid velocity is RG32F; stiffness doubled (20
  material substeps); failed deformations are repaired; thin walls remove only inward
  motion. Friction alpha stays 0.5 (about 30 degree piles measured).

Verified in this session (details and the new test coverage are in VALIDATION): Release
build exit 0; ctest 52/52 passed (45 before the sand pass); `POWDER_REPLAY=all` exit 0
with all 12 scene passes.
`benchmark-realtime` **failed** the 30 FPS combined gate: combined GPU mean about
33.4-33.6 ms against the 33.33 ms budget, GPU p95 about 46-47 ms under the 50 ms limit.
A/B runs with the original shaders on the same machine were in the same 31.4-33.7 ms
band while a VirtualBox VM was running, so the gate is borderline and unmet, not
passed, and the preferred 60 FPS combined target is also unmet. After the sand pass the
combined GPU mean is about 35.0 ms, 1.5 ms above the baseline measured alternately the same
session. Static review, GPU
fixtures and offscreen frames cover the HUD and composite; no native interactive
observation was made.

Open items:

1. Re-run `benchmark-realtime` on a quiet machine with no VM, then copy the raw logs
   (and this session's) into `build/goal-baseline/` with source, shader and binary
   hashes. Do not claim the 30 FPS gate until it passes there.
2. Native interactive checks: live mouse painting including fast strokes, the `H` key
   in a live window, HiDPI and DPI transitions, and visual temporal flicker of the
   dither and sand grain.
3. Fast strokes (offscreen replay of a 600-cell sweep, all brushes): overlapping water
   and sand stamps emitted 2.7-3.2x the held-tick particles and a radius-24 water
   stroke exhausted the 262,144-particle pool (about 22,000 blocked spawns, the last
   fifth of the stroke missing); stamps now skip the previous stamp's disc, so cells get
   2 sand or 4 water particles and no spawn is blocked. Still open: Windows coalesces
   `WM_MOUSEMOVE` to about one cursor event per `glfwPollEvents`, so the callback
   trail is frame-rate dense there (sub-frame curve detail needs `GetMouseMovePointsEx`
   or raw input), and a radius 1-3 brush faster than about 64 cells per tick still
   leaves beaded gaps because the 64-stamp bound spreads the stamps.
4. Native look-and-feel checks for the dark cave pass: motion feel at the real refresh
   rate (slot selection, help fade and timeout, painting pulse and lantern swell), the
   lantern and glow on a real monitor and at other brightness settings, the HUD at HiDPI
   scales and in a narrow-margin window. `docs/screenshot.png` still shows the cozy look.
5. Sand pass leftovers. The gas pressure solve sits near its 0.015 gate beside small sand
   obstacles on the floor (the `combined` replay hit 0.0157 at every sand threshold below
   0.6 before the shipped ones; 1 of 10 sand-brush offsets still does): the gas solver
   needs a look at wedge geometry. The water solver has no particle density constraint, so
   water moved by escape or drag stays displaced. Only the surface of a dense sand body is
   dragged and buoyed. The domain-wall half-cell (sand particles stop 0.5 cell inside the
   domain while painted walls stop at the face) was left: fixing it needs the grid wall
   normals, the support stencil and about ten test expectations to change together.

## Completed goal and native follow-up - 2026-10-02

This execution record supersedes the historical status claims below. Existing changes on `main` are the starting source;
`build/goal-baseline/` preserves that source and its SHA-256 inventory. No commits,
branches, dependency upgrades, or publication are part of this goal.

Native interactive verification was waived by the maintainer ("Proceed without it") after
the desktop lock blocked it. The original goal was completed under that waiver.
The subsequent desktop-automation pass verified native behavior and exposed missed
brief input, now fixed. Current correctness checks pass, but four fresh benchmark
runs narrowly miss the unchanged 30 FPS mean target. The current handoff below
records this open performance finding and the remaining native-check limits.

### Phase 1 - Establish and meet correctness and performance gates

- [x] Read all first-party source, shaders, documentation, build/release files,
  and inspect the existing Windows deliverable. Record algorithm defects separately
  from hypotheses that need GPU evidence.
- [x] Configure and build the unchanged source, compile every shader on the actual
  GPU, and run each existing replay. Retain failures, renderer/driver identity,
  configuration, and per-pass timings under the ignored build directory.
- [x] Add small native GPU regression tests using the existing GLFW/GLAD/GLUtil
  dependencies. Reproduce transfer scaling, pressure projection beside solids,
  boundary sign/mask behavior, reset/erase behavior, and invalid configuration
  handling before changing their implementations.
- [x] Fix evidenced defects in independently verified batches. Preserve the
  particle/grid architecture unless an algorithm correction is necessary for
  correct behavior. Correct validation that can pass a nonfunctional simulation.
- [x] Measure complete simulation plus rendering, including scheduling. Use
  explicit particle loads, fixed input schedules, warmup, and frame percentiles;
  exclude validation readback overhead from GPU cost and report wall time separately.
- [x] Meet the complete-frame targets at the original final checkpoint; retain
  failed candidates as evidence. The native follow-up reopens the mean performance
  finding: four current-build runs reach 29.71-29.83 FPS against the 30 FPS gate.
- [x] Verify reset isolation, particle capacity, sustained scripted combined runs,
  and standalone-package shader selection with native GPU tests.
- [x] Subsequent desktop automation: brief input, material brushes, erasing, diagnostic
  shortcuts, width resizing, pointer alignment, close and relaunch.
- Still unverified: true held-key repeat timing, DPI transitions, continuous drag
  coverage between samples and sustained native stress at the benchmark load.

Reference machine: Ryzen 7 7800X3D, Radeon RX 7800 XT (OpenGL renderer confirmed
below). Starting acceptance target: 1000x1000 simulation, 500x500 gas,
fixed 1/60-second simulation steps; idle at least 60 FPS and combined workloads of
100,000 water plus 50,000 sand particles with smoke/fire at least 30 FPS on average,
95th-percentile complete GPU frame at most 50 ms. Keep the original 60 FPS combined
goal as the preferred result. Performance targets never excuse mass loss, disabled
physics, weakened error thresholds, or untested lower resolution. Confirm fixture
loads and record any target revision with measured evidence before accepting it.

Correctness gates: all runtime shaders compile; no OpenGL errors, NaN/Inf, unexpected
particle loss/creation or overflow; source-free water/sand mass drift at most 0.5%;
pressure projection measurably reduces divergence including obstacle-adjacent cells;
painted boundaries preserve their cell mask and block crossing; gas must ignite
before an extinguishing check can pass. Repeated scenes must start from the same
state and agree within a justified tolerance. Historical replay failures remain
recorded even when their fixture or metric is corrected.

### Phase 2 - Review algorithms and verify the final deliverable

- [x] Review every algorithm family: input/timestep, particle allocation/compaction,
  transfer kernels, integration/collision, pressure/free surfaces, gas advection and
  combustion, sand yield/spreading, boundary construction, tile scheduling,
  rendering, configuration, and packaging. Retain sound choices; replace erroneous
  or measured bottlenecks with the simplest correct alternative.
- [x] Pin changes to analytic GPU tests and representative scenes. Re-run the
  automated Phase 1 gates after the final algorithm changes, including a Release
  build, all six repeated scenes, real-time benchmark and package relocation.
- Native checks were initially waived; the follow-up evidence is bounded as above.
- [x] Update README, PLAN, and VALIDATION to describe implemented, verified behavior,
  exact commands, hardware/results, and genuine limitations. Obtain independent
  review and inspect the final diff against the preserved starting source.

Algorithm review and corrections run alongside Phase 1 where needed for correctness
or performance, with the maintainer's permission to reorder the work. Final algorithm
acceptance requires re-running the gates on the complete implementation.
No finite test set proves absence of every possible bug;
completion means the full reviewed scope and the non-waived acceptance gates pass.

### Baseline record

- Initial state: `main` at `4030c32`, 27 modified/deleted tracked paths and 40
  untracked paths; all pre-existing work preserved.
- Static inventory: 53 shader files, all referenced by `CreatePrograms`, no missing
  shader paths. No dedicated TODO file or source TODO markers were found.
- Existing `dist/win11` bundle dates from March 2026 and contains the old cellular
  shaders; it does not validate the current particle implementation.
- MSVC 19.44 and Windows SDK 10.0.26100 were found. Offline CMake configure exited
  1 (`Could not determine GLFW target name`): GLFW/GLAD source caches are absent.
  This initial failure was resolved after explicit user approval to download the
  pinned dependencies.
- Source audit has confirmed fixed-point water decode scaling, FLIP force ordering,
  solid-pressure operator mismatch, boundary signed-zero classification, replay
  erase/reset gaps, and tightly packed initialization uploads with default row
  alignment. GPU reproductions and fixes are pending.
- Read coverage: 67 project files (5 C++, 53 shaders, 9 docs/build/config), plus the
  packaged README and 27 historical shaders. Packaged shaders match tag `v0.3.0`;
  ZIP contents match the extracted package. Detailed audit evidence is under
  `build/goal-baseline/`.
- Prepared `tests/GpuTests.cpp` and six CTest entries: runtime shader compilation,
  fixed-point water transfer, boundary mask preservation, wall pressure projection,
  smoke dissipation, and nonfinite pressure detection. Tests use real hidden
  OpenGL contexts and existing dependencies. Their first GPU run is recorded below.
- Corrected numeric environment parsing in `src/Config.hpp` and integrated it
  into `PowderApp.cpp`. Missing variables preserve defaults; present malformed,
  empty, nonfinite, overflowing or underflowing values now fail with the setting
  name.
- Native parser evidence: original behavior failed 23 of 33 normal checks and
  2 of 3 explicit-empty checks (exit 1). The corrected parser passed 34 top-level
  checks plus 3 child-process empty-value checks, built with MSVC 19.44,
  `/std:c++20 /EHsc /W4 /WX` (build/test exit 0). Logs are under
  `build/goal-baseline/config-*.log`. CMake's Windows empty-variable launcher
  failed and was replaced by a tested native child-process fixture.
- Moved the unchanged `SimulationConfig` defaults and actual loader into
  `Config.hpp`, then tested domain validation before applying it. The previous
  loader failed 47 of 115 checks (exit 1); the corrected loader passed all 115
  checks plus 3 empty-value child checks, with `/W4 /WX` build/test exit 0.
  This covers all 30 numeric override mappings, tile sleep's 8-bit limit,
  timestep/repeat policy bounds, substep ordering, coefficients and tolerances.
  Signed forces and valid zero coefficients remain supported. Defaults were
  independently compared with the starting source and match exactly.
  Commands and hashes: `build/goal-baseline/semantic-validation.md`.
- These native checks do not prove safe GPU arithmetic for arbitrary extreme
  settings. The gas CFL consumer's buoyancy-based speed estimate and narrowing
  overflow remain open, along with runtime integration and all GPU gates.
- Sand's missing restoring stress needs a real constitutive correction. A bounded
  comparison of pressure/friction projection and APIC-MPM is recorded in
  `build/goal-baseline/deliverable-audit.md`. APIC-MPM with Hencky stress and
  Drucker-Prager plasticity is a candidate to test, not an implemented or measured
  result. Pile volume, compression, sliding, and stiffness/substep cost must pass
  before accepting it.

### First runtime baseline

- Approved dependency downloads completed: GLFW `3.4` at
  `7b6aead9fb88b3623e3b3725ebb42670cbe4c579`, GLAD `v0.1.36` at
  `1ecd45775d96f35170458e6b148eb0708967e402`. No versions were upgraded.
- Original snapshot and current source both built in Release. GLAD's packaged
  UTF-8 specification initially failed under Windows' default text encoding
  (`XMLSyntaxError`, build exit 1). `PYTHONUTF8=1` on the build process resolves it.
  Builds use packaged specifications (`GLAD_REPRODUCIBLE=ON`) and an isolated
  environment of the installed Python 3.12.10, without additional Python packages.
- Actual renderer: AMD Radeon RX 7800 XT, OpenGL 4.3 core
  `26.8.1.260810`, GLSL 4.60; 48 image units, 32 compute image uniforms,
  96 compute SSBO blocks/bindings, and 1024 workgroup invocations.
- Initial CTest: config and all-shader compilation passed; five GPU cases failed
  (CTest exit 8). Transfer returned `0.001953` instead of `8`; a single solid
  became 9 mask cells; wall projection left divergence `0.75`; smoke remained
  `0.799805` instead of `0.4`; NaN pressure raised no invalid counter.
- All six original replays, each with two repetitions, returned exit 0. This is
  evidence of weak existing acceptance checks, not evidence that physics works.
  Their per-pass timings omit tile scheduling/rendering and use uncontrolled
  particle loads, so they do not satisfy the performance targets above.
  Logs: `build/goal-baseline/replay-initial-*.log`, `ctest-initial.log`, and
  `build-*-utf8.log`. The original snapshot and failure evidence remain preserved.

### Current handoff

Still on the original dirty `main` at `4030c32`; no branches, commits or published
package changes. The source snapshot and compiled original baseline are preserved.
Desktop automation exposed missed brief input. `src/PowderApp.cpp` now handles brush/view
selection and Escape in the existing press callback, and installs GLFW sticky
mouse mode so a click survives until the next simulation tick. The focused
regression in `tests/AppLifecycleTests.cpp` fails before the fix (exit 1, brush key
49 lost), then passes. It also checks all mappings, event order, release/repeat
semantics and replay isolation. Independent review found no actionable defect.
Logs: `native-input-{red,green}.log`; original two files: `native-input-before/`.

The updated Release build passes 44/44 cases (exit 0, 22.36 seconds,
`native-input-integration-{build,ctest}.log`). All six replay scenes pass twice
(`native-input-replay-{water,gas,sand,boundary,leakage,combined}.log`). Native checks
cover brief clicks/keys, brushes/erase, width resizing and pointer alignment,
close/relaunch. Plain F1 conflicts with Lightshot; Ctrl+F1 reaches composite.
Some quiet debug views and the small smoke response limit visual conclusions;
automated nonzero-field render tests supplement them. Held repeat, DPI and native
stress remain unverified. Evidence: `native-input-ui-verification.json`.

The earlier benchmark passed at GPU/wall 31.566/31.724 ms, p95 44.481 ms
(`final-complete-realtime.log`). The fresh run and three predeclared consecutive
repeats now measure 29.71-29.83 wall FPS (33.520-33.661 ms), below the unchanged
30 FPS mean gate. Each exits 1 with `combined full-frame acceptance failed`.
GPU p95 remains below 50 ms; counters/mass drift are zero and physics iteration
signatures match. Current failures supersede the earlier timing pass for this
session. Evidence: `native-input-realtime.log` and
`native-input-realtime-repeat-{1,2,3}.log`; the cause has not been established.
Two same-session runs of the preserved pre-input half-storage executable also
miss at 29.43/29.81 FPS with matching physics signatures and 55 identical shaders
(`native-input-realtime-control-{1,2}.log`). This is a matching benchmark-physics
control, not the exact original final executable. Input handlers are outside this
benchmark's timed path. The evidence does not attribute the slowdown to the input
fix; it also does not establish the session/hardware cause.
The preferred 60 FPS combined target is also unmet.

The local candidate is `build/goal-baseline/PowderGame-win11-x64-candidate-20261002-native.zip`:
the input-fixed Release executable, 55 matching shaders, eight runtime DLLs,
licenses, corresponding first-party source and one SHA-256 manifest. Fresh Unicode
extraction checks every checksum and runs two water replays from an unrelated
working directory. The package notes disclose the performance miss. Evidence:
`candidate-native-archive-verification.json`, `candidate-native-extracted-replay.log`
and `final-native-checkpoint-sha256.csv`. Prior candidates, the older `dist/win11`
release, and all 67 original snapshot files are preserved. No source was deleted
during this goal. Existing dirty changes remain uncommitted.

The original gas replay failure (reaction 22.64) is preserved in
`final-replay-gas.log`. Actual field measurements show flame rising above and
spreading wider than the falling water, while coupling agrees with its independent
formula. Moving the source above the plume and covering its width fixes the fixture:
y164/radius24/33 frames, 236,676 particles versus the original 245,200. The causal
test uses actual replay geometry; final reaction is 0.00213 with water versus
139.73 without it. Ignition, overlap, quench formula, exact count/mass and zero
counters are required. The 2.5 extinction threshold and physics are unchanged.
`gas-quenching-green.log` and `final-replay-gas-quenching-green.log` pass; the latter
covers both actual replay repetitions. Earlier rejected geometry is retained.

The earlier shared checkpoint passes 39 cases (exit 0, 16.01 seconds,
`preconditioner-integration-build.log` and `preconditioner-integration-ctest.log`).
It includes all material/guard, fire/diffusion, tile coverage, wide transfer, replay
and package fixes below. Source, executables, shaders and 132 hashes are preserved
in `build/goal-baseline/preconditioner-checkpoint/`. The final 44-case run above
also covers the later pressure and coupling changes.

The earlier 25-test checkpoint remains preserved in `water-cg-integration-ctest.log`.
Water replay passed both repetitions
with identical aggregate signatures and no debug failures (`water-cg-replay.log`).
These results cover the pressure, gas decay, particle collision, timestep and replay
metric corrections below. Subsequent focused checkpoints are recorded here;
the final integration and real-time benchmark above supersede their pending status.
The original completion used the maintainer's native-check waiver; the later follow-up
and remaining limits are recorded above.

Verified corrections since the first five shader fixes:

- Weighted water Jacobi removes the closed-pocket checkerboard oscillation
  (residual 1 -> 0). Gas uses the matching solid-aware operator, explicit coarse
  spacing, blocked-donor interpolation and fresh coarse corrections each cycle.
  A real host projection fixture reduces wave RMS divergence 0.02947 -> 0.000978
  after 24 cycles; obstacle-adjacent maximum drops 0.18956 -> 0.007782. A two-chamber,
  one-cell-neck fixture also passes its per-chamber convergence checks. Conservative
  coarsening is still approximate; these fixtures do not prove every geometry.
- FLIP retains the raw pre-force grid. Gravity is applied consistently in divergence
  and projection; the host force fixture changes velocity by -1 as expected.
  Matching quadratic gathers and bilinear RK2 without a second affine correction
  pass analytic tests. Clamped edge samples retain full kernel support. Projection
  preserves transferred air-face velocities: free-surface velocity error fell from
  0.2493 to 0.00048. That fixture allows 0.002 for R16F rounding; the original
  overly strict zero-RHS failure is preserved in `surface-mass-neck-baseline.log`.
- Reset clears omitted water state, resets ping/frame state, rebuilds boundaries,
  initializes empty cells as air, and handles packed odd-width uploads. Scripted
  erase now follows the actual erase path. All application fixtures pass.
- Nonconservative reseeding created 91 particles from a stationary 50-particle patch
  in one step. Its pass, program and internal configuration fields were removed;
  the source-free mass fixture now passes. The unused shader source is retained;
  source deletion was not authorized and is unnecessary for runtime correctness.
  Original source remains in the baseline snapshot.
- Bounded water/sand spawn reservations pass eight capacity cases, including a
  near-wrap invalid cursor that formerly overwrote existing particles.
- Consumer-specific OpenGL memory barriers cover buffer clears/readback and texture
  readback. Validation rejects all 12 injected NaN/Inf scalar/reaction/leakage cases.
- Gas velocity damping moved after the MacCormack limiter, with elapsed-time scaling
  and buoyancy applied afterward. Seven failing GPU cases became ten passing cases,
  including retention 0/0.5/1 over different substep counts.
- Substep estimates use double arithmetic and reject requirements above their
  configured budget before integer narrowing. The initial arithmetic regression
  passed 117 native checks plus three empty-value child checks. Subsequent gas
  characteristic bounds and particle transport tests cover actual displacement;
  arbitrary extreme states still require the runtime numeric diagnostics.
- Sand now uses a matching quadratic gather, moves with its capped final velocity,
  and applies damping once per elapsed timestep. The focused GPU fixtures pass;
  cap displacement fell from 1.66609 to 0.0166664. A one-cell transfer halo preserves
  mass and affine moments at all domain edges. Swept particle collision passes 36
  cases, including thin walls, corners, long paths and domain edges.

Water pressure investigation: actual replay counts fell from 118,391 to 536 as the
iteration cap increased from 28 to 256, but all four runs failed. A fixed 64x32 pool
proved both slow convergence and half-float stagnation: R16F stopped changing from
4,096 through 16,384 iterations at maximum residual 0.34375 (812 cells above 0.12),
whereas the otherwise identical R32F solve reached 0.000397. A CPU SOR comparison
also failed the larger 160x160 pool after 512 sweeps. A GPU conjugate-gradient solve
with R32F pressure now passes the pool fixture in 59 iterations (maximum true
residual 0.059685) and the water replay. The subsequent combined-workload correction
uses a default cap of 1024, warm starts, early convergence exit and active liquid
bounds. Empty water takes four dispatches; the pressure threshold remains 0.12.

Replay validation itself had seven reproduced failures: allocation cursors counted
dead particles, count-only metrics hid changed particle mass, nonfinite particle
state was accepted, and a gas scene could pass without ignition. The correction
now scans actual particle state and requires ignition before extinction; its green
check passed in the 25-test checkpoint.

Full-frame benchmark: 1000x1000 rendering and simulation, 500x500 gas, 60 warmup
plus 240 measured frames, exactly 100,000 water and 50,000 sand particles, scripted
smoke/fire. Default-cap idle GPU/wall means were 11.45/12.91 ms; combined means
15.21/16.80 ms, GPU p95 18.95 ms. Sampled particle mass drift was zero, but pressure
reported 6,781,044 failed cells over the run (exit 1). Cap 128/256/512 runs also
failed and idle wall time worsened to 17.31/25.35/41.00 ms. All first failures occurred
at frame 97 while the final water substep reported a small recursive residual;
the attribution below resolves that misleading aggregate. These are failed benchmark
results, not an accepted 59.5 FPS claim. Evidence: `full-frame-benchmark-initial.log`
and `benchmark-water-caps.json`.

The first frame-97 pressure failure was gas: conservative coarse masks omitted a
thin fluid strip but treated it as a Neumann boundary, creating a spurious constant
correction mode. Mixed omitted cells now impose zero correction while physical
walls remain Neumann. Manufactured strips, waves and narrow-neck fixtures pass.
The larger water pool also needed the higher CG cap and active bounds. The resulting
single-tick benchmark passes: idle GPU/wall 8.213/8.408 ms; combined GPU/wall
17.839/18.071 ms, p95 35.377 ms, zero counters and zero sampled mass drift
(`benchmark-pressure-bounds-1024.log`). This predates the new sand material model.

One simulation tick per rendered frame measures throughput, not necessarily real
time. `benchmark-realtime` now measures two 1/60-second ticks per combined rendered
frame, matching the 30 FPS target, with the same mass/counter/p95 gates. Its first
material-model run failed speed: idle GPU/wall 8.622/8.793 ms; combined
61.114/61.337 ms (16.3 FPS), p95 80.331 ms. All counters and sampled mass drift were
zero. A diagnostic repeat measured 60.613/60.828 ms, p95 79.808; final-tick means
were water 19.781 ms, gas 6.826 ms and sand 3.014 ms. The last water solve averaged
193 CG iterations, maximum 587. Logs: `benchmark-material-realtime.log` and
`benchmark-material-realtime-profile.log`. The 30 FPS/50 ms p95 acceptance target
is unchanged. A 16x16 block-Jacobi preconditioner with 32 local weighted sweeps
now passes the frozen pressure system: cold GPU solve falls from about 31.0 to
12.4 ms; snapshot warm start falls from about 10.3 to 6.0 ms, with independently
computed maximum residual below 0.06. Its real-time scene measures GPU/wall
39.702/39.864 ms, p95 46.846 ms, zero counters and zero sampled mass drift
(`pressure-gpu-block32.log`, `pressure-block32-realtime.log`). The mean still
fails 30 FPS. Fusing partial dot-product reductions cuts each active iteration
from eight dispatches to six. Packed shared storage avoids the first fused
candidate's measured slowdown. The final fused 32-sweep candidate passes four
pressure checks and the frozen-system true residual gate; its real-time GPU/wall
means are 38.458/38.628 ms, p95 45.825 ms, with zero counters and mass drift
(`pressure-fused32-final-focused.log`, `pressure-fused32-realtime.log`). This
still fails mean performance (exit 1). The source checkpoint is `pressure-fused32/`.
Catch-up ticks retain earlier
tick failure counters; the real-time benchmark rejects non-default timestep overrides.

The gas review found 528 pressure dispatches per default tick, including empty
fields. Its historical residual counter threshold of 20 would accept the existing
manufactured wave/neck/strip inputs without any pressure solve; the independent
test oracles are substantially stricter. A frozen-state cycle/timing comparison
passed for zero, obstacle-wave, narrow-neck, thin-strip, sparse-fire and combined
states (`gas-characterization.log`). At four cycles the real source states have
stored residual maxima 0.00159/0.00332 and actual projected maxima 0.00391/0.00415.
The harder manufactured states need more work and show a half-precision floor.
The first minimal trial used a separate 16-byte counter buffer with the unchanged
residual kernel: check initially/every four cycles, stop only below maximum 0.004,
and retain the 24-cycle ceiling. The final default maximum gate tightened from
20 to 0.015; independent manufactured RMS checks remain unchanged. No additional
RMS reduction was introduced before measuring this strict stopping rule.
That first trial passes five focused checks, including unchanged manufactured
outputs, exact final counter counts, and zero/invalid RHS skipping all cycles.
However, the full real-time trial fails: idle improves to 2.72 ms, combined rises
to 40.38 ms (p95 49.68), and one gas residual exceeds the stricter final limit at
the first measured frame (`gas-early-stop-realtime.log`). The candidate is not
accepted. First-failure attribution must distinguish iteration error from the
R16F floor before choosing further changes; the frozen tick-120 results alone
were insufficient.
The captured failure is gas cell (249,69): GPU/CPU failing counts agree at 1,
maximum stored residual 0.0151978, RMS 0.0003160, maximum pressure 5.06641.
The RHS mean is approximately -4.09e-9. Frozen pressure/RHS/mask and pre/post
projection MAC fields are retained. On that exact system, R32F pressure reaches
residual maximum 0.003934 and actual projected maximum 0.004105 in eight cycles,
versus R16F maximum 0.015198 after 24. This is measured precision sensitivity,
not proof that additional half-precision iterations can never pass 0.015.
The R32F real-time candidate has zero counters/mass drift but still misses speed:
GPU/wall means 38.953/39.111 ms, p95 47.124 ms
(`gas-pressure-r32-realtime.log`). Test-only profiling finds 400 of 480 projections
consume all 24 cycles. One frozen slow state already has maximum/RMS residual
0.005001/0.000116 after four cycles; after 24 its maximum remains 0.004844, still
above the strict early-stop rule. The next measured trial used a maximum-plus-RMS
stopping rule; the final maximum and independent manufactured gates stayed intact.
That trial passes six focused checks and frozen-system cases. It reduces residual
statistics per 16x16 group, reads 16 KiB, and stops only when maximum is at most
min(configured threshold, 0.01) and RMS at most 0.001. Masked/padded lanes, exact
counts and nonfinite input are tested independently. The two-tick combined run
now passes: GPU/wall means 31.833/31.989 ms (31.41/31.26 FPS), GPU p95 44.972 ms,
zero counters and sampled mass drift (`gas-stats-realtime.log`). The exact source
and binary are preserved in `gas-stats-checkpoint/`. It includes the additional
dead-output cleanup below, but predates the remaining half-storage guards and
removal of a proven unused pre-projection center-velocity dispatch. The final
integrated run above now covers both changes and passes combined acceptance.
Upper-edge
transport interpolation reproduced small asymmetric errors in forward and corrected
scalar/U/V impulses. Six clamping-bound corrections pass paired lower/upper controls
and three affected CTests (`gas-endpoint-final-red.log`, `gas-endpoint-green.log`).
A 3x3 GPU fixture confirms that projection maps faces bounded by M to center
velocity (0.5M, 1.5M), exceeding the assumed sqrt(2)M while leaving zero divergence.
All four transport shaders now bound sampled characteristic components by the
configured maximum, preserving the physical projected MAC field. Eight signed
trace regressions failed before this correction and pass afterward; four GPU
and two host checks pass (`gas-cfl-green.log`, `gas-cfl-host-green.log`).

Water's corresponding top/right RK2 sampler error is also corrected: last-edge
impulses moved 0.4995 instead of 0.5; all four mirrored cases now move 0.5
(`water-edge-{red,green}.log`, 52 integration checks). Sand gravity +/-1e8
previously dispatched and silently saturated its half-float grid at 65504;
the force impulse is now rejected before dispatch with setting names, while
+/-6e7 controls remain valid (`sand-force-{red,green}.log`). This preflight bounds
forcing alone; the actual half-storage output checks below cover additive overflow.

The host review removed render-rate-dependent bracket polling in favor of native
key PRESS/REPEAT events, retaining radius limits 1..96. Public callback fixtures
pass; actual held-key OS interaction remains unverified under the maintainer's waiver. A real
relinked GPU program reproduced stale uniform locations after retirement (0
instead of 0.5, with a GL error). The cache now belongs to the app and entries are
erased on program deletion, preserving hot lookup caching.

Producer/consumer review removed five obsolete cellular textures totaling
14,000,000 logical bytes, dead material/smoke-pressure ping flags and unused
accessors. Velocity textures only held zero; the former stress field is derived
exactly from material == solid. A composite oracle covers empty/solid cells and
sand overlapping each; RGBA hash 5043923365591529381 is byte-identical on this GPU
before/after cleanup. Seven focused input/cache/reset/erase/boundary/upload/render
checks pass (`input-cache-green.log`, `state-simplification-baseline.log`). The final
package and 44-case integration checks also pass.

A second producer/consumer trace removed another 28,000,000 logical bytes: two
unused water center-velocity textures, their 16 MB cell momentum buffer, and an
unused sand raster velocity texture. The cell momentum atomics/clear/decode and
dead output stores are removed. Water MAC faces, render velocity, cell mass and
sand occupancy remain authoritative and retain their analytic oracles. The two
cleanup batches together remove 42 MB, independent of the earlier tile scheduler.

Actual-host near-limit additive cases demonstrate why gravity-only guards are
not complete: water computed approximately +/-65544 and sand +/-65680 but stored
+/-65504 with no diagnostic. Direct producer fixtures also reproduce this in MAC
decode, divergence, projection and sand grid update. Guards on the actual values
before half-float storage now diagnose these unsupported states. All 22 direct
signed endpoint, overflow and nonfinite cases pass, with representable controls
unchanged; the final 44-case suite includes this gate. Red evidence is retained in
`water-force-additive-final-red.log`, `sand-force-additive-red.log` and
`half-storage-red.log`.

Build/package staging now uses CMake's declared runtime DLL list and installed
MSVC release redistributables, including them beside the app and in relocated
packages. GLAD generation uses its pinned bundled specification by default.
Offline shared GLFW/GLAD configure/build and fresh ASCII/Unicode package checks
pass. Replaying the old exe/shader-only staging reproduces a missing-DLL failure
(`0xc0000135`, CMake exit 1); current staging passes (`shared-package-green.log`).
The package test uses fresh directories and executable-adjacent built shaders to
avoid stale libraries or source-tree files hiding omissions. Replay frames now pump window events before their timing interval;
scripted input remains protected by the replay callback guard.

Additional focused checkpoints:

- Water transfers honor stored particle mass and use signed low-word/carry momentum
  sums. Unequal masses and 262,144 same/opposing-velocity particles pass exact
  integer-sum and R16F-output checks, including exact face-word oracles in the
  39-test checkpoint.
- Gas transport clips traces and donors at walls, including diagonally isolated
  cells. Coupling retention scales with elapsed time, and fire brushes respect
  water and the actual gas mask. Direct transport/coupling/brush fixtures pass.
  Combustion now uses actual consumed fuel/oxidizer for all products; diffusion is
  no-flux at walls and sealed diagonal corners. A host bound keeps diffusion plus
  cooling stencil weights nonnegative or rejects the configured substep budget.
  The fire fixture passes 287 checks and its host substep/rejection fixture passes
  (`fire-green.log`); earlier 11 shader and three host failures remain recorded.
- Runtime shaders come only from the executable's adjacent `shaders` directory.
  ASCII/Greek standalone-package tests and shader-only incremental recopy pass;
  the strengthened error-message assertion passes in the 39-test checkpoint.
- Boundary masks are authoritative cell masks. Only the sand static SDF uses jump
  flooding, cached until actual geometry changes. Six boundary/lifecycle checks
  pass. GPU mean/p95 static-repeat cost falls from 0.751/0.969 to 0.025/0.025 ms;
  repaint falls from 0.654/0.655 to 0.435/0.436 ms (`boundary-green.log`).
- Sand now stores elastic deformation and rest area, transfers Hencky stress, and
  projects principal logarithmic strains onto a Drucker-Prager yield cone. Grid
  contact separates normal motion and applies Coulomb friction. All six sand
  fixtures pass, including stress/torque, near-singular strain, yielding, contact,
  dense momentum, transfer support and settling at two timesteps
  (`sand-material-final.log`). Alpha 0.5 was calibrated between measured 0.2/0.4/0.6
  pile slopes; it passes the original height gate at dt 0.001/0.0005 (2.641/2.718),
  RMS speeds 0.058/0.044 and negligible mass drift. Default material wave speed
  requires 18 substeps per 1/60-second tick. The original final checkpoint passed
  30 FPS with those substeps intact; the fresh timing shortfall is recorded above.
  The unused density-yield configuration was removed.
  Independent review found alpha-zero isotropic tension returning before tension
  release. Axis/rotated expansions now release while compression is preserved;
  the reproduced failure and passing material test are in `sand-alpha-zero-*.log`.
- The dense-versus-tiled coupling comparison reproduced missed cooling in weak
  tails and water spawned into
  sleeping regions. Positive water now wakes activity, scheduled after simulation;
  three scenarios match dense output exactly in the 39-test checkpoint. Follow-up
  timings show activity plus tiled coupling is slower than dense coupling even at
  zero/sparse load (0.480/0.486 ms versus 0.023/0.018 ms; dense 0.033 versus 0.018 ms),
  with identical fields after 64 steps. The runtime scheduler has been replaced
  by two direct coupling dispatches; six image resources, six buffers and six
  programs are removed. Shader source files remain preserved. Five affected
  CTests pass; direct numeric lifecycle checks cover weak tails, moved water and
  newly spawned water. Direct mean times are 0.0108/0.0114/0.0141 ms for idle,
  sparse and dense fields (`dense-coupling-tests.log`, `dense-coupling-benchmark.log`).
  The retired miss counter alone never established coverage.
- Offscreen rendering now passes actual extraction corner/constant checks and all
  ten render modes. Upper-edge interpolation formerly attenuated a unit corner
  to 0.997559; matching clamping preserves it. Reproduction and green logs:
  `render-modes-{red,green}.log`, `dense-render-build.log`. This does not replace
  native interactive control/DPI verification, which the maintainer subsequently waived.
- Replay regression tests reproduced six false passes: single-particle/count or
  mass changes, empty combined content, and water that never reaches the obstacle.
  Counts are now exact, mass comparison allows only summation roundoff, barriers
  span the domain with nearby sources, and actual particle contact/zero crossing
  is required. Whole-frame timestamps include all simulation passes and rendering. Focused replay
  metric/readback tests and both repetitions of all six actual scenes pass.

Interactive time now follows elapsed time with fixed simulation steps, carrying
fractional time and limiting stall catch-up to four steps. Clock tests cover
30/60/144/240 Hz and stalls; 118 native configuration/clock checks plus three
empty-value child checks pass. Cursor mapping uses logical window dimensions.
The original interactive visual/DPI checks were unverified at this checkpoint.
The desktop-automation helper launched the verified checkpoint but could not activate it;
window capture showed the locked Windows desktop. No input was sent to the lock
screen. The owned test process was stopped to avoid contaminating GPU measurements.
The maintainer subsequently waived the native interactive checks.

The original goal completed with the native-check waiver. The follow-up verified
brief native behavior and fixed input loss, but reopened the mean performance
finding. Next performance work should explain the current timing difference and
restore measurable margin below 33.333 ms without weakening correctness or load.
All red/green and failed timing evidence is retained under `build/goal-baseline/`.

---

The initial audit of the starting implementation used to follow here. It is in git history up to `0bcb329` (v0.7.0).

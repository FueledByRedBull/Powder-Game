# Powder Game Completion Roadmap

## Completed goal and native follow-up - 2026-10-02

This execution record supersedes the historical status claims below. Ponytail full
mode applies throughout. Existing changes on `main` are the starting source;
`build/goal-baseline/` preserves that source and its SHA-256 inventory. No commits,
branches, dependency upgrades, or publication are part of this goal.

The user explicitly instructed "Proceed without it" after the desktop lock blocked
native interactive verification. The original goal was completed under that waiver.
The subsequent "do computer use" request verified native behavior and exposed missed
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
- [x] Subsequent Computer Use: brief input, material brushes, erasing, diagnostic
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
or performance, using the user's permission to reorder the work. Final algorithm
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
Computer Use exposed missed brief input. `src/PowderApp.cpp` now handles brush/view
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
The original completion used the user's native-check waiver; the later follow-up
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
pass; actual held-key OS interaction remains unverified under the user waiver. A real
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
  native interactive control/DPI verification, which the user subsequently waived.
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
The Computer Use helper launched the verified checkpoint but could not activate it;
window capture showed the locked Windows desktop. No input was sent to the lock
screen. The owned test process was stopped to avoid contaminating GPU measurements.
The user subsequently instructed proceeding without native interactive checks.

The original goal completed with the native-check waiver. The follow-up verified
brief native behavior and fixed input loss, but reopened the mean performance
finding. Next performance work should explain the current timing difference and
restore measurable margin below 33.333 ms without weakening correctness or load.
All red/green and failed timing evidence is retained under `build/goal-baseline/`.

---

## Historical initial audit (superseded)

The remaining text is the preserved audit of the starting implementation. Its
status claims are historical; the completed goal and current handoff above supersede them.

This roadmap is based on a source audit of the current C++/OpenGL project state. The original plan is directionally good, but several parts are already partially implemented and a few important contracts are currently wrong or stale. The priority is to make the simulation correct and testable before tuning visuals or adding more features.

## Executive Summary

The project is a real GPU simulation prototype, not a stub. It already has:

- Fixed `1/60` simulation dt.
- A mostly centralized `SimulationConfig`.
- Boundary state textures for occupancy, SDFs, and masks.
- Particle water with MAC faces, pressure projection, FLIP blend, reseeding, and derived render fields.
- Half-resolution gas with MAC velocity, scalar fields, MacCormack-style advection, combustion, and a multigrid-like pressure solve.
- Particle/P2G sand with derived occupancy and velocity.
- Tile activity, tile sleep, compacted tile lists, GPU timing queries, replay scenes, and debug counters.

The project is not yet fully operational. The main blockers are:

1. The full-resolution boundary SDF/mask path appears broken: `sdf_full` is built from stale `mask_full` instead of fresh `solid_occupancy`.
2. Sand-to-boundary is one frame stale because boundary runs before the current sand rasterization.
3. Water is only an approximate particle/MAC liquid; it does not yet use `liquid_phi` for free-surface pressure classification.
4. Gas/fire is mostly unified, but several coupling and heat/smoke shaders are dead or folded into other passes.
5. Sand is particle/P2G, but not yet an MLS-MPM-lite dry granular model with volume, deformation/plasticity, or a real frictional yield solve.
6. Tile activity is built but only used by coupling; most heavy simulation passes still dispatch globally.
7. Replay validation exists, but it is not deterministic proof, not wired into CI, and misses transient debug counter failures.
8. Docs and shader inventory still describe old cellular sand and old split fire/heat/smoke paths.

## Current Architecture Assessment

### Frame Order

Current order in `src/PowderApp.cpp` is:

1. Spawn / brush
2. Boundary
3. Tile activity
4. Water
5. Gas
6. Sand
7. Coupling
8. Render extraction
9. Render

Target order should be:

1. Brush
2. Boundary
3. Water
4. Gas
5. Sand
6. Coupling
7. Render extraction
8. Render

The current order is close, but sand occupancy used by boundary is from the previous frame. That can be acceptable only if documented as a one-frame-late coupling decision, but the target says sand occupancy updates next frame boundary while also saying boundary includes dynamic sand rasterization. The implementation should make this contract explicit:

- Previous-frame `sand_occupancy` is the blocker for this frame's water/gas.
- Current sand pass rasterizes `sand_occupancy_next`.
- Next frame boundary consumes that rasterized field.

If same-frame sand blocking is required, sand rasterization or a light pre-boundary sand occupancy update must run before boundary.

### Authoritative Domains

The intended truth domains are sound:

- Full-resolution material/static solid + boundary.
- Full-resolution particle/MAC liquid.
- Half-resolution MAC gas/combustion.

Current code mostly follows this, but old material velocity/stress textures and old cellular sand shaders remain. They should be removed or quarantined once the particle paths are verified.

## Phase 0 - Make The Current Build Auditable

Goal: make every future change measurable.

Tasks:

- Restore local build validation. The audit shell could not run `cmake` because it was not on PATH. Install or expose CMake in the development shell, or document the exact Visual Studio Developer PowerShell command.
- Add a `VALIDATION.md` or README section with the exact commands for:
  - configure
  - build
  - run app
  - run replay scene
  - run all replay scenes
- Add a shader compile validation path. Today shaders compile only at runtime through OpenGL program creation. Options:
  - Add a headless smoke-test executable that initializes GL and creates all programs.
  - Add `glslangValidator` or equivalent if available, but keep OpenGL runtime compile as the authoritative check.
- Wire replay mode into CI where a GL context is available. If CI cannot provide GL, add a local-only documented replay gate and keep CI build-only.
- Add a validation result format that prints scene name, pass/fail metrics, debug counters, particle overflows, and timing aggregates.

Acceptance:

- `cmake --build ...` works from documented commands.
- Every shader used by `PowderApp` is compiled by a validation command.
- Replay failures print the metric that failed.
- Audit no longer depends on visual inspection alone.

## Phase 1 - Fix Boundary As The First Correctness Blocker

Goal: make solids and sand blockers authoritative and remove stale/self-referential boundary fields.

Current critical issue:

- `boundary_build.comp` writes fresh dynamic occupancy to `boundary_state_.solid_occupancy`.
- `RunBoundaryPass()` then builds `sdf_full` from `boundary_state_.mask_full`.
- `mask_full` is regenerated only after `sdf_full`.
- This makes the full boundary SDF/mask stale or self-derived.

Tasks:

- Change full SDF build input from `mask_full` to `solid_occupancy`.
- Preserve `static_occupancy -> sdf_static -> mask_static` for static-only collision where needed.
- Define boundary outputs clearly:
  - `static_occupancy`: painted solid only.
  - `solid_occupancy`: painted solid plus sand occupancy.
  - `sdf_static`: static-only SDF.
  - `sdf_full`: static plus sand SDF.
  - `mask_static`: static-only binary blocker.
  - `mask_full`: static plus sand binary blocker for liquid.
  - `sdf_gas` and `mask_gas`: downsampled static plus sand blockers for gas.
- Keep water and gas out of all boundary blocker fields.
- Verify `boundary_downsample.comp` reads the intended fresh full occupancy, not a stale mask.
- Add a boundary debug replay scene with:
  - thin wall
  - corner
  - sand pile blocker
  - water source above blocker
  - gas source near blocker

Acceptance:

- Painted solids block water in the first frame after painting.
- Sand blocks water/gas according to the documented one-frame or same-frame contract.
- Water and gas never become blockers in boundary textures.
- Thin wall and corner leakage stay under threshold.

## Phase 2 - Stabilize Water Into A Real APIC/FLIP Liquid

Goal: finish the water rewrite so `water_amount` and `water_velocity` are derived outputs from particle/MAC truth.

Already present:

- Water particles with position, velocity, mass, and affine slots.
- Safe bounded spawn/reseed allocator with `particleOverflowBlocked`.
- Particle compaction into scratch buffer.
- P2G with quadratic weights to MAC faces.
- MAC face velocity normalization.
- Pressure, divergence, projection.
- FLIP/PIC blend.
- RK2-style advection against SDF.
- `liquid_volume`, `liquid_phi`, `water_amount`, and render velocity outputs.

Main gaps:

- `liquid_phi` is derived but not used for pressure/free-surface classification.
- Pressure solves all non-solid cells instead of liquid cells with air/free-surface boundary conditions.
- APIC affine update is approximate: G2P replaces affine with sampled velocity gradient, not a full APIC transfer.
- Reseeding can create mass from smoothed volume if thresholds drift.
- Some spawn/clear paths still write derived `water_amount` / `water_velocity` directly.

Tasks:

- Define water particle layout in a shared comment or struct document:
  - `state0.xy`: position
  - `state0.zw`: velocity
  - `state1.x`: live flag
  - `state1.y`: mass
  - `state1.zw`, `state2.xy`: affine matrix
  - remaining slots reserved or removed
- Add explicit liquid cell classification from `liquid_volume` / `liquid_phi`.
- Update water divergence, pressure, and projection shaders to:
  - solve only in liquid cells
  - use solid SDF/mask for collision
  - use air/free-surface boundary where `liquid_phi` indicates non-liquid
- Make P2G/G2P APIC transfer mathematically consistent:
  - keep face weights and velocity sums
  - compute affine term from weighted face/grid velocity deltas, not only a sampled gradient shortcut
  - preserve FLIP blend default at `0.95`
- Make reseeding conservative:
  - target 3 particles per under-resolved wet cell
  - block overflow and increment `particleOverflowBlocked`
  - avoid reseeding from render-smoothed amount
  - add mass/particle-count metrics for baseline scenes
- Ensure `water_amount` and `water_velocity` are written only by extraction/finalization/clear paths that are explicitly derived-output maintenance.
- Remove or wire `water_particle_raster.comp`; do not leave dead water shader paths.

Acceptance:

- Water source monotonically fills a pool without pressure artifacts in empty air.
- Thin walls and corners do not leak beyond threshold.
- Baseline scenes do not hit particle overflow.
- Replay reports water particle count, amount sum, max velocity, pressure residual, and NaN/Inf count.

## Phase 3 - Finish Unified Gas, Fire, Heat, And Smoke

Goal: make gas/fire/heat one coherent half-resolution stack with no split proxy physics.

Already present:

- Half-resolution staggered gas `u/v`.
- Centered derived gas velocity.
- Smoke density, temperature, fuel, oxidizer, and reaction textures.
- MacCormack-style scalar advection with limiter.
- MacCormack-style velocity advection/correction.
- Combustion source terms in `fire_rd.comp`.
- Multigrid-like pressure solve.
- Fire render field derived from reaction and temperature.

Milestone C completed:

- Unified gas/fire/heat/smoke now runs through the canonical gas stack:
  1. Advect velocity and scalars with MacCormack.
  2. Add buoyancy from temperature and smoke.
  3. Solve gas pressure with configured V-cycle cap/tolerance.
  4. Project staggered gas velocity.
  5. Apply combustion, temperature diffusion/cooling, smoke yield, and reaction decay.
  6. Apply water suppression through canonical `liquid_volume`, reaction, fuel, oxidizer, and temperature fields.
- Fire remains render-derived from reaction rate and temperature. There is no separate fire-state simulation.
- Fire-to-smoke and fire-to-temperature are owned by `fire_rd.comp`; the old standalone gas/fire compatibility shaders were deleted:
  - `coupling_smoke.comp`
  - `heat_diffuse.comp`
  - `spawn_heat.comp`
  - `smoke_vorticity.comp`
  - `smoke_pressure_jacobi.comp`
- Oxidizer behavior is a closed brush-added system. There is no ambient oxidizer refill path yet, so sustained fire depends on explicitly supplied fuel and oxidizer.
- Gas collision is mask-only from the downsampled boundary field. `sdf_gas` remains available for a later soft-collision pass but is not part of the current gas solver contract.
- Gas pressure residual tolerance and iteration cap are explicit `SimulationConfig` values, not hidden literals.
- Replay accumulates debug counters across the full scripted scene, so gas pressure failures are not lost between frames.

Acceptance:

- `POWDER_REPLAY=gas` passes with zero accumulated debug counters.
- Smoke plume rises and dissipates without growing forever in the gas replay.
- Fire burns while brush-supplied fuel/oxidizer are present.
- Water suppression extinguishes reaction within the fixed replay window.
- Fire rendering changes only through reaction and temperature.
- No unused gas/fire compatibility shader remains in the shipped shader directory.

## Phase 4 - Replace Sand Approximation With MLS-MPM-Lite Dry Granular Sand

Goal: finish sand as direct particle/grid dry granular simulation and remove old cellular compatibility paths.

Already present:

- Particle sand spawn/clear/P2G/grid update/particle step/rasterize.
- Particle state has position, velocity, live flag, mass, and affine slots.
- Full-resolution grid mass and momentum.
- Derived `sand_occupancy` and `sand_velocity`.
- Basic gravity, damping, SDF projection, friction-like velocity adjustment, and water drag.

Milestone D completed:

- Sand now uses the direct particle/grid path as the authoritative simulation path.
- Sand particles are compacted each sand step, and `spawn_cursor` is reset to the compacted live count so erased/inactive slots are reused.
- Sand source spawning is density-aware through `sand_occupancy`, preventing baseline replay sources from injecting into already filled cells until particle capacity is hit.
- The sand particle layout is documented by shader state:
  - `state0.xy`: position
  - `state0.zw`: velocity
  - `state1.x`: live flag
  - `state1.y`: mass
  - `state1.zw` and `state2.xy`: affine velocity term
  - `state2.z`: reserved particle volume slot
- Water-to-sand coupling is canonical in `sand_grid_update.comp`, using `liquid_volume` and water velocity during the sand grid update. The redundant post-sand coupling pass was removed.
- Sand self-interaction is grid-density based: compression above `sand_density_yield_threshold` applies a Coulomb/Drucker-Prager-lite velocity yield clamp. Static collision still uses `sdf_static`.
- `sand_occupancy` and `sand_velocity` are rasterized after particle advection for next-frame boundary and rendering.
- Replay sand mass is validated from compacted live particle count rather than filtered render occupancy.
- Old cellular sand shaders and redundant coupling shader were deleted:
  - `coupling_sand_drag.comp`
  - `sand_prepare.comp`
  - `sand_desired.comp`
  - `sand_resolve.comp`
  - `sand_commit.comp`
  - `sand_capture.comp`
- `stress_a_` / `stress_b_` remain as full-res solid/material visual state for existing render paths, not as sand simulation state.

Acceptance:

- `POWDER_REPLAY=sand` passes with zero accumulated debug counters.
- `POWDER_REPLAY=combined` passes with zero accumulated debug counters.
- `POWDER_REPLAY=all` passes.
- Sand mass is conserved within `0.5%` in baseline scenes.
- Particle overflow is not hit in baseline scenes.
- Old cellular sand shaders are gone from the shipped shader directory.

## Phase 5 - Coupling Cleanup

Goal: make cross-material effects explicit, field-based, and non-circular.

Milestone E completed:

- Coupling contracts are explicit in `RunCouplingPass`:
  - water -> reaction/fuel/oxidizer/temperature: `liquid_volume` suppresses canonical gas fields.
  - fire -> smoke/temperature: owned by `RunFireHeatPass` / `fire_rd.comp`, sourced from `reaction_rate`.
  - water -> sand: owned by `RunSandPass` / `sand_grid_update.comp`, sourced from `liquid_volume` and water velocity.
  - sand -> boundary: rasterized `sand_occupancy` feeds the next frame boundary build.
- No physics pass binds render-extracted textures as inputs.
- Redundant water-sand coupling was removed; there is one canonical water-to-sand path.
- The combined replay scene exercises water, gas/fire, sand, and coupling together.

Acceptance:

- `RunCouplingPass` documents source/output fields.
- `POWDER_REPLAY=combined` passes.
- `inactive_tile_misses` remains zero in replay acceptance.

## Phase 6 - Tile Activity And Performance

Goal: make active tiles an actual performance feature, not just a coupling optimization.

Already present:

- Full and gas tile activity textures.
- Tile sleep after 8 inactive frames.
- Tile dilation/downsample.
- Compacted tile lists and indirect dispatch buffers.
- `inactiveTileMisses` counter.

Milestone F completed:

- Active tile lists remain enabled where safe today: coupling runs through compacted tile lists and indirect dispatch.
- Particle compaction is now present for water and sand, and replay signatures include active particle counts.
- Timing query readback is replay-only. Interactive mode does not block on query or texture readback.
- Replay timing is accumulated per frame and reports per-pass average and max cost instead of last-frame samples.
- Pressure iteration/cycle caps and thresholds are exposed through `SimulationConfig` and environment overrides.
- Tile-list expansion decisions are documented:
  - coupling: safe and enabled.
  - particle loops: require particle bins beyond current compaction.
  - render extraction: not enabled because inactive tiles would need decay/clear semantics to avoid stale render outputs.
  - pressure solves: not enabled without active-domain boundary design.

Acceptance:

- Combined replay uses average total pass time <= `combined_target_frame_ms` and worst pass <= `combined_worst_frame_ms`.
- Replay output prints per-pass average/max timing.
- Active tile misses stay at zero in acceptance scenes.

## Phase 7 - Debug Counters And Validation

Goal: make failures visible and persistent enough to catch.

Required counters:

- `nan_inf_detected`
- `particle_overflow_blocked`
- `pressure_nonconverged`
- `inactive_tile_misses`

Milestone G completed:

- Debug counters remain per-frame for debug rendering and are accumulated across replay frames for pass/fail validation.
- Replay failure output prints named counters and scene metrics.
- Water and gas pressure convergence checks use configurable thresholds.
- Additional NaN/Inf checks were added to gas correction/combustion/project and sand grid/particle/raster paths.
- Replay runs each requested scene twice by default in one process.
- Determinism is tolerance-based because GPU atomic ordering is not bit-exact. The signature compares:
  - derived water amount sum
  - liquid volume sum
  - smoke density sum
  - temperature sum
  - reaction sum
  - sand occupancy sum
  - water and sand live particle counts

Acceptance:

- Any transient overflow/nonconvergence/NaN in replay fails the scene.
- Failure output identifies the failed counter and scene.
- Deterministic replay uses documented tolerance metrics from `SimulationConfig`.

## Phase 8 - Public Configuration And Internal Contracts

Goal: make solver behavior configurable without hidden constants scattered through shaders and C++.

Milestone H completed:

- `SimulationConfig` remains the source of solver defaults.
- `LoadConfigFromEnvironment()` exposes controlled environment overrides for:
  - fixed dt
  - tile sleep
  - water/gas CFL and substep ranges
  - APIC/FLIP blend
  - water/gas pressure tolerances and iteration caps
  - gas combustion, cooling, diffusion, and buoyancy
  - sand drag, velocity cap, spawn density, and yield/friction
  - replay repeat count and determinism tolerances
  - combined replay timing thresholds
- Ambiguous accessors were replaced:
  - `CurrentFire()` -> `CurrentReactionRate()`
  - `CurrentHeat()` -> `CurrentTemperature()`
  - `CurrentSmokeVel()` -> `CurrentGasVelocityCenter()`
- Texture/field contracts:
  - boundary: `static_occupancy`, `solid_occupancy`, `sdf_static`, `sdf_full`, `sdf_gas`, and masks.
  - water: particles and MAC faces are authoritative; `liquid_volume` / `liquid_phi` classify liquid; `water_amount` / `water_velocity` are derived outputs.
  - gas: MAC velocity and pressure plus smoke, temperature, fuel, oxidizer, and reaction are authoritative; render gas/smoke/temp/fire fields are derived outputs.
  - sand: particles and grid mass/momentum are authoritative; `sand_occupancy` and `sand_velocity` are derived outputs.

Acceptance:

- Solver-critical thresholds/caps touched by Phases 5-8 have matching config values or named constants.
- Debug views and code accessors use canonical field names.
- Field contracts are documented above and in code names.

## Phase 9 - Documentation And Cleanup

Goal: remove stale descriptions and make the repo match the implementation.

Tasks:

- Replace `PLAN.md` with either:
  - historical plan plus status notes, or
  - this roadmap as the current canonical plan.
- Update README:
  - current architecture
  - current controls
  - actual frame pipeline
  - replay/validation commands
  - build commands for Windows and Linux
- Remove stale statements:
  - sand prepare/desired/resolve/commit as active path
  - active tile culling as if it applies to all heavy work
  - split smoke/fire/heat if unified stack is canonical
- Remove unused shaders after each subsystem is validated.
- Keep `dist/` and build directories ignored; do not commit generated outputs unless release packaging requires them.

Acceptance:

- README pipeline matches `RunFrame`.
- Shader directory contains only shipped shaders plus clearly named experimental files.
- CI/build docs match the actual developer environment.

## Suggested Milestones

### Milestone A - Boundary And Validation Baseline

- Fix `sdf_full` source.
- Add clear validation commands.
- Improve replay failure output.
- Add accumulated debug counters.

Exit criteria:

- Boundary debug scene passes.
- Water hits painted solids correctly.
- Replay failures are actionable.

### Milestone B - Water Correctness

- Add liquid cell/free-surface pressure classification.
- Finish APIC/G2P transfer.
- Make reseeding conservative.
- Remove dead water shader path.

Exit criteria:

- Water jet, pool, spill, source, and thin-wall scenes pass.
- No baseline particle overflow.

### Milestone C - Gas/Fire Unification

- Clean reaction/temperature/smoke contracts.
- Wire or delete unused gas/coupling shaders.
- Decide oxidizer model.
- Improve suppression and smoke metrics.

Exit criteria:

- Smoke plume, sustained fire, and water suppression scenes pass.
- Fire remains render-derived.

### Milestone D - Sand Completion

- Add sand particle compaction/reuse.
- Add volume and granular yield response.
- Consolidate water-sand coupling.
- Remove old cellular sand shaders/docs.

Exit criteria:

- Sand pile/repose/erosion scenes pass.
- Sand mass conserved within `0.5%`.

### Milestone E - Performance And Shipping Readiness

- Expand active-tile use where safe.
- Add timing aggregates.
- Wire replay into CI or documented local gate.
- Update README/PLAN.

Exit criteria:

- Combined scene meets target FPS range on reference hardware.
- CI/local validation catches shader and replay regressions.
- Documentation matches reality.

## Known High-Risk Files

- `src/PowderApp.cpp`: owns frame order, resource binding, replay, timing, and most lifecycle contracts.
- `src/PowderApp.hpp`: owns config/state names and particle/texture contracts.
- `shaders/boundary_*.comp`: highest immediate correctness risk due to stale full boundary path.
- `shaders/water_*.comp`: needs free-surface pressure, true APIC transfer, and conservative reseeding.
- `shaders/smoke_*.comp`, `shaders/gas_*.comp`, `shaders/fire_rd.comp`: mostly implemented but needs contract cleanup.
- `shaders/sand_particle_*.comp`, `shaders/sand_grid_update.comp`, `shaders/sand_rasterize.comp`: particle path is active but not yet full MLS-MPM-lite.
- `shaders/coupling_*.comp`: coupling exists but should be consolidated and audited for persistent writes.
- `README.md` and `PLAN.md`: stale relative to current implementation.

## Validation Gap From This Audit

I could not run the local C++ build because `cmake` was not available on PATH in the audit shell. Replay was also not run because it requires a working build and OpenGL runtime context. Treat this roadmap as a static source audit until those validation commands are restored and executed.

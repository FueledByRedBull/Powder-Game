# Powder Game Architecture and Plan

The work and evidence ledger is `ROADMAP.md`; commands and measured results
are in `VALIDATION.md`. This file describes the current architecture. Historical
cellular sand milestones have been superseded by the particle implementation.

## Goals and constraints

- 1000x1000 simulation and rendering; 500x500 gas.
- Sand, water, painted solids, smoke, fire, and temperature coupling.
- C++20 and OpenGL 4.3 compute, subject to the resource limits in README.
- GPU simulation. Small CPU pressure-convergence readbacks trade synchronization
  for avoiding hundreds of unnecessary dispatches. Full-state readback is for tests.
- Default fixed tick 1/60 second, elapsed-time accumulation, four-tick stall cap.
- Reference acceptance: 60 FPS idle; 30 FPS with 100,000 water and 50,000 sand
  particles plus smoke/fire, GPU frame p95 at most 50 ms. The preferred goal is
  60 FPS combined. The earlier RX 7800 XT checkpoint reached 31.5 FPS; the native
  input follow-up measures 29.71-29.83 FPS and misses the unchanged mean gate.
  GPU p95 remains below 50 ms. Two ticks run per rendered frame; display latency is excluded.

## State and ownership

- Painted material is an R16UI cell grid. Sand and water are authoritative particle
  SSBOs, each capped at 262,144 live particles; bounded reservations prevent wrap.
- Material has one texture. Removed cellular velocity/stress and spare material
  fields saved 14 MB; equivalent solid shading is derived from the material value.
  Removing unused water/sand center outputs and water cell momentum saves another
  28 MB of logical storage plus the accumulation and stores that fed those outputs.
- Water records contain position, velocity, active flag, mass and affine velocity
  in three vec4s (48 bytes). No particle reseeding creates mass from render fields.
- Sand adds rest area and elastic deformation in four vec4s (64 bytes). The grid
  transfer has a one-cell halo, retaining complete quadratic support at domain edges.
- Integer particle transfers accumulate weights and signed momentum; low-word/carry
  pairs prevent dense momentum from overflowing 32-bit sums.
- Water faces and most derived/gas fields use half precision. Water/gas pressure
  and water CG vectors use single precision because half precision stalled measured
  solves. Live particle-grid velocity producers diagnose nonfinite or unrepresentable
  half-float outputs through counter zero and neutralize only those failed values.
- Full, static and gas masks preserve cell topology. Only static sand collision
  needs a signed distance field, rebuilt by jump flooding when geometry changes.
- Gas stores staggered velocity, smoke, fuel, oxidizer, temperature and reaction in
  ping-pong textures. Scalar combustion fields are bounded dimensionless quantities.

## Water

Mass-weighted quadratic particle-to-grid transfer uses wide fixed-point momentum
sums; blocked or out-of-domain support is omitted. The raw
pre-force velocity remains available for FLIP; gravity enters the projected grid.
The pressure operator uses matching divergence/gradient coefficients, thresholded
free-surface liquid classification and solid Neumann boundaries. GPU conjugate gradient uses an
R32F warm start, active liquid bounds, a 16x16 block-Jacobi preconditioner,
early convergence checks and a true final residual check. The preconditioner
uses 32 fixed local weighted sweeps; fused partial reductions keep each global
iteration to six dispatches. The default ceiling is 1024 iterations, with a
true residual threshold of 0.12.

Particle velocity blends FLIP/PIC and recovers affine moments from the quadratic
kernel. Both samples of RK2 transport obey the velocity cap. Damping scales with
elapsed time. Swept cell traversal blocks thin walls, corners and domain exits;
blocked normal velocity and affine rows are removed. Substeps enforce the configured
CFL budget and reject impossible settings before simulation dispatch.

## Sand

Particles carry elastic deformation and rest area. Quadratic APIC transfers feed
Hencky elastic stress into grid momentum. Separating grid contact and Coulomb wall
friction act before grid-to-particle transfer. The velocity gradient advances the
elastic deformation, then principal logarithmic strains undergo Drucker-Prager
plastic projection. Swept collision remains a geometric crossing guard.

The implemented material uses a fixed yield cone, without hardening. Reference
density is 2, shear modulus and first Lame parameter are 12000, friction alpha is
0.5, and wall friction is 0.62. These are simulation units. Alpha was calibrated
against measured pile support; this is not a validated physical sand material.
Wave speed plus the particle velocity cap determines stable material substeps
(default 18 per tick). Insufficient budgets are rejected before dispatch.

## Gas and coupling

Forward/reverse semi-Lagrangian transport uses a limited MacCormack correction.
Traces stop at walls, bilinear donors must be reachable, and the limiter uses those
same donors. Source and forward trace velocities obey the configured component
cap; projection can exceed that cap without changing its divergence correction.
Dissipation occurs after limiting and scales with elapsed time.
Buoyancy precedes a solid-aware three-level multigrid pressure projection. Coarse
cells omitted by mixed solid/fluid footprints impose zero correction; true physical
walls remain Neumann. This distinction prevents an artificial coarse null mode.
Pressure uses R32F storage: a captured evolving scene exceeded the tightened
0.015 residual gate with half precision and passed with single precision. The
24-cycle ceiling remains. Checks initially and every four cycles stop when the
stored-operator residual maximum is at most min(configured threshold, 0.01) and
RMS at most 0.001. An independent final maximum diagnostic always runs.
Performance results and the current mean-frame-time shortfall are in VALIDATION.

Combustion, temperature diffusion and dissipation evolve after transport/projection.
Reaction products use actual available fuel/oxidizer consumption. Temperature
diffusion respects sealed corners and no-flux walls; its explicit timestep keeps
nonnegative stencil weights or rejects an insufficient substep budget. Water suppresses heat/reaction with elapsed-time
retention. Sand receives grid drag from water, and its rasterized occupancy blocks
water/gas on the next tick. Rendering fields are derived, not authoritative mass.

## Scheduling and rendering

Each tick runs spawn/erase, boundary masks, water, gas, sand, coupling and render
extraction. Heavy solver work remains full-grid where globally coupled. Coupling
runs directly over the gas grid: measured tile scheduling cost more than the work
it skipped in idle, sparse and dense scenes, with identical resulting fields.
The composite pass runs once per displayed frame.
Bracket sizing uses native key press/repeat events rather than rendered-frame count.
Brush/view selection and Escape use press callbacks, preserving short taps and
event order. GLFW sticky mouse buttons retain a brief click until a simulation tick
consumes it. Painting samples the current cursor; it does not reconstruct a path
between samples.
Uniform-location caches belong to the app and expire on program deletion.
Unused cell-velocity outputs and their momentum accumulation are removed; FLIP,
transport, drag and rendering consume the retained MAC-derived fields.
Consumer-specific OpenGL barriers cover image, storage-buffer,
texture sampling and CPU readback boundaries.

## Verification and limitations

Analytic GPU fixtures cover transfer moments, dense sums, pressure, walls, material
stress/yield, integration, chemistry and lifecycle behavior. Replays require actual
matter, conserved particle mass, ignition before extinction, obstacle contact and
zero crossing. Count/mass repeat checks are separate from visual-field tolerance.
GPU timestamps measure complete frames, including all simulation and rendering.

The 44-test suite, repeated scenes and standalone shader-package checks pass.
The follow-up benchmark narrowly misses 30 FPS; its correctness gates pass.
Computer Use exercised brief input, brushes, erasing, diagnostic shortcuts,
width resizing, close and relaunch after the original interactive waiver.
Held-key timing, DPI transitions and sustained native stress remain unverified.
Linux and MinGW packaging are build targets but have not been executed in
this Windows validation environment. No release is published by this goal.

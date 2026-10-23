# Powder Game algorithm reference

This document explains how every simulated element and every supporting system works, why it works that way, which
constants drive it and where it breaks. It describes the source in this tree (`src/`, `shaders/`); when in doubt the
code is authoritative. Architecture notes live in [PLAN.md](../PLAN.md), commands and measured results in
[VALIDATION.md](../VALIDATION.md). Measurements quoted here come from VALIDATION (AMD Radeon RX 7800 XT, Windows 11,
driver 26.6.2) and name the fixture or benchmark that produced them.

Contents

1. Overview: scale, grids, data, tick order
2. Scheduling: clock, tick order, readbacks, failure counters, preflight
3. Boundaries: rock, masks, signed distance, erase
4. Water
5. Oil
6. Sand
7. Sand-water coupling
8. Gas: smoke, fire, temperature
9. Wood
10. Steam, evaporation, flash and sink accounting
11. Rendering
12. Input and brushes
13. References

---

## 1. Overview

### 1.1 Scale and units

One simulation cell is 5 mm. The domain is 1000 x 1000 cells (5 m x 5 m), so a fall across the whole height takes
$`\sqrt{2 \times 5 / 9.81} = 1.01\,\mathrm{s}`$ and ends at 9.9 m/s (1981 cells/s). All rates are in cells and seconds
at that scale:

- $`g = 9.81\,\mathrm{m/s^2} = 1962\,\mathrm{cells/s^2}`$ for water and sand.
- The gas runs on a 500 x 500 grid of 1 cm gas cells; gas velocities and gas rates are in gas cells per second
  (1 gas cell/s = 1 cm/s).
- The default tick is 1/60 s (`SimulationConfig::fixed_dt`, `POWDER_FIXED_DT`, accepted range 1/240 to 1/15 s).
- Water mass unit: one particle has mass 1; a cell at rest holds 4 (`water_rest_density`). Sand mass unit: one grain
  has mass 1; the reference density is 2 grains per cell (`sand_reference_density`), so occupancy 1 is a cell at
  rest density.
- Gas scalars (smoke, temperature, fuel, oxidizer, flame) are bounded dimensionless values in $`[0, 1]`$. Temperature 1
  is a flame roughly 900 K above ambient; oxidizer 1 is fresh air.

Coordinates have the origin at the bottom-left, y up. Cell $`(i, j)`$ covers $`[i, i+1) \times [j, j+1)`$; its centre is
$`(i + 0.5,\ j + 0.5)`$. Particle positions live in $`[0, N)`$ on each axis.

**Notation.** The formulas below use these symbols throughout:

- $`\Delta t`$ is the step of the pass in question (a tick, a gas step or a substep); $`\mathbf{g}`$ is the gravity
  vector and $`g = \lVert\mathbf{g}\rVert`$.
- $`\mathbf{x}_p, \mathbf{v}_p`$ are a particle's position and velocity, $`\mathbf{C}_p`$ its APIC affine matrix;
  $`\mathbf{x}_f`$ is a MAC face position, $`\mathbf{x}_i`$ a sand grid node; $`w`$ is a transfer weight (quadratic
  B-spline $`N`$ unless stated otherwise).
- $`\mathbf{u} = (u, v)`$ is a grid velocity, $`p`$ a pressure, $`b`$ a pressure right-hand side, $`r`$ a residual,
  $`\kappa`$ the porous sand-face conductance `kSandLeak`.
- $`\phi`$ is the water cell field `phi` (a level-set-like fill indicator); $`\phi_s`$ and $`\phi_w`$ are sand and
  water volume fractions; $`\psi`$ is a transported gas scalar; $`T`$ is the gas temperature.
- Sand: $`\mathbf{F}`$ is the elastic deformation gradient, $`J = \det\mathbf{F}`$, $`\boldsymbol{\epsilon}`$ the
  Hencky (log) strain with principal values $`\epsilon_1, \epsilon_2`$, $`\boldsymbol{\tau}`$ the Kirchhoff stress,
  $`\mu`$ and $`\lambda`$ the Lame moduli, $`\eta`$ the viscosity, $`\rho`$ the reference density, $`\alpha`$ the
  Drucker-Prager friction slope, $`\varphi`$ a friction angle and $`q`$ the hardening state. The friction
  coefficients of the $`\mu(I)`$ rheology carry subscripts ($`\mu_s`$, $`\mu_2`$).
- $`\mathrm{clamp}(x)`$ with one argument clamps to $`[0, 1]`$; $`\mathrm{mix}(a, b, t) = a + (b - a)\,t`$
  and $`\mathrm{smoothstep}(e_0, e_1, x)`$ are the GLSL functions.

### 1.2 Grids and staggering

| Grid | Size | Contents |
| --- | --- | --- |
| Material | 1000 x 1000 R16UI | 0 empty, 2 rock, 3 intact wood, $`256 + f`$ burning wood, $`512 + f`$ charred wood |
| Water MAC | u 1001 x 1000, v 1000 x 1001, R16F | face velocities: current (projected) and previous (pre-force) |
| Water cell fields | 1000 x 1000 | liquid volume, phi, amount (R16F); cell velocity (RG16F); pressure (R32F); divergence (R16F) |
| Water packed | 1000 x 1000 R8UI + RG16F | calm-water particle counts (bits 0-6), wake flag (bit 7); wake velocity |
| Sand grid | 1002 x 1002 nodes | nodes at cell centres plus a one-node halo; mass (u32), momentum (2 x i32 pairs), moisture (u32); velocity RG32F |
| Sand cell fields | 1000 x 1000 | occupancy R16F, moisture R8, packed count R8UI, packed means RGBA32F |
| Gas MAC | u 501 x 500, v 500 x 501, R32F | face velocities, ping-pong plus a scratch copy |
| Gas cell fields | 500 x 500 | smoke, temperature, fuel, oxidizer (R16F), reaction RG16F (x burn rate, y flame), steam R16F, pressure R32F, divergence R32F |
| Boundary masks | full 1000 x 1000 R8UI, static R8UI, gas 500 x 500 R8UI | topology for every solver (section 3) |
| Signed distance | 1000 x 1000 R16F | distance to painted solids (sand contact) |
| Render fields | 1000 x 1000 R16F/RG16F | smoke, temperature, emissive fire, gas velocity upsampled for drawing |
| Light | 250 x 250 RGBA16F, 8 mips | emitted light of the fire (composite only) |

Velocities are staggered (MAC grid, Harlow and Welch 1965): $`u_{i,j}`$ sits at $`(i,\ j + 0.5)`$, $`v_{i,j}`$ at
$`(i + 0.5,\ j)`$. Pressure and scalars sit at cell centres. The sand grid is collocated: one node per cell centre.

### 1.3 What is authoritative

Authoritative state (conserved, checked by tests):

- Water and oil: the particle records (`WaterState::particles`, 48 bytes each) plus the packed counts in
  `water_packed`. Moving plus packed is an exact integer invariant; evaporation and burning are explicit, counted
  sinks.
- Sand: the grain records (`SandState::particles`, 64 bytes each) plus the packed counts in `sand_packed`. Moving
  plus packed is exact.
- Painted material (rock, wood) in the material texture.
- Gas: the face velocities and the five scalar fields.

Derived each tick and never fed back as mass: liquid volume, phi, water amount and cell velocity (from particles),
sand occupancy and moisture (from grains), boundary masks and signed distance (from material and occupancy), the
oil field, render fields and the light texture. Rendering reads but never writes simulation state.

### 1.4 Per-tick pass order

`PowderApp::RunFrame(dt)` runs one tick:

| # | Pass (host function) | Main shaders | Reads | Writes |
| --- | --- | --- | --- | --- |
| 1 | Spawn and erase (`RunSpawnPass` -> `StampStroke` -> `StampBrush`) | `spawn_full`, `spawn_smoke`, `sand_particle_spawn`, `water_particle_spawn`, `*_particle_clear`, `gas_face_to_center` | cursor, masks, last liquid volume and occupancy | material, particles, gas fields |
| 2 | Boundary (`RunBoundaryPass`) | `boundary_build`, `boundary_seed`, `boundary_jumpflood`, `boundary_distance` (only when geometry changed), `boundary_downsample` | material, last sand occupancy, last liquid volume and oil field | full, static and gas masks, signed distance |
| 3 | Water (`RunWaterPass`) | `water_sleep`, `water_pack`, `water_particle_compact`, then per substep `water_particle_p2g`, `water_mac_build`, `water_particle_finalize`, `water_divergence`, `pressure_pockets`, `pressure_cg`, `pressure_cg_reduce`, `pressure_mg`, `pressure_residual`, `water_project`, `water_mac_finalize`, `water_particle_step` | masks, last sand grid velocity and occupancy, oil field | water records and fields |
| 4 | Gas, every tick (`RunGasTick` -> `RunGasPass` and `RunGasProjectionStage`) | one substep: `gas_velocity_advect`, `gas_face_to_center`, `smoke_advect`, `smoke_correct`, `gas_velocity_correct`, then `fire_rd`; the pressure solve spans two ticks: the first begins it after its substep (`smoke_divergence`, `gas_pressure_kind`, `pressure_pockets`, `pressure_residual`, `gas_cg` with `gas_pressure_relax` and `gas_pressure_restrict` for about half of the iterations), the second finishes it before its substep (the remaining `gas_cg`, `pressure_residual`, `smoke_project`, `gas_face_to_center`) | gas mask (a copy, for the solve) | gas fields |
| 5 | Sand (`RunSandPass`) | `sand_sleep`, `sand_pack`, `sand_particle_compact`, per substep `sand_particle_p2g`, `sand_grid_update`, `sand_particle_step`, then `sand_particle_p2g` (occupancy) and `sand_rasterize` | signed distance, this tick's liquid volume and water velocity | sand records and fields |
| 6 | Coupling (`RunCouplingPass`) | `coupling_fire`, `coupling_heat`, `wood_burn` (if wood was painted), `phase_particles`, `oil_field`, `steam_update` (if oil or a heat source was painted) | liquid volume, oil field, occupancy, gas fields | gas fields, material, water records, steam |
| 7 | Render extraction and sparks (`RunRenderFieldExtractionPass`, `RunSparkPass`) | `render_extract`, `spark_pick`, `spark_step` | gas fields, boundary mask, water amount | render fields, spark ring (11.4) |

Once per displayed frame `Render()` runs `light_build` (lit composite only), `glGenerateMipmap` and the
`composite.frag` draw, then `DrawSparks()` over it (lit frames only, `spark.vert`); the interactive loop adds `DrawHud()` (`hud.frag`).

Passes communicate through the previous pass's output within the tick, or the previous tick's output across the
order: water sees the sand walls rasterized at the end of the previous tick, the gas mask uses the previous tick's
liquid volume, and sand sees the water fields of the current tick.

---

## 2. Scheduling

### 2.1 Fixed step clock

**Method.** `FixedStepClock` (`src/FixedStepClock.hpp`) accumulates wall time and returns how many whole ticks to run
this frame:

1. `Advance(elapsed, dt)`: $`\mathit{acc} \leftarrow \mathit{acc} + \mathit{elapsed}`$; the cap is 1 if
   $`2\,c_{\text{tick}} > 0.75\,\Delta t`$ (with $`c_{\text{tick}}`$ the tick cost), else 2;
   $`\mathit{steps} = \min(\mathit{cap}, \lfloor \mathit{acc} / \Delta t \rfloor)`$;
   $`\mathit{acc} \leftarrow \min(\mathit{acc} - \mathit{steps}\,\Delta t,\ \Delta t)`$, so any backlog beyond one tick
   is dropped.
2. `MeasureTicks(seconds, ticks)`: the per-tick wall cost of the frame's ticks. The estimate jumps up immediately to a
   larger measurement and decays toward smaller ones by 5% per frame.
3. `RunInteractiveFrame` runs the ticks (`RunFrame`, counters reset on the first tick only), measures them, renders
   with `Render(true)`, draws the HUD and swaps with adaptive vsync (swap interval -1 where `*_swap_control_tear`
   exists, else 1).

Ticks end in blocking readbacks (2.3), so their wall time tracks their GPU cost.

**Why.** Frame rate is preferred over strict real time. A catch-up tick runs only when two ticks fit in three quarters
of a tick (tick cost under about 6.25 ms at 60 Hz). The previous rule (two ticks after any frame of at most 1.5 ticks)
let a 12 ms tick catch up whenever the backlog reached a tick, so a loaded scene alternated 16 and 28 ms frames and
read as 30-40 FPS. Now a tick that fits a frame runs once per frame; a missed refresh loses that tick (a few percent of
slow motion under jitter) instead of doubling into a 30 ms frame; a tick longer than a frame slows the simulation
instead of compounding. `powder_config_tests` checks that a 12 ms tick on a 60 Hz display with 2 ms misses never runs
a two-tick frame and keeps at least 95% of real time, and that 5 ms ticks keep real time.

**Limits.** Under sustained load the simulation runs slower than real time by design. Replays, tests and benchmarks run
fixed ticks and do not use the clock. The clock in a live window is not verified beyond the unit model and the shared
frame body (VALIDATION, Current status).

| Constant | Value | Basis |
| --- | --- | --- |
| `fixed_dt` | 1/60 s | display rate; range 1/240 to 1/15 |
| `kCatchUpBudget` | 0.75 of a tick | leaves a quarter of the frame for rendering |
| tick-cost decay | 5% per frame | one slow tick stops catch-up at once; a short lull does not restart it |

### 2.2 Tick order and the gas tick pair

The order is the table in 1.4. The gas advances on every tick by that tick's dt, and its pressure solve spans a pair of
ticks (`kGasTickStride = 2`; ticks 1, 3, 5, ... after `InitializeState` begin a solve, ticks 2, 4, 6, ... finish it;
section 8.2). Brush stamps, coupling and render extraction act on every tick and read gas whose velocity is projected
up to one tick late.

### 2.3 Readbacks and fences

The GPU is otherwise asynchronous; these are the only blocking points of a tick:

| Readback | Size | Purpose |
| --- | --- | --- |
| Water cursor after compaction (`RunWaterPass`) | 8 bytes | live record count (sizes P2G and the particle step dispatches) and fastest speed (sizes substeps) |
| Water CG running flag (`RunWaterProjection`) | 12 bytes per confirmation, behind a `glFenceSync` | stop decision; one confirmation per solve when the blind batch covers it, see 4.9 |
| Sand cursor after compaction (`RunSandPass`) | 24 bytes | live count, fastest speed, live cell box |
| Gas residual partials (`RunGasProjection::needs_iteration`) | about 1000 vec4 per check | stop decision at each check; see 8.5 |

Particle metrics and full-state readbacks exist for tests and replays only (`ReadParticleMetrics` reads the cursor,
then only that prefix of the pool).

### 2.4 Failure counters

Five `uint` counters live in `debug_state_.counters` (SSBO binding 7 in most shaders), cleared at the start of each
rendered frame:

| Index | Name | Incremented when |
| --- | --- | --- |
| 0 | `nan_inf_detected` | a nonfinite or unrepresentable value (for example a half-float overflow past 65504) is read or produced; the value is neutralised (zeroed or the record deactivated) |
| 1 | `particle_overflow_blocked` | a pool, a fixed-point sum or a packed count would overflow, or a wake cannot reserve its slots |
| 2 | `pressure_nonconverged` | a solve breaks down, or a cell fails the independent true-residual check |
| 3 | `inactive_tile_misses` | reserved (no current writer) |
| 4 | `water_velocity_clamped` | a water particle's velocity or advection speed was cut by the speed cap |

Counters 0-3 are failures: replays and benchmarks fail on any nonzero value. Counter 4 is a diagnostic reported in
every replay line (F10 blue channel, shared with counters 2 and 3), not a failure. The F10 view shows them as colour.

### 2.5 Preflight budget rejection

Unsafe configurations are rejected before any dispatch, with an exception naming the setting:

- `powder_config::LoadConfigFromEnvironment` validates every `POWDER_*` override (ranges, finiteness,
  $`\min \le \max`$).
- `RunWaterPass`: the speed cap must fit `water_max_substeps` at `water_cfl_limit`
  (`SubstepCount(water_max_velocity, dt, cfl, ...)` throws), and the gravity impulse of one substep at the fewest
  substeps must fit in binary16 ($`\lvert g\,\Delta t\rvert \le 65504`$).
- `RunSandPass`: $`\max(\text{wave}, \text{viscous}, \text{advective at cap})`$ substeps must fit `sand_max_substeps`;
  the Hencky coefficients must be finite in float; the per-substep gravity impulse must fit 65504 cells/s.
- `RunGasPass`: transport substeps at the speed bound and diffusion substeps
  ($`(D + \text{cooling})\,\Delta t \le 1`$) must fit `gas_max_substeps`.

Substeps are never clipped to stay inside a budget; a scene that would need more is an error, not a silently
unstable run.

---

## 3. Boundaries: rock, masks, signed distance, erase

### 3.1 Method

`RunBoundaryPass` runs every tick:

1. `boundary_build.comp` per cell: painted material $`\ge 2`$ (rock or any wood state) is solid. The full mask is
   1 for a painted solid, else 3 if sand occupancy $`> 0.75`$, 2 if $`> 0.25`$, else 0. The static mask is 1 for
   painted solids only.
2. When the static geometry changed (`boundary_state_.static_dirty`, set by any material paint or erase, and every 15
   ticks once wood has been painted since the last reset): `boundary_seed.comp` marks cells on the solid/open interface (8-neighbourhood),
   `boundary_jumpflood.comp` runs jump flooding with steps 512, 256, ..., 1 (Rong and Tan 2006) to find each cell's
   nearest seed, and `boundary_distance.comp` writes the signed distance
   $`\lVert \mathbf{x}_{\text{cell}} - \mathbf{x}_{\text{seed}} \rVert + 0.5`$, negative inside solids, the domain
   diagonal when no seed exists. The $`+0.5`$ offset keeps the sign correct on both sides of an interface whose seeds
   are cell centres.
3. `boundary_downsample.comp` per gas cell: 1 if any full-grid cell in its 4 x 4 footprint (its 2 x 2 cells plus a
   one-cell halo) is painted solid; else 3 if any is dense sand (full mask 3); else 3 if all four of its own cells
   hold at least half the rest volume of water, not oil ($`V_{\text{liquid}}\,(1 - \text{oil share}) \ge 0.08`$ with
   $`V_{\text{liquid}}`$ = `liquid_volume`, the previous tick's field); else 0.

### 3.2 Who uses what

| Consumer | Uses |
| --- | --- |
| Water P2G, MAC build, divergence, solve, projection, particle sweeps | full mask: 1 = rest wall, 2/3 = moving porous sand wall |
| Sand grid contact and particle sweeps | signed distance (sign only for sweeps; gradient for normals) |
| Gas transport, solve, projection, combustion | gas mask: 1 = wall, 3 = dense sand (porous to the pressure solve) or filled water |
| Sand packing, sand spawn | signed distance, material |

Only sand contact needs distances; everything else needs topology, so the jump flood runs only on geometry changes.

### 3.3 Rock (solid) and erase

Rock is material 2, painted by `spawn_full.comp` (`replaceMaterial`). It is inert: it does not move, burn or conduct.
Painting rock also clears the water amount, packed water and packed sand in its cells, and `StampBrush` clears water
and sand records whose cell lies in the disc, so nothing is frozen inside a new wall. Composite shading of rock is in
11.3.

Erase (`StampBrush`, `BrushMode::Erase`): `spawn_full.comp` in clear mode zeroes material, packed sand, water
amount and velocity, packed water, and the gas temperature and flame of the gas cell under each erased cell;
`*_particle_clear.comp` deactivate records whose cell centre lies in the disc (the same test as the grid clears); and
`spawn_smoke.comp` in clear mode zeroes the gas faces (including the right and top faces shared with neighbours),
smoke, temperature, fuel and flame and resets oxidizer to fresh air. Particle clears test only the disc, never blocked
cells elsewhere (that once deleted water under sand across the map).

### 3.4 Limits

- Sand below a quarter of rest density neither walls nor displaces water; below three quarters it does not wall gas.
- The distance field is approximate (jump flooding can pick a non-nearest seed in rare configurations); sweeps use only
  its sign and normals use its local gradient.
- Wood burning away changes the geometry without a readback, so the distance field is refreshed every quarter second
  once wood has been painted since the last reset, even after it has all burnt (a marked shortcut in `RunWoodPass`).

---

## 4. Water

Water is a FLIP/APIC particle-in-cell liquid (Brackbill and Ruppel 1986; Zhu and Bridson 2005; Jiang et al. 2015) on
the MAC grid, with a density-corrected pressure projection solved by multigrid-preconditioned conjugate gradient
(McAdams et al. 2010) and swept particle advection.

### 4.1 Records and pool

`WaterParticle` is three vec4s (48 bytes):

| Component | Contents |
| --- | --- |
| state0.xy, .zw | position (cells), velocity (cells/s) |
| state1.x | 0 inactive; else 1 + still-tick timer (integer 0-255, `water_sleep.comp`) |
| state1.y | mass (1) |
| state1.zw, state2.xy | APIC affine matrix $`\mathbf{C}`$, column-major (column 0 = state1.zw) |
| state2.z | oil flag (0 water, 1 oil) plus a per-record tag in the fraction (always below 0.5) |
| state2.w | lit flag of oil (1 burning) |

The pool holds `kMaxWaterParticles` $`= 2^{21}`$ records (96 MB) twice for compaction. Startup checks
`GL_MAX_SHADER_STORAGE_BLOCK_SIZE`. Live records sit below the spawn cursor. The host keeps `record_bound`, the exact
live count read after the last compaction plus the upper bound of every spawn since, and dispatches compaction, the
sleep tally, the phase pass and erase over it.

Pool budget: `RunSpawnWaterParticles` skips a whole stamp when
`record_bound` $`+\ \text{region area} \times \text{particles per cell}`$ would exceed the pool less
`kWaterWakeReserve` (a sixteenth, 131,072 slots kept for packed water that wakes). The
brush therefore stops at a full pool at the same tick every run. Before, the GPU's per-record reservation at the cap
dropped records in atomic order and counted each as overflow (`app-water-pool-full`). The pool holds about half the
screen of moving water. A larger pool ($`2^{22}`$ records), a budget that grows when the last tick took fewer
substeps, and packing from the velocity relative to the surrounding flow were all measured and rejected. A tick
costs about

```math
t \approx N\,(1.7 + 1.8\,s)\ \mathrm{ns} \;+\; \text{pressure solve over the liquid area}
```

for $`N`$ moving records and $`s`$ substeps (a still pool of 0.25-1 million cells, no packing: 5.6/7.0/12.2/15.4 ms at 1,
2, 3 and 3.8 million records and one substep, 8.9-28 ms at three), and a churning pour sits at two or three substeps, so
the frame time of a pour, not the pool, bounds the fill: budgets of 3, 2.5 and 1.97 million records after one, two
and three substeps took a profile pour from 13.7 to 18.5 ms GPU mean (1.93 to 2.59 million records at the end, 48% to
65% fill), and a calm-only extension added 2 ms to the mean frame of a 30 s hold (15.7 against 13.6) for 2 points of
fill (VALIDATION, water surface pass).

### 4.2 Spawn

`water_particle_spawn.comp`, one invocation per cell of the brush's bounding box:

1. Skip cells outside the disc, inside the previous stamp's disc of the same tick (`skipActive`, see 12.3), or
   blocked in the full mask.
2. Density gate (interactive brushes only): skip a cell whose liquid volume already reaches
   $`0.04 \times \text{particles per cell}`$ (the brush's own density), so a held brush stops stacking. Scripted and
   replay brushes pass 0 and keep exact emission.
3. For each of the 4 records: a uniformly random position anywhere in the cell (not a 2 x 2 lattice, which fell as
   rigid rows and stacked into teeth), velocity = brush velocity plus a uniform spread of $`\pm`$`velocitySpread`.
4. Sub-tick age: each record is born a random age $`a \in [0, \Delta t)`$ before the tick and has already flown
   ballistically, $`\mathbf{x} \leftarrow \mathbf{x} + \mathbf{v}\,a + \tfrac{1}{2}\mathbf{g}\,a^2`$,
   $`\mathbf{v} \leftarrow \mathbf{v} + \mathbf{g}\,a`$. The flight is kept only if its straight path, sampled every
   quarter cell, stays in open cells; otherwise the record is born at the brush. Without it every record left at the
   tick start, one slab per tick, and a held small brush poured a string of beads (`app-water-stream`: birth-phase
   concentration 0.13 -> 0.005).
5. Slots are reserved by compare-and-swap below the pool cap; failures count `particle_overflow_blocked`.

Hashes are PCG-style integer hashes of cell, record index and a per-tick seed (`spawnSeed`).

### 4.3 Compaction into bins

`water_particle_compact.comp` runs three phases over `record_bound` records each tick, before the substeps:

1. Phase 0: each kept record (active, not in a cell flagged to pack, see 4.13) computes its bin: 8 x 8 cells in
   row-major order. Records arrive mostly sorted, so a workgroup max-scans the starts of runs of equal bins and the
   last record of each run adds the run length to that bin with one atomic.
2. Phase 1: one workgroup turns the bin counts into exclusive offsets (each lane scans a chunk, then a Hillis-Steele
   scan of the 256 chunk sums, Hillis and Steele 1986) and writes the total to the spawn cursor.
3. Phase 2: the same run logic reserves each run's slots (`atomicAdd` on the bin's offset) and scatters the records.
   Each copied record is deactivated in the source, so the source becomes an all-inactive scratch pool and nothing is
   cleared per tick. The pass also records the fastest speed with `atomicMax` on float bits.

Records with invalid positions go to bin 0 and are still copied, so the particle passes count them. Order within a
bin is atomic and arbitrary; no pass depends on record order (fixed-point sums, per-record state, hashes of the
record's own state).

**Why.** Sorting lets P2G and the particle step touch nearby cells together. In the `pour` profile at 1.6-2.1M
records P2G fell from 6.8 to 1.4 ms and the particle step from 5.2 to 0.8 ms per tick, for 0.4 ms of compaction.
Bins of 4, 16 and 32 cells were tried: 16 and 32 spill workgroups past P2G's shared tile (P2G 2.6-7.1 ms).

### 4.4 Particle to grid (P2G)

`water_particle_p2g.comp`, one invocation per live record:

1. Validate: skip inactive records; count nonfinite or out-of-domain state; skip records whose cell is blocked (a
   record caught in sand waits for the particle step to free it; its zero-momentum weight would brake the water around
   it); reject records whose momentum could overflow a 32-bit word.
2. Cell weight (bilinear, cell centred): each of the four weights goes to its cell unless that cell is blocked, in
   which case it goes to the record's own cell, so a film against a wall keeps its density.
3. Face momentum (quadratic B-spline, APIC): for each of the 3 x 3 u faces and 3 x 3 v faces, weight
   $`w = m\,N(\mathbf{x}_f - \mathbf{x}_p)`$, value $`\mathbf{v}_p + \mathbf{C}_p(\mathbf{x}_f - \mathbf{x}_p)`$. Faces
   on the domain edge or with a blocked cell on either side get nothing.
4. Fixed point: weights are $`\tilde{w} = \mathrm{round}(4096\,w)`$ (u32). Momentum is
   $`\mathrm{round}(\text{value} \times \tilde{w})`$ in units of 1/4096 (cells/s x mass), stored as a signed low
   word plus a wrap counter (`ivec2`, `signedCarry`), so dense momentum cannot overflow. Momentum uses the quantized
   weight, so a face average stays a convex mix of particle velocities even where edge-of-support weights round to
   zero; with the unrounded weight a face once read 26,000 cells/s from 2400 cells/s particles
   (`gpu-water-sparse-face`).

Shared-memory path: the workgroup's 256 records find their cell bounding box. If it spans at most 20 cells per axis
(one or two bins), every deposit goes to a 24 x 24 shared tile (cells, u and v weights, u and v sums) and each touched
entry is added to the global buffers once. Integer sums are associative, so the grids are bit-identical to direct
deposits (`gpu-water-p2g` compares sorted and shuffled records). Wider workgroups deposit directly.

**Why integer sums.** Atomic float adds depend on order; integer sums make every repeat bit-exact, which replay
signatures and the CG's bit-exact reductions rely on.

### 4.5 MAC faces and particle fields

`water_mac_build.comp` (per face, through a 20 x 20 shared tile of packed counts):

- Open face: $`\text{velocity} = \text{momentum sum} / (\text{weight sum} + \text{packed weight})`$. A packed cell
  (4.13) deposits rest-density water at rest: the quadratic face kernel averaged over the cell is the cubic B-spline
  (weights $`\tfrac{1}{48}, \tfrac{23}{48}, \tfrac{23}{48}, \tfrac{1}{48}`$ across the face axis,
  $`\tfrac{1}{6}, \tfrac{2}{3}, \tfrac{1}{6}`$ along it), the bilinear cell kernel averaged over the cell is
  $`\tfrac{1}{8}, \tfrac{3}{4}, \tfrac{1}{8}`$.
- Face with a blocked cell: 0 if either side is painted solid or both sides are blocked; otherwise (water against
  sand) the sand grid velocity of the sand cell, so the pressure solve treats sand as a moving wall (section 7).
- Values that would overflow binary16 count as nonfinite and store 0.

Each substep builds this into `mac_u_prev`/`mac_v_prev` (the pre-force field, FLIP's baseline); the projection writes
`mac_u`/`mac_v`. After the last substep the build runs once more into `mac_u`/`mac_v` for other passes.

`water_particle_finalize.comp` (per cell) derives:

- local volume $`V_{\text{local}} = 0.04 \times \text{particle-equivalents}`$; smooth volume $`V_{\text{smooth}}`$ = a
  4-2-1 tent filter over 3 x 3 cells, times 0.96; liquid volume
  $`V = \mathrm{clamp}\,\big(\max(V_{\text{local}}, \mathrm{mix}(V_{\text{local}}, V_{\text{smooth}}, 0.45)), 0, 1\big)`$.
  A cell at rest reads 0.16.
- The liquid $`\phi`$ of the pressure solve is the particle density blurred over 3 x 3 cells with the weights
  $`\tfrac{1}{4}(1, 2, 1) \otimes (1, 2, 1)`$ (blocked cells left out of the average and the weights renormalised;
  cells outside the domain repeat the edge cell), $`\bar\rho = \sum_i w_i\, m_i / (\sum_i w_i\, m_{\text{rest}})`$ with
  $`m_i`$ the particle-equivalents of cell $`i`$, mapped linearly so the contour $`\bar\rho = \rho_0`$
  (`water_surface_level`, 0.25) sits at the threshold $`T`$ (`water_liquid_phi_threshold`, 0.42):

  ```math
  \phi = \mathrm{clamp}\,\big(T + 2T\,(\rho_0 - \bar\rho),\ -1,\ 1\big)
  ```

  A cell is liquid when $`\phi < T`$, that is when the blurred density passes a quarter of rest, so one particle
  crossing a cell edge no longer flips a cell of the solve (the first attempt classified by the cell's own fill:
  $`\phi = 0.46 - 1.18\,V`$, liquid above about 0.85 particle-equivalents).
- amount $`= \mathrm{mix}\,\big(V,\ 1 - \mathrm{smoothstep}(0.06, 0.42, 0.46 - 1.18\,V),\ 0.28\big)`$, the render and
  coupling field (unchanged; it does not use the solve's $`\phi`$).
- Blocked cells get volume 0, $`\phi = -1`$.

`water_mac_finalize.comp` averages faces to the cell-centred velocity (zero in blocked cells).

### 4.6 Divergence and density control

`water_divergence.comp`, per liquid cell ($`\phi < 0.42`$, not blocked; others write 0):

1. $`(\nabla\cdot\mathbf{u})_c = (u_{\text{right}} - u_{\text{left}}) + (v_{\text{up}} - v_{\text{down}})`$ from the
   pre-force faces, with gravity $`g\,\Delta t`$ added to open v faces (faces of moving sand keep the sand velocity;
   painted walls and the domain edge are 0).
2. Density target: error $`e = \text{particles} / \text{rest} - 1`$, clamped to $`[-0.5, 0.5]`$ in the bulk and
   $`[0, 0.5]`$ elsewhere. A cell is bulk when the cell above it is a wall, the domain top, or holds at least 0.75 of
   rest density. The term $`-k_\rho\,e`$ is added ($`k_\rho`$ = `water_density_rate` = 20 /s), so an over-dense cell
   is asked to expand and a void inside the bulk to contract. The free surface and sparse spray (air above) only ever
   expand.
3. Packed cells and their 3 x 3 neighbours skip the correction: a packed cell's deposit is not its particles' exact
   deposit, and correcting that difference drove flows that woke 90% of a still deep pool.
4. Nonfinite or overflowing results count and store 0.

**Why.** Particles drift out of a divergence-free flow's volume: without a target a settled dam break kept 3% voids and
6% over-dense cells (std 0.29); with it 0.03% voids and none over-dense (std 0.04). The term changes only the
right-hand side; the operator, gradient and projection stay the same, so they remain consistent. The bulk test looks
at the cell above: requiring every neighbour to be nearly full left clusters of entrained voids at real gravity (a dam
break kept 24% voids). Density correction follows Kugelstadt et al. 2021. `POWDER_WATER_DENSITY_RATE=0` disables it;
high rates also slow thin flows.

### 4.7 Sealed pockets

**Problem.** A 4-connected region of liquid cells with no air neighbour and no sand face, walled in by painted solid
and the domain edge alone, is a pure Neumann system: its Laplacian is singular (the constant is its null space) and
$`L\,p = b`$ is solvable only if $`b`$ sums to zero over the region (the compatibility condition, Bridson 2015, ch. 5).
The velocity divergence telescopes to zero there, but density targets need not: an overfilled sealed tank asks every
cell to expand, and a pinhole a wood stamp leaves in a wall asks for a volume change no pressure can make. CG stalled
on the remainder and then diverged (pressure $`6 \times 10^{17}`$), and later ticks failed on nonfinite input
(`app-water-sealed-cell`, `app-water-sealed-pocket`).

**Method.** `RemovePocketMeans(false)` runs `pressure_pockets.comp` after every divergence pass, five dispatches:

0. Label each 16 x 16 tile with a union-find in shared memory and point each cell at its tile-local root.
1. Unite across tile borders with a concurrent union-find over the grid (Playne and Hawick 2018), path halving,
   hooking the larger root under the smaller with `atomicMin` and retrying on a lost race.
2. Flag roots of regions with a grounding neighbour (an air cell or a sand cell).
3. For ungrounded regions, count cells and sum the right-hand side in 64-bit fixed point (scale 1024, low word plus
   carry), order-independent so repeats are bit-exact.
4. Subtract each pocket's mean (the projection of $`b`$ onto the range of $`L`$). For water, the pocket's root cell is
   made air for this solve ($`\phi`$ raised above the threshold; the next field build restores it), pinning the
   pressure to zero. With the mean removed every other equation keeps its exact solution and the root absorbs only
   rounding.

**Why the pin.** Mean removal alone was not enough: the multigrid preconditioner, whose coarse cells turn to air where
a pocket's wall is thinner than they are, amplified the null space, and a sealed 200 x 80 tank's pressure drifted to
$`4.4 \times 10^{5}`$ within one 24-iteration solve, failing the residual gate on rounding alone. PCG on a consistent
singular system converges (Kaasschieter 1988). A one-cell pocket ends with $`b = 0`$, which replaced the earlier
pinhole special case.

Cost: five dispatches per solve, about 0.04-0.06 ms per frame at 1000 x 1000 (7.3 us per dispatch for water; 25.7 us
before the tile-local first phase).

### 4.8 Pressure operator

The discrete system per liquid cell $`c`$ is

```math
\sum_{n \in \text{open}} (p_c - p_n) \;+\; \sum_{n \in \text{sand walls}} \kappa\, p_c \;=\; -(\nabla\cdot\mathbf{u})_c
```

with air neighbours at $`p = 0`$ (Dirichlet, contribute to the diagonal only), painted walls and the domain edge as
Neumann (no term), sand walls as porous Dirichlet with conductance $`\kappa`$ = `kSandLeak` = 0.005. The pressure is
the scaled pressure $`\Delta t\, p / (\rho h)`$ in $`\mathrm{cells^2/s}`$, so the projection subtracts plain
differences. `pressure_cg.comp`, `pressure_mg.comp`, `pressure_residual.comp` and `water_project.comp` use the same
coefficients, so divergence, operator and projection stay consistent at walls, sand and the free surface.

**Ghost-fluid free surface** (Gibou et al. 2002; Enright et al. 2003; Losasso et al. 2004). The zero of the pressure
sits at the surface, not at the centre of the first air cell. For a liquid cell $`c`$ with an air neighbour $`n`$ the
surface lies at $`\theta`$ of the way from $`c`$ to $`n`$, found by linear interpolation of $`\phi`$ between the two
centres:

```math
\theta = \frac{\phi_c - T}{(\phi_c - T) - (\phi_n - T)},\qquad
\gamma = \frac{1}{\mathrm{clamp}(\theta,\ \theta_{\min},\ 1)}
```

and the face conducts $`\gamma`$ instead of 1: the diagonal gains $`\gamma`$ for the air neighbour, and the projection
subtracts $`\gamma\,(p_n - p_c)`$ with $`p_n = 0`$ on that face. $`\theta_{\min}`$ is `water_ghost_theta_min`, 0.25. A
face whose $`\phi`$ values do not straddle $`T`$ (a liquid cell pinned to air by a sealed pocket, 4.7) keeps $`\gamma = 1`$.
The matrix stays symmetric (the conductance belongs to the face and the air cell has no row) and positive definite,
so CG is unchanged; $`\gamma`$ enters only the diagonal and the face update, so the CG tile work and the V-cycle's
off-diagonal terms are as before. In a hydrostatic column whose top cell reads $`\theta = \tfrac12`$ the pressure is
$`p_y = 4/\gamma + 4\,(32 - y)`$, top cell 2 instead of 4 (`app-water-ghost-fluid`).

The multigrid preconditioner keeps the unit coarse operators of 4.9 but adds to a coarse liquid cell's diagonal half
the sum of its children's excess over their unit-conductance diagonal (the Galerkin product with the transfers
would add about the whole sum). Without it the conductances raised CG iterations by 14% in a pour and 38% in the profile's `user` scene, with it they
change by -4% and +8% (VALIDATION). Because $`M^{-1}`$ is still a fixed symmetric map with a contracting smoother, it stays
symmetric positive definite (`app-water-multigrid`).

Why a blurred density and not the fill fractions: with $`\theta`$ from the cell's own fill the surface pressure
followed the particle noise (settled pools moved 2-3 times faster, CG needed half again as many iterations). The
three-by-three blur linearises the density across the surface, so interpolating it places the surface to a fraction
of a cell (a flat surface half way through a cell row reads $`\theta = \tfrac12`$ exactly), and the smooth classification keeps
the cells of the solve from flickering with single particles. (Zhu and Bridson 2005 build the surface from kernel-averaged
particle positions and radii, which needs a second particle-to-grid pass of position sums; the cell densities of the
existing pass blurred on the grid give a linear phi from the data already there.) The contour sits at a quarter of rest density rather
than a half, which would be the true surface of a flat pool, because cells below half still need their pressure for
the density correction (4.6) to hold the front of a dam break (at one half the settled dam break kept
over-dense cells and failed the gate), and because it keeps sparse sheets liquid. Measured against the fill-fraction
operator on settled pools: mean speed 0.10 -> 0.004 cells/s, level spread 0.24 -> 0.03 cells, 10 -> 3.2 CG iterations;
the dam break's mean speed halves and its over-dense share stays under the 0.5% gate.

Rejected again: velocity extrapolation one cell into the air (the face beyond a liquid-air face takes that face's
projected value). With the smooth surface it made settled pools jitter more (mean speed 0.004 -> 0.009 cells/s, calm
surface std 0.08 -> 0.14) and cost 10% more CG iterations. A 5 x 5 blur, the blur for $`\theta`$ only (the liquid cells
staying those of the fill-fraction phi) and the union and intersection of the two classifications were also measured
(VALIDATION): the 5 x 5 kernel costs 0.3 ms per frame more and left the dam break at the density gate, the blur for
$`\theta`$ alone calmed pools 3.6 times against the original but 7 times less than the kept classification, and the
union and intersection were no better than either.

### 4.9 MGPCG solve

**Tile list.** After `pressure_cg.comp` phase 0 (per cell: warm-start residual $`\mathbf{r} = \mathbf{b} - A\mathbf{p}`$
from the stored R32F pressure with today's mask, diagonal, Jacobi-scaled copies; invalid input flagged with diagonal
-1), `pressure_cg_reduce.comp` reduces the initial $`\mathbf{r}\cdot\mathbf{z}`$, $`\max\lvert r\rvert`$ and the liquid
bounding box. Phase 8 flags every 16 x 16 tile holding a liquid cell, invalid input or a stray residual; phase 9 (one
workgroup) compacts the flags into a list in grid order and writes the indirect dispatch arguments. All later CG steps
run one workgroup per listed tile.

**Iteration.** Each CG iteration is three steps, two CG dispatches around the preconditioner:

1. Phase 2: every workgroup forms the global $`\mathbf{d}\cdot A\mathbf{d}`$ from the per-tile sums in list order,
   computes $`\alpha`$, updates $`\mathbf{p}`$ and $`\mathbf{r}`$ for its tile.
2. `pressure_mg.comp` V-cycle: $`\mathbf{z} = M^{-1}\mathbf{r}`$ (15 dispatches at 1000 x 1000), with per-tile
   $`\mathbf{r}\cdot\mathbf{z}`$ and $`\max\lvert r\rvert`$.
3. Phase 1: every workgroup forms the global $`\mathbf{r}\cdot\mathbf{z}`$ and $`\max\lvert r\rvert`$, tests
   convergence, computes $`\beta`$, the new direction $`\mathbf{d} \leftarrow \mathbf{z} + \beta\,\mathbf{d}`$ and
   $`A\mathbf{d}`$ for its tile (written to scratch, since neighbouring tiles still read this tile's old
   $`\mathbf{d}`$).

Because every workgroup forms the same sums from the same list in the same order, all reach the same decision without
a separate reduction dispatch, and the sums repeat bit for bit. The first workgroup records the result; when the solve
stops it zeroes the tile and multigrid dispatch counts, so later queued dispatches launch nothing. (A
last-workgroup-done reduction was tried and raced on this driver.)

**Multigrid preconditioner** (`pressure_mg.comp`, after McAdams et al. 2010): one V-cycle from zero over levels that
halve the grid until the coarsest fits one 8 x 8 workgroup (1000 -> 500 -> ... -> 8, seven coarse levels).

- Coarsening (once per solve, phases 0, 1, 5): a coarse cell is air if any child is air, else liquid if any child is
  liquid, else a wall that is sand-porous if a child is sand. Each level uses the unscaled 5-point operator plus half
  the sum of its children's ghost-fluid excess diagonal (4.8); level boxes cover the liquid bounds plus one cell.
- Restriction is the transpose of bilinear prolongation (weights $`\tfrac14, \tfrac34, \tfrac34, \tfrac14`$ per axis,
  summing to 4, which absorbs the factor of four between grid spacings).
- Prolongation treats walls as Neumann: a wall partner of a fine cell's bilinear stencil
  ($`\tfrac{9}{16}, \tfrac{3}{16}, \tfrac{3}{16}, \tfrac{1}{16}`$) takes the cell's own coarse value, and restriction
  lends the same weights back.
- Smoothing: one damped Jacobi sweep ($`\omega = 2/3`$) before and after each coarse correction; the coarsest level
  runs 16 Jacobi sweeps from zero in shared memory.
- Restriction and prolongation stage a block's neighbourhood in shared memory; level-0 steps (restriction to level 1, prolongation back) run per listed CG
  tile, coarser ones over each level's box.

Every part is a fixed symmetric linear map and the smoother contracts, so $`M^{-1}`$ is symmetric positive definite and
CG stays valid (`app-water-multigrid` checks $`\langle M\mathbf{a}, \mathbf{b}\rangle = \langle \mathbf{a}, M\mathbf{b}\rangle`$
to $`2.8 \times 10^{-9}`$ relative and $`\langle M\mathbf{a}, \mathbf{a}\rangle > 0`$).

**Convergence and the host.** The host never waits before the solve. It dispatches the previous solve's iteration
count blind, queues the restart below, and copies the running flag to a staging buffer behind a fence. A solve the blind
batch covered is confirmed by that one fence and nothing else is read back. While a fence finds the solve running the
host adds one iteration, then 1, 2 and 4 more per fence, waiting for the fence and mapping the copy each time, until a
fence finds the solve stopped or the 1024 cap is reached (speculative iterations launch nothing once stopped). CG stops
when the recursive $`\max\lvert r\rvert \le 0.06`$ (half the 0.12 threshold). Recursive residuals drift from the true
$`\mathbf{b} - A\mathbf{p}`$ as R32F rounds a large pressure (a deep packed pool reaches $`\lvert p\rvert`$ about
85,000, one ulp 0.008), so a stopped solve restarts from $`\mathbf{b} - A\mathbf{p}`$ (phase 0 and the reduce with
`restart`, keeping the tile list and iteration count); a restart whose true residual already passes the full threshold
stops with the pressure unchanged. The GPU decides whether a restart is due, so the host queues it at every fence
without waiting: phase 10 of `pressure_cg.comp` sets an armed flag when the solve has stopped without breakdown after
iterating since the last (re)start (`legBase`) and writes the indirect arguments of the restart's cell passes, which are
empty otherwise, and the other restart passes return unless the flag is set. The two sum slots alternate with the global
iteration number, so a restart needs no host knowledge of its position, and the final pressure, iteration count and
restart point are the same for every host schedule (`app-water-solve-schedule`). Finally
`pressure_residual.comp` always checks $`\max\lvert \mathbf{b} - A\mathbf{p}\rvert \le 0.12`$ per cell from the stored
pressure and counts every failing cell in `pressure_nonconverged`.

**Why.** Against the old block-Jacobi CG: a 64 x 32 pool 32 -> 5 iterations, a 998 x 400 tank 359 -> 6, live
8x-gravity systems 107-164 -> 10-12; heavy solves 5-19x faster. The solve is latency-bound (17 dependent
dispatches per iteration of 3-10 us), so its cost follows the GPU clock more than the liquid volume. Measured and
rejected: two smoothing sweeps (20-30% fewer iterations, same time), a 16 x 16 coarsest level (tank 6 -> 23
iterations), liquid-priority coarsening (57 and 106), interpolating toward zero at walls (tank 28 instead of 6), an
in-workgroup bottom solve (shared memory cut occupancy: tank 4.4 -> 7.0 ms), a fused CG setup pass (slower),
block-occupancy skipping and fewer command barriers (no change). The restart replaced a failure on a deep packed pool
(true residual 0.125 after the recursive one reached 0.043); restarting at half the threshold instead moved the
`app-water-density` dam break std from 0.108 to 0.120, so the kept version restarts only as described.

### 4.10 Projection

`water_project.comp` per face: open faces get
$`u^{\text{prev}} + (g\,\Delta t \text{ on } v) - \gamma\,(p_{\text{right}} - p_{\text{left}})`$ ($`\gamma`$ of 4.8, 1 except
at a liquid-air face); faces between water and sand
(no painted solid) get $`u^{\text{prev}} - \kappa\,(p_b - p_a)`$, keeping the sand velocity plus the leak; faces
against painted solid or the domain edge get 0. Air cells read pressure 0, so air-side faces keep their transferred
velocity for particles whose kernel crosses the surface. Overflowing faces count and store 0. On the last substep
`water_pack.comp` phase 2 reads these faces (4.13).

### 4.11 Particle step (G2P and advection)

`water_particle_step.comp` per live record:

1. Record tag: if the fraction of state2.z is 0, hash the record's position bits once and add a tag in $`(0, 0.5)`$.
2. Gather from the quadratic kernel: PIC velocity from the projected faces, the previous (pre-force) velocity from the
   same faces, and the APIC affine matrix
   $`\mathbf{C}_p = 4 \sum w\, \mathbf{v}\, (\mathbf{x}_f - \mathbf{x}_p)^{\mathsf T}`$ (Jiang et al. 2015).
   Live-face substitution: a face with a blocked cell on both sides is inside a wall and holds no fluid velocity, so
   the kernel reads the nearest live face toward the particle instead (up to three steps). Painted walls are then
   free-slip like the domain edge and the kernel keeps its full support; sand's one-sided faces keep their velocity.
3. FLIP/PIC blend:
   $`\mathbf{v} = \mathrm{mix}\,\big(\mathbf{v}_{\text{pic}},\ \mathbf{v}_p + (\mathbf{v}_{\text{pic}} - \mathbf{v}_{\text{prev}}),\ 0.97\big)`$
   (Zhu and Bridson 2005 use 0.95-0.99; 0.99 left visible noise).
4. Support: the pressure acceleration
   $`\mathbf{a} = (\mathbf{v}_{\text{pic}} - \mathbf{v}_{\text{prev}}) / \Delta t - \mathbf{g}`$; support
   $`s = \mathrm{clamp}(\lVert\mathbf{a}\rVert / g, 0, 1)`$. It is 0 in free fall and open air and about 1 in a
   resting pool or on a ramp.
5. Sand drag:
   $`\mathbf{v} \leftarrow \mathrm{mix}\,\big(\mathbf{v}, \mathbf{v}_{\text{sand}}, 1 - e^{-120\,\phi_s \Delta t}\big)`$
   with $`\phi_s`$ the bilinear sand occupancy and $`\mathbf{v}_{\text{sand}}`$ its grid velocity (section 7).
6. Oil correction (section 5).
7. Bulk damping: $`\mathbf{v} \leftarrow \mathbf{v}\, e^{-1.0\, s\, \Delta t}`$. Only pressure-supported water loses
   speed; free fall, streams and droplets ($`s = 0`$) keep theirs.
8. `water_velocity_damping` (1.0, none; defined per 1/120 s) and the speed cap 2400 cells/s; any cut sets the clamp
   diagnostic.
9. Swept advection:
   $`\text{moves} = \mathrm{clamp}\,\big(\lceil \max(\lVert\mathbf{v}\rVert, \lVert\mathbf{v}_{\text{grid}}\rVert)\,\Delta t / 2 \rceil, 1, 64\big)`$
   sub-moves of at most `water_advection_cfl` = 2 cells. Each sub-move is RK2 through the projected field where the
   cell holds liquid volume, else on the record's own velocity (a droplet that left its deposit flies ballistically); a
   midpoint inside a wall reuses the first stage. `moveWithCollisions` traverses the crossed cells exactly (a DDA over
   cell edges, never a distance-field step), stops at the first blocked face, removes the inward displacement and
   velocity component and the matching affine rows, and sweeps the remaining tangent once more. A hit point that rounds
   a few ulps into the next cell is clamped back into the last open cell traversed (this once froze records inside
   walls in the `boundary` and `leakage` replays). A record inside sand is first carried out (`escapeSand`, section 7);
   one inside painted geometry waits with zero velocity for a brush to clear it.
10. Clump jitter: if the final cell's liquid volume exceeds three times rest (0.48), take a swept position-only step
    of up to $`\pm 0.25`$ cells per axis, hashed from position, velocity, the per-substep seed and the record tag.
11. Oil drift (section 5).

**Domain edge.** The edge is a wall like a painted one: `blockedCell` is true outside the grid, so records sweep to
the outer face (positions in $`[0, N)`$). The old clamp to $`[0.5, N - 0.5]`$ kept a pool half a cell off the floor and
side walls (`app-water-wall`: min x/y 0.5/0.5 -> 0.33/0). Water against a side wall rests about 0.3 cells off it, as
off a painted face, because the density correction expands the edge cells that hold the weight displaced from the wall.

**Why the tag and jitter.** Long sub-moves through one projected field converge records onto single points (dam break
cells at 22x rest), which then share one velocity. The jitter separates them, but records that had also converged in
velocity (the 0.97 blend shrinks a difference below float precision within seconds) drew one jitter together and never
separated: a calm dam break kept 95 records in one cell (std 0.225 at bulk damping 0.6). With the tag, std 0.096 at
1.0. Sizing substeps from bulk liquid only (3+ particles per cell) gave std 0.14 and was rejected.

**Why bulk damping.** Real viscosity is negligible at this scale (a cm-scale wave decays over minutes), but a poured
pool's turbulence and wall boundary layers dissipate it within seconds, which an inviscid 5 mm grid does not resolve.
Without it a poured half-screen pool sloshed at p50 92 cells/s after 40 s. `app-water-calm` (90-cell pool in a
300-cell tank): moving p50 6.0/2.4/0.08 cells/s at 4/6/10 s after the pour, 85% packed at 10 s (0.4 /s: 14.5/9.6/2.8,
6% packed). Half-screen pour: p50 3 cells/s at 7 s, 52% packed at 10 s, 90% at 12 s, settled GPU mean 11.2 ms. 0.6 and
up used to clump the dam break until the record tag fixed that.

### 4.12 Substeps and CFL

After compaction the host reads the fastest speed (start of the tick) and sets

```math
\text{substeps} = \mathrm{clamp}\,\left(\left\lceil \frac{\min(v_{\text{fastest}}, 2400)\,\Delta t}{16} \right\rceil, 1, 6\right)
```

one pressure solve per substep, 16 cells per solve (`water_cfl_limit`). At the cap that is 3 solves; up to 1920 cells/s
it is 2; still water takes 1. The CFL slack covers a tick of gravity. Particles still move at most 2 cells per swept
sub-move inside a substep (4.11 step 9), so they never skip cells.

**Why.** Each MGPCG solve costs about 2-3.5 ms, so the solve count is the budget. Earlier, limits of 20 and 40 cells
per solve clumped the dam break (std 0.32-0.33) before the record tag and the jitter existed; at 16 with both the dam
break std is 0.10 at 12 and 16, 0.11-0.14 at 20.

### 4.13 Packed (sleeping) water

**Method.** At the start of each tick:

1. `water_sleep.comp` (over `record_bound`): advance each record's still-tick timer (state1.x = 1 + ticks slower than
   2 cells/s, saturating at 255, reset by faster motion) and tally per cell all records and ready ones (timer at least
   30 ticks, mass 1, not oil).
2. `water_pack.comp` phase 0: flag a cell to pack (tally bit 31) when it is not packed, holds the rest count
   $`\pm 1`$ (3 to 5) of records, all ready, and its 7 x 7 neighbourhood is liquid (at least half rest density in the
   last field build, no wall or sand, inside the domain).
3. Compaction drops exactly the flagged cells' tallied records.
4. Phase 1: flagged cells store their count in `water_packed` (bits 0-6). Cells flagged to wake (bit 7, set by the
   previous tick's phase 2) turn back into records: all of the cell's count or none (one `atomicAdd` reservation;
   a full pool leaves it packed and counts the overflow), one record per quarter of the cell in turn, jittered within
   it, with the stored wake velocity. Woken records join the cursor, so the live-count readback includes them.
5. After the tick's last projection, phase 2 flags a packed cell to wake when a face moves faster than 5 cells/s or a
   non-liquid, wall or sand cell comes within two cells (5 x 5), storing the projected cell velocity.

While packed, a cell stays in the grid as rest-density water at rest through `water_mac_build.comp` (4.5), so liquid
volume, phi, render, sand coupling and the pressure solve see it as water; the divergence skips the density correction
around it (4.6). Erase, solid and water-clearing brushes clear packed cells with the water.

**Why.** Rules read only the previous state and write only their own cell, so nothing has a sideways order. Moving plus
packed is an exact integer invariant (`app-water-pack-conservation`), and replays, signatures and
`ReadWaterLiveParticles` count both. Evidence for the rules: with exactly the rest count required, under a third of
settled cells pack (counts 3/4/5 are 23%/27%/20% of a stirred bulk), so $`\pm 1`$ is admitted; keeping the density
correction around packed cells left a 500-deep pool 5-10% packed and churning, without it 93%; uniform wake positions
raised the dam break std to 0.147, stratified quarters give 0.106. The surface band (three or more cells) and water
within three cells of walls or sand never pack, so waves and wall contact stay particle-resolved.

**Limits.** Still half-screen water was already cheap without packing (the pressure solve and grid passes, not P2G,
dominate still water); packing pays off in records and pool budget, not much in time. Water does not pack while a pour
stirs it: in the churn after a full-width pour 80-90% of the deep cells hold three to five records and sit inside
liquid, and under 0.1% have every record still for 30 ticks, since the whole pool swings at 5-15 cells/s. Packing
from the velocity relative to the 7 x 7 flow (readiness without the swing) gained 2 points of fill on average over
nine sweep speeds of a 30 s hold (from -5 to +10), and a bulk limit above 2 cells/s froze slow levelling flows: a tilt
that levels in 10 s unpacked stays 2.5-2.9 cells high with a limit of 4 (`app-water-pack-slosh`). Giving packed cells
the velocity they had (momentum in the deposit, wake on departure from it) was worse: the packed mass cannot follow its
own flow, so the interface starved. Accumulating the impulse a packed cell receives (wake once it would have reached the
wake speed) kept slow tilts alive and also kept 85% of the pool unpacked. None is kept. Oil never packs (a packed cell wakes as plain water and would erase the oil flag). `water_sleep_ticks = 0`
disables packing.

### 4.14 Water constants

| Constant | Value | Units at 5 mm | Basis |
| --- | --- | --- | --- |
| `water_gravity` | $`-1962\,\mathrm{cells/s^2}`$ | $`9.81\,\mathrm{m/s^2}`$ | real gravity |
| `water_max_velocity` | 2400 cells/s | 12 m/s | above full-domain free fall (9.9 m/s); 5 mm drops reach ~9 m/s terminal (Gunn and Kinzer 1949); never cuts a fall |
| `water_cfl_limit` | 16 cells per solve | | solve cost; 3 solves at the cap |
| `water_min/max_substeps` | 1 / 6 | | budget checked before dispatch |
| `water_advection_cfl` | 2 cells per sub-move | 1 cm | FLIP practice of CFL 1-5 with substepped advection (Bridson 2015) |
| `water_flip_blend` | 0.97 | | Zhu and Bridson 2005: 0.95-0.99 |
| `water_velocity_damping` | 1.0 (none) | per 1/120 s | grid transfer dissipation already exceeds real viscosity |
| `water_bulk_damping` | $`1.0\,\mathrm{s^{-1}} \times s`$ (support) | | settles a poured pool in 6-10 s (4.11) |
| `water_rest_density` | 4 particle masses per cell | | brush emits 4 per cell |
| `water_amount_per_particle` | 0.04 | volume units | a cell at rest reads 0.16 |
| `water_density_rate` | 20 /s | | closes air pockets in ~0.05-0.1 s without spikes |
| density error cap / bulk share | 0.5 / 0.75 of rest | | `water_divergence.comp` |
| `water_liquid_phi_threshold` | 0.42 | | the blurred density's contour sits here |
| `water_surface_level` | 0.25 of rest density | | contour of the liquid phi; at 0.5 the dam break fails its density gate, at 0.3 settled pools move 7 times faster and at 0.4 60 times |
| `water_ghost_theta_min` | 0.25 | | clamp of theta, conductance at most 4; 0.1: pools as calm, pour CG +9%; 0.5: pools 7 times faster |
| `water_pressure_iterations` | 1024 | | cap; true-residual check catches a capped solve |
| `water_pressure_residual_threshold` | 0.12 /s (max per cell) | | CG stops at 0.06 |
| `kSandLeak` | 0.005 of an open face | | keeps water sealed in sand nonsingular |
| multigrid | $`\omega = 2/3`$, 1 pre and 1 post sweep, 16 coarsest sweeps, 8 x 8 coarsest | | 4.9 |
| clump jitter | volume $`> 3 \times`$ rest, $`\pm 0.25`$ cells | | 4.11 |
| `water_sleep_speed` / `_ticks` / `water_wake_speed` | 2 cells/s / 30 ticks / 5 cells/s | 1 cm/s, 0.5 s, 2.5 cm/s | 4.13 |
| brush | 4 per cell, (0, -100) cells/s $`\pm 10`$ | 0.5 m/s $`\pm 5`$ cm/s | spout outflow |
| pool | $`2^{21}`$ records, reserve $`2^{17}`$ | 96 MB x 2 | 4.1 |
| P2G fixed-point scales | 4096 (weight), 4096 (velocity) | | 4.4 |

### 4.15 Water limits and failure modes

- Inviscid; turbulence and boundary layers are represented only by bulk damping of supported water.
- The free surface is a ghost-fluid condition on a blurred density (4.8): sub-cell in position along the face axes,
  first order in the contour's shape, and it follows particle noise only at the blur's scale. Settled pools show
  surface std 0.1 cells and level spread 0.03 (the first attempt 0.2-0.4 and 0.24).
- A cell whose blurred density is under a quarter of rest is air to the solver: a single cell of four particles
  surrounded by air reads 0.14 and falls ballistically, as does any sheet one particle thick (sparse spray is
  ballistic). A sheet one cell thick on a floor still reads 0.5 and stays liquid.
- Long sub-moves can still converge records; the clump jitter is a deterministic position-only fix, not a physical
  force.
- The pool holds about half the screen of moving water; at the budget the brush stops.
- Deep pools approach R32F pressure precision (restart from the true residual handles it; a solve that hits the
  1024 cap is caught by the residual check and counted).
- Water sealed inside dense sand drains only through `kSandLeak`.

---

## 5. Oil

### 5.1 Method

Oil is a water record with state2.z = 1 (`water_particle_spawn.comp`, `oilFlag`); compaction copies whole records, so
the flag and the lit state survive. It shares the water grid, pressure solve and density control. The oil-specific
parts run only once oil (or a heat source, for the phase passes) has been painted since the reset
(`oil_present_`, `heat_present_`).

1. Tally (`phase_particles.comp`, coupling pass, every tick): surviving records are counted per cell (oil and all).
   `oil_field.comp` turns the tallies into an RG16F field: x the oil share of the cell's records, y oil in full cells
   (oil records over the rest density, saturating at 1), and clears the tallies.
2. Variable-density correction (`water_particle_step.comp`): the shared solve treats all liquid as water, so its
   pressure gradient accelerates oil too little by $`\rho_w / \rho_{\text{oil}}`$. Every record, water or oil, gains

   $`\displaystyle \Delta\mathbf{v} = \left(\frac{1}{0.87} - 1\right) \chi_{\text{oil}}\, \mathbf{a}\, \Delta t, \qquad \mathbf{a} = \frac{\mathbf{v}_{\text{pic}} - \mathbf{v}_{\text{prev}}}{\Delta t} - \mathbf{g}`$

   with $`\chi_{\text{oil}}`$ the bilinear oil share and the pressure acceleration $`\mathbf{a}`$ capped at
   $`\lVert\mathbf{a}\rVert \le 2g`$. In a pool $`\mathbf{a}`$ is about $`-\mathbf{g}`$ (upward), so the mixture gains
   about $`+0.15\,g`$ where it holds oil and oil bodies rise while water flows around them; in open air and free fall
   $`\mathbf{a} = 0`$, so oil falls at $`g`$ and drips off ceilings like water.
3. In-cell drift: after advection, oil records step up and water records down by $`20\,\mathrm{cells/s} \times \Delta t`$,
   scaled by
   $`\text{drive} = \mathrm{clamp}\,\big(2 \times \text{(other liquid volume in this cell or the one it moves into)} / \text{rest volume}, 0, 1\big) \times s`$
   (support), as a swept position-only move. Nothing separates in free fall.
4. Ignition and burning (section 10.3): exposed oil under hot gas is lit, feeds its gas cell fuel vapour every tick
   and is consumed at 2/s per lit record.
5. Fire suppression ignores oil: `coupling_fire.comp`, `coupling_heat.comp` and the gas mask's water closure scale the
   water amount by $`(1 - \text{oil share})`$.
6. Quench by flash (section 10.4): water landing on or sinking through a burning slick boils off and smothers the gas
   above it.

### 5.2 Why

A force applied to the oil records alone built per-record slip that FLIP kept and shot oil jets out of the pool. The
earlier constant $`(1 - 0.87)\,g`$ times the share pushed oil up even in air: gravity reaches a record only through the
grid, and next to a ceiling the wall faces carry none of it, so the constant push won and oil piled upside down under
ceilings and bowls. Scaling by the solved pressure acceleration makes the correction exactly zero where no pressure
supports the liquid (`app-oil-air`: an open blob drops 65 cells in 0.25 s against 61 for free fall; no slow records
under a rock ceiling after 3 s). The drift is the in-cell separation the shared grid velocity cannot represent: 20
cells/s is the terminal rise of a 5 mm oil drop in water from the Schiller and Naumann (1933) drag correlation (1 mm
drops rise at about 3 cm/s).

### 5.3 Constants

| Constant | Value | Basis |
| --- | --- | --- |
| `kOilDensity` | 0.87 | crude or vegetable oil, $`0.85\text{-}0.92\,\mathrm{g/cm^3}`$ |
| correction factor | $`1/0.87 - 1 = 0.149`$ | variable-density correction |
| correction cap | $`2g`$ | an impact's pressure spike must not fling oil |
| `kOilDrift` | 20 cells/s (10 cm/s) | terminal rise of a one-cell drop |
| `kOilIgnitionTemperature` | 0.3 (0.2 to stay lit) | section 10.3 |
| `kOilBurnRate` | 2 /s per lit record | a 5-cell slick burns out in about 3 s |
| brush | as water: 4 per cell at 0.5 m/s | same spout |

### 5.4 Limits

- One pressure solve for both liquids: oil-water interfaces are diffuse at cell scale and pressure is that of a
  water-density mixture plus the correction.
- The oil share is a per-cell record ratio, so a thin film of a few records is noisy (the renderer filters and dilates
  it).
- Flame spread along a slick is chaotic: one ignition point burned 21-98% of the oil as liquid details changed, so
  `app-oil-burn` lights three spots.

---

## 6. Sand

Sand is an explicit material point method (MPM) continuum with APIC transfers, Hencky elasticity and Drucker-Prager
plasticity in log strain (Klar et al. 2016), extended with a capped volume correction, friction hardening, $`\mu(I)`$
rate dependence (Jop et al. 2006), simplified wet-sand cohesion (after Tampubolon et al. 2017), confined-contact
dissipation and sleep-packing. These are simulation units tuned to observed behaviour, not a validated physical sand.

### 6.1 Records and pool

`SandParticle` is four vec4s (64 bytes):

| Component | Contents |
| --- | --- |
| state0 | position (cells), velocity (cells/s) |
| state1.x | 0 inactive; else 1 + still-tick timer (integer 0-255) + hardening fraction $`q / (q + 4)`$ (rounded stochastically where the timer makes the step coarse, 6.6) |
| state1.y | mass (1) |
| state1.zw, state2.xy | APIC affine matrix (column-major) |
| state2.z | volume correction (remembered log dilation; rest area is mass / reference density) |
| state2.w | friction trait (integer in $`[-31, 31]`$) + moisture (fraction in $`[0, 1)`$) |
| state3 | elastic deformation gradient $`\mathbf{F}`$ (column-major) |

The pool holds `kMaxSandParticles` $`= 2^{20}`$ grains (64 MB) twice. Settled sand leaves the pool for the packed
texture, so the pool bounds moving grains, not the sand on screen.

### 6.2 Spawn

`sand_particle_spawn.comp`: 2 grains per cell of the disc (outside the previous stamp's disc, not on painted solid,
only where occupancy is below 0.225), uniformly random positions in the cell, velocity = brush velocity $`\pm 10`$
cells/s, $`\mathbf{F} = \mathbf{I}`$, no volume correction, a random friction trait in $`[-31, 31]`$, moisture 0.

Pool budget: `RunSpawnSandParticles` skips a whole stamp when `record_bound` $`+\ \text{region area} \times \text{grains per
cell}`$ would exceed the pool less `kSandWakeReserve` (a sixteenth, 65,536 slots kept for packed sand that wakes), as for
water (4.1). The brush stops at a full pool at the same tick every run. Before, the shader reserved each grain by
compare-and-swap at the cap, which dropped grains in atomic order and counted each as overflow (`app-sand-pool-full`).

### 6.3 Tick structure (`RunSandPass`)

1. Preflight (2.5): wave substeps
   $`= \max\big(\lceil \Delta t\, c / 0.4 \rceil,\ \lceil 8\,\eta\,\Delta t / \rho \rceil\big)`$ with wave speed
   $`c = \sqrt{(\lambda + 2\mu)/\rho} = 1084\,\mathrm{cells/s}`$, viscous bound at half the 2D explicit diffusion
   limit; required $`= \max\big(\text{wave},\ \lceil 2400\,\Delta t / 1.0 \rceil\big)`$ must be at most 64.
2. Sleep tally (`sand_sleep.comp`) and pack decision (`sand_pack.comp` phase 0), 6.9.
3. Compaction (`sand_particle_compact.comp`): append-compact live grains (no binning), drop grains of cells flagged to
   pack, deactivate the source, reduce the live cell box and the fastest speed.
4. Pack apply (`sand_pack.comp` phase 1): packs and wakes; woken grains are appended at the compacted cursor and join
   its box and fastest speed.
5. Readback (one sync): live count, fastest speed, box. Substeps
   $`= \max\big(\text{wave substeps},\ \lceil \min(v_{\text{fastest}}, 2400)\,\Delta t / 1 \rceil\big)`$; 1 when no
   grain lives (a substep still clears the velocity field water reads). Default moduli give 46.
6. Node box: the grains' cells plus the one-cell stencil and the distance the fastest can travel this tick, joined
   with last tick's box, so nodes the sand left are zeroed.
7. Per substep: P2G with stress (6.4), grid update (6.5), particle step (6.6).
8. Occupancy transfer: P2G without stress (also gathering moisture), then `sand_rasterize.comp` (6.8).

### 6.4 P2G and Hencky stress

`sand_particle_p2g.comp` per grain, quadratic B-spline weights on the 3 x 3 nodes:

1. Principal stretches of $`\mathbf{F}`$ by polar decomposition (closed-form 2D: rotation from
   $`(F_{00} + F_{11},\ F_{01} - F_{10})`$, then the symmetric part's eigen-decomposition), log strain
   $`\boldsymbol{\epsilon} = \ln\boldsymbol{\sigma}`$.
2. Hencky (St. Venant-Kirchhoff in log strain) Kirchhoff stress, principal
   $`\boldsymbol{\tau} = 2\mu\,\boldsymbol{\epsilon} + \lambda \mathrm{tr}(\boldsymbol{\epsilon})`$, rotated back
   with the left singular vectors.
3. Kelvin-Voigt viscosity for confined grains:
   $`\boldsymbol{\tau} \leftarrow \boldsymbol{\tau} + J\, \chi_{\text{conf}}\, \eta\, (\mathbf{C} + \mathbf{C}^{\mathsf T})`$,
   the confinement $`\chi_{\text{conf}}`$ ramping from 0 at $`J = 0.95`$ to 1 at $`J = 0.85`$.
4. Deposit to each node: mass $`w\,m`$ and momentum

   $`\displaystyle w\, m \big(\mathbf{v}_p + \mathbf{C}_p (\mathbf{x}_i - \mathbf{x}_p)\big) - \Delta t\, V_0\, \boldsymbol{\tau}\, \nabla w, \qquad V_0 = m / \rho_{\text{ref}}`$

   (fixed point 4096 with carry words, like water). Each deposit marks its 16 x 16 node tile in `tile_flags`.

Nonfinite $`\mathbf{F}`$ deposits mass and momentum without stress and counts; the particle step repairs it.

### 6.5 Grid update

`sand_grid_update.comp` per node over the box (one workgroup per 16 x 16 tile; tiles without deposits are skipped
except on the last substep, which leaves the velocity field whole for other passes):

1. $`\mathbf{v} = \text{momentum} / \text{mass}`$; the node zeroes its momentum (the particle step zeroes mass), so the
   grid is cleared once per tick, not per substep.
2. Water around the node (3 x 3 cells of the liquid volume): submersion $`\sigma_w`$ = fullest cell / 0.16, flow
   $`\mathbf{u}_w`$ = volume-weighted water velocity.
3. Gravity with buoyancy: $`v_y \leftarrow v_y - g\,\Delta t\,(1 - \sigma_w / 2.65)`$.
4. Drag: $`\mathbf{v} \leftarrow \mathrm{mix}\,\big(\mathbf{v}, \mathbf{u}_w, 1 - e^{-60\,\sigma_w \Delta t}\big)`$,
   stable at any $`\Delta t`$.
5. `sand_damping` (1.0, none; per 1/60 s).
6. Packed reaction (`packedImpulse`): packed cells around the node push like the frozen grains they hold, with the
   P2G stress term $`-\Delta t\, V\, \boldsymbol{\tau}\, \nabla w`$ of grains at rest: isotropic Kirchhoff stress
   $`(\lambda + \mu) \min(\ln J_{\text{mean}}, 0)`$, quadratic weight and gradient averaged over the cell
   ($`\tfrac16, \tfrac23, \tfrac16`$; gradients $`-\tfrac12, 0, \tfrac12`$). The push cancels motion into the packed
   sand up to the stored stress and never launches a grain. Tangential motion then loses
   $`\min(\text{grip}, \lVert\mathbf{v}_t\rVert)`$ with $`\text{grip} = 0.62 \times \text{reaction} + \text{bond}`$,
   where the bond of damp packed cells is

   $`\displaystyle \text{bond} = \sqrt{2}\, \alpha\, (\lambda + \mu) \times \text{cohesion} \times \mathrm{capillary}(\text{moisture}) \times \overline{e^{-q / \text{softening}}} \times \text{volume} \times \lVert\nabla w\rVert`$

   with the overline the mean over the cell's grains, stored at pack time (6.9), not the exponential of their mean $`q`$.

7. Walls with Coulomb friction (`separate`: remove the inward normal component, scale the tangent by
   $`\max\big(0,\ 1 - 0.62\, \lVert\mathbf{v}_n\rVert / \lVert\mathbf{v}_t\rVert\big)`$): the halo nodes are the
   domain wall; static solid nodes (signed distance $`\le 0`$) use the distance gradient as normal, or for a thin wall
   with open cells on both sides the side holding more sand mass; packed nodes use the gradient of a packed/open field.
8. Nonfinite or $`\lVert\mathbf{v}\rVert > 65504`$ counts and stores 0.

### 6.6 Particle step and plasticity

`sand_particle_step.comp` per grain:

1. Gather $`\mathbf{v}`$, the APIC matrix $`\mathbf{C}_p = 4 \sum w\, \mathbf{v}_i\, (\mathbf{x}_i - \mathbf{x}_p)^{\mathsf T}`$
   and the velocity gradient $`\nabla\mathbf{v} = \sum \mathbf{v}_i\, (\nabla w)^{\mathsf T}`$ from the nodes, zeroing
   each stencil node's mass for the next substep.
2. Contact damping: $`\mathbf{v}`$ and $`\mathbf{C}`$ are multiplied by $`e^{-6\, \chi_{\text{conf}}\, \Delta t}`$,
   $`\chi_{\text{conf}} = \max\big(\text{ramp of } J \text{ from } 0.95 \text{ to } 0.85,\ \mathrm{capillary}(\text{moisture})\big)`$
   (liquid bridges dissipate, Herminghaus 2005); speed cap 2400.
3. Moisture, once per tick (first substep): relax toward the fullest water cell in the 3 x 3 around the grain
   (saturation = fill / 0.16) at 4 /s when wetting, 0.02 /s when drying; capped at 0.999.
4. Trial deformation $`\mathbf{F}^* = \exp(\Delta t\, \nabla\mathbf{v})\, \mathbf{F}`$. The matrix exponential (closed
   form for 2 x 2 via the traceless part) keeps
   $`\det\mathbf{F}^* = \exp(\Delta t \mathrm{tr}\,\nabla\mathbf{v}) \det\mathbf{F} > 0`$; forward Euler
   $`\mathbf{I} + \Delta t\, \nabla\mathbf{v}`$ inverted when a grain at 1500 cells/s met resting sand
   ($`\Delta t\, \lVert\nabla\mathbf{v}\rVert = 1.09`$; `sand-impact`).
5. Friction slope $`\alpha = 0.5\, (1 + 0.2\, \text{trait} / 31)\, h(q)`$, with the Klar hardening

   $`\displaystyle h(q) = \frac{\sin\varphi_F(q)}{\sin 35^\circ}, \qquad \varphi_F(q) = 35^\circ + (9q - 10)\, e^{-0.2 q}\ \text{degrees}`$

   fresh grains start at about 0.74 of $`\alpha`$, peak near 1.3 at $`q`$ around 6, settle to 1.
6. Return mapping (`projectDeformation`) in log strain, with the remembered dilation $`c_v`$ added to the volume
   strain: $`e_v = \epsilon_1 + \epsilon_2 + c_v`$.
   - $`e_v > 0`$ (separated): stress-free, $`\mathbf{F} = \mathbf{R}`$ (rotation only), $`c_v = \min(e_v, 0.14)`$,
     $`q \mathrel{+}= \lVert\boldsymbol{\epsilon}\rVert`$.
   - Otherwise $`c_v = 0`$. Pressure $`p = (\lambda + \mu) \max(-e_v, 0)`$; inertial number
     $`I = \dot\gamma\, d / \sqrt{p / \rho}`$ with
     $`\dot\gamma = \sqrt{2}\, \lVert\mathrm{dev}(\mathrm{sym}\,\nabla\mathbf{v})\rVert`$ and $`d = 0.08`$
     cells; slope $`= \alpha\, \big(1 + 0.68\, I / (0.279 + I)\big)`$ ($`\mu(I)`$).
   - $`\delta = \lVert\mathrm{dev}\,\boldsymbol{\epsilon}\rVert + \dfrac{\lambda + \mu}{\mu}\, \text{slope}\, (e_v - \text{bond})`$,
     $`\text{bond} = 0.15 \mathrm{capillary}(\text{moisture})\, e^{-q / 0.5}`$.
     If $`\delta \le 0`$ (inside the cone) $`\mathbf{F}`$ stays; else the deviator shrinks by
     $`(1 - \delta / \lVert\mathrm{dev}\,\boldsymbol{\epsilon}\rVert)`$ (clamped at the cone tip),
     $`q \mathrel{+}= \min\big(\max(\delta, 0), \lVert\mathrm{dev}\,\boldsymbol{\epsilon}\rVert\big)`$.
   In 2D the cone is $`\sin\varphi = \sqrt{2}\,\alpha`$. The bond widens the cone by that much compression
   (Mohr-Coulomb shear strength at zero pressure) with tension still separating. A deformation that cannot be projected
   counts, resets $`\mathbf{F}`$ to its rotation and keeps the grain moving.
7. $`q`$ is stored back as $`q / (q + 4)`$ in state1.x's fraction. The fraction shares a float with the sleep timer, so
   its step grows with the timer, to $`1.5 \times 10^{-5}`$ at 128-255 ticks, and a creep increment below half of it
   rounded away every substep: a slowly straining grain never hardened (the fraction moves $`dq / 16`$ near $`q = 4`$).
   Where the step is coarse (timer 16 and up, step $`\ge 1.9 \times 10^{-6}`$) the sum is rounded stochastically: the
   exact error of the nearest float, as a share of the step, is the chance of rounding the other way, so the
   expectation is exact. The random number hashes the grain's position and velocity bits and a per-substep counter
   (`roundSeed`), so a run repeats. Finer steps keep round-to-nearest: counting the increments below $`6 \times 10^{-8}`$
   there toughened the damp column of `sand-wet` (it shed a median 0.085 of its grains over 12 perturbed runs against
   0.2-0.3 with nearest rounding, which the wet-sand fracture was tuned with). `sand-hardening`: a grain at timer 255,
   $`q = 4`$, strained $`1.4 \times 10^{-5}`$ a step for 2000 steps gains $`1.77 \times 10^{-3}`$ of fraction (expected
   $`1.76 \times 10^{-3}`$); round to nearest gains 0.
8. Swept move with restitution 0.04 through cells blocked by signed distance $`\le 0`$, packed cells and the domain
   (the domain edge is the outer face of the edge cells), the same exact traversal and last-open-cell clamp as water.

```math
\mathrm{capillary}(m) = \mathrm{clamp}(10\, m, 0, 1) \times \mathrm{clamp}\,\big(10\,(1 - m), 0, 1\big)
```

none dry, full from 10% to 90% moisture, gone at saturation where the liquid bridges merge (Mitarai and Nori 2006).

### 6.7 Why these choices

- Stiffness is the repose root cause. At 196,000 moduli a pile stored elastic shear about
  $`2\alpha\, p / (\lambda + \mu)`$, about 0.6 at a 50-cell base, and sagged to 26-28 degrees whatever $`\alpha`$ was
  ($`\alpha`$ 0.4/0.6/0.9 gave 24.5/26.6/24.1). Moduli x2/x4/x8 at $`\alpha = 0.6`$ gave 31.8/32.8/32.3; x4 (784,000)
  was chosen at 46 substeps. Real sand ($`E \sim 10\text{-}100\,\mathrm{MPa}`$) would need wave speeds of 20,000+
  cells/s. A 100-cell column now compresses to $`J`$ about 0.9.
- $`\alpha = 0.5`$ with hardening: above an effective ~0.6 piles pack looser and spread wider instead of steeper; poured
  piles measure 31.5 degrees (dry repose 30-35 degrees, Al-Hashemi and Al-Amoudi 2018); settled grains average $`q`$
  about 12, factor about 1.2.
- $`\mu(I)`$: $`\mu_s = \tan 20.9^\circ`$, $`\mu_2 = \tan 32.76^\circ`$, $`I_0 = 0.279`$ (Jop et al. 2006); deep beds
  stay near $`\mu_s`$, thin fast surface flows reach $`\mu_2`$ (centre-pour piles 29.3 -> 30.7 degrees, packing off).
- Volume correction cap $`\ln(0.64 / 0.555) = 0.14`$, dense over loose random packing (Scott 1960; Onoda and Liniger
  1990). Uncapped, MPM volume drift overpacked a poured pile fourfold.
- Wet sand: cohesion 0.15 of log volume strain is a Mohr-Coulomb cohesion of about
  $`\tan\varphi\, (\lambda + \mu)\, c`$, about 3.6 kPa at 5 mm (damp sand: a few kPa of suction stress, Lu and Likos
  2004). Softening by plastic strain is the rupture of liquid bridges as grains shear or separate (Willett et al.
  2000): a damp 40 x 80 column keeps a 71-cell core and sheds crumbs. 0.3 stood as one block or toppled whole; 0.075 or
  softening 0.25 slumped to 37-38 cells; a shifted tensile apex (first try) fractured and flattened damp columns at 2
  grains per cell.
- Confined dissipation: without it a deep fill rang for seconds. The viscosity ($`\eta = 200`$) removes grain-scale
  jitter; the contact damping (6 /s) the bed's bulk vertical mode (period about 1 s at these moduli), which is out of
  the explicit viscosity's reach. Free, loose and surface sand ($`J`$ about 1) is untouched.
- Matrix-exponential $`\mathbf{F}`$ update: see 6.6 step 4.
- Per-grain friction spread ($`\pm 20\%`$) makes piles irregular, as natural sand mixes grain shapes.

### 6.8 Occupancy and moisture rasterization

The last P2G ($`\Delta t = 0`$) deposits mass and moisture-weighted mass only. `sand_rasterize.comp` adds packed cells
as grains spread over their cell (quadratic kernel averaged, $`\tfrac16, \tfrac23, \tfrac16`$ per axis) and writes
occupancy $`= \mathrm{clamp}(\text{mass} / 2, 0, 1)`$ (R16F) and mean moisture (R8). A cell at rest density
reads 1; a poured pile 0.4-1; a lone grain peaks at 0.28. Occupancy feeds the masks (section 3), water drag and
coupling, the spawn limit and rendering.

### 6.9 Sleep-packing

**Method.**

1. `sand_sleep.comp` (over `record_bound`): timer = 1 + ticks slower than 4 cells/s (saturating at 255, kept with the
   hardening fraction); tally per cell grains and ready grains (timer $`\ge 60`$, mass 1, $`0 < J < 64`$) and sum the
   ready grains' $`J`$, hardening fraction, moisture and bond $`e^{-q / 0.5}`$ in fixed point ($`\times 4096`$).
2. `sand_pack.comp` phase 0 decides from the previous packed state and this tally, writing only its own tally bits:
   - calm = no open side steeper than 45 degrees and not disturbed. A side is open when the side cell and the cells
     below and above it yield (open, or moving sand falling faster than 4 cells/s). Disturbed: any sand node in the
     3 x 3 faster than 60 cells/s, moving sand driven into the cell faster than 20 cells/s, or half-full water there
     faster than 40 cells/s.
   - arched = packed or solid cells on both lower diagonals.
   - held up: walk down from the cell through packed cells, at most 3 rows (`kSupportDepth`). The first cell that is
     not packed decides: solid or grains hold it up (moving sand holds it until it leaves or disturbs the cell); an
     open cell holds it up only if arched over. Three packed rows in a row count as held.
   - A packed cell wakes when it is not held up or is not calm; if it stays, all-ready grains in it pack on top (up to
     64 per cell).
   - An unpacked cell packs when all its grains are ready, it is calm and supported (solid, packed or all-ready below,
     or arched).
3. Compaction drops exactly the tallied grains of packing cells.
4. Phase 1 applies: a pack adds the count to the R8UI texture and merges the means into `packedFTex` (RGBA32F: mean
   $`J`$, hardening fraction, moisture, bond); a wake reserves all the cell's grains or none (compare-and-swap; a full
   pool keeps it packed and counts the overflow) and recreates them at hashed positions with the cell's sand-node
   velocity, isotropic $`\mathbf{F} = \sqrt{J}\, \mathbf{I}`$, the mean moisture, a fresh trait and the hardening split
   by the mean bond $`\bar b`$: $`\mathrm{round}(\bar b N)`$ of the $`N`$ grains come back intact ($`q = 0`$) and the
   rest cracked at the fraction that keeps the cell's mean hardening, $`\bar h N / (N - n_{\text{intact}})`$. Packed
   cells that neither pack nor wake soak up and dry like moving grains. The phase zeroes the tally and sums it consumed.

While packed, cells are walls at their own nodes and push on neighbouring nodes (6.5 step 6), count in occupancy, and
are cleared by erase and solid brushes.

**Why.** Moving plus packed is an exact integer invariant (`app-sand-sleep-conservation`). Packed cells must push like
frozen grains: with bare walls the first moving layer above a packed surface bore its load alone, compacted and packed
denser (1.76 against 1.47 grains per cell) and packed piles came out flat (38 cells tall against 48). Frictionless,
the reaction let flows slide off and spread; inert grid mass instead stuck flows and built 54-cell piles at 41-46
degrees. Readiness 60 ticks (1 s) spans a period of the pile's bulk mode: an unpacked pile reaches its 32-33 degrees by
relaxing for a second or two after each flow (dynamic angle below the static one, Daerr and Douady 1999; the BCRE
picture of a static bed eroded or fed by a flowing layer, Bouchaud et al. 1994, Douady, Andreotti and Daerr 1999).
With 0.2 s readiness, grains still for a moment of that motion packed and froze the steeper shape (sand-sleep-bias,
eight seeds: 12 ticks 37.4 degrees, 40 ticks 33.9, 60 ticks 32.7 against 32.5 unpacked). Rejected: BCRE erosion
(a layer faster than 10 cells/s above a packed cell wakes it) reached 34.9 degrees but leaned piles (centroid 1.2
cells against 0.3) and woke quiet ones; slope-gated packing left 6-10% of grains moving at 36 degrees; longer readiness
only at the free surface stood at 38.5; restoring the mean stretch on wake measured no better than isotropic $`J`$.
Damp grains pack like dry ones and their packed cells grip by their bonds; before, a damp clump with a packed core shed
its moving rim layer by layer, and letting bonded grains pack without the grip collapsed a column to 43.6 cells.

Hole collapse. Only the packed cell directly above a hole used to wake: any grain below held a cell up, so each row above
waited for the one under it to fall away. A packed pile took hundreds of ticks to sink into a hole a moving pile fills in
20 (erase at the foot of a 48-cell pile: peak 48 to 40 in 10 ticks moving; packed 46 at tick 40, 42 at 80, 39 at 240),
and its collapse peak stood 2.1 cells higher (40.3 against 38.2 over 16 seeds, 40.7 against 38.3 over 32 more). The cell
now looks down through up to three packed rows to the first cell that is not packed, so loss of support climbs three rows
a tick. The pace is measured, not derived (collapse peak over 16 seeds, moving 38.2): 1 row 40.3, 3 rows 39.0, 4 38.7,
6 37.8, 12 36.8; past that the pile ends lower and denser than the moving one. Over 48 seeds with three rows the packed
collapse peak is 38.7 against 38.3. Treating grains that fall faster than 4, 10 or 20 cells/s as no support collapsed a
little better (38.4 with three rows) but kept 270 grains of a settled pile moving (`app-sand-sleep-no-flicker`).

### 6.10 Cost reductions

- Tally and compaction scan only `record_bound`; compaction deactivates what it reads, so the scratch pool is never
  cleared; the apply phase clears the tally it consumed (they were cleared in full, 20 MB, per tick).
- P2G and the particle step dispatch the live count; grid passes cover the live box joined with last tick's.
- The grid is cleared once per tick: the grid update zeroes momentum it reads, the particle step zeroes the mass of its
  stencil, and P2G marks deposit tiles so the grid update skips empty ones except on the last substep (the sparse-grid
  idea of GPU MPM, Gao et al. 2018).
- One substep when no grain lives.

The combined replay's sand pass fell from 3.9-4.1 to 2.7-2.8 ms at the same 46 substeps. A per-substep `glFlush` made
it worse (3.9 ms); setting uniforms once per tick gained under 0.1 ms; neither kept.

### 6.11 Sand constants

| Constant | Value | Units at 5 mm | Basis |
| --- | --- | --- | --- |
| `sand_gravity` | $`1962\,\mathrm{cells/s^2}`$ | $`9.81\,\mathrm{m/s^2}`$ | real gravity |
| `sand_max_velocity` | 2400 cells/s | 12 m/s | as water |
| `sand_shear_modulus`, `sand_lame_lambda` | 784,000 | simulation units | $`4 \times (24000 \times 1962/240)`$; repose root cause (6.7) |
| wave speed | $`\sqrt{(\lambda + 2\mu)/\rho} = 1084\,\mathrm{cells/s}`$ | 5.4 m/s | |
| `sand_cfl_limit` / `sand_advection_cfl` | 0.4 / 1.0 cell | | explicit MPM stability (Jiang et al. 2016); the larger of the two, not the sum |
| substeps | 46 (wave), 14 (viscous), 40 (advective at the cap); max 64 | | |
| `sand_reference_density` | 2 grains per cell | saturated/packed sand $`1.9\text{-}2.1\,\mathrm{g/cm^3}`$ | occupancy unit |
| `sand_friction_alpha` | 0.5, $`\pm 20\%`$ per grain (`sand_friction_variation`) | $`45^\circ`$ nominal cone | $`31.5^\circ`$ measured piles |
| Klar hardening $`h_0, h_1, h_2, h_3`$ | 35, 9, 0.2, 10 degrees | | Klar et al. 2016 |
| `sand_friction_rate_gain`, `sand_inertial_i0`, `sand_grain_diameter` | 0.68, 0.279, 0.08 cells | 0.4 mm grains | Jop et al. 2006 |
| `sand_max_dilation` | 0.14 | log volume | $`\ln(0.64/0.555)`$ |
| `sand_cohesion`, `sand_cohesion_softening` | 0.15, 0.5 | ~3.6 kPa | Mitarai and Nori 2006; Lu and Likos 2004; Willett et al. 2000 |
| `sand_wetting_rate`, `sand_drying_rate` | 4 /s, 0.02 /s | | soaks in a fraction of a second, dries in about a minute (a sandbox compromise) |
| `sand_viscosity` | 200 | mass / (cell s) | confined grains |
| `sand_contact_damping` | 6 /s | | confined or damp grains |
| `sand_friction_coefficient` (walls, packed) | 0.62 | | quartz on rock 0.5-0.7; about $`\tan 31.5^\circ`$ |
| `sand_restitution` | 0.04 | | |
| `sand_damping` | 1.0 (none) | per 1/60 s | dissipation from plasticity and friction |
| `sand_drag`, `sand_grain_density` | 60 /s, 2.65 | | section 7 |
| `sand_spawn_occupancy_limit` | 0.225 | | brush stops stacking |
| `sand_sleep_speed`, `_ticks` | 4 cells/s, 60 ticks | 2 cm/s, 1 s | 6.9 |
| `kSupportDepth` | 3 packed rows a tick | | measured pace of the collapse front, 6.9 |
| `kSandWakeReserve` | $`2^{20} / 16`$ = 65,536 slots | | kept for packed sand that wakes, 6.2 |
| wake speeds impact / press / water | 60 / 20 / 40 cells/s | | 6.9 |
| `kMaxPackedGrains` | 64 per cell | | deep beds compress to $`J`$ about 0.25 (7-8 per cell) |
| brush | 2 grains per cell, $`\pm 10`$ cells/s | | |

### 6.12 Sand limits and failure modes

- Elastic stiffness is far below real sand, so piles still compress elastically ($`J`$ about 0.9 at a 100-cell base)
  and the stress-wave substeps dominate the cost; softer or pressure-dependent stiffness would bring back the sag, an
  implicit elastic solve is out of scope.
- The repose angle emerges from transfer dissipation, hardening and packing, not from $`\alpha`$ alone.
- A regular one-grain-per-cell lattice is a degenerate MPM bed that creeps at 1-7 cells/s and never packs (the brush
  and wakes always jitter positions). The acceptance benchmark's sand block used to be such a lattice; it is now two
  hashed grains per cell, the brush's bed (VALIDATION, Sand pass 5).
- Packed piles stand within about 1.5 degrees of unpacked ones on average (flank 33.5 against 32.1 over 48 seeds; the
  8-seed gate allows 2) and individual seeds lean (the 8-seed mean absolute centroid offset is 0.74 packed against
  0.29, with the gate 0.5 apart); a collapse peak matches within 0.5 cells (38.7 against 38.3 over 48 seeds).
- Packing is cell-mean for $`J`$ and moisture; hardening and bond come back as two groups per cell (intact, $`q = 0`$,
  and cracked) that keep the cell's means, not each grain's own state.
- The hardening state is rounded stochastically only where the sleep timer makes its step coarse (timer 16 and up):
  unbiased, with up to a step (1.5e-5) of error per update. Below that, increments under 6e-8 of the fraction still round
  away (6.6).
- Only the surface of a dense body sees water drag and buoyancy (section 7).

---

## 7. Sand-water coupling

### 7.1 Method

1. Moving porous wall: water sees sand where occupancy exceeds a quarter (mask 2 or 3). `water_mac_build.comp` gives
   faces between water and sand the sand grid velocity; divergence and projection keep it (no gravity on such faces),
   so the pressure solve pushes water out of the way and the level rises.
2. Porosity: faces against sand conduct at $`\kappa`$ = `kSandLeak` = 0.005 of an open face in the operator, the
   residual check and the projection (4.8, 4.10); the multigrid coarsens sand-only footprints to porous walls.
3. Caught records: a water record inside a sand cell (`escapeSand`) is carried a whole number of cells along the
   shortest axis to open water (never across a painted wall, up to 24 cells), keeping its place in the cell; its
   velocity gains the sand's outward speed $`\max\big((\mathbf{v}_{\text{sand}} - \mathbf{v}) \cdot \mathbf{n}, 0\big)`$;
   P2G skips it while caught.
4. Drag on water: `water_particle_step.comp` relaxes each record toward the sand velocity at
   $`k_{\text{drag}} \times \rho_{\text{ref}} \times \phi_s = 120\,\phi_s\,\mathrm{s^{-1}}`$ (sand_drag times reference
   density times bilinear occupancy), exponentially, before the speed cap.
5. Drag and buoyancy on sand: `sand_grid_update.comp` nodes gather the water in their 3 x 3 cells; submersion is the
   fullest cell over 0.16, the flow the volume-weighted velocity; gravity loses $`1 / 2.65`$ of its weight when
   submerged, and the velocity relaxes toward the flow by $`1 - e^{-60\, \sigma_w \Delta t}`$.
6. Wake: packed sand wakes when half-full water in its 3 x 3 flows faster than 40 cells/s.
7. Moisture: grains and packed cells wet toward the surrounding water fill (6.6 step 3).
8. Domain edge: grains and water both reach the domain face and deposit into the same edge cells; water reads the sand
   velocity only at interior cell nodes, so coupling is unchanged there.

### 7.2 Why

The porosity is needed: water sealed inside sand is otherwise a singular system with a nonzero net flux, which blew the
solver up (thousands of nonconverged cells in the `sand` replay). The escape removed frozen water: a 400 x 100 pool
with 2900 poured grains had 6,100-13,400 records frozen inside sand and a level that never moved; after, none, and the
surface rises 2.7 cells (about the 3.7 the sand's footprint displaces). The drag rates are equal and opposite:

```math
F = \rho_s\, \phi_s\, k_s\, \Delta v = \rho_w\, \phi_w\, k_w\, \Delta v
```

The settling speed $`g\,(1 - 1/2.65) / 60 = 20\,\mathrm{cells/s}`$ (10 cm/s) matches a dense cloud of 0.3-0.5 mm grains
(Ferguson and Church 2004 give 4-7 cm/s per grain); on the grid a grain falls at -47.7 cells/s after 0.2 s in air,
-24.1 with buoyancy alone and -9.0 with drag and buoyancy (`sand-coupling`). The sand grid velocity is RG32F: binary16
stalled fast sand and added gradient noise.

### 7.3 Limits

- Sand nodes inside a dense body see no water, so only its surface is dragged and buoyed; dense bodies sink faster than
  dilute grains.
- Sand below a quarter of rest density neither walls nor displaces water.
- Water moved by the escape or the drag is not pushed back to its old density by anything but the density correction.

---

## 8. Gas: smoke, fire and temperature

The gas is an Eulerian smoke-and-fire solver on the 500 x 500 gas grid (1 cm cells): semi-Lagrangian transport with a
limited MacCormack correction (Selle et al. 2008), buoyancy and vorticity confinement (Fedkiw et al. 2001), a
combustion model with an expansion source (Nguyen et al. 2002), an open top and a flexible MGPCG pressure solve.

### 8.1 Fields

| Field | Format | Notes |
| --- | --- | --- |
| u, v faces | R32F, ping-pong + scratch | at the 300 cells/s cap a half-float face steps by 0.25 cells/s, the size of the residual gate |
| centre velocity | RG16F | face average, for traces |
| smoke, temperature, fuel, oxidizer | R16F, ping-pong + scratch | oxidizer starts at 1 (fresh air) |
| reaction | RG16F | x: fuel burned per second (expansion source); y: flame |
| pressure, divergence | R32F | divergence was R16F; see 8.5 |

### 8.2 Stepping: one substep per tick, one pressure solve per tick pair

`RunGasTick` runs `RunGasPass(dt, project = false)` on every tick. The substep count is
$`\max(\text{transport}, \text{diffusion})`$, and at the default $`\Delta t = 1/60`$ s

```math
\text{transport} = \left\lceil \frac{\sqrt{2} \times 300 \times \tfrac{1}{60}}{8} \right\rceil = 1,
\qquad
\text{diffusion} = \left\lceil (10 + 2.5) \times \tfrac{1}{60} \right\rceil = 1
```

so each tick transports and forces the gas (`RunSmokePass`) and runs combustion and heat (`RunFireHeatPass`) once. The
pressure solve is `RunGasProjectionStage(begin, finish, budget)` over two ticks:

1. First tick of a pair (the first tick after `InitializeState`, then every second one), after its substep: copy the gas
   mask to `mask_solve`; `smoke_divergence.comp` on the velocity just advected; footprint kinds and pocket means; the
   first residual check, $`\mathbf{s} = \mathbf{b} - L\mathbf{x}`$ and the first V-cycle; then CG iterations up to
   $`\max(0, (n - 2) / 2)`$ with $`n`$ the previous solve's iteration count. The first half also carries the setup and
   the preconditioner's first V-cycle, the second ends with the final check and the projection, hence the 2.

2. Second tick, before its substep: the remaining iterations to the stop rule (8.5) or the cap, the final residual
   diagnostic, `smoke_project.comp` on the faces as they are then, and the centre velocity. Its substep follows from the
   projected field.

The CG state ($`\mathbf{x}, \mathbf{s}, \mathbf{z}, \mathbf{d}, \mathbf{q}`$, the scalars $`\mathit{rz}, \alpha, \beta`$,
the multigrid levels), the iteration count and the check schedule persist between the halves, so a split solve is the
one-call solve: from identical states the pressure and the projected faces are bit identical (`app-gas-split-solve`).
The operator is one system, too: the kinds, the residual checks and `smoke_project.comp` read `mask_solve`, a copy of
the gas mask taken when the solve began, because water, sand and rock change the live mask every tick. On the live mask
the final diagnostic counted 79 failing cells when rock was painted between the halves. `RunGasProjection()`, and
`RunGasPass(dt)` with its default flag, do a complete solve in one call (fixtures).

**Coupling lag.** The velocity is projected one tick after its divergence was measured. Brush stamps, coupling and the
water and sand passes act in between, so a stamped velocity (up to 68 /s of divergence at the brush edge in the fire
scene) is corrected by the next solve instead of this one, and a face beside a cell that became solid keeps its projected
value until the next substep's advection zeroes it. The 30 Hz step had the same wait for a stamp made on its off ticks.
Scalars (smoke, temperature, fuel, flame) now step every tick instead of every other one. Substeps alternate between
advecting with the unprojected velocity (the one after a pair's second substep) and the projected one, the 30 Hz
step's alternation in the other order.

**Why.** The 30 Hz step put two substeps and the whole solve on one tick (about 5.6 ms) and nothing on the other, so
frames alternated heavy and light and the p95 and maximum frame barely moved. The solve is about 90% of the gas time
(transport and combustion about 0.6 ms of 6 per tick), so splitting the transport alone could not balance it; the
solve itself is split. The pair costs what the 30 Hz step did (240 solves in `benchmark-realtime` either way), but each
tick carries half of it. Measurements are in VALIDATION.

### 8.3 Transport

Per transport substep (`RunSmokePass`):

1. `gas_velocity_advect.comp`: forward semi-Lagrangian trace of every open face (centre velocity, bilinear, capped
   per component at 300) into the scratch faces.
2. `gas_face_to_center.comp` on the scratch faces.
3. `smoke_advect.comp`: forward trace of the five scalars into the scratch fields.
4. `smoke_correct.comp`: MacCormack correction
   $`\psi = \psi_{\text{fwd}} + \tfrac{1}{2}\big(\psi^n - \mathrm{reverse}(\psi_{\text{fwd}})\big)`$, the reverse
   trace following $`\mathbf{u}^n`$; clamped to the min/max of the forward trace's admissible donors (the limiter);
   then dissipation.
5. `gas_velocity_correct.comp`: the same correction and limiter on the faces, then velocity dissipation, vorticity
   confinement and buoyancy (8.4), clamped to $`\pm 300`$.
6. Swap ping-pong buffers; project on the last substep.

Wall handling in every trace (`clipTrace`, `transportStencil`): a trace walks the cells it crosses and stops before a
solid face or a sealed diagonal corner; each bilinear donor must be open (for a face component, both cells of the
face) and reachable from the stopped point by an unblocked trace, otherwise its weight is dropped and the rest
renormalised; with no admissible donor the cell keeps its own value. The limiter uses those same donors. Traces stop at
walls, so a trace of several cells is safe at CFL 8.

Dissipation, applied after the limiter (so original extrema cannot restore it) and scaled by elapsed time: smoke
$`0.996^{60\Delta t}`$, temperature, fuel and flame $`0.996^{60\Delta t}`$, velocity $`0.994^{60\Delta t}`$; oxygen does
not dissipate.

### 8.4 Forces

On v faces: $`\Delta v = (1200\, T - 10\, \text{smoke})\, \Delta t`$ ($`\mathrm{gas\ cells/s^2}`$), using the face
average of the two cells. Vorticity confinement on both faces:
$`\mathbf{f} = \varepsilon\, (\mathbf{N} \times \boldsymbol{\omega})`$ with $`\boldsymbol{\omega}`$ the curl of the
centre velocity, $`\mathbf{N}`$ the unit gradient of $`\lvert\omega\rvert`$ and $`\varepsilon = 40\,\mathrm{s^{-1}}`$,
averaged from the two adjacent cells (Fedkiw et al. 2001). Source and forward-trace velocities obey the component cap;
the projection may exceed it without changing its correction.

### 8.5 Projection

`RunGasProjection`:

1. `smoke_divergence.comp`: $`b = \nabla\cdot\mathbf{u} - 2\, r_x`$ per open cell, with $`r_x`$ = `reaction.x`
   (burning cells ask to expand by the volume released per unit fuel burned). R32F: as half floats the right-hand
   side's rounding reached 0.5 /s in the combined scene, twice the gate, and the projected field kept it
   (`app-gas-rhs-precision`: projected divergence error max 0.51 -> 0.14 /s, RMS 0.025 -> 0.0023, iterations
   unchanged).
2. `gas_pressure_kind.comp` builds footprint kinds once per solve for every level: level 0 from the gas mask
   (0 fluid, 4 porous sand for mask 3, 1 wall); a coarse cell is fluid when any child is, porous when all are solid
   and one is porous, else a wall; footprints past an odd edge count only their children inside the domain.
3. Sealed pockets (`RemovePocketMeans(true)`, `pressure_pockets.comp` on the level-0 kinds): once burning has been
   painted (`oil_present_` or `heat_present_`), each region with no open-top or porous neighbour has its right-hand
   side mean removed, as for water (4.7). No pin is needed: the coarse kinds merge thin walls into fluid, and with
   the R32F right-hand side a sealed fire burned 30 s with bounded pressure and clean counters
   (`app-gas-sealed-pocket`), where the half-float remainder broke CG within 13 ticks.
4. Flexible PCG (`gas_cg.comp`), warm-started from the previous pressure. Phases: 0
   $`\mathbf{s} = \mathbf{b} - L\mathbf{x}`$; precondition (V-cycle, then 4: $`\mathbf{s}\cdot\mathbf{z}`$, 5:
   $`\beta = (\mathbf{s}\cdot\mathbf{z} - \mathbf{s}\cdot\mathbf{z}_{\text{old}}) / \mathit{rz}`$ (flexible,
   Polak-Ribiere form; $`\mathbf{z}_{\text{old}}`$ is the previous preconditioned residual, $`\mathit{rz}`$ the
   $`\mathbf{s}\cdot\mathbf{z}`$ of the current direction), 6: $`\mathbf{d} \leftarrow \mathbf{z} + \beta\,\mathbf{d}`$);
   per iteration 1: $`\mathbf{q} = L\mathbf{d}`$ and $`\mathbf{d}\cdot\mathbf{q}`$, 2: $`\alpha`$ (one workgroup,
   fixed-order sums), 3: $`\mathbf{x} \mathrel{+}= \alpha\,\mathbf{d}`$, $`\mathbf{s} \mathrel{-}= \alpha\,\mathbf{q}`$.
   Iterations past round-off ($`\lvert\mathit{rz}\rvert < 10^{-10}\, \mathit{rz}_0`$) freeze $`\mathbf{x}`$.
5. V-cycle preconditioner over six levels (500, 250, 125, 63, 32, 16): each level runs two red-black Gauss-Seidel
   sweeps before its coarser level and two in reverse colour order after; the 16 x 16 bottom runs 16 red-black pairs
   then 16 black-red pairs in one workgroup, so the cycle is symmetric. A level's four colour steps are one dispatch
   (`gas_pressure_relax.comp`, `smoothSteps`): each 16 x 16 tile stages a 5-cell halo in shared memory, runs the steps
   with the valid region shrinking a cell per step and writes to the other of two textures; the pre-smoothing also
   writes the residual, the post-smoothing first adds the coarse correction (prolongation fused). Restriction
   (`gas_pressure_restrict.comp`) is the exact transpose of bilinear prolongation (weights summing to 4; a wall or
   out-of-range donor replaced by the fine cell's own parent). Every level uses the unscaled 5-point operator.
6. Boundary conditions: walls Neumann; dense sand (gas mask 3) conducts at $`\kappa`$ (`kSandLeak`) toward zero
   pressure; the top edge is open air (`gas_open_top`, zero pressure at the ghost row) and the other edges are walls;
   on coarse levels the open top weighs $`h / (\text{distance to the fine ghost row})`$, which matters on odd levels
   whose top row straddles the edge.
7. Stopping (`needs_iteration`, a blocking readback of the stored-operator residual from `pressure_residual.comp`):
   checked before the first iteration, at $`\max(3, \text{previous count} - 1)`$ and then every two iterations; stop
   when $`\max\lvert r\rvert \le \tfrac{2}{3} \times 0.25`$ and $`\mathrm{RMS} \le 0.25 / 15`$. Cap 48. The independent
   final check counts every cell above 0.25 /s.
8. `smoke_project.comp`: open faces $`\mathbf{u} \mathrel{-}= \nabla p`$; the top faces carry the outflow
   ($`v_{\text{top}} \mathrel{+}= p`$ of the top cell); a face between open gas and dense sand (mask 3) gets
   $`\mathbf{u} \mathrel{-}= \kappa\, \nabla p`$ with zero pressure on the sand side, the weight the operator gives it;
   faces with a wall on a side, or sand on both, are 0. Then centre velocity.

**Why.** Burning releases volume, so the system needs an outlet: the open top lets hot gas and smoke leave through the
cave roof. A standalone V-cycle iteration (the previous version) averaged 27 cycles in the combined benchmark with 143
of 960 solves at the cap and 1835 residual failures; MGPCG averaged about 16 iterations, at most 21, with none, at that time (now 14.6 and 18 in `benchmark-realtime`, 8.2). A
deliberately broken solve (CG update disabled) reports about 2.1 million residual failures in the fire scenes, so the
gate is live. Gas sealed in sand pockets otherwise formed singular systems and CG broke down there, hence the porous
sand. Omitting mixed coarse footprints as zero-correction cells stalled the solve near ledges (2-4 residual after 24
cycles). The residual threshold (0.25 /s) is about one half-float step of a face at the 300 cells/s cap,
$`8 \times 10^{-4}`$ of the cap; it is tied to the speed cap, and the faces are now R32F so the gate sits above their
rounding.

### 8.6 Combustion and heat

`fire_rd.comp` per open gas cell, every transport substep:

1. $`\text{ignition} = \mathrm{smoothstep}\,\big(0.18, 0.55, \max(T, \text{flame})\big)`$.
2. $`\text{consumed} = \min\big(10\, \Delta t \times \text{fuel} \times \text{oxidizer} \times \text{ignition},\ \text{fuel},\ \text{oxidizer} / 1.5\big)`$.
3. $`\text{fuel} \mathrel{-}= \text{consumed}`$; $`\text{oxidizer} \mathrel{-}= 1.5\, \text{consumed}`$, then relaxes toward
   1 by $`1 - e^{-3\Delta t}`$ (entrained air).
4. $`\text{flame} = \mathrm{clamp}\,\big(\text{flame}\, e^{-8\Delta t} + \text{consumed}\big)`$;
   $`r_x = \text{consumed} / \Delta t`$ (`reaction.x`).
5. $`T = \mathrm{clamp}\,\big(T + 1.6\, \text{consumed} + (10 \mathrm{lap}(T) - 2.5\, T)\, \Delta t\big)`$;
   $`\mathrm{lap}`$ is a nine-point stencil (edges 0.2, diagonals 0.05, weights summing to 1) with no flux
   through walls or between sealed corners. Its effective diffusivity is
   $`0.3 \times 10 = 3\,\mathrm{gas\ cells^2/s}`$ (hot air about
   $`2 \times 10^{-4}\,\mathrm{m^2/s} = 2\,\mathrm{gas\ cells^2/s}`$). The preflight keeps
   $`(D + \text{cooling})\,\Delta t \le 1`$ so the centre weight stays nonnegative.
6. $`\text{smoke} = \mathrm{clamp}\,\big(\text{smoke} + 0.35\, \text{consumed} - 0.08\, \text{smoke}\, \Delta t\big)`$.

The fire brush adds fuel only (no oxygen), so burning starves the core and the flame sits on its edges. A released
fire is out within about 0.5 s (`app-fire-dynamics`: under 5% at 1 s; a held fire rises 139 cells above its source
within 0.5 s of release).

### 8.7 Water and sand against the gas

- Water closes gas cells: a gas cell whose four cells all hold at least half the rest volume of water (not oil) is gas
  mask 3 (3.1), so smoke and flame cannot cross a pool; the gas cell over its surface stays open so surface water
  still meets the hot gas (`app-gas-water`).
- `coupling_fire.comp` per gas cell, every tick: water $`W`$ = 2 x 2 mean of liquid volume $`\times (1 - \text{oil share})`$;
  sand $`S`$ = 2 x 2 mean occupancy; suppression
  $`s = \max\big(\mathrm{clamp}(2.4\, W), \mathrm{clamp}(S / 0.35)\big)`$;

  $`\displaystyle \text{flame} \leftarrow \text{flame} \max(1 - 2.2\, s, 0)^{60\Delta t}, \qquad \text{fuel} \leftarrow \text{fuel}\, (1 - 0.85\, s)^{60\Delta t}, \qquad \text{oxidizer} \leftarrow \text{oxidizer} \max(1 - 1.2\, s, 0)^{60\Delta t}`$

  Sand at 0.35 mean occupancy smothers a cell like a fully wet one.
- `coupling_heat.comp`: $`T \leftarrow T\, \big(1 - \mathrm{clamp}(1.68\, W)\big)^{60\Delta t}`$.

### 8.8 Smoke and fire brushes

`spawn_smoke.comp` per gas cell overlapping the disc: stamps take the larger of field and brush values (velocity: the
larger magnitude, capped), plus the cursor velocity at half (gas cells) and a $`\pm 15`$ cm/s random spread per gas
cell, so overlapping or held stamps keep a steady source instead of saturating. Smoke: density 0.5, $`T = 0.2`$ (warm
smoke, about 180 K above ambient), 0.2 m/s up. Fire: fuel 0.6, $`T = 0.6`$, flame 0.4, 0.2 m/s up, blocked where the
cell has water. Each invocation writes its own left and bottom faces (and the right or top face only at the grid
edge), so max stores need no atomics; clearing also zeroes the shared right and top faces, since zero stores are
idempotent.

### 8.9 Gas constants

| Constant | Value | Units | Basis |
| --- | --- | --- | --- |
| `gas_max_velocity` | 300 gas cells/s | 3 m/s | buoyant flames and plumes rise at about 2 m/s (McCaffrey 1979) |
| `gas_cfl_limit`, substeps | 8, 1-2 | | traces stop at walls |
| `kGasTickStride` | 2 | | ticks per pressure solve |
| `gas_buoyancy_temperature`, `_smoke` | 1200, 10 $`\mathrm{gas\ cells/s^2}`$ per unit | | $`g\, \Delta T / T`$ about $`3g`$ at $`T = 1`$; flame tips reach 1-2 m/s within 0.2 s |
| `gas_vorticity` | 40 /s | | Fedkiw et al. 2001; flames puff at a few Hz (Cetegen and Ahmed 1993) |
| `gas_expansion` | 2 per unit fuel | | burnt gas 5-7x less dense; Nguyen et al. 2002 |
| `gas_combustion_rate`, `gas_oxidizer_consumption` | 10 /s, 1.5 | | fuel burns within 0.1-0.3 s in fresh air |
| `gas_oxygen_rate` | 3 /s | | entrainment |
| `gas_reaction_decay` | 8 /s | | a flame dies within about 0.1 s of losing fuel |
| `gas_heat_release`, `gas_smoke_yield` | 1.6, 0.35 | | |
| `gas_temperature_cooling` | 2.5 /s | | plume gas cools in a fraction of a second |
| `gas_temperature_diffusion` | 10 (effective 3 $`\mathrm{gas\ cells^2/s}`$) | | the old 0.045 changed R16F temperature less than its rounding |
| `gas_smoke_dissipation` | 0.08 /s | | |
| dissipation density / scalar / velocity | 0.996 / 0.996 / 0.994 | per 1/60 s | |
| `gas_water_cooling` | 2.4 (heat $`0.7\times`$) | | |
| `kSandSmother` | 0.35 | occupancy | |
| `gas_pressure_iterations` | 48 | | cap |
| `gas_pressure_residual_threshold` | 0.25 /s | | 8.5 |
| multigrid | 6 levels, 2 RB sweeps, 16 bottom pairs | | 8.5 |

### 8.10 Gas limits and failure modes

- Half resolution (1 cm), and one pressure solve per two ticks: the velocity is projected one tick after its divergence
  was measured (8.2, coupling lag), and a velocity stamped in between is corrected a solve later.
- Semi-Lagrangian transport is diffusive even with MacCormack; vorticity confinement restores curls but is not a
  turbulence model.
- Smoke and temperature are bounded scalars, not mass or energy; the expansion source is the only density effect.
- Gas leaks into dense sand and submerged cells at `kSandLeak` and is not tracked there: those cells hold no gas
  unknowns, so the leaked volume leaves the system (as through the open top). Until v0.6.0 `smoke_project.comp`
  wrote 0 on those faces, inconsistent with the operator: with sand dropped through a flame the projected divergence
  reached 15.9 /s (max) against the 0.25 gate; with the weighted correction it is 0.165 (`app-gas-rhs-precision`).
- Fire inside a fully sealed pocket stays bounded only through mean removal.
- Steam does not push the gas (section 10).

---

## 9. Wood

### 9.1 Method

Wood is painted material with a per-cell state machine in the material value:

| Value | State |
| --- | --- |
| 3 | intact |
| $`256 + f`$ | burning, $`f = 0, \dots, 255`$ fuel left |
| $`512 + f`$ | charred and out, $`f`$ fuel left |

All states are solid (material $`\ge 2`$) for every solver, the masks and the signed distance. `RunWoodPass` runs
`wood_burn.comp` once per tick (when wood has been painted since the reset), one gas cell per invocation, so each
invocation owns the four material cells it writes:

- Mode 0 (update): heat = hottest temperature in open gas cells within two gas cells (the gas mask's one-cell halo
  closes the cells right next to wood); wet = largest water amount in the 3 x 3 around the cell.
  - wet $`> 0.05`$: a burning cell becomes charred ($`512 + f`$), keeping its remaining fuel.
  - burning: loses one fuel unit per tick; at 0 it becomes empty (material 0).
  - intact or charred, heat $`\ge 0.35`$: catches with probability 0.02 per tick (hash), becoming $`256 + f`$ (intact
    starts with $`f = 255`$; charred keeps what it had, so charred wood can re-ignite).
- Mode 1 (fuel feed): each open gas cell counts burning wood cells in the 10 x 10 simulation cells around it (past the
  mask halo); $`\text{feed} = \min(\text{count} / 10, 1)`$; $`\text{fuel} = \max(\text{fuel}, 0.45\, \text{feed})`$,
  $`T = \max(T, 0.6\, \text{feed})`$, $`\text{flame} = \max(\text{flame}, 0.3\, \text{feed})`$. The gas burns that
  vapour into flame, heat and smoke, and that heat lights the next wood.

The static distance field is rebuilt every 15 ticks once wood has been painted since the last reset, since burnt cells vanish without a readback.
The composite draws wood as planks that char to grey ash as fuel goes and glow while burning (11.3).

### 9.2 Why

A per-tick catch chance (mean 50 ticks, about 0.8 s while hot) makes fire creep along a beam instead of jumping its
length in one tick; 255 units at one per tick burn a cell in about 4.25 s. Feeding with max, not add, keeps the source
steady like the fire brush. `app-wood-fire`: a 400 x 10 beam lit at one end caught at 0.5 s, the fire reached 260
cells along at 8 s, and 969 of 4000 cells had burnt away 6 s later, with clean counters.

### 9.3 Constants and limits

| Constant | Value |
| --- | --- |
| ignition temperature | 0.35 |
| catch chance | 0.02 per tick while hot |
| burn time | 255 ticks (about 4.25 s) per cell |
| wet threshold | water amount 0.05 in the 3 x 3 |
| feed | fuel 0.45, $`T`$ 0.6, flame 0.3 at 10 burning cells |

Limits: burning rate is per tick, not per second (a different `fixed_dt` changes burn time); the distance field lags
burnt-out wood by up to 15 ticks; wood does not conduct heat internally (fire spreads only through the gas).

---

## 10. Steam, evaporation, flash and sink accounting

### 10.1 Steam

Steam is an R16F gas-grid scalar (`phase_state_.steam_a/b`, ping-pong) updated by `steam_update.comp` every tick
while oil or heat exists: semi-Lagrangian trace with the gas centre velocity plus a 100 gas cells/s rise (a trace
ending in a wall keeps the cell's own value), times $`e^{-0.8\Delta t}`$ (condensation within about 1.5 s), plus 0.3
per evaporated record in the cell, clamped to 1. It is drawn white and does not push the gas (presentation only).

### 10.2 Evaporation

`phase_particles.comp`, per water record: a record is exposed when the cell above or beside it is open air (not a wall,
liquid volume below 0.3 of rest). Exposed water under heat evaporates at
$`2\,\mathrm{s^{-1}} \times \mathrm{smoothstep}(0.25, 0.7, \text{hot})`$, where hot is the hottest temperature or
flame within four gas cells above. Removal is stochastic per record: a hash of the record's own state (never its slot,
which compaction reorders) below $`1 - e^{-\text{rate}\,\Delta t}`$. The record is deactivated, counted in the
evaporated sink and added to its gas cell's steam source.

### 10.3 Oil ignition and burning

Exposed oil is lit when hot $`\ge 0.3`$ (0.2 once lit) and stays lit while that holds (`state2.w`). A lit record adds
one to its gas cell's lit-oil count; `steam_update.comp` feeds that gas cell
$`\text{fuel} = \max(\text{fuel}, 0.6\, \text{feed})`$, $`T = \max(T, 0.7\, \text{feed})`$,
$`\text{flame} = \max(\text{flame}, 0.4\, \text{feed})`$ with $`\text{feed} = \min(0.5 \times \text{count}, 1)`$ (two
lit records already make a full flame), so a slick burns on by itself like wood. Lit records are consumed at 2/s and
counted in the burned sink. Covered (by liquid, sand or a wall) or cooled, a lit record goes out.

### 10.4 Flash quench

A water record flashes when the cell below holds oil (share $`> 0.25`$) and either it is exposed or its own cell holds
oil (it has landed on a slick or is sinking through one; the pool under a slick does not count), under heat
$`\ge 0.3`$ within six gas cells (twelve cells) above. It evaporates at 30/s through the evaporation sink, and the
column of four gas cells over it is quenched: `steam_update.comp` sets temperature and flame to 0 and oxidizer to 0
until fresh air mixes back (`fire_rd.comp`'s oxygen relaxation). The burning oil under it, its ignition window now
cold, goes out.

**Why.** A lit slick is far above 100 C, so water does not survive sinking into it. A flashing record takes about 70 J
of latent heat, some hundred times what a 1 cm gas cell holds at flame temperature, and its roughly
$`50\,\mathrm{cm^3}`$ of steam displaces the air (steam smothering). `app-oil-quench`: flame half a second after dousing
8.5 against 329 for the slick left alone (without the flash 267 against 417), with only a few percent of the water
flashing. Before, water sank through the oil and the slick burned on.

### 10.5 Sink accounting

`phase_state_.sinks` holds two cumulative counters since the reset: evaporated water records and burned oil records
(`ReadPhaseSinks`). Phase sinks remove moving records only; packed cells are deep liquid away from air, so the census
of moving plus packed records and the sinks are disjoint. The invariants checked by tests and benchmarks:

- water: moving + packed + evaporated = poured (`app-steam`, `app-gas-quenching`; the benchmark's mass drift adds back
  counted evaporation);
- oil: moving + burned = poured (`app-oil-burn`);
- sand: moving + packed = poured, exactly (`app-sand-sleep-conservation`).

Source-free particle mass drift above 0.5%, unexpected creation or loss and nonzero failure counters are failures.

### 10.6 Constants

| Constant | Value | Basis |
| --- | --- | --- |
| `kWaterEvaporateRate` | $`2\,\mathrm{s^{-1}} \times \mathrm{smoothstep}(0.25, 0.7, \text{heat})`$ | a thin film per second under full flame |
| `kWaterFlashRate` | 30 /s | mean life 1/30 s |
| `kSteamRise` | 100 gas cells/s | 1 m/s |
| `kSteamCondense` | 0.8 /s | gone within about 1.5 s |
| `kSteamPerParticle` | 0.3 | |
| exposure | liquid volume $`< 0.3`$ rest beside or above | |
| heat windows | 4 gas cells (evaporation, ignition), 6 (flash) | |

Limits: steam carries no heat or volume into the gas; evaporation is driven by a heat threshold, not an energy budget.

---

## 11. Rendering

Rendering is presentation only and feeds nothing back. `Render()` draws the HUD-free scene stretched over the
framebuffer (replays and pixel tests use it); `Render(true)` letterboxes the simulation into the largest square
rectangle centred in the framebuffer (`FitSimRect`, uniform `simRect`) and the shader paints the margin with the same
cave-wall albedo, so there is no seam. The only time input is `fxTime`, simulation seconds (tick count x fixed step,
wrapped at an hour), used only in lit frames, so a frame is a function of the simulation state.

### 11.1 Render extraction

`render_extract.comp` (per tick, full resolution): bilinear upsampling of the gas fields, with smoke, temperature and
flame raised to the mean of a 3 x 3 ring of taps 0.55 gas cells apart where that is larger (softens the 2x upsample);
emissive fire $`= \mathrm{clamp}\,\big(\text{flame}\, (0.66 + 0.20\, T) + 0.11\, T\big)`$; smoke
$`\times 0.9`$.

### 11.2 Emitted light and lantern

`light_build.comp` (once per `Render` when the composite is lit): a 250 x 250 RGBA16F texture, one texel per 4 x 4
cells. Four bilinear taps one cell off the texel centre are exactly the mean of its footprint. Light =
warm colour (from deep orange to brighter orange with flame) $`\times\, (8\, \text{flame} + T^4)`$: it comes from the flame, plus a
little from gas near $`T = 1`$. The host calls `glGenerateMipmap`; the composite reads mips 1-4 (8 to 64 cells per
texel) with weights 0.4, 0.5, 0.6, 0.5 (summing to 2), so a flame lights its surroundings without turning the whole
screen orange.

Cursor lantern: only letterboxed frames with the cursor over the simulation get it (replays and pixel tests never do).

```math
R = (130 + 1.5\, r_{\text{brush}})\,(1 + 0.12\, e_{\text{paint}})\ \text{cells}, \qquad
f = 1 - \mathrm{smoothstep}(0, 1, d / R), \qquad
\text{glow} \mathrel{+}= (1.0, 0.46, 0.16) \times 2.4\, f^2
```

with $`r_{\text{brush}}`$ the brush radius, $`e_{\text{paint}}`$ the painting ease and $`d`$ the distance.

### 11.3 Composite (`composite.frag`)

Debug views F2-F10 read the raw fields (boundary, water, water velocity, gas velocity, gas pressure, smoke,
temperature, fuel/flame/temperature, debug counters plus a pass-timing bar chart) and are unlit. The composite (F1)
builds an albedo in this order:

1. Back wall: near-black static value noise on 4 x 4, 8 x 8 and 16 x 16 blocks only (no per-cell grain), so it reads as
   distant. Open cells get a drop shadow from rock or wood up to four cells up-left (60% loss) and contact occlusion
   next to solids (30%, applied after sand and water).
2. Rock: multi-tone cool grey from the cell and its 2 x 2, 8 x 8 and 16 x 16 blocks, olive moss patches, a darker
   interior and a crusty rim (six cells deep on exposed tops, three on sides and undersides) that fades out by a
   per-cell threshold, so it breaks up like dithered pixel art.
3. Wood: brown planks with grain and plank lines that char toward grey ash with burnt fraction; burning cells get an
   ember glow.
4. Sand: coverage $`\mathrm{smoothstep}(0.04, 0.11, \cdot)`$ of the filtered occupancy (sampled at twice the
   occupancy, saturated, which keeps the look tuned on the earlier half-density occupancy); ochre that darkens with
   occupancy, per-cell shade and light and dark specks, darkening by up to 45% with moisture (water in the pores
   scatters less, Lekner and Dorf 1988), a pale rim on exposed tops.
5. Water: coverage $`1 - e^{-26\, w}`$ of a filtered and dilated amount $`w`$, so one- or two-particle water stays
   visible. A gap of a few cells with water three cells away on all four sides draws as water (particle holes do not
   read as dark streaks). Depth from three taps above (5, 12 and 24 cells, per-pixel jittered, each bilinear half a
   cell off the column so one missing cell does not streak) times taps seven cells to each side; murky teal darkening
   with depth; a bright free-surface line two cells deep; static shimmer cells near the surface; foam only above
   35-70 cells/s.
6. Oil: the oil share filtered over 3 x 3 and dilated, so a thin film joins into one band; amber where thin or near the
   top, near-black brown where deep; a thin-film sheen (an amber-biased rainbow along the top cells, hue drifting with
   low-frequency noise).
7. Smoke: a grey haze that shows even when thin, warmed slightly where hot; it is albedo, so the fire lights it.
8. Steam: a white plume over everything.
9. Lighting (lit frames): light = ambient $`(0.18, 0.205, 0.30) \times`$ vignette + glow + lantern, summed in linear
   units and applied as its square root (albedo times light in gamma-2 space). The vignette is
   $`1 - 0.82\, \mathrm{smoothstep}(0.3, 1, d_{\text{centre}})`$ with $`d_{\text{centre}}`$ the distance from
   centre. The back wall gets half the glow and lantern light. Warm light desaturates the colour by up to 40% of the
   warm share, so lit stone reads orange-brown. Water and oil glints and a little steam self-light are added after the
   light.
10. Fire (additive, soft-saturated as $`1 - (1 - c)\, e^{-\text{add}}`$): flame on a blackbody-like ramp of the gas
    temperature (deep red, orange, yellow, yellow-white core) with strength from the flame field; a glow of hot gas
    ($`T^4`$) that outlives the flame; a bloom of eight fixed temperature taps at 8 and 14 cells; in lit frames rising
    noise tongues (two octaves of value noise stretched upward and scrolled with `fxTime`, cutting thin flame harder)
    (sparks and embers are particles drawn after the composite, 11.4); heat shimmer offsets the whole lookup above and
    inside hot gas; wood embers.
11. Sub-LSB dither against banding.

`lighting_enabled_` (uniform `lighting`) switches the light, lantern, tongues and shimmer off (and `Render` skips `DrawSparks`), leaving plain
albedo: the CPU oracle in `app-render-modes` mirrors the albedo math (including the PCG2D hash, Jarzynski and Olano
2020) for scenes without water, smoke or fire; `app-lighting` checks the light qualitatively. The composite uses all 16
fragment texture units GL 4.3 guarantees (units 0-15), so a new sampler must reuse one.

### 11.4 Sparks and embers (`spark_pick.comp`, `spark_step.comp`, `spark.vert`, `spark.frag`)

Sparks and embers are simulation state, not a shader effect: a ring of $`N = 16384`$ records (two `vec4`, 512 KB,
`SparkState`) advanced once per tick at the end of the render-extraction pass, and only once something that can burn was
painted (`heat_present_`). The draw reads the ring and nothing else (no clock), so a frame is a function of the
simulation state; a rerun of the same ticks gives byte-identical records (`app-sparks`). A record is
$`(x, y, v_x, v_y)`$ in cells and cells/s and (age, life, drag time $`\tau`$, seed $`\times 2`$ + ember); life 0 is dead.

Spawning is a contest, so it needs no counter and does not depend on the order invocations run in. Per gas cell and
tick `spark_pick.comp` hashes (cell, tick) to a number $`u \in [0, 1)`$; only $`u < 0.02`$ read any texture. With the
gas temperature $`T`$, the reaction rate $`r`$ and $`h = \max(T, r)`$:

```math
p_{\mathrm{flame}} = 0.0045\,\mathrm{smoothstep}(0.35, 0.8, h), \qquad
p_{\mathrm{ember}} = 0.0005\,\mathrm{smoothstep}(0.45, 0.9, h) + 0.002\, w
```

with $`w`$ the burning share (material 256 to 511) of the 4 x 4 cells around the gas cell, so wood throws embers of its
own. $`u < p_{\mathrm{flame}}`$ bids a flame spark and $`p_{\mathrm{flame}} \le u < p_{\mathrm{flame}} + p_{\mathrm{ember}}`$ an
ember. Burning oil needs no case: it feeds the gas above it, which burns (10.3). A bid goes to bucket
$`H(\text{cell}, \text{tick}) \bmod 128`$ with the key (13-bit hash priority, ember bit, cell index + 1) by `atomicMax`,
which is order independent, so the winners are a function of the fields and the tick. Then `spark_step.comp` gives
tick $`t`$ the 128 slots from $`128\,(t \bmod 128)`$: slot $`b`$ of the range takes bucket $`b`$'s winner, born at a hashed
point of its gas cell, or is retired if the bucket is empty. The ring period is 128 ticks (2.13 s), longer than the
longest life (2.1 s), so a slot is overwritten only after its spark died. At most 128 spawns a tick (7680 per second);
a flame of 400 burning gas cells makes about 110 flame sparks and 12 embers a second (0.0045 and 0.0005 per cell
and tick), so about 50 sparks and 20 embers are alive at once.

| | Flame spark | Ember |
| --- | --- | --- |
| life | 0.30-0.65 s | 0.9-2.1 s |
| drag time $`\tau`$ | 0.04-0.09 s | 0.12-0.26 s |
| burst on top of the gas velocity | 10-60 cells/s within 1.6 rad of up | 60-180 cells/s within 1 rad of up |
| thermal lift $`\lambda`$ | 0.9 | 0.5 |
| restitution | 0.2 | 0.4 |
| colour at birth | yellow, $`T_0`$ 0.8-1.0 | orange, $`T_0`$ 0.55-0.8 |

Motion, per tick, exactly integrated for a constant acceleration over the tick. The gas velocity $`\mathbf{u}`$ is the
bilinear value between gas cell centres, doubled to cells/s; $`\theta = 1 - \mathrm{age}/\mathrm{life}`$ is the heat;
$`g`$ is `|water_gravity|` (1962 cells/s$`^2`$); $`\mathbf{a}_{\mathrm{turb}}`$ is a hashed random acceleration of at most
900 (sparks) or 300 (embers) cells/s$`^2`$ per axis:

```math
\mathbf{v}_\infty = \mathbf{u}(\mathbf{x}) + \tau\,\big(\mathbf{a}_{\mathrm{turb}} - g\,(1 - \lambda\,\theta)\,\hat{\mathbf{y}}\big), \qquad
\mathbf{v}' = \mathbf{v}_\infty + (\mathbf{v} - \mathbf{v}_\infty)\, e^{-\Delta t / \tau}
```

A hot spark is nearly weightless and follows the plume; as it cools ($`\theta \to 0`$) gravity returns and it falls
relative to the gas at $`g \tau`$ (120-250 cells/s for an ember). Speed is capped at 700 cells/s. The move is swept in
$`\lceil \max(|v_x|, |v_y|)\,\Delta t \rceil \le 8`$ sub-steps of at most a cell. Entering rock (boundary mask 1, and the
domain's sides and floor) reflects the blocked axis (a corner both) by the restitution above, slows the tangential
speed by 30%, rests a normal speed under 40 cells/s, and cools the spark by $`0.1\,\mathrm{life}\,\min(1, |v|/150)`$ of
its life. Sand (mask 2 and 3), the top of the domain, water amount above 0.03 at the end of the move, and an age that
such a hit has carried to the life put it out.

Drawing (`DrawSparks`, lit composite frames only, inside `Render` after the composite): one instanced quad per record,
dead records collapsed outside the clip volume, from the ring as two per-instance attributes. The streak runs from the
head at the spark's cell back along its velocity by 0.022 s of travel (at most 7 cells) and is at least 1.8 pixels wide;
colour is the fire's ramp at $`T = T_0 (1 - \mathrm{age}/\mathrm{life})^{1.4}`$ (yellow through red as it cools),
intensity 1.3 (spark) or 1.5 (ember) with a fast fade-in, a fade-out over the last 40% of its life and a flicker
$`0.75 + 0.25 \sin(47\,\mathrm{age} + 2\pi\,\mathrm{seed})`$. Two passes: first a soft quad of radius 6 cells that lights
the scene around it, blended as $`d(1 + 0.35\,c\,f^2)`$ with $`f = 1 - r^2`$; then the streak, blended as the fire's own
soft saturation, $`d + (1 - d)(1 - e^{-c w})`$ with $`w`$ across and along the streak (a third at the tail). Unlit
frames and the debug views skip both passes, so the pixel oracles never see a spark.

Cost on the RX 7800 XT: the two passes together take 0.008 ms per tick (median; 0.02 ms at most) in a large fire
holding about 4000 sparks, and the two draw passes add 0.03 ms to a 1000 x 1000 lit frame.

### 11.5 HUD (`hud.frag`, `src/HudFont.hpp`)

One fullscreen pass blended with premultiplied alpha over the finished frame, interactive loop only. Layout is in
units of `uiScale` framebuffer pixels (one step per about 450 framebuffer rows), every plate and tile edge on a unit,
so the pixel art stays crisp at integer scale; text is a 5 x 7 bitmap font baked at startup into a one-channel atlas
and sampled on whole texels; only the brush ring is anti-aliased. The stack (inventory plate of eight 10-unit slots
with pixel-art motifs, key numbers, material name and brush radius, over a help panel or an "H HELP" hint) sits in the
left letterbox margin when `hud::FitsInMargin` says it fits (a 1280 x 900 window's 190 px margin at scale 2) and
overlays the corner otherwise. The brush ring is evaluated in grid space so its radius matches the spawn shaders; it
thickens and brightens while painting, dashed and reddish with a cross for erase. Motion is host state (`HudMotion`):
help, painting and slot selection move toward their targets by $`1 - e^{-\Delta t / \tau}`$ per frame
($`\tau`$ = 50-90 ms, settling in about 150 ms, interruptible, snapping after a stall). The help panel hides itself
`hud::kHelpSeconds` (8 s) after it was last shown when `help_hide_time_` is armed. Nothing in the HUD animates on its
own.

---

## 12. Input and brushes

### 12.1 Events

- Keys (`KeyCallback`): `[` and `]` change the radius (1-96) on press and repeat, so sizing follows native key repeat;
  1-8 select the brush, F1-F10 the view, H toggles help, Esc quits, all on press, preserving order and short taps.
- Mouse buttons use GLFW sticky buttons, so a brief click survives until a frame sees it. A left press resets the
  stroke, the cursor trail and the move history, and records the press position.
- `UpdateInput` (once per frame) maps the cursor to a grid cell (`MapCursorToGrid` converts window units to
  framebuffer pixels before subtracting the letterbox offset, so DPI scaling and margins map to the cell the shaders
  paint) and sets `painting_` only when the cursor is over the simulation and the window is hovered (GLFW keeps
  reporting the last position after the cursor leaves). The spawn pass paints from `painting_`, the state this frame
  read.

### 12.2 Cursor trail and Windows coalesced moves

`CursorPosCallback` appends every distinct cell the cursor reports (event rate, not frame rate) to `cursor_trail_`
(bounded at 256; past that the newest overwrites the last entry, a marked shortcut). An event outside the simulation
ends the stroke. Windows coalesces the moves between two events into the last one, so on Windows the callback also
queries `GetMouseMovePointsEx` (display points, 16-bit coordinates sign-extended for monitors left of or above the
primary) and replays the moves since the previous event (`MovesSince`: the history entries after `last_move_`, oldest
first; none after a press, and none when the previous move has left the history, which leaves a straight bridge).

### 12.3 Stamping a stroke (`StampStroke`)

1. Brush velocity: the stroke's travel per tick, smoothed by $`1 - e^{-\Delta t / 0.05\,\mathrm{s}}`$ and capped at 800
   cells/s (4 m/s); a new stroke starts at rest. It is added to water and sand spawn velocities, and at half (gas
   cells) to smoke and fire stamps.
2. Path: the previous stamp (if the stroke continues with the same brush), the trail, then the cursor.
3. `StrokePoints`: with three or more points the path is a centripetal Catmull-Rom spline (Yuksel et al. 2011,
   evaluated with the Barry-Goldman pyramid; end segments reflect their neighbour), so a quick circle stays round;
   a turn sharper than 60 degrees at either end of a segment stays a corner. The densified path is resampled by arc
   length at most $`\max(1, \text{radius} / 2)`$ cells apart, at most 64 stamps per tick. Eight samples of a
   radius-100 circle stay within 0.96 cells of it (a polyline cuts 7.6 cells in).
4. Each stamp runs `StampBrush`; every stamp after the first skips the previous stamp's disc
   (`skip_previous_stamp`), so a swept cell receives one held tick of particles, not three or four, and a fast stroke
   does not drain the pools. Gas stamps use max, so overlap does not saturate.
5. Catch-up ticks in the same frame find an empty trail and stamp only the cursor, keeping the per-tick emission rate.
   Release, leaving the simulation or changing brush restarts the stroke.

Scripted and replay brushes are single-point stamps with exact emission (no density gate, no skip).

### 12.4 Brush table (`BuildBrushConfig`)

| Brush | Effect |
| --- | --- |
| 1 Sand | 2 grains per cell, $`\pm 10`$ cells/s spread, occupancy limit 0.225 |
| 2 Water | 4 records per cell at (0, -100) cells/s $`\pm 10`$, density gate 0.16, sub-tick ages |
| 3 Solid | material 2 where not already solid; clears water, packed water, packed sand and particles in the disc |
| 4 Erase | clears material, packed cells, particles in the disc and the gas (oxidizer reset to air) |
| 5 Smoke | gas: smoke 0.5, $`T`$ 0.2, (0, 20) gas cells/s $`\pm 15`$ |
| 6 Fire | gas: fuel 0.6, $`T`$ 0.6, flame 0.4, (0, 20) gas cells/s $`\pm 15`$; blocked by water |
| 7 Wood | material 3 where not already solid |
| 8 Oil | as water, records tagged oil |

Defaults: sand brush, radius 12 cells (6 cm). Hashes seed from the frame index and the stamp centre, so replays repeat.

### 12.5 Limits

Live mouse painting, the H key and held-key timing in a native window, DPI transitions and sustained native stress are
not verified (VALIDATION). Cursor jumps longer than 64 stamps spread evenly and leave gaps.

---

## 13. References

Papers and books cited in the code, PLAN.md and this document.

- Al-Hashemi, H. M. B., and Al-Amoudi, O. S. B. (2018). A review on the angle of repose of granular materials. Powder
  Technology.
- Bouchaud, J.-P., Cates, M. E., Ravi Prakash, J., and Edwards, S. F. (1994). A model for the dynamics of sandpile
  surfaces (BCRE). Journal de Physique I.
- Brackbill, J. U., and Ruppel, H. M. (1986). FLIP: a method for adaptively zoned, particle-in-cell calculations of
  fluid flows in two dimensions. Journal of Computational Physics.
- Bridson, R. (2015). Fluid Simulation for Computer Graphics, 2nd edition. CRC Press.
- Cetegen, B. M., and Ahmed, T. A. (1993). Experiments on the periodic instability of buoyant plumes and pool fires.
  Combustion and Flame.
- Daerr, A., and Douady, S. (1999). Two types of avalanche behaviour in granular media. Nature.
- Douady, S., Andreotti, B., and Daerr, A. (1999). On granular surface flow equations. European Physical Journal B.
- Drucker, D. C., and Prager, W. (1952). Soil mechanics and plastic analysis or limit design. Quarterly of Applied
  Mathematics.
- Enright, D., Nguyen, D., Gibou, F., and Fedkiw, R. (2003). Using the particle level set method and a second order
  accurate pressure boundary condition for free surface flows. Proc. 4th ASME-JSME Joint Fluids Engineering Conference.
  (Ghost-fluid free surface, 4.8.)
- Fedkiw, R., Stam, J., and Jensen, H. W. (2001). Visual simulation of smoke. SIGGRAPH.
- Ferguson, R. I., and Church, M. (2004). A simple universal equation for grain settling velocity. Journal of
  Sedimentary Research.
- Gao, M., Wang, X., Wu, K., Pradhana, A., Sifakis, E., Yuksel, C., and Jiang, C. (2018). GPU optimization of material
  point methods. ACM Transactions on Graphics.
- Gibou, F., Fedkiw, R., Cheng, L.-T., and Kang, M. (2002). A second-order-accurate symmetric discretization of the
  Poisson equation on irregular domains. Journal of Computational Physics. (Symmetric ghost-fluid discretisation,
  conductance $`1/\theta`$, 4.8.)
- Gunn, R., and Kinzer, G. D. (1949). The terminal velocity of fall for water droplets in stagnant air. Journal of
  Meteorology.
- Harlow, F. H., and Welch, J. E. (1965). Numerical calculation of time-dependent viscous incompressible flow of fluid
  with free surface. Physics of Fluids. (MAC grid.)
- Herminghaus, S. (2005). Dynamics of wet granular matter. Advances in Physics.
- Hillis, W. D., and Steele, G. L. (1986). Data parallel algorithms. Communications of the ACM. (Inclusive scan.)
- Jarzynski, M., and Olano, M. (2020). Hash functions for GPU rendering. Journal of Computer Graphics Techniques.
  (PCG2D.)
- Jiang, C., Schroeder, C., Selle, A., Teran, J., and Stomakhin, A. (2015). The affine particle-in-cell method.
  SIGGRAPH.
- Jiang, C., Schroeder, C., Teran, J., Stomakhin, A., and Selle, A. (2016). The material point method for simulating
  continuum materials. SIGGRAPH course.
- Jop, P., Forterre, Y., and Pouliquen, O. (2006). A constitutive law for dense granular flows. Nature.
- Kaasschieter, E. F. (1988). Preconditioned conjugate gradients for solving singular systems. Journal of
  Computational and Applied Mathematics.
- Klar, G., Gast, T., Pradhana, A., Fu, C., Schroeder, C., Jiang, C., and Teran, J. (2016). Drucker-Prager
  elastoplasticity for sand animation. SIGGRAPH.
- Kugelstadt, T., Longva, A., Thuerey, N., and Bender, J. (2021). Implicit density projection for volume conserving
  liquids. IEEE Transactions on Visualization and Computer Graphics 27(4). (Density correction.)
- Lekner, J., and Dorf, M. C. (1988). Why some things are darker when wet. Applied Optics.
- Losasso, F., Gibou, F., and Fedkiw, R. (2004). Simulating water and smoke with an octree data structure. ACM
  Transactions on Graphics (SIGGRAPH). (Ghost-fluid free surface in a liquid solver, 4.8.)
- Lu, N., and Likos, W. J. (2004). Unsaturated Soil Mechanics. Wiley.
- McAdams, A., Sifakis, E., and Teran, J. (2010). A parallel multigrid Poisson solver for fluids simulation on large
  grids. Symposium on Computer Animation. (MGPCG.)
- McCaffrey, B. J. (1979). Purely buoyant diffusion flames: some experimental results. NBS report.
- Mitarai, N., and Nori, F. (2006). Wet granular materials. Advances in Physics.
- Nguyen, D. Q., Fedkiw, R., and Jensen, H. W. (2002). Physically based modeling and animation of fire. SIGGRAPH.
- Onoda, G. Y., and Liniger, E. G. (1990). Random loose packings of uniform spheres and the dilatancy onset. Physical
  Review Letters.
- Playne, D. P., and Hawick, K. (2018). A new algorithm for parallel connected-component labelling on GPUs. IEEE
  Transactions on Parallel and Distributed Systems.
- Rong, G., and Tan, T.-S. (2006). Jump flooding in GPU with applications to Voronoi diagram and distance transform.
  Symposium on Interactive 3D Graphics and Games.
- Schiller, L., and Naumann, A. (1933). Drag correlation for spheres.
- Scott, G. D. (1960). Packing of spheres. Nature.
- Selle, A., Fedkiw, R., Kim, B., Liu, Y., and Rossignac, J. (2008). An unconditionally stable MacCormack method.
  Journal of Scientific Computing.
- Tampubolon, A. P., Gast, T., Klar, G., Fu, C., Teran, J., Jiang, C., and Museth, K. (2017). Multi-species simulation
  of porous sand and water mixtures. SIGGRAPH.
- Willett, C. D., Adams, M. J., Johnson, S. A., and Seville, J. P. K. (2000). Capillary bridges between two spherical
  bodies. Langmuir.
- Yuksel, C., Schaefer, S., and Keyser, J. (2011). Parameterization and applications of Catmull-Rom curves.
  Computer-Aided Design.
- Zhu, Y., and Bridson, R. (2005). Animating sand as a fluid. SIGGRAPH.

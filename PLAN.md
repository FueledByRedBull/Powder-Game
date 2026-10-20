# Powder Game Architecture and Plan

The work and evidence ledger is `ROADMAP.md`; commands and measured results
are in `VALIDATION.md`. This file describes the current architecture. Historical
cellular sand milestones have been superseded by the particle implementation.

## Goals and constraints

- 1000x1000 simulation and rendering; 500x500 gas.
- Eight brushes (sand, water, solid, erase, smoke, fire, wood, oil) plus steam,
  sleeping sand, packed water and temperature coupling.
- C++20 and OpenGL 4.3 compute, subject to the resource limits in README.
- GPU simulation. A tick waits on the GPU once for the fastest water speed and once
  per water solve to confirm convergence; full-state readback is for tests.
- Default fixed tick 1/60 second with elapsed-time accumulation. A frame runs at most
  two ticks, one after an overrun, and drops the backlog (see Scheduling).
- Reference acceptance: 60 FPS idle; 30 FPS with 100,000 water and 50,000 sand
  particles plus smoke/fire, GPU frame p95 at most 50 ms. The preferred goal is
  60 FPS combined. The v0.5.0 tree meets the combined timing budgets (about 21-23 ms GPU
  mean, p95 about 27-28 ms) but the benchmark still fails on two non-converged pressure
  solves (see VALIDATION, Current status). Two ticks run per rendered frame; display
  latency is excluded.

## Scale and constants

One cell is 5 mm, so the 1000-cell domain is 5 m tall and a full-height drop takes
sqrt(2 * 5 / 9.81) = 1.01 s, ending at 9.9 m/s (1981 cells/s). Every rate below is in cells
and seconds at that scale (realism pass, 2026-10-20; see VALIDATION for measurements).

| Constant | Value | Basis | Reasoning |
| --- | --- | --- | --- |
| water_gravity, sand_gravity | -1962 / 1962 cells/s^2 | g = 9.81 m/s^2 / 5 mm | real time instead of the old 140/240 (5-8 s falls) |
| water_velocity_damping | 1.0 (none) | viscous decay of cm-scale waves takes minutes (nu = 1e-6 m^2/s) | grid transfer dissipation is already larger than real viscosity |
| water_bulk_damping | 0.4 /s, scaled by support s = clamp(\|a_grid - g\| / g) | a poured pool's turbulence and wall boundary layers dissipate it within seconds; the inviscid 5 mm grid resolves neither | applied only to pressure-supported water (pool, wave, film on a ramp; s ~ 1); free fall, streams and droplets (a = g, s = 0) keep their speed. Poured half-screen (2.5 m deep) pool, bulk p50 speed at 7/11/12 s: 170/111/112 cells/s undamped, 24/5/24 with it (a slowly decaying seiche), packing engages (17k packed vs 0). 0.6 calmed faster (p50 2.2 at 12 s) but clumped the dam break (std 0.225, gate 0.12), so 0.4 |
| sand_damping | 1.0 (none) | air drag on a grain stream is small over 5 m | dissipation comes from plasticity and friction |
| water_max_velocity, sand_max_velocity | 2400 cells/s (12 m/s) | above full-domain free fall 9.9 m/s; 5 mm drops reach ~9 m/s terminal (Gunn and Kinzer 1949) | never cuts a fall; splash outliers are clamped and counted |
| water_cfl_limit (pressure) | 16 cells per solve, max 6 solves | each MGPCG solve costs ~2-3.5 ms on the RX 7800 XT | 2 solves per tick up to 1920 cells/s. Long sub-moves through one projected field converge particles onto single points (dam break cells at 22x rest at 20 and 40), which then share one velocity; particles in cells over 3x rest take a deterministic swept random step of up to 0.25 cells (water_particle_step): dam break std 0.10 at 12 and 16, 0.11-0.14 at 20; sizing from bulk-liquid particles only (3+ particles per cell) gave std 0.14 at 16 and was rejected |
| water_advection_cfl | 2 cells per swept sub-move | FLIP practice of CFL 1-5 with substepped advection (Bridson 2015) | particles never skip cells; droplets outside the liquid fly ballistically |
| water_flip_blend | 0.97 | Zhu and Bridson 2005 use 0.95-0.99 | lively splashes; 0.99 left visible noise |
| water_density_rate | 20 /s | density correction as in Kugelstadt et al. 2019 | closes air pockets in ~0.05-0.1 s without spikes; voids above liquid only contract |
| water brush | 4 particles/cell, -100 cells/s (0.5 m/s), +-10 cells/s spread, random positions in the cell | spout outflow | no 2x2 lattice teeth; the stream wobbles and breaks up |
| sand_shear_modulus, sand_lame_lambda | 196000 | 24000 x 1962/240, keeping a 100-cell column at J ~0.68 | real sand (E ~ 10-100 MPa) would need wave speeds of 20000+ cells/s |
| sand_cfl_limit / sand_advection_cfl | 0.4 wave / 1.0 advective | explicit MPM stability (Jiang et al. 2016) | larger of the two, not their sum: 23 still, up to 40 at the cap |
| sand_friction_alpha | 0.6 (+-20% per grain) | measured cone 27-31 deg with mu(I); dry sand repose 30-35 deg (Al-Hashemi and Al-Amoudi 2018) | per-grain spread makes piles irregular; 0.75 measured flatter (24-26 deg: more stored elastic shear), so the static alpha stays; below target, see VALIDATION |
| sand_friction_rate_gain, sand_inertial_i0, sand_grain_diameter | 0.68, 0.279, 0.08 cells (0.4 mm) | mu(I) = mu_s + (mu_2 - mu_s) I / (I0 + I) with mu_s = tan 20.9, mu_2 = tan 32.76, I0 0.279 (Jop et al. 2006) | alpha scales by mu(I) / mu_s per grain; I = shear rate d / sqrt(p / rho) from the trial Hencky pressure. Deep beds stay near mu_s, thin fast surface flows reach mu_2: centre-pour piles 29.3 -> 30.7 deg (benchmark-feel sandpour, packing off) |
| sand_max_dilation | 0.14 | ln(0.64 / 0.555): random close over random loose packing (Scott 1960, Onoda and Liniger 1990) | cap of the Klar et al. 2016 volume correction; uncapped, MPM volume drift overpacked a poured pile fourfold (packing 4.0) |
| sand_cohesion | 0.3 (log volume strain) | damp sand has a Mohr-Coulomb cohesion intercept with almost no tensile strength (Mitarai and Nori 2006) | widens the cone by that much compression at full capillary strength, tension cut-off kept; 0.06-0.15 measured no steeper than dry at these soft moduli |
| sand_wetting_rate, sand_drying_rate | 4 /s, 0.02 /s | sand soaks within a fraction of a second; drying takes minutes to hours | moisture relaxes up toward the 3x3 water fill, dries in about a minute (a sandbox compromise) |
| sand_friction_coefficient (wall) | 0.62 | quartz on rock, mu ~ 0.5-0.7 | unchanged |
| sand_drag | 60 /s | settling g(1 - 1/2.65)/k = 20 cells/s = 10 cm/s, a dense cloud of 0.3-0.5 mm grains (Ferguson and Church 2004 gives 4-7 cm/s per grain) | clouds, slow sinking |
| sand_grain_density | 2.65 | quartz | buoyancy removes 1/2.65 of a submerged grain's weight |
| sand_reference_density | 2.0 | saturated/packed sand 1.9-2.1 g/cm^3 | unchanged (mass unit of occupancy) |
| brush velocity | polyline travel per tick, tau 50 ms, cap 800 cells/s (4 m/s) | cursor speed | added to water and sand spawns, and at half (gas cells) to smoke and fire stamps |
| gas_max_velocity | 300 gas cells/s (3 m/s; a gas cell is 1 cm) | buoyant flames and plumes rise at about 2 m/s (McCaffrey 1979: intermittent-flame velocity 1.9 Q^(1/5) m/s, Q in kW) | was 10 gas cells/s (5 cm/s); one transport substep per tick at CFL 8 (traces stop at walls, so a 7-cell trace is safe) |
| gas_buoyancy_temperature, gas_buoyancy_smoke | 1200 / 10 gas cells/s^2 per unit | T = 1 is a flame about 900 K above ambient: g dT/T is about 3 g, drag and mixing keep real flames near 2 m/s | flame tips reach 1-2 m/s within 0.2 s |
| gas_vorticity | 40 /s | Fedkiw et al. 2001 confinement | restores the curls and licking that semi-Lagrangian transport smooths away; flames puff at a few Hz (f = 1.5/sqrt(D) Hz, Cetegen and Ahmed 1993) |
| gas_expansion | 2 per unit fuel burned | burnt gas is 5-7x less dense; Nguyen et al. 2002 divergence source | pushes air out of the flame front |
| gas_combustion_rate, gas_oxygen_rate | 10 /s, 3 /s | fuel burns within 0.1-0.3 s of release in fresh air; oxygen (oxidizer 1 = air) is replenished by entrainment | the fire brush adds fuel only; burning starves the core so the flame sits on its edges |
| gas_reaction_decay, gas_temperature_cooling | 8 /s, 2.5 /s | a flame dies within about 0.1 s of losing fuel; plume gas cools by mixing and radiation in a fraction of a second | a released fire is out within about 0.5 s (fire-dynamics test: under 5% at 1 s) |
| gas_temperature_diffusion | 10 (effective 3 gas cells^2/s) | hot air diffusivity is about 2e-4 m^2/s = 2 gas cells^2/s | the old 0.045 changed R16F temperature by less than its rounding step |
| gas_pressure_residual_threshold | 0.25 /s | one half-float step of a face near the speed cap | 8e-4 of the cap, stricter relative to speed than the old 0.015 at a 10 cells/s cap |

### Interactive defaults (feel pass, 2026-10-20)

| Default | Value | Why |
| --- | --- | --- |
| Window | 85% of the primary work area's height, width = height + 2 x HUD stack width at the UI scale, centred (1280x900 on a 1080p screen, about 1753x1183 on 1440p); replays stay 1280x900 | bigger cells on large screens while the HUD keeps its letterbox margin; windowed, not maximized, because a maximized wide screen is mostly letterbox |
| Swap interval | adaptive vsync (-1) where `*_swap_control_tear` exists, else 1 | no tearing at rate, no half-rate stall on a late frame |
| Starting brush | sand, radius 12 cells (6 cm) | sand is the genre's first verb; 8 was a thin trickle at the new window size (about 28 px across on 1440p) |
| Brush emission | sand 2 grains/cell/tick; water and oil 4/cell at 0.5 m/s with the density gate; smoke 0.5 density +0.2 T at 0.2 m/s; fire 0.6 fuel +0.6 T | unchanged from the realism pass: a held brush makes a continuous stream that never stacks |
| Lantern | 130 cells + 1.5 per brush cell, strength 2.4, swells 12% while painting (eased with the ring) | lights a hand-sized patch of the cave without flattening the fire's glow |
| Ambient | moonlight about 0.2 with a strong vignette | the cave reads at a glance; fire and lantern stay the light that matters |
| Help panel | up at launch, hides after `hud::kHelpSeconds` (8 s); H toggles; "H HELP" hint remains | first-run guidance that gets out of the way |
| HUD motion | selection, help and painting ease with tau 50-90 ms (settled in about 150 ms), interruptible; radius changes are instant | keyboard actions repeat often, so feedback is quick and nothing loops |
| Water bulk damping | 0.4 /s on supported water | splashes and waves stay, a poured pool settles over about 10 s instead of sloshing for minutes (Scale and constants) |

Sand rheology pass (2026-10-20): volume correction (Klar et al. 2016, capped), mu(I) friction (Jop et al. 2006) and
simplified wet-sand cohesion (after Tampubolon et al. 2017); see Sand. Klar's hardening of the friction angle is not
implemented (no free particle component).

## State and ownership

- Painted material is an R16UI cell grid. Sand and water are authoritative particle
  SSBOs, preallocated at 2,097,152 water and 1,048,576 sand records (96 MB and 64 MB, each
  twice for compaction; startup checks GL_MAX_SHADER_STORAGE_BLOCK_SIZE). Bounded reservations
  prevent wrap. Live records sit below each pool's spawn cursor; water compaction and erase
  dispatch a host bound (last live count plus every spawn region since), and compaction
  deactivates each record it copies, so the scratch pool needs no clear. Metric readbacks read
  the cursor, then only that prefix. Settled sand leaves the sand pool for the packed texture (below), so the
  sand cap bounds moving grains, not the sand on screen.
- Material has one texture. Removed cellular velocity/stress and spare material
  fields saved 14 MB; equivalent solid shading is derived from the material value.
  Removing unused water/sand center outputs and water cell momentum saves another
  28 MB of logical storage plus the accumulation and stores that fed those outputs.
- Water records contain position, velocity, active flag, mass and affine velocity
  in three vec4s (48 bytes). No particle reseeding creates mass from render fields.
- Sand adds rest area and elastic deformation in four vec4s (64 bytes). The grid
  transfer has a one-cell halo, retaining complete quadratic support at domain edges.
  The sand grid velocity is RG32F: binary16 stalled fast sand and added gradient noise.
- Integer particle transfers accumulate weights and signed momentum; low-word/carry
  pairs prevent dense momentum from overflowing 32-bit sums.
- Water faces and most derived/gas fields use half precision. Water/gas pressure
  and water CG vectors use single precision because half precision stalled measured
  solves. Live particle-grid velocity producers diagnose nonfinite or unrepresentable
  half-float outputs through counter zero and neutralize only those failed values.
- Full, static and gas masks preserve cell topology. Only static sand collision
  needs a signed distance field, rebuilt by jump flooding when geometry changes.
  The full mask is 0 open, 1 painted solid, 2 sand above a quarter of rest density
  (a wall for water) and 3 above three quarters (a wall for gas too).
- Gas stores staggered velocity, smoke, fuel, oxidizer, temperature and reaction in
  ping-pong textures. Scalar combustion fields are bounded dimensionless quantities.

## Water

Mass-weighted quadratic particle-to-grid transfer uses wide fixed-point momentum
sums; blocked or out-of-domain support is omitted from the face sums, while the
bilinear cell weight that lands in a wall goes to the particle's own cell, so a film
against a wall keeps its density. The raw
pre-force velocity remains available for FLIP; gravity enters the projected grid.
The pressure operator uses matching divergence/gradient coefficients, thresholded
free-surface liquid classification and solid Neumann boundaries. GPU conjugate gradient uses an
R32F warm start, a multigrid V-cycle preconditioner (MGPCG), early convergence checks and a
true final residual check. The default ceiling is 1024 iterations, with a true residual
threshold of 0.12.

The preconditioner (`pressure_mg.comp`) is one V-cycle from zero over levels that halve the
grid down to 8x8 (seven coarse levels at 1000x1000). Each level smooths with one damped
Jacobi sweep (omega 2/3) before and one after its coarse correction, restricts with the
transpose of bilinear prolongation (weights summing to 4, so the coarse operator is the
unscaled 5-point stencil), and the coarsest runs 16 Jacobi sweeps from zero. Coarse cells are
air (Dirichlet) if any child is air, else liquid if any child is liquid, else a wall that is
sand-porous (`kSandLeak`) when a child is sand. Prolongation treats walls as Neumann: a wall
partner of a fine cell's bilinear stencil takes the cell's own coarse value, and restriction
lends the same weights back; interpolating toward zero at walls instead took 28 instead of 6
iterations on a deep tank. Every part is a fixed symmetric linear map and the smoother
contracts, so M^-1 is symmetric positive definite (`app-water-multigrid` checks
<Ma,b> = <a,Mb> and <Ma,a> > 0). Restriction and prolongation stage a block's neighbourhood
in shared memory; level 0 and 1 steps run per listed CG tile, coarser ones over each level's
box (the liquid bounds plus one cell), and each solve classifies the levels once (phases 0, 1
and 5). Deeper smoothing, a 16x16 coarsest level, liquid-priority coarsening, an
in-workgroup bottom solve, a fused CG setup and block-occupancy skipping were measured and
not kept (VALIDATION).

The iterations run only over the 16x16 grid tiles that hold liquid (or invalid input or a
stray residual): after the full-grid initial residual, `pressure_cg.comp` flags each tile
(phase 8) and one workgroup compacts the flags into a list in grid order (phase 9), which
also writes the indirect dispatch arguments. An iteration is A*d (1), alpha with the p/r
update (2), the 15 V-cycle dispatches, and the convergence test with beta and the d update
(3). Each tile stores its sums per list position, and every workgroup forms the global sums
it needs from the list in list order, so all reach the same decision without a separate
reduction dispatch and the sums repeat bit for bit; the first workgroup records the result
and, when the solve stops, zeroes the tile and multigrid dispatch counts so later
dispatches launch nothing. (A last-workgroup-done reduction was tried and raced on this
driver.) The host never waits before the solve: it dispatches the previous solve's
iteration count blind, then copies the running flag behind a fence and queues 1, then 2,
then 4 more iterations while the CPU waits for each fence, until a fence finds the solve
stopped or the cap is reached. CG stops on its recursive residual (at half the threshold),
which drifts from the true b - Ap as R32F rounds a large pressure (a deep packed pool reaches
|p| of about 85,000, where one ulp is 0.008): a converged solve therefore restarts from b - Ap
(phase 0 and `pressure_cg_reduce.comp` with `restart`, keeping the tile list and iteration
count), and one whose true residual already passes the full threshold stops there unchanged;
one that does not iterates on, again confirmed at fences, up to the same cap. A solve that
hits the cap is still caught by the true residual check. The solve is latency-bound (about 18 dispatches per iteration), so its cost
follows the GPU clock more than the liquid volume.

Density control: the divergence carries a target. A cell above rest density (4 particles
of mass one) is asked to expand at `water_density_rate` (6/s) times its error, and a void
inside the bulk, one whose four neighbours each hold at least 0.75 of rest or are walls,
to contract; the error is capped at 0.5. The free surface and uniformly sparse clouds
only ever expand, never contract. This pins a pool's level to its particle volume and
removes seeding noise (a settled dam break measured 3% voids and 6% over-dense cells,
std 0.29, before; 0.03% voids, none over-dense, std 0.04 after). The term is part of the
right-hand side only: the operator, gradient and projection are unchanged.
`POWDER_WATER_DENSITY_RATE=0` disables it; high rates also slow thin flows.

Calm water (packing). At the start of a tick `water_sleep.comp` advances a still-tick timer kept in
`state1.x` (1 + ticks slower than `water_sleep_speed`, 2 cells/s, saturating at 255) and tallies each cell's
particles and ready ones (timer at least `water_sleep_ticks`, 30, and the brush's particle mass).
`water_pack.comp` phase 0 flags a cell to pack when it holds the rest count +-1 (3 to 5) of particles, all
ready, and its 7x7 neighbourhood is liquid (at least half rest density in the last field build, no wall or sand,
inside the domain); compaction drops exactly those particles and phase 1 stores their number in the R8UI
`water_packed` texture. After the tick's last projection phase 2 flags a packed cell to wake (bit 7) when one of
its faces moves faster than `water_wake_speed` (5 cells/s) or a non-liquid, wall or sand cell comes within two
cells, and keeps the projected cell velocity; the next tick's phase 1 turns its count back into particles at
hashed positions in the cell with that velocity (all or none: a full pool keeps it packed and counts
`particleOverflowBlocked`). Erase, solid and water-clearing brushes clear packed cells with the water. Moving plus
packed particles is an exact integer invariant (`app-water-pack-conservation`); replays, signatures and
`ReadWaterLiveParticles` count both. Each rule reads the previous state and writes only its own cell, so nothing
has a sideways order.
Packed cells stay in the grid as rest-density water at rest: `water_mac_build.comp` gathers, through a 20x20
shared tile, each packed cell's P2G kernels averaged over the cell (cubic B-spline face weights, 1/8-3/4-1/8 cell
weights, zero momentum), so the liquid volume, phi, render, sand coupling and the pressure solve see them as
water. The divergence skips the density correction for packed cells and their 3x3 neighbours: a packed cell's
deposit is not its particles' exact deposit, and correcting that difference at 20/s drove flows that woke 90%
of a still deep pool (5-10% stayed packed instead of 93%). The surface band (three or more cells) and water within
three cells of walls or sand never pack, so waves and wall contact stay particle-resolved. `water_sleep_ticks` 0
disables packing.

Particle velocity blends FLIP/PIC and recovers affine moments from the quadratic
kernel. Damping scales with elapsed time. Swept cell traversal blocks thin walls,
corners and domain exits; blocked normal velocity and affine rows are removed. A
face with solid or sand on both sides holds no fluid velocity, so grid-to-particle
transfer and advection read the nearest live face toward the particle instead (the
same substitute for the previous and the projected fields, keeping the kernel's full
support): painted walls are free-slip like the domain edge, and sand's moving wall
faces (one side sand, one side water) keep their velocity.

Substeps follow the fastest particle at the start of the tick: the compaction pass
records its speed and the live count, the host reads both back (one extra synchronization
per tick; water P2G and the particle step dispatch that count, not the pool) and
derives the count from the CFL limit, from one substep for still water up to
`water_max_substeps`. The speed cap `water_max_velocity` (160 cells/s, above the
damped free-fall speed of about 145) must fit that budget at the CFL limit, which, like
a gravity impulse one substep cannot store, is rejected before any dispatch. A particle
whose velocity or advection speed is cut by the cap is counted in
`water_velocity_clamped` (fifth debug counter, F10 blue channel, every replay line). It
is a diagnostic: replays and benchmarks fail on the first four counters only.

## Sand

Particles carry elastic deformation and rest area. Quadratic APIC transfers feed
Hencky elastic stress into grid momentum. Separating grid contact and Coulomb wall
friction act before grid-to-particle transfer. The velocity gradient advances the
elastic deformation, then principal logarithmic strains undergo Drucker-Prager
plastic projection. Swept collision remains a geometric crossing guard.

The implemented material uses a fixed yield cone, without hardening. Reference
density is 2 (water is 1), shear modulus and first Lame parameter are 24000,
friction alpha is 0.5, and wall friction is 0.62. These are simulation units; this
is not a validated physical sand material. In 2D the cone is sin(phi) = sqrt(2)
alpha, 45 degrees nominal. The grid transfer dissipates, and poured piles measure
about 30 degrees, so alpha stays at its measured value: the nominal 33 degrees
(alpha 0.385) gives piles near 24. Stiffness was doubled from 12000 because the base
of a 100-cell column compressed to J 0.55 (now 0.68), at 20 instead of 18 material
substeps per tick. Wave speed plus the particle velocity cap determines stable
material substeps. Insufficient budgets are rejected before dispatch. A deformation
that cannot be projected is counted, reset to its rotation and the particle keeps
moving; transfer keeps its mass.

Particle record: state2.z holds the volume correction (rest area is mass over reference density), state2.w the
friction trait as an integer in [-31, 31] plus the moisture in its fraction. The plastic projection adds the volume
correction to the log volume strain: a grain pulled apart is stress-free and remembers the dilation (up to
`sand_max_dilation`), so it must recompact before it bears load again. Friction alpha scales with mu(I) / mu_s. Moisture
relaxes once per tick toward the fullest water cell around the grain (`sand_wetting_rate`) and dries at
`sand_drying_rate`; capillary strength is 0 dry, full from 10% to 90% moisture and 0 again saturated, and widens the
cone by `sand_cohesion` of compression (tension still separates: two grains per cell cannot carry tensile stress, and a
shifted apex fractured and flattened damp columns). Damp grains get the confined grains' contact damping (liquid
bridges dissipate). The occupancy transfer also gathers moisture into a node buffer; `sand_rasterize.comp` writes the
cell's mean moisture (moving and packed) to an R8 texture, and composite.frag darkens sand by up to 45% with it.
Packed cells keep their grains' mean moisture in `packedFTex.z` (the mean stretch is symmetric, so [1][0] is spare),
soak up water around them but do not dry, and woken grains restart with it.

Confined sand dissipates: grains with volume ratio J below 0.95 (ramped to full at 0.85, so
free, loose and surface sand is untouched) get a Kelvin-Voigt stress J eta (C + C^T) on the
APIC velocity gradient (`sand_viscosity` 200; its explicit diffusion limit, at half the 2D
bound, joins the wave limit: 14 substeps, under the 23 the waves need) and a contact damping
of their velocity and affine at `sand_contact_damping` 6/s. Without them a deep fill rang
for seconds: the viscosity removes the grain-scale jitter, the damping the bed's bulk
vertical mode (period about 2 s at these moduli, out of the explicit viscosity's reach).

Occupancy is mass over reference density, so a cell packed at rest density reads 1.
A poured pile is loose (about 0.4 to 1), a lone grain peaks at 0.28.

Sleeping sand (sleep-packing). Each tick, before compaction, `sand_sleep.comp` advances a
still-tick timer kept in `state1.x` (1 + ticks slower than `sand_sleep_speed`, 4 cells/s,
saturating at 255) and tallies per cell its grains, the ready ones (timer at least
`sand_sleep_ticks`, 12) and their summed left stretch. `sand_pack.comp` then decides per cell
from the previous packed state and this tally, writing only its own flags: a cell whose
grains are all ready packs when it is supported (floor, wall, packed or all-ready below) and
calm (no side open steeper than 45 degrees, no sand node in its 3x3 faster than
`sand_wake_impact_speed` 60, no half-full water there faster than `sand_wake_water_speed` 40);
a packed cell wakes when it loses support or is not calm. The apply phase adds a pack to the
R8UI count (up to 64 per cell; deep beds compress to seven or eight) and turns a wake back into
grains at hashed positions in the cell with the cell's sand-node velocity and the mean volume
ratio J (isotropic F), reserving all of them or none (a full pool keeps the cell packed and
counts `particleOverflowBlocked`). Compaction drops exactly the tallied grains of packed cells,
so moving plus packed grains is an exact integer invariant (`app-sand-sleep-conservation`).
Packed cells are walls to the moving sand (grid nodes separate with wall friction, swept
collision stops at them), count in occupancy (drawn with the cell-averaged quadratic kernel,
so the boundary masks and spawn limit see them) and are cleared by erase and solid brushes.
The compaction also reduces the live cell box; P2G and the particle step dispatch the live
count and the grid clear/update cover that box (with a margin for the tick's travel) joined
with last tick's, so nodes the sand left are zeroed. `sand_sleep_ticks` 0 disables packing.

## Sand and water

Water sees sand as a wall wherever occupancy exceeds a quarter (mask 2 or 3). The wall
moves: water_mac_build gives faces next to sand the sand grid velocity, divergence
and projection keep it, so the pressure solve pushes water out of the way and the
level rises. Faces against sand also conduct at kSandLeak (0.005) of an open face. This
porosity is needed: water sealed inside sand is otherwise a singular system with a
nonzero net flux, which blew the solver up. A water particle caught in a sand cell
is carried a whole number of cells along the shortest axis to open water (never
across a painted wall, up to 24 cells), keeping its place in the cell and gaining
the sand's outward speed; it deposits nothing while caught. Sand grid nodes gather
the water in their 3x3 cells: submersion is the fullest cell, the flow is the
volume-weighted velocity. Gravity loses 1 / reference density of its weight when
submerged (buoyancy), and the velocity relaxes toward the flow by
1 - exp(-sand_drag submerged dt), stable at any dt. Water particles relax toward
the sand velocity at sand_drag reference_density occupancy, the equal and opposite
rate from F = rho_s phi_s k_s dv = rho_w phi_w k_w dv, before the velocity cap, so
water speeds keep their CFL bound. Because sand grid nodes inside a dense body see no
water, only its surface is dragged; dense bodies sink faster than dilute grains.

## Gas and coupling

Forward/reverse semi-Lagrangian transport uses a limited MacCormack correction whose reverse
trace follows u^n (Selle et al. 2008). Traces stop at walls, bilinear donors must be reachable,
and the limiter uses those same donors. Source and forward trace velocities obey the configured
component cap; projection can exceed that cap without changing its divergence correction.
Dissipation occurs after limiting and scales with elapsed time. Oxygen does not dissipate.
Buoyancy (beta T - alpha smoke) and vorticity confinement (Fedkiw et al. 2001) precede the
projection. Burning cells ask the projection to expand by gas_expansion times the fuel burned per
second (reaction.x, Nguyen et al. 2002), so the system needs an outlet: the top edge is open air
(zero pressure, `gas_open_top`), the other edges stay walls, and the top faces carry outflow.
The pressure solve is a flexible preconditioned conjugate gradient (`gas_cg.comp`), warm-started from
the previous solve, with one multigrid V-cycle as the preconditioner. Levels are 500 down to 16 cells;
each smooths with two red-black Gauss-Seidel sweeps in place before its coarser level and two in the
reverse colour order after, and one workgroup runs 16 red-black pairs each way on the 16x16 bottom in a
single dispatch, so the V-cycle is symmetric. Every level uses the unscaled 5-point operator; restriction
is the exact transpose of prolongation (bilinear weights summing to 4, a wall or out-of-range donor
replaced by the fine cell's own parent), as in the water MGPCG. Footprint kinds are built once per solve
(`gas_pressure_kind.comp`): a coarse cell is fluid when any child is, porous sand when all children are
solid and one is sand, else a wall; footprints past an odd edge count only their children inside the
domain. Dense sand (gas mask 3) conducts at kSandLeak like the water solve's sand walls: gas sealed in
sand pockets otherwise formed singular systems with incompatible right-hand sides, and CG broke down there
(a stalled residual, then infinite pressure). The open top weighs h / distance to the fine ghost row on
each level, which matters on the odd levels whose top row straddles the edge. Each solve checks the
stored-operator residual (`pressure_residual.comp`, the same operator) just before the previous solve's
iteration count and then every two iterations; it stops when the maximum is at most two thirds of the
threshold and the RMS at most a fifteenth (the old 0.01 and 0.001 at 0.015). The cap is 48 iterations.
An independent final maximum diagnostic always runs. A standalone V-cycle iteration (the previous
version) averaged 27 cycles in the combined benchmark with 143 of 960 solves at the cap; MGPCG averages
16 iterations, at most 21, with no residual failures.
Performance results and the current mean-frame-time shortfall are in VALIDATION.

Combustion, temperature diffusion and dissipation evolve after transport/projection.
The fire brush adds hot fuel and burns in ambient air: oxidizer is the oxygen fraction of fresh
air, starts at 1 and relaxes back toward it at gas_oxygen_rate. Brush stamps take the larger of
field and brush value (velocity: the larger magnitude), plus the cursor velocity and a +-15 cm/s
random spread per gas cell, so overlapping stamps do not saturate.
Reaction products use actual available fuel/oxidizer consumption. Temperature
diffusion respects sealed corners and no-flux walls; its explicit timestep keeps
nonnegative stencil weights or rejects an insufficient substep budget. Water suppresses heat/reaction with elapsed-time
retention. Sand and water exchange momentum as described above; sand's rasterized
occupancy walls off water and gas on the next tick. Rendering fields are derived,
not authoritative mass.

## Oil, steam and smothering

Oil is a water record with `state2.z = 1` (the 48-byte layout is unchanged; spawn writes the tag, compaction copies
it). It shares the water grid, pressure solve and density control. `phase_particles.comp` tallies oil and all
records per cell and `oil_field.comp` turns the tallies into an RG16F field (x: oil share, y: oil in full cells).
Separation (`water_particle_step.comp`): every record gets an upward `(1 - 0.87) g` times the bilinear oil share, a
Boussinesq body force on the mixture, so oil bodies rise while water flows around them; within a cell oil drifts up
and water down at 60 cells/s scaled by the other liquid in the cell or the one it moves into. A force on the oil
records alone was tried: FLIP kept the per-record slip it built and shot oil jets out of the pool. Oil weighs 0.87 of
water everywhere it is the local liquid, so a drop in open air also falls at 0.87 g (accepted, cosmetic).

A record is exposed when the cell above or beside it is open air. Exposed oil under gas at the ignition temperature
(0.3, hottest temperature or flame within four gas cells above) is lit (`state2.w`), stays lit above 0.2, and every
lit record feeds its gas cell hot fuel vapour, heat and flame (`steam_update.comp`, max like the fire brush), so a
slick burns on by itself like wood; it is consumed at 2/s per lit record. Exposed water evaporates at 2/s times
`smoothstep(0.25, 0.7, heat)` into the gas cell above as steam, an R16F gas-grid scalar carried by the gas velocity
plus a 100 gas cells/s rise and condensing at 0.8/s; it is drawn white and does not push the gas. Both removals are
explicit sinks: `phase_state_.sinks` counts evaporated and burned records since the reset (`ReadPhaseSinks`), and the
`app-oil-burn`, `app-steam` and `gas-quenching` fixtures check live records plus sinks against what was poured.
Oil does not quench: the water amount in `coupling_fire.comp` and `coupling_heat.comp` is scaled by one minus the oil
share. Sand smothers: a gas cell whose 2x2 mean sand occupancy reaches 0.35 is suppressed like a fully wet one.
The passes run in `RunCouplingPass` once oil, fire or wood has been painted since the reset.

## Scheduling and rendering

Each tick runs spawn/erase, boundary masks, water, gas, sand, coupling and render
extraction. Heavy solver work remains full-grid where globally coupled. Coupling
runs directly over the gas grid: measured tile scheduling cost more than the work
it skipped in idle, sparse and dense scenes, with identical resulting fields.
The composite pass runs once per displayed frame.

`FixedStepClock` accumulates elapsed time in 1/60-second ticks. A frame that kept up (at
most 1.5 ticks of wall time) may run two ticks; after an overrun it runs one; any backlog
beyond one tick is dropped. A tick that costs more than its own duration therefore slows
the simulation down instead of compounding into longer frames (the earlier four-tick
catch-up turned a 25 ms tick into 100-180 ms frames). Below 40 Hz every frame counts as
an overrun, so such displays run the simulation in slow motion. Replays, tests and
benchmarks run fixed ticks and do not use the clock. The interactive loop body is
`RunInteractiveFrame` (ticks, `Render(true)`, `DrawHud`, swap), shared with the
`benchmark-interactive` profile; it requests adaptive vsync (swap interval -1) where
`*_swap_control_tear` is available, so a late frame tears instead of waiting a refresh.
The spawn pass paints from `painting_`, the button state `UpdateInput` read that frame.

The interactive loop draws `Render(true)` and then `DrawHud()`. `Render(true)` draws
the whole framebuffer in one composite pass: the simulation fills the largest
grid-aspect (square) rectangle centred in it (`FitSimRect`, passed as `simRect`) and the
shader paints the letterbox margin with the same cave-wall albedo, so there is no seam.
The clear colour is black and only shows if a draw fails. `Render()` with no
argument still stretches the simulation over the whole framebuffer; replays and pixel
tests use it, so they never contain the HUD. `MapCursorToGrid` converts window units
to framebuffer pixels before subtracting the letterbox offset, so margins and DPI
scaling map to the same cell the shaders paint. Painting requires the cursor to be
over the simulation and the window hovered, because GLFW keeps reporting the last
position after the cursor leaves.

`DrawHud(now)` is one fullscreen `hud.frag` pass, blended with premultiplied alpha over
the finished frame and sized in whole-number steps of about 450 framebuffer rows. Plates,
tiles and the brush ring are anti-aliased signed-distance shapes; text is the one-channel
atlas sampled on whole texels. Plates and tiles are pixel art on whole units (a unit is
`uiScale` pixels, so edges stay crisp at integer scale); only the brush ring is
anti-aliased. The stack is anchored top-left: an inventory plate (a row of eight 10-unit
slots with pixel-art motifs, their key numbers, the selected material's name and the brush
radius) over a compact help panel (key labels and descriptions, toggled by H, which also
swaps it for a dim "H HELP" hint). The plates are dark, low-opacity boxes with a one-unit
warm edge and cut corners; the selected slot has a bright warm outline and its icon at
full brightness. In a 1280x900 window the whole stack (2-unit edge and padding) exactly fills the 190 px letterbox margin
and never touches the play area (`hud::FitsInMargin`); with a narrower margin the same
stack overlays the corner of the simulation with slightly more transparent plates. The
help panel is up at launch and hides itself `hud::kHelpSeconds` after it was last shown
(`help_hide_time_`, armed by the interactive loop and by H, so tests that set
`help_visible_` are unaffected). The brush ring is evaluated in grid space so its radius
matches the spawn shaders: a thin tinted ring that thickens and brightens while the button
is held, dashed and reddish with a cross for erase.
Every region tests its bounding box first. Text comes from a 5x7 bitmap font baked at
startup (`src/HudFont.hpp`), which also owns the layout constants passed to the shader.
Motion is host state in `HudMotion`: help visibility, painting and each slot's selection
move toward their targets by `1 - exp(-dt / tau)` per frame (tau 50-90 ms), so a change
settles in about 150 ms, starts fast, retargets without a jump when interrupted, and snaps
after a stall. Nothing in the HUD moves on its own: the painting ring is thicker and brighter
but steady (it used to pulse at 1.2 s and its erase dashes crawled), so the eye stays on the matter. The window title is refreshed
about twice a second with the material, brush radius and frame rate.

`composite.frag` styling is presentation only, feeds nothing back and has no time
dependence. Everything in the scene is an albedo that the light multiplies at the end,
and only flames, embers and the hot-gas halo are emissive. Albedos: a near-black cave
wall of static per-cell noise (the same function fills the letterbox margin); solid rock as
multi-tone cool grey from the cell and its 2x2, 8x8 and 16x16 blocks, with olive moss
patches, a darker interior and a crusty rim (six cells deep on exposed tops, three on the
sides and undersides) that fades out by a per-cell threshold so it breaks up like
dithered pixel art; ochre sand with per-cell shade and light and dark specks, a soft
coverage edge and a pale top rim. Water is murky teal that darkens with depth, with a thin
bright surface line, static shimmer cells near the surface and foam only for fast flow;
coverage saturates quickly, so thin one- or two-particle water stays visible (earlier
thresholds sat above typical free-surface amounts and hid it). Smoke is a pale grey haze lit
by the scene. Fire is additive flame on a blackbody-like ramp of the gas temperature (deep red,
orange, yellow, yellow-white core), an orange glow of hot gas that outlives the flame, a bloom from four jittered taps of the same temperature field, and
static per-cell embers inside the halo, all soft-saturated.

Light is `ambient + glow + lantern`, summed in linear units and applied as its square
root (albedo times light in gamma-2 space), with a strong vignette on the ambient. The
ambient is a dim cool moonlight (about 0.2), so the composition still reads. The glow
comes from `light_build.comp`, which runs once per `Render` when lighting is on: it
averages the render temperature and fire fields into a 250x250 RGBA16F texture (one
texel per 4x4 cells), the host calls `glGenerateMipmap`, and the composite reads mips 1-4
(8 to 64 cells per texel) with weights summing to 2, so a flame lights its surroundings without
turning the whole screen orange. The light comes from the flame, plus a little from gas near T = 1. Warm light also washes the blue out of the stone, so lit
rock reads orange-brown. The lantern is a radial falloff around the cursor (base radius
130 cells plus 1.5 per brush cell, swelling 12% while painting) that only letterboxed
frames with the cursor over the simulation get; replays and pixel tests never see it.
`lighting_enabled_` (uniform `lighting`) switches the light and the lantern off, leaving
plain albedo. The debug views F2-F10 are unchanged and unlit. The render-modes test
mirrors the albedo math on the CPU (lighting off) for scenes without water, smoke or fire,
and separately checks that sparse water renders blue; the lighting test checks the light
qualitatively.

Bracket sizing uses native key press/repeat events rather than rendered-frame count.
Brush/view selection and Escape use press callbacks, preserving short taps and
event order. GLFW sticky mouse buttons retain a brief click until a simulation tick
consumes it. Cursor and left-press callbacks collect the cells the cursor reported
since the last stamp (bounded, event rate rather than frame rate); while the button
stays held inside the simulation a tick stamps the polyline from the previous stamp
(or from the press) through those cells to the cursor, evenly spaced at no more than
max(1, radius/2) cells (at most 64 stamps per tick). Catch-up ticks stamp only the
cursor, and release, leaving the simulation or changing brush restarts the stroke.
Stamps overlap, so each sand and water stamp skips the previous stamp's disc: a swept
cell receives one held tick of particles (not three to four) and a fast stroke does
not drain the particle pools. Smoke and fire stamps take the larger of field and
brush, so overlap does not build a saturated trail. Scripted replay brushes remain single-point stamps. A solid brush also
clears the water and sand particles in its cells; erase clears them only inside its
disc. Particle clears test the cell a particle occupies against the disc, matching
the grid clears, and no longer cull particles in blocked cells elsewhere on the map
(that had deleted water under sand across the whole grid). Gas clearing, used by
erase, also zeroes the right and top velocity faces shared with neighbouring cells.

The interactive water brush carries a density gate: `water_density_limit` is the
particles emitted per cell times the mass per particle, and `water_particle_spawn.comp`
skips cells whose liquid volume has already reached it, so a held brush stops stacking
particles. Scripted and replay brushes pass zero and keep exact emission.
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

The 80-case CTest suite (including the shader-package check and the boundary and
leakage replays) and all six repeated replay scenes pass on the v0.5.0 tree. The
real-time benchmark meets its timing budgets there but fails on its
`pressure_nonconverged` counter; see VALIDATION. The HUD, letterbox and
cursor mapping, stroke interpolation, water density gate and composite look are
covered by the `app-hud`, `app-brush-input`, `app-erase` and `app-render-modes`
fixtures and offscreen frames. An earlier Computer Use pass exercised brief input,
brushes, erasing, diagnostic shortcuts, width resizing, close and relaunch. Live
mouse painting, the H key in a live window, held-key timing, DPI transitions and
sustained native stress remain unverified.
Linux and MinGW packaging are build targets but have not been executed in
this Windows validation environment. No release is published by this goal.

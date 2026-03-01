# Powder Game Plan (C++20, OpenGL 4.3+ Compute)

## Goals
- 1000x1000 simulation window at 60 fps on a modern GPU.
- Materials: sand, water, smoke, fire, heat.
- GPU-first pipeline; CPU only orchestrates, spawns, and renders.

## Constraints
- C++20, OpenGL 4.3+ compute shaders.
- No CPU-GPU readback in the frame loop.
- Structure of Arrays (SoA) layout; pack only when fields are read together.

## Grid Resolutions
- Base grid: 1000x1000 (cells).
- Water (LBM D2Q9): full res.
- Fire reaction-diffusion: full res.
- Heat/temperature: half res (500x500).
- Smoke velocity/density: quarter res (250x250).

## Core Data Layout (SoA)
Base grid textures:
- material_id: R16UI (material in low bits; sleep/static/flags in high bits).
- velocity: RG16F (shared for sand and general advection).
- sand_stress: R16F.

Water LBM (ping-pong):
- lbm_a0: RGBA16F
- lbm_a1: RGBA16F
- lbm_a2: RG16F (or RGBA16F if alignment issues)
- lbm_b0: RGBA16F
- lbm_b1: RGBA16F
- lbm_b2: RG16F
- water_pressure: R16F (standalone)

Smoke (quarter res, ping-pong):
- smoke_vel_a: RG16F
- smoke_vel_b: RG16F
- smoke_den_a: R16F
- smoke_den_b: R16F

Fire (full res, ping-pong):
- fire_rd_a: RG16F (U,V in Gray-Scott)
- fire_rd_b: RG16F
- fire_temp_a: R16F (full-res temperature for blackbody color mapping)

Heat (half res, ping-pong):
- heat_a: R16F
- heat_b: R16F

Active tiles:
- tile_active: R8UI (one texel per 16x16 tile).
- tile_bounds: optional SSBO for compact active list (future optimization).

## Sand (Verlet + Swept DDA)
- Verlet integration with restitution and damping on collision.
- Multi-step DDA along the parabolic arc each frame; cap max steps.
- GPU conflict handling:
  - Phase 1: compute desired target cell into SSBO (one per particle).
  - Phase 2: resolve conflicts deterministically using atomics on a per-cell winner buffer.
  - Phase 3: commit winners, losers keep residual velocity.
- Angle of repose:
  - Each sand cell stores stress; if local slope > threshold, relax with cellular rules.

## Water (LBM D2Q9)
- Collision step: relax toward equilibrium with tunable omega.
- Streaming step: propagate to neighbors.
- Boundary: bounce-back at solid material_id.
- Pressure derived from density; stored in water_pressure.
- Keep LBM full res.

## Smoke (Stable Fluids, quarter res)
- Velocity advection (semi-Lagrangian).
- Projection to enforce incompressibility.
- Vorticity confinement for curl.
- Density advection.
- Up-sample to full res for rendering and coupling.

## Fire (Reaction-Diffusion + Heat)
- Gray-Scott model for flame front (full res).
- Heat diffusion on half res; bilinear sampling into fire for color/ignition.
- Color mapping via blackbody approximation in shader.
- Optional embers: SSBO of particles with ballistic motion and cooling.

## Coupling (Cross-Material)
- Sand-water: water flow applies drag to sand velocity; sand blocks LBM.
- Fire-heat: heat drives ignition; fire adds heat.
- Smoke buoyancy: heat field adds upward force in smoke velocity.
- Water-fire: water dampens or extinguishes fire; optional steam generation.
- All coupling done via explicit passes with resolution-aware sampling.

## Chunking on GPU
- Tile size: 16x16.
- tile_active updated by:
  - input spawns
  - any cell movement or solver output
- Compute passes early-out if tile is inactive.
- Optional future: compact active tiles into SSBO with atomic append to reduce dispatch cost.

## Frame Pipeline (per frame)
1. Update tile_active (input spawns, wake neighbors).
2. Sand pass (Verlet + swept DDA + stress relaxation).
3. Water LBM (collision + streaming).
4. Smoke stable fluids (advect, project, vorticity, density).
5. Fire RD + heat diffusion.
6. Coupling passes (buoyancy, damping, ignition). Note: this introduces a one-frame lag for forces like buoyancy, which is acceptable and more stable.
7. Render (materials + fire color + smoke density).

## OpenGL Implementation Notes
- Compute shaders with workgroup 16x16.
- Ping-pong buffers per solver.
- Use glMemoryBarrier after each pass:
  - GL_SHADER_IMAGE_ACCESS_BARRIER_BIT
  - GL_SHADER_STORAGE_BARRIER_BIT as needed
- Avoid CPU readbacks in the frame loop.

## Performance Targets
- Keep total passes minimal; no extra read/modify/write.
- Prefer RG16F over RG32F unless precision issues appear.
- Limit sand DDA steps per frame.

## Milestones
1. GPU infrastructure: textures, ping-pong, dispatch, render.
2. Sand only (Verlet + DDA + stress).
3. Water LBM full res.
4. Smoke stable fluids at quarter res.
5. Fire RD + heat diffusion.
6. Coupling passes.
7. Optimization pass: tile compaction, format tuning.

## Risks
- Bandwidth pressure from full-res LBM + full-res RD in same frame.
- Conflict resolution cost for sand on GPU.
- Parameter tuning for stability and look.
- Gray-Scott F/k sensitivity: narrow stable region for flame-like patterns; budget tuning time before coupling to heat.

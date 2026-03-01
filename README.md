# Powder Game

[![CI](https://github.com/FueledByRedBull/Powder-Game/actions/workflows/ci.yml/badge.svg)](https://github.com/FueledByRedBull/Powder-Game/actions/workflows/ci.yml)
[![Release](https://github.com/FueledByRedBull/Powder-Game/actions/workflows/release.yml/badge.svg)](https://github.com/FueledByRedBull/Powder-Game/actions/workflows/release.yml)

A GPU-first powder sandbox built in C++20 with OpenGL 4.3 compute shaders.

`1000 x 1000` simulation grid, real-time materials, and a multiphase compute pipeline for sand, water, smoke, fire, and heat.

## About

Powder Game is an experimental real-time simulation project focused on a fully GPU-driven architecture.

- Tech stack: `C++20`, `OpenGL 4.3+`, compute shaders
- Design goal: rich multi-material interactions without CPU readback in the frame loop
- Scope: sand, water, smoke, fire, heat, and cross-material coupling
- Platform support: Linux and Windows 11 (standalone package flow included)

## Why This Project

This project focuses on one goal: keep the simulation on the GPU and maintain interactive performance while layering multiple coupled systems.

## Features

- `Sand` with Verlet-style motion + conflict-resolved movement
- `Water` via D2Q9 LBM at full resolution
- `Smoke` stable-fluid style solver at quarter resolution
- `Fire` reaction-diffusion + half-res heat coupling
- Cross-material coupling passes (fire/heat/smoke/water/sand)
- No CPU readback in the frame loop
- Active-tile culling pass to skip heavy work in idle regions
- Tuned texture formats (smoke density in `R8`) to reduce bandwidth
- Windows packaging script for standalone `.exe` output

## Controls

- `1`: Sand brush
- `2`: Water brush
- `3`: Solid brush
- `4`: Erase brush
- `5`: Smoke brush
- `6`: Fire brush
- `[` / `]`: Brush radius down/up
- `Left Mouse`: Paint
- `Esc`: Quit

## Quick Start (Linux)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/powder_game
```

Notes:

- OpenGL `4.3+` is required.
- Dependencies are auto-fetched by CMake if not available as system packages.

## Windows 11 Standalone Build (from Linux)

Build and package a Win11 x64 zip:

```bash
./build-win11.sh
```

Output:

- `dist/win11/PowderGame-win11-x64.zip`

## Pipeline Overview

Per-frame compute flow:

1. Spawn/input pass
2. Sand pass (prepare, desired, resolve, commit)
3. Water LBM pass + pressure extraction
4. Smoke pass (advect, vorticity, project)
5. Fire + heat passes
6. Coupling passes
7. Composite render

## Project Layout

- `src/`: C++ app and GL utilities
- `shaders/`: compute + render shaders
- `PLAN.md`: architecture and milestone plan
- `build-win11.sh`: Win11 standalone packaging

## Current Status

Milestones `1-7` are implemented from `PLAN.md`.

Current focus areas:

- simulation tuning and visual polish
- deeper active-tile compaction strategies (indirect-dispatch path)

## Build Philosophy

- GPU-first orchestration
- SoA-friendly texture layout
- explicit memory barriers between passes
- ping-pong resources for solver stability

# Powder Game

[![CI](https://github.com/FueledByRedBull/Powder-Game/actions/workflows/ci.yml/badge.svg)](https://github.com/FueledByRedBull/Powder-Game/actions/workflows/ci.yml)
[![Release](https://github.com/FueledByRedBull/Powder-Game/actions/workflows/release.yml/badge.svg)](https://github.com/FueledByRedBull/Powder-Game/actions/workflows/release.yml)

A GPU-first powder sandbox built in C++20 with OpenGL 4.3 compute shaders.

`1000 x 1000` simulation grid, real-time materials, and a multiphase compute pipeline for sand, water, smoke, fire, and heat.

## About

Powder Game is an experimental real-time simulation project focused on a fully GPU-driven architecture.

- Tech stack: `C++20`, `OpenGL 4.3+`, compute shaders
- Design goal: rich multi-material interactions with GPU simulation and bounded CPU orchestration
- Scope: sand, water, smoke, fire, heat, and cross-material coupling
- Build targets: Linux and Windows 11; current runtime validation uses Windows 11

## Why This Project

This project focuses on one goal: keep the simulation on the GPU and maintain interactive performance while layering multiple coupled systems.

## Features

- `Sand` with quadratic APIC transfers, elastic deformation, Hencky stress, Drucker-Prager plasticity, and Coulomb contact
- `Water` with quadratic particle/MAC transfers, APIC/FLIP velocity updates, RK2 transport, and a single-precision conjugate-gradient pressure solve
- `Gas` on a half-resolution velocity + scalar domain for smoke, temperature, oxidizer, and combustion
- Derived render/debug fields for water, gas velocity, gas pressure, smoke, temperature, and combustion
- Cross-material coupling passes (fire/heat/smoke/water/sand)
- Small pressure convergence readbacks stop completed GPU solves; full particle readback is validation-only
- Direct coupling passes, cached static boundaries, and bounded water-pressure work
- Half-precision transport/render fields, single-precision pressure, and wide fixed-point particle momentum sums
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
- `F1`: Composite; `F2`: Boundary; `F3`: Water; `F4`: Water velocity
- `F5`: Gas velocity; `F6`: Gas pressure; `F7`: Smoke; `F8`: Temperature
- `F9`: Fuel/reaction; `F10`: Failure counters
- `Esc`: Quit

## Quick Start (Windows 11)

From a Visual Studio 2022 Developer PowerShell with CMake and Python available:

```powershell
$env:PYTHONUTF8 = '1'
cmake -S . -B build-msvc -A x64
cmake --build build-msvc --config Release --parallel
.\build-msvc\Release\powder_game.exe
```

Keep the generated `shaders` directory and copied DLLs beside the executable. The app deliberately
loads that copy, including when started from another working directory. CMake
refreshes runtime files on shader-only builds. `PYTHONUTF8` lets the pinned GLAD
generator read its bundled OpenGL specification consistently on Windows locales.
Windows packages include declared shared-library dependencies and the MSVC release
runtime when applicable; `runtime-dlls.txt` is the build's staging manifest.

## Quick Start (Linux)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/powder_game
```

Notes:

- OpenGL `4.3+` is required, with at least 18 image units/compute image uniforms,
  19 combined shader output resources, and 8 SSBO binding points. The app checks
  these limits on startup; a version string alone does not guarantee support.
- Dependencies are auto-fetched by CMake if not available as system packages.

## Windows 11 Standalone Build (from Linux)

Build and package a Win11 x64 zip:

```bash
./build-win11.sh
```

Output:

- `dist/win11/PowderGame-win11-x64.zip`

## Pipeline Overview

Each fixed simulation tick runs:

1. Spawn/input pass
2. Boundary masks and cached static distance field
3. Water particle/grid, pressure, and particle update
4. Gas advection, multigrid pressure, and combustion
5. Sand material substeps and rasterization
6. Direct gas-grid coupling
7. Render-field extraction

Rendering follows the fixed ticks. Elapsed time determines the number of ticks;
stall catch-up is bounded at four. Water/gas CFL and sand material-wave constraints
determine substeps; settings that exceed their configured budgets fail explicitly.

## Project Layout

- `src/`: C++ app and GL utilities
- `shaders/`: compute + render shaders
- `PLAN.md`: architecture and milestone plan
- `build-win11.sh`: Win11 standalone packaging

## Current Status

The stabilization and algorithm review goal was completed with native interactive
checks waived. A subsequent Computer Use pass verified brief input, brushes,
erasing, width resizing and relaunch, and fixed missed short key taps/clicks.
Held-key timing, DPI transitions and sustained native stress remain unverified.
[ROADMAP.md](ROADMAP.md) records verified changes,
historical failures, outstanding work, and acceptance targets. [VALIDATION.md](VALIDATION.md)
contains build/test/replay commands and hardware-specific measurements. The existing
`dist/win11` bundle is an older release and is not evidence for the current source.

Run native and GPU checks with `ctest --test-dir build-msvc -C Release --output-on-failure -j 1`.
GPU tests require a working OpenGL context. The real-time target is 30 FPS at
1000x1000 with 100,000 water and 50,000 sand particles plus smoke/fire; 60 FPS is
preferred. The earlier RX 7800 XT checkpoint reached 31.5 FPS. After the native
input fix, four runs measured 29.71-29.83 FPS, narrowly missing the unchanged
30 FPS gate; GPU p95 stayed below 50 ms with zero errors and sampled mass drift.
These measurements include two simulation ticks plus rendering per frame and
exclude display/vsync latency. All 44 tests and two repetitions of all six replay
scenes pass on the input-fixed build. See VALIDATION for the performance follow-up.

## Build Philosophy

- GPU-first orchestration
- SoA-friendly texture layout
- explicit memory barriers between passes
- ping-pong resources for solver stability

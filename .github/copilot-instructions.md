# Copilot Instructions

## Project Overview

A **polyhedral dice roller** for the Miyoo Mini Flip (MMF) handheld console running OnionOS. Simulates realistic 3D dice physics using Bullet3, renders with Raylib/TinyGL software rasterization (no 3D GPU), and displays the result on a 750×560 framebuffer via MI GFX hardware-accelerated blitting.

**Why unusual**: The MMF's SSD202D SoC has no 3D GPU, no EGL/GLES support, and no working SDL GL context. Solution: `PLATFORM_MEMORY` + `GRAPHICS_API_OPENGL_11` with [TinyGL](https://github.com/jserv/tinygl) software renderer (jserv fork, not C-Chads), rendering directly into a framebuffer pointed at `/dev/fb0`.

## Build Commands

Requires [`just`](https://just.systems/), Docker, and SSH access to the device.

```sh
# 1. Fetch glibc sysroot from the device (one-time setup)
just sysroot-fetch <mmf-host>

# 2. Cross-compile for MMF (runs inside Docker)
just build

# 3. Deploy to device
just deploy <mmf-host>

# 4. Test locally (emulated ARM, screenshot after N frames)
just try 60

# Clean build output
just clean
```

**Build pipeline**: `just build` → Docker image build → `docker/build.sh` runs:
1. **Pre-bake assets** (host x86 only) — compute lighting SH coefficients, bake hardwood floor texture, pre-slice skybox tiles
2. **Compile TinyGL** — all `.c` files in `third_party/tinygl/src/` → static library
3. **Build Raylib** — `third_party/raylib/src/Makefile` with `PLATFORM_MEMORY` + `GRAPHICS_API_OPENGL_11`
4. **Link binary** — `src/{main.cpp,physics.cpp,rendering.cpp,stat_shim.c,tinygl_stubs.c}` + libraylib.a + libTinyGL.a → `dist/raylib-cube`
5. **Bundle OnionOS app** — copy `assets/{launch.sh,config.json,icon.png}` to `dist/`

Build artifacts in `dist/`. Incremental builds use ccache (`.cache/ccache/`).

**Override flags** (rarely needed):
```sh
RAYLIB_PLATFORM=PLATFORM_MEMORY RAYLIB_GRAPHICS=GRAPHICS_API_OPENGL_11 just build
```

**No test suite** — testing is via `just try` (headless ARM emulation + screenshot).

## Architecture

### High-level app flow (main.cpp)

```
Startup:
  InitWindow() → InitPlatform() (opens /dev/fb0, queries MMF framebuffer resolution)
  Boot splash screen shows 6 init steps
  InitPhysics(), InitNumberAtlas(), InitWoodTexture(), InitSkybox(), InitScratchTexture(), InitDirtTexture()
  ThrowAll() — spawn initial dice

Main loop (30 FPS):
  1. StepPhysics(dt) — advance Bullet3 simulation
  2. Handle input: hotbar (D-pad), camera (L/R1/L/R2/Y/X), throw (A button)
  3. Detect settled dice, read face-up value
  4. RenderFrame():
     - 3D pass: draw scene in correct z-order (skybox → ground → shadows → reflections → dice → effects → decals)
     - 2D pass: draw hotbar UI + help overlay
  5. Optional headless screenshot (--screenshot N flag for `just try`)

Shutdown:
  CloseWindow() (closes /dev/fb0, stops polling input)
```

**Global mutable state** (defined in `physics.h`, owned by physics engine):
- `ActiveDie dice[MAX_ACTIVE_DICE]` — array of 12 dice with Bullet physics bodies
- `int numDice` — how many are currently active
- `int hotbarCount[NUM_DICE_TYPES]` — queued count for each die type (d4, d6, d8, d10, d12, d20)
- `int hotbarSel` — which die type is selected in UI
- `int riggedValue` — for debug mode: -1 = fair roll, else force this face (env var `RAYLIB_MMF_RIG`)

### Rendering pipeline (rendering.cpp, in strict z-order)

Called each frame from `RenderFrame()`:
1. **Skybox** — cylindrical panorama background (clamped repeat)
2. **Ground** — pre-lit hardwood floor quad with baked specular (SH coefficients pre-computed at build time)
3. **Shadows** — per-die orthogonal projection onto ground plane
4. **Reflections** — y-flipped mirror of all dice (semi-transparent, blended)
5. **Dice faces** — Gouraud-lit (per-vertex lighting with two-light key+fill setup); sorted back-to-front (painter's algorithm)
6. **Scratch overlay** — per-die additive blend of scratch highlights (from normal map)
7. **Dirt overlay** — per-die bump highlights from dirt texture
8. **Edges** — subtle wireframe edge-only quads (post-hoc silhouette)
9. **Bloom halos** — specular glow around dice (additive quad halos, not screen-space)
10. **Post-process** — bloom + depth fog (opt-in via `enablePostProcess`)
11. **Hotbar** — 2D UI bar at bottom (die type selector + count display)

**Critical ordering**: Dice must be sorted back-to-front **before** any dice are drawn, then all geometry interleaved per die (face → shadow → reflection → scratch → numbers → edges → bloom). Depth writes disabled during dice drawing (to prevent Z-fighting with overlays).

### Physics engine (physics.cpp, Bullet3 integration)

- **World setup**: gravity = {0, -15, 0}, floor plane at y = -1
- **Dice spawning** (`ThrowAll()`): for each queued die in hotbar, create Bullet rigid body with random position/velocity, add to world
- **Stepping** (`StepPhysics(dt)`): `world->stepSimulation(dt)`, then check each die for settlement (velocity < threshold)
- **Face detection** (`GetFaceUpValue()`): after die settles, find which face normal points most upward (dot product with Y-axis); read the `.value` from that face struct
- **Debug rigging**: if `riggedValue >= 0`, call `SnapDieToValue()` to rotate settled die so target face is up (purely visual, for testing)

### Cross-compile pipeline

```
Docker (ubuntu:22.04 + arm-linux-gnueabihf toolchain)
  ↓
Pre-bake (host x86 only): tools/prebake.c
  - Compute SH coefficients for two-light setup (stored in src/prebaked_sh.inc)
  - Bake floor texture (lit + specular) → assets/baked_floor.png
  - Pre-slice skybox tiles → assets/skybox_tile_*.png
  ↓
Compile TinyGL: third_party/tinygl/src/*.c
  - Flags: -O3, -march=armv7-a -mfpu=neon-vfpv4, -ffast-math, -fno-stack-protector
  - Output: libtinygl.a
  ↓
Build Raylib: third_party/raylib/src/Makefile (PLATFORM_MEMORY + GRAPHICS_API_OPENGL_11)
  - Output: libraylib.a (static)
  ↓
Compile app: src/{main.cpp, physics.cpp, rendering.cpp, stat_shim.c, tinygl_stubs.c}
  - Link order (circular deps): --start-group -lraylib -lTinyGL tinygl_stubs.o --end-group
  - Output: dist/raylib-cube
  ↓
Bundle: copy assets/ and launch.sh → dist/
```

## Key Conventions

### Target & Hardware

- **Target**: `armv7-unknown-linux-gnueabihf` (armhf), MMF native resolution **750×560** (enforced to be multiple of 4 for TinyGL)
- **SoC**: Sigmastar SSD202D dual-core ARM Cortex-A7, ~1.5 GHz
- **No 3D GPU**, no EGL/GLES, no DRM/KMS
- **Framebuffer**: `/dev/fb0`, ARGB8888 format, double-buffered via `FBIOPAN_DISPLAY` ioctl

### Build flags & defines

- **Compiler**: `arm-linux-gnueabihf-gcc/g++` inside Docker (ccache for incremental builds)
- **Arch flags**: `-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard` (Cortex-A7 NEON support)
- **Optimization**: `-O3 -ffast-math -ftree-vectorize` (aggressive FP optimizations safe for dice game)
- **Stack**: `-fno-stack-protector` (required for MMF ABI)
- **Linking**: `-ffunction-sections -fdata-sections -Wl,--gc-sections` (size reduction; binary ~6MB)
- **Raylib defines**: `RAYLIB_MMF_FB` (fb0 + MI GFX support), `RAYLIB_USE_TINYGL` (TinyGL ZBuffer integration); both in `CUSTOM_CFLAGS`

### Linking & dependencies

- **Raylib & TinyGL**: always **static** (libraylib.a, libTinyGL.a)
- **System libs**: glibc, libm, libpthread, libdl (dynamic, from sysroot)
- **Link order** (circular deps): `--start-group -lraylib -lTinyGL tinygl_stubs.o --end-group`
- **Sysroot**: `third_party/mmf-sysroot/` fetched via `just sysroot-fetch`; symlinks missing `.so` stubs (e.g. `libc.so → libc.so.6`)

### Special files

- **`stat_shim.c`**: shim `stat()` → `__xstat()` (sysroot lacks `libc_nonshared.a`); must always compile+link with main
- **`tinygl_stubs.c`**: provides GL functions missing from TinyGL (e.g. `glOrtho`, `glColor4ub`, `glVertex2i`)
- **`main.cpp`**: game loop, input handling, camera controls, frame coordination
- **`physics.cpp/physics.h`**: Bullet3 integration; spawning dice, stepping sim, detecting settlement, reading face values
- **`rendering.cpp/rendering.h`**: all visual output; z-order rendering, Gouraud shading, post-processing
- **`dice_defs.h`**: pure data; geometry (vertices/faces), MMF button mappings, constants

### Runtime environment variables

Set in `assets/launch.sh` (automatically passed to binary):
- `RAYLIB_MMF_SHOWFPS=1` — overlay FPS counter (also toggled via getenv in main.cpp)
- `RAYLIB_MMF_MIGFX=1` — enable MI GFX hardware blit (if libs available in dist/libs/)
- `RAYLIB_MMF_RIG=1` — enable debug rigging mode (B button cycles forced die values)
- `RAYLIB_MMF_SCALE=2` — framebuffer scale factor (rare; affects resolution)

### Input & controls

**Hotbar** (hotbar.cpp not present; implemented in main.cpp input loop):
- D-Pad Left/Right: cycle through die types
- D-Pad Up/Down: +/- count (max 6 per type, 12 total)
- A button: throw all configured dice

**Camera** (orbit + pan + freelook):
- L1/R1: rotate horizontally
- L2/R2: zoom in/out
- Y/X: tilt up/down (default mode)
- SELECT + D-Pad/Y/X: pan camera in world space
- START + D-Pad/Y/X: freelook (turn view direction from current position)
- SELECT or START (tapped alone): toggle help overlay
- B button: cycle rigged die value (debug mode only, if env var enabled)

**Buttons mapped to keycodes**: MMF_DPAD_* (265–262), MMF_A (32), MMF_X/Y/B/L1/R1/L2/R2 (340–259), MMF_SELECT/START (345, 257) — see `dice_defs.h`.

### Data structures

**`ActiveDie`** (dice_defs.h):
- `.typeIdx` — which DiceDef (0–5 for d4, d6, d8, d10, d12, d20)
- `.body` — Bullet rigid body pointer
- `.targetValue` — for rigging (debug mode)
- Managed by physics.cpp

**`DiceDef`** (dice_defs.h, static array):
- `.name` — "d4", "d6", etc.
- `.numValues` — highest face (4, 6, 8, 10, 12, 20)
- `.scaleFactor` — size tweak so all look proportional
- `.invertUpside` — d4 reads BOTTOM face, others read TOP
- `.numVerts`, `.rawVerts[][]` — canonical coordinates (pre-scale)
- `.numFaces`, `.faces[]` — face definitions (triangles, quads, pentagons)

**`Face`** (dice_defs.h):
- `.idx[]` — vertex indices (max 5 for pentagons)
- `.count` — 3/4/5 (tri/quad/pent)
- `.value` — number (1–20), or -1 for non-numbered edge faces (d10 only)

### Rendering notes

- **Transparency**: dice are semi-opaque (`DICE_ALPHA = 130`). Fresnel-driven edge opacity simulates glass with IOR ~1.5 (see `LitVertex` in rendering.cpp)
- **Lighting**: two-light key+fill setup (SH coefficients pre-computed at build time in `prebaked_sh.inc`)
- **Painter's algorithm**: dice sorted back-to-front before drawing to handle overlapping semi-transparent faces
- **Depth writes disabled** during dice drawing (to prevent Z-fighting with overlays)
- **Floor**: pre-lit and baked (no real-time shading); supports reflections of dice
- **Skybox**: cylindrical wrap (no top/bottom cap); clamped repeat for edges

### Shell & scripting

- `justfile` uses **zsh** (`set shell := ["zsh", "-c"]`)
- No Bash-isms in recipe bodies; stick to POSIX shell or explicit zsh syntax
- Recipes support env var substitution (`{{justfile_directory()}}`, `{{host}}`, etc.)

# Copilot Instructions

## Project Overview

Cross-compiles a [Raylib](https://www.raylib.com/) 3D demo (spinning cube) for the **Miyoo Mini Flip (MMF)** handheld running OnionOS. Uses `PLATFORM_MEMORY` + `GRAPHICS_API_OPENGL_11` with [TinyGL](https://github.com/C-Chads/tinygl) (C-Chads fork) as the software OpenGL 1.1 renderer, blitting directly to `/dev/fb0` — no EGL, no SDL, no DRM (none of those work on MMF).

## Build Commands

Requires [`just`](https://just.systems/), Docker, and SSH access to the device.

```sh
# 1. Fetch glibc sysroot from the device (one-time setup)
just sysroot-fetch <mmf-host>

# 2. Cross-compile for MMF (runs inside Docker)
just build

# 3. Deploy to device
just deploy <mmf-host>

# Clean build output
just clean
```

`just build` builds the Docker image (`docker/Dockerfile`), then runs `docker/build.sh` inside it. Build artifacts land in `dist/`. Incremental builds use ccache (cached in `.cache/ccache/`).

**Override build flags** via env vars before `just build`:
```sh
RAYLIB_PLATFORM=PLATFORM_MEMORY RAYLIB_GRAPHICS=GRAPHICS_API_OPENGL_11 just build
```

There is no test suite.

## Architecture

### Cross-compile pipeline

```
just build
  → docker build (ubuntu:22.04 + arm-linux-gnueabihf toolchain)
  → docker run → docker/build.sh
      → compile TinyGL .o files + ar rcs libTinyGL.a
      → make -C third_party/raylib/src  (static libraylib.a, PLATFORM_MEMORY + GL 1.1)
      → arm-linux-gnueabihf-gcc src/{main,stat_shim,tinygl_stubs}.c → dist/raylib-cube
      → cp assets/{launch.sh,config.json,icon.png} → dist/
```

### Renderer: TinyGL

`third_party/tinygl` (git submodule) is the [C-Chads TinyGL fork](https://github.com/C-Chads/tinygl) — a fast integer/fixed-point OpenGL 1.1 software renderer. It replaces raylib's built-in `rlsw` software renderer which was too slow on MMF (5–8 FPS).

Key integration points:
- Raylib is built with `GRAPHICS_API_OPENGL_11` (not `_SOFTWARE`). Its `rlgl.h` includes `<GL/gl.h>` which resolves to TinyGL's `include/GL/gl.h` via `-I` flag.
- TinyGL provides most `gl*` symbols. Missing ones (e.g. `glOrtho`, `glColor4ub`, `glScissor`) are in `src/tinygl_stubs.c`.
- In `rcore_memory.c`, `ZB_open()` + `glInit()` initialise TinyGL with the platform pixel buffer (zero-copy: TinyGL renders directly into MMA-allocated memory when MI GFX is active).
- TinyGL pixel format is ARGB8888 (`0x00RRGGBB`), matching `E_MI_GFX_FMT_ARGB8888` — no pixel conversion needed for the MI GFX blit path.
- TinyGL requires render width to be a multiple of 4 (`xsize & ~3`); this is enforced in `InitPlatform()`.

### Raylib submodule is a fork

`third_party/raylib` tracks **`tslmy/raylib` branch `mmf`** (not upstream). The fork patches `src/platforms/rcore_memory.c` to:
- Open `/dev/fb0` and blit the software framebuffer using `FBIOPAN_DISPLAY` for double-buffering.
- Initialise TinyGL ZBuffer and pass pixel buffer for zero-copy rendering (gated on `RAYLIB_USE_TINYGL`).
- Hardware-accelerate blit to display via MI GFX 2D engine (gated on `RAYLIB_MMF_FB` + env var).
- Poll `/dev/input/event*` (evdev) for quit keys: `MENU`, `HOME`, `POWER`, `ESC`, `Q`, `ENTER`, `BACKSPACE`.

Never replace the submodule with upstream raylib — the MMF-specific patches are essential.

### Sysroot

`third_party/mmf-sysroot/` is fetched from the device (not committed). It provides the device's glibc so the binary links against the correct version. `docker/build.sh` symlinks missing `.so` stubs (e.g. `libc.so → libc.so.6`) and passes `--sysroot` + `-rpath-link` to the linker.

### `stat_shim.c`

The MMF sysroot lacks `libc_nonshared.a`, so `stat()` is shimmed by forwarding to `__xstat()`. This file must always be compiled and linked alongside `main.c`.

### `tinygl_stubs.c`

Provides GL functions missing from TinyGL that raylib references:
- **Real implementations**: `glOrtho` (orthographic projection matrix), `glColor4ub` (wraps `glColor4f`), `glVertex2i` (wraps `glVertex3f`)
- **No-op stubs**: `glDepthFunc`, `glLineWidth`, `glColorMask`, `glScissor`, `glPixelStorei`, `glTexSubImage2D`, `glGetTexImage`, `glDrawElements`

### OnionOS app bundle (`dist/`)

`deploy` rsync's `dist/` to `/mnt/SDCARD/App/raylib-cube/` on the device. The bundle requires:
- `raylib-cube` — the ARM binary
- `launch.sh` — sets `LD_LIBRARY_PATH`, CPU governor, and env vars, then runs the binary
- `config.json` — OnionOS metadata (`label`, `icon`, `launch`)
- `icon.png` (optional)

## Key Conventions

- **Target**: `armv7-unknown-linux-gnueabihf` (armhf), MMF native resolution **750×560**.
- **Static linking**: raylib and TinyGL are always linked statically. Dynamic libs are only used for system libc/libm/libpthread/libdl.
- **Link order**: `-Wl,--start-group -lraylib -lTinyGL tinygl_stubs.o -Wl,--end-group` (circular deps between raylib→TinyGL→stubs→TinyGL).
- **Raylib build defines**: `RAYLIB_MMF_FB` (fb0/MI GFX support), `RAYLIB_USE_TINYGL` (TinyGL ZBuffer integration). Both passed via `CUSTOM_CFLAGS`.
- **Runtime env vars** (set in `assets/launch.sh`):
  - `RAYLIB_MMF_SHOWFPS=1` — overlay FPS counter (also toggled in `main.c` via `getenv`)
  - `RAYLIB_MMF_MIGFX=1` — enable MI GFX hardware blit if libs are present in `dist/libs/`
  - `RAYLIB_MMF_SCALE=2` — framebuffer scale factor
- **Compiler flags**: `-fno-stack-protector` (required for MMF ABI), `-march=armv7-a -mfpu=neon-vfpv4` (Cortex-A7 NEON), `-ffunction-sections -fdata-sections -Wl,--gc-sections` (size reduction).
- **Shell**: `justfile` uses `zsh` (`set shell := ["zsh", "-c"]`).

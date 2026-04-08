# Copilot Instructions

## Project Overview

Cross-compiles a [Raylib](https://www.raylib.com/) 3D demo (spinning cube) for the **Miyoo Mini Flip (MMF)** handheld running OnionOS. Uses `PLATFORM_MEMORY` + `GRAPHICS_API_OPENGL_SOFTWARE` and blits directly to `/dev/fb0` — no EGL, no SDL, no DRM (none of those work on MMF).

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
RAYLIB_PLATFORM=PLATFORM_MEMORY RAYLIB_GRAPHICS=GRAPHICS_API_OPENGL_SOFTWARE just build
```

There is no test suite.

## Architecture

### Cross-compile pipeline

```
just build
  → docker build (ubuntu:22.04 + arm-linux-gnueabihf toolchain)
  → docker run → docker/build.sh
      → make -C third_party/raylib/src  (static libraylib.a, PLATFORM_MEMORY + software renderer)
      → arm-linux-gnueabihf-gcc src/main.c src/stat_shim.c → dist/raylib-cube
      → cp assets/{launch.sh,config.json,icon.png} → dist/
```

### Raylib submodule is a fork

`third_party/raylib` tracks **`tslmy/raylib` branch `mmf`** (not upstream). The fork patches `src/platforms/rcore_memory.c` to:
- Open `/dev/fb0` and blit the software framebuffer using `FBIOPAN_DISPLAY` for double-buffering.
- Poll `/dev/input/event*` (evdev) for quit keys: `MENU`, `HOME`, `POWER`, `ESC`, `Q`, `ENTER`, `BACKSPACE`.

Never replace the submodule with upstream raylib — the MMF-specific patches are essential.

### Sysroot

`third_party/mmf-sysroot/` is fetched from the device (not committed). It provides the device's glibc so the binary links against the correct version. `docker/build.sh` symlinks missing `.so` stubs (e.g. `libc.so → libc.so.6`) and passes `--sysroot` + `-rpath-link` to the linker.

### `stat_shim.c`

The MMF sysroot lacks `libc_nonshared.a`, so `stat()` is shimmed by forwarding to `__xstat()`. This file must always be compiled and linked alongside `main.c`.

### OnionOS app bundle (`dist/`)

`deploy` rsync's `dist/` to `/mnt/SDCARD/App/raylib-cube/` on the device. The bundle requires:
- `raylib-cube` — the ARM binary
- `launch.sh` — sets `LD_LIBRARY_PATH`, CPU governor, and env vars, then runs the binary
- `config.json` — OnionOS metadata (`label`, `icon`, `launch`)
- `icon.png` (optional)

## Key Conventions

- **Target**: `armv7-unknown-linux-gnueabihf` (armhf), MMF native resolution **750×560**.
- **Static linking**: raylib is always linked statically (`RAYLIB_LIBTYPE=STATIC`). Dynamic libs are only used for system libc/libm/libpthread/libdl.
- **Raylib build define**: `RAYLIB_MMF_FB` is passed via `CUSTOM_CFLAGS` when building raylib. Code in `rcore_memory.c` gates MMF-specific framebuffer logic on this define.
- **Runtime env vars** (set in `assets/launch.sh`):
  - `RAYLIB_MMF_SHOWFPS=1` — overlay FPS counter (also toggled in `main.c` via `getenv`)
  - `RAYLIB_MMF_MIGFX=1` — enable MI GFX hardware blit if libs are present in `dist/libs/`
  - `RAYLIB_MMF_SCALE=2` — framebuffer scale factor
- **Compiler flags**: `-fno-stack-protector` (required for MMF ABI), `-ffunction-sections -fdata-sections -Wl,--gc-sections` (size reduction).
- **Shell**: `justfile` uses `zsh` (`set shell := ["zsh", "-c"]`).

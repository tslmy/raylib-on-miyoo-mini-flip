# Porting Raylib to Miyoo Mini Flip

A tiny [Raylib][rl] 3D demo (spinning cube) for [Miyoo Mini Flip (MMF)][mm] running OnionOS.

<img width="614" height="453" alt="image" src="https://github.com/user-attachments/assets/5052dc45-fb43-47a0-b084-1252678eeae3" />

This repo is designed to reuse the same cross-compile approach I used in [`hmbrg`](https://github.com/tslmy/hmbrg):

- Docker-based armv7 toolchain
- Sysroot copied from the device to avoid glibc mismatches

[rl]: https://www.raylib.com/
[mm]: https://lomiyoo.com/products/miyoo-mini-flip

## Quick start

Clone this repo recursively, as this repo uses submodules for raylib and TinyGL (tracking forks under `tslmy/` on branch `mmf`). You'll need the command runner program [`just`](https://just.systems/).

1) Fetch the sysroot from the device (via SSH):

```bash
just sysroot-fetch <mmf-host>
```

2) Build for MMF:

```bash
just build
```

3) Deploy to device:

```bash
just deploy <mmf-host>
```

The app will install to `/mnt/SDCARD/App/raylib-cube` and show up in the OnionOS Apps list.

## How It Works

The MMF's SSD202D SoC (dual-core ARM Cortex-A7) has **no 3D GPU**, so we need a software renderer.

### Renderer: TinyGL

We use [TinyGL][tgl] (C-Chads fork), a fast integer/fixed-point **OpenGL 1.1** software renderer. Raylib is built with `GRAPHICS_API_OPENGL_11` so its `rlgl.h` calls map directly to TinyGL's GL functions.

TinyGL renders into a framebuffer via its `ZBuffer`, which we point at MMA-allocated memory for zero-copy display. Our [fork][tglf] adds:

- **Extended texture formats**: `GL_RGBA`, `GL_LUMINANCE`, `GL_LUMINANCE_ALPHA` (the original only supported `GL_RGB`)
- **Alpha-aware blending**: `GL_SRC_ALPHA` / `GL_ONE_MINUS_SRC_ALPHA` blend factors (needed for text rendering)
- **Per-pixel alpha preservation** through the texture resize pipeline

### Platform: PLATFORM_MEMORY

Raylib's [`PLATFORM_MEMORY`][pm] provides a minimal platform layer. Our [Raylib fork][rlf] extends it with:

- **MI GFX hardware blit** (primary path) — uses the SSD202D's 2D blitter for format conversion and display
- **CPU blit fallback** — manual ARGB→framebuffer conversion when MI GFX is unavailable
- **TinyGL ZBuffer integration** — direct rendering into MMA memory
- **Input polling** via `/dev/input/event*` for quit keys

### Display pipeline

```
TinyGL renders (ARGB) → MMA buffer → MI GFX blit → /dev/fb0
                                    ↘ CPU blit (fallback)
```

[tgl]: https://github.com/C-Chads/tinygl
[tglf]: https://github.com/tslmy/tinygl/tree/mmf
[rlf]: https://github.com/tslmy/raylib/tree/mmf
[pm]: https://github.com/raysan5/raylib/blob/c5fc7716229cef1727e7baf325a695a0ac00cf27/src/Makefile#L37-L38

> [!NOTE]
> I tried EGL/GLES. I failed. See `docs/raylib-port-notes.md` for what I tried & learned.

## Troubleshooting

**Framebuffer access**. This build blits to `/dev/fb0` via MI GFX (or CPU fallback). If the app exits immediately, check OnionOS permissions and confirm the device is not running a conflicting app.

## Repo layout

- `src/main.c`: Demo program (spinning cube with FPS counter)
- `src/tinygl_stubs.c`: GL functions missing from TinyGL that Raylib requires
- `docker/`: Cross-compile toolchain
- `third_party/raylib`: Raylib fork (`tslmy/raylib`, branch `mmf`)
- `third_party/tinygl`: TinyGL fork (`tslmy/tinygl`, branch `mmf`)
- `assets/launch.sh`: Device launcher script
- `assets/config.json`: OnionOS App metadata
- `docs/raylib-port-notes.md`: Porting research notes

## Notes

- This project assumes `armv7-unknown-linux-gnueabihf` (armhf) and MMF's stock libc.
- Raylib is linked **statically** by default to reduce runtime dependencies on MMF.
- Quit: press `MENU`, `HOME`, `POWER`, `ESC`, `Q`, `ENTER`, or `BACKSPACE`.

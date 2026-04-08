# Porting Raylib to Miyoo Mini Flip

A tiny [Raylib][rl] 3D demo (spinning cube) for [Miyoo Mini Flip (MMF)][mm] running OnionOS.

<img width="614" height="453" alt="image" src="https://github.com/user-attachments/assets/5052dc45-fb43-47a0-b084-1252678eeae3" />

This repo is designed to reuse the same cross-compile approach I used in [`hmbrg`](https://github.com/tslmy/hmbrg):

- Docker-based armv7 toolchain
- Sysroot copied from the device to avoid glibc mismatches

[rl]: https://www.raylib.com/
[mm]: https://lomiyoo.com/products/miyoo-mini-flip

## Quick start

Clone this repo recursively, as this repo uses a submodule for raylib (tracking my fork `tslmy/raylib` on branch `mmf`). You'll need the command runner program [`just`](https://just.systems/).

1) Fetch the sysroot from the device (via SSH):

```bash
just sysroot-fetch <mmf-host>
```

1) Build for MMF:

```bash
just build
```

1) Deploy to device:

```bash
just deploy <mmf-host>
```

The app will install to `/mnt/SDCARD/App/raylib-cube` and show up in the OnionOS Apps list.

## How It Works

This project compiles Raylib for the "memory platform", or `PLATFORM_MEMORY`. Its makefile [describes][pm] it as "Memory [framebuffer][fb] output, using software renderer, no OS required". In its filesystem, Linux exposes this framebuffer at the path `/dev/fb0`, so we are targeting at [exactly that][tf].

This choice [requires][rq] us to use the "software renderer" (`GRAPHICS_API_OPENGL_SOFTWARE`). If I'm not mistaken, this refers to "[rlsw v1.5][rs] - An OpenGL 1.1-style software renderer implementation" in Raylib.

In my patch to Raylib, [a small input poller][ip] reads `/dev/input/event*` to support quitting. That's my stubby implementation for [@raysan5's to-do][td] introduced in [47a8b55][47].

[rs]: https://github.com/raysan5/raylib/blob/c5fc7716229cef1727e7baf325a695a0ac00cf27/src/external/rlsw.h#L3
[rq]: https://github.com/raysan5/raylib/blob/c5fc7716229cef1727e7baf325a695a0ac00cf27/src/platforms/rcore_memory.c#L491
[fb]: https://en.wikipedia.org/wiki/Framebuffer
[tf]: https://github.com/tslmy/raylib/commit/443acc300f948c6f99daf8f4deaed005b5730fdb#diff-347102e6d31df9888d75e091e8719bae10a768ef3201fc8499c8160a9f59cc6dR595-R624
[pm]: https://github.com/raysan5/raylib/blob/c5fc7716229cef1727e7baf325a695a0ac00cf27/src/Makefile#L37-L38
[ip]: https://github.com/tslmy/raylib/commit/443acc300f948c6f99daf8f4deaed005b5730fdb#diff-347102e6d31df9888d75e091e8719bae10a768ef3201fc8499c8160a9f59cc6dR540-R563
[td]: https://github.com/raysan5/raylib/blob/443acc300f948c6f99daf8f4deaed005b5730fdb/src/platforms/rcore_memory.c#L537
[47]: https://github.com/raysan5/raylib/commit/47a8b554bce936596e910aaeecf3d524c83ba97c

> [!NOTE]
> I tried EGL/GLES. I failed. See `docs/raylib-port-notes.md` for what I tried & learned.

## Troubleshooting

**Framebuffer access**. This build uses raylib's software renderer and blits directly to `/dev/fb0`. If the app exits immediately, check OnionOS permissions and confirm the device is not running a conflicting app.

## Repo layout

- `src/main.c`: Demo program (spinning cube)
- `docker/`: Cross-compile toolchain
- `assets/launch.sh`: Device launcher script template
- `assets/config.json`: OnionOS App metadata
- `docs/raylib-port-notes.md`: Porting research notes and console logs

## Notes

- This project assumes `armv7-unknown-linux-gnueabihf` (armhf) and MMF's stock libc.
- raylib is linked **statically** by default to reduce runtime dependencies on MMF.
- Quit: press `MENU`, `HOME`, `POWER`, `ESC`, `Q`, `ENTER`, or `BACKSPACE`.

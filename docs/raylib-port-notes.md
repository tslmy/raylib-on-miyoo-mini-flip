# Raylib Port Research Notes (MMF / OnionOS)

These notes capture what we tried and learned while porting raylib to Miyoo Mini Flip (MMF), focusing on the raylib backend choice and graphics initialization.

## TL;DR

- **EGL/GLES path failed** on MMF (EGL init and GL context creation failed).
- **SDL2 path failed** (no working GL context with the MMF SDL2 driver).
- **Framebuffer + TinyGL works**: raylib `PLATFORM_MEMORY` + `GRAPHICS_API_OPENGL_11` with [TinyGL](https://github.com/jserv/tinygl) (software OpenGL 1.1 rasterizer), MI GFX hardware blit to `/dev/fb0`.

## Platform Facts Observed

- `/dev/dri` does not exist on the device (no KMS/DRM).
- EGL/GLES libraries can be present, but `eglInitialize` fails.
- SDL2 video driver exposes `Mini` but cannot create GL context.

## Evidence (console snippets)

Below are the key logs plus the **probe code fragments** that produced them. The probes were temporary and later removed to keep the repo lean, but these snippets are representative of what we ran.

### SDL + EGL path failing

```
INFO: Initializing raylib 6.0
INFO: Platform backend: DESKTOP (SDL)
[SDL] MMIYOO_VideoInit
That operation is not supported
[SDL] EGL Display 0x5f77d0
[SDL] Failed to get EGL config
FATAL: PLATFORM: Failed to initialize graphics device
```

### SDL GL context creation fails directly

```
[sdl-probe] video drivers (1):
  - Mini
[sdl-probe] current video driver: Mini
That operation is not supported
[sdl-probe] SDL_GL_CreateContext failed: That operation is not supported
```

Probe fragment:

```c
printf("[sdl-probe] video drivers (%d):\n", SDL_GetNumVideoDrivers());
printf("[sdl-probe] current video driver: %s\n", SDL_GetCurrentVideoDriver());
SDL_Window *w = SDL_CreateWindow("probe", 0, 0, 640, 480, SDL_WINDOW_OPENGL);
SDL_GLContext ctx = SDL_GL_CreateContext(w);
if (!ctx) printf("[sdl-probe] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
```

### EGL probe

```
[egl-probe] dlopen libEGL.so: ok
[egl-probe] dlopen libGLESv2.so: ok
[egl-probe] eglInitialize failed: 0x3001
[egl-probe] result: RGB565=fail RGBA8888=fail
```

Probe fragment:

```c
void *egl = dlopen("libEGL.so", RTLD_NOW);
void *gles = dlopen("libGLESv2.so", RTLD_NOW);
printf("[egl-probe] dlopen libEGL.so: %s\n", egl ? "ok" : dlerror());
printf("[egl-probe] dlopen libGLESv2.so: %s\n", gles ? "ok" : dlerror());

EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
if (!eglInitialize(dpy, &major, &minor)) {
  printf("[egl-probe] eglInitialize failed: 0x%04x\n", eglGetError());
}
```

### /dev/dri missing

```
ls: /dev/dri: No such file or directory
```

Probe fragment:

```sh
ls /dev/dri
```

### GLIBC mismatch (when not linking with MMF sysroot)

```
./raylib-cube: /lib/libm.so.6: version `GLIBC_2.29' not found
./raylib-cube: /lib/libc.so.6: version `GLIBC_2.33' not found
```

This was triggered by running the binary without the MMF sysroot in the link path.

## Working Path (Final)

We use raylib’s **memory platform** with **TinyGL** as the OpenGL 1.1 software rasterizer:

- `PLATFORM_MEMORY`
- `GRAPHICS_API_OPENGL_11`
- [TinyGL](https://github.com/jserv/tinygl) (jserv fork) — fast integer/fixed-point OpenGL 1.1 implementation

We initially tried raylib’s built-in `rlsw` software renderer (`GRAPHICS_API_OPENGL_SOFTWARE`), but it only achieved ~5–8 FPS. TinyGL was dramatically faster.

Implementation details:

- `third_party/raylib/src/platforms/rcore_memory.c` manages the display pipeline.
- TinyGL renders into MMA-allocated memory via its `ZBuffer` (zero-copy).
- **MI GFX hardware blit** (primary path) transfers the ARGB framebuffer to `/dev/fb0`.
- **CPU blit** (fallback) does manual ARGB→framebuffer conversion when MI GFX is unavailable.
- Our TinyGL fork (`tslmy/tinygl`, branch `mmf`) adds `GL_RGBA`/`GL_LUMINANCE`/`GL_LUMINANCE_ALPHA` texture support and `GL_SRC_ALPHA` blending for text rendering.

This reliably renders the spinning cube with text overlay on MMF.

## Input / Quit

Because SDL isn’t used, we poll evdev input directly (`/dev/input/event*`).
Quit on key codes:

`MENU`, `HOME`, `POWER`, `ESC`, `Q`, `ENTER`, `BACKSPACE`.

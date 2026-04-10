#!/bin/sh
set -eu

RAYLIB_DIR=/build/raylib
SRC_DIR=/build/src
SYSROOT="${MMF_SYSROOT:-/build/sysroot}"
OUT_DIR="$SRC_DIR/dist"
ASSETS_DIR="$SRC_DIR/assets"

echo "============== Entered build script =============="

# ────────── Pre-bake assets (runs on host x86) ──────────
# Computes SH coefficients and bakes the lit floor texture at build time
# so the ARM device doesn't have to spend seconds doing it at boot.
PREBAKE_TOOL="$SRC_DIR/tools/prebake"
STB_DIR="$RAYLIB_DIR/src/external"
echo "------ Pre-baking assets ------"
gcc -O2 -I"$STB_DIR" -o "$PREBAKE_TOOL" "$SRC_DIR/tools/prebake.c" -lm
"$PREBAKE_TOOL" "$ASSETS_DIR" "$SRC_DIR/src"
# prebake outputs: src/prebaked_sh.inc, src/baked_floor.png
# Move baked floor to assets so it gets bundled with the binary
mv -f "$SRC_DIR/src/baked_floor.png" "$ASSETS_DIR/baked_floor.png"

# Raylib sources (submodule) must be present.
test -d "$RAYLIB_DIR/src" || { echo "Raylib source not found at $RAYLIB_DIR/src" >&2; exit 1; }
# MMF sysroot provides device glibc to avoid version mismatches.
test -d "$SYSROOT" || { echo "MMF sysroot not found at $SYSROOT" >&2; exit 1; }

PLATFORM="${RAYLIB_PLATFORM:-PLATFORM_MEMORY}"
GRAPHICS="${RAYLIB_GRAPHICS:-GRAPHICS_API_OPENGL_11}"

TINYGL_DIR=/build/src/third_party/tinygl
BULLET_DIR=/build/src/third_party/bullet3
MMF_ARCH_FLAGS="-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard"
# -ffast-math: allow aggressive FP optimizations (NEON fused multiply-add,
# reciprocal sqrt estimates, etc.) — safe for a dice game, big speedup.
MMF_FAST_MATH="-ffast-math -ftree-vectorize"

# ---------- Build TinyGL ----------
echo "------ Building TinyGL ------"
rm -f "$TINYGL_DIR"/src/*.o "$TINYGL_DIR"/src/libTinyGL.a
# Compile all TinyGL .o files (skip the archive step — its Makefile hardcodes `ar`).
TINYGL_OBJS="api.o list.o vertex.o init.o matrix.o texture.o misc.o clear.o light.o clip.o select.o get.o zbuffer.o zline.o ztriangle.o zmath.o image_util.o msghandling.o arrays.o specbuf.o memory.o ztext.o zraster.o zpostprocess.o"
for obj in $TINYGL_OBJS; do
  src="$TINYGL_DIR/src/$(echo "$obj" | sed 's/\.o$/.c/')"
  $CC -Wall -O3 -std=c99 -pedantic -DNDEBUG -fno-stack-protector $MMF_ARCH_FLAGS $MMF_FAST_MATH -Wno-unused-function -I"$TINYGL_DIR/include" -c "$src" -o "$TINYGL_DIR/src/$obj"
done
(cd "$TINYGL_DIR/src" && rm -f libTinyGL.a && $AR rcs libTinyGL.a $TINYGL_OBJS)

# ---------- Build Bullet3 (unity build) ----------
echo "------ Building Bullet3 ------"
BULLET_BUILD_DIR="$BULLET_DIR/build_mmf"
mkdir -p "$BULLET_BUILD_DIR"
BULLET_CXXFLAGS="-O3 -fno-stack-protector -DNDEBUG -DBT_NO_PROFILE $MMF_ARCH_FLAGS $MMF_FAST_MATH -I$BULLET_DIR/src"
$CXX $BULLET_CXXFLAGS -c "$BULLET_DIR/src/btLinearMathAll.cpp"      -o "$BULLET_BUILD_DIR/btLinearMathAll.o"
$CXX $BULLET_CXXFLAGS -c "$BULLET_DIR/src/btBulletCollisionAll.cpp" -o "$BULLET_BUILD_DIR/btBulletCollisionAll.o"
$CXX $BULLET_CXXFLAGS -c "$BULLET_DIR/src/btBulletDynamicsAll.cpp"  -o "$BULLET_BUILD_DIR/btBulletDynamicsAll.o"
(cd "$BULLET_BUILD_DIR" && rm -f libBullet.a && $AR rcs libBullet.a btLinearMathAll.o btBulletCollisionAll.o btBulletDynamicsAll.o)

# ---------- Build raylib ----------
cd "$RAYLIB_DIR/src"
make clean || true
# TinyGL provides <GL/gl.h>; put its include path first so raylib picks it up.
RAYLIB_CFLAGS="-fno-stack-protector -DRAYLIB_MMF_FB -DRAYLIB_USE_TINYGL $MMF_ARCH_FLAGS $MMF_FAST_MATH -I$TINYGL_DIR/include"
echo "------ Building raylib ($PLATFORM, $GRAPHICS) ------"
make \
  PLATFORM="$PLATFORM" \
  GRAPHICS="$GRAPHICS" \
  RAYLIB_LIBTYPE=STATIC \
  CUSTOM_CFLAGS="$RAYLIB_CFLAGS" \
  CC="$CC" \
  AR="$AR" \
  RANLIB="$RANLIB"

# Sysroot lacks some linker-expected sonames; add them so ld can resolve.
ensure_linker_so() { [ -e "$SYSROOT/lib/$1" ] || [ ! -e "$SYSROOT/lib/$2" ] || ln -s "$2" "$SYSROOT/lib/$1"; }

ensure_linker_so libc.so libc.so.6
ensure_linker_so libpthread.so libpthread.so.0
ensure_linker_so libdl.so libdl.so.2
ensure_linker_so libm.so libm.so.6
ensure_linker_so libgcc_s.so libgcc_s.so.1

# Collect sysroot search paths for link + rpath-link resolution.
SYSROOT_DIRS="$SYSROOT/lib $SYSROOT/usr/lib $SYSROOT/usr/lib32 $SYSROOT/customer/lib"
SYSROOT_LDFLAGS=""
RPATH_LINKS=""
for dir in $SYSROOT_DIRS; do
  [ -d "$dir" ] || continue
  SYSROOT_LDFLAGS="$SYSROOT_LDFLAGS -L$dir"
  RPATH_LINKS="$RPATH_LINKS -Wl,-rpath-link,$dir"
done

# Compile with MMF headers; keep code size down via section garbage collection.
# Pass the same arch flags as the raylib build so the ABI matches.
CFLAGS="-DGRAPHICS_API_OPENGL_11 -I$RAYLIB_DIR/src -I$TINYGL_DIR/include -I$BULLET_DIR/src -I$SRC_DIR/src -I/usr/include -I/usr/include/arm-linux-gnueabihf -O3 -ffunction-sections -fdata-sections -fno-stack-protector $MMF_ARCH_FLAGS $MMF_FAST_MATH"
# Link against the MMF sysroot to keep glibc compatibility.
LDFLAGS="--sysroot=$SYSROOT $SYSROOT_LDFLAGS -Wl,--gc-sections $RPATH_LINKS"
LDFLAGS="$LDFLAGS -L$RAYLIB_DIR/src -L$TINYGL_DIR/src -L$BULLET_BUILD_DIR"

mkdir -p "$OUT_DIR"

# Build app + stat shim + TinyGL stubs (sysroot lacks libc_nonshared.a).
$CXX $CFLAGS -c -o "$OUT_DIR/main.o" "$SRC_DIR/src/main.cpp"
$CXX $CFLAGS -c -o "$OUT_DIR/physics.o" "$SRC_DIR/src/physics.cpp"
$CXX $CFLAGS -c -o "$OUT_DIR/rendering.o" "$SRC_DIR/src/rendering.cpp"
$CC $CFLAGS -c -o "$OUT_DIR/stat_shim.o" "$SRC_DIR/src/stat_shim.c"
$CC $CFLAGS -c -o "$OUT_DIR/tinygl_stubs.o" "$SRC_DIR/src/tinygl_stubs.c"

$CXX -o "$OUT_DIR/raylib-cube" \
  "$OUT_DIR/main.o" \
  "$OUT_DIR/physics.o" \
  "$OUT_DIR/rendering.o" \
  "$OUT_DIR/stat_shim.o" \
  $LDFLAGS \
  -Wl,--start-group -lraylib -lTinyGL -lBullet "$OUT_DIR/tinygl_stubs.o" -Wl,--end-group \
  -lm -lpthread -ldl -lc -lgcc_s -lgcc -lstdc++

# Build evdev probe tool (standalone, no raylib dependency).
$CC -O2 -fno-stack-protector $MMF_ARCH_FLAGS --sysroot=$SYSROOT $SYSROOT_LDFLAGS $RPATH_LINKS \
  -o "$OUT_DIR/evdev-probe" "$SRC_DIR/src/evdev_probe.c" -lc

# Strip reduces binary size (optional).
if [ -n "${STRIP:-}" ]; then
  $STRIP "$OUT_DIR/raylib-cube" || true
  $STRIP "$OUT_DIR/evdev-probe" || true
fi

# Bundle OnionOS launcher metadata.
cp -f "$ASSETS_DIR/launch.sh" "$OUT_DIR/launch.sh"
cp -f "$ASSETS_DIR/config.json" "$OUT_DIR/config.json"

test -f "$ASSETS_DIR/icon.png" && cp -f "$ASSETS_DIR/icon.png" "$OUT_DIR/icon.png"

# Bundle skybox image for 3D environment.
test -f "$ASSETS_DIR/skybox.png" && cp -f "$ASSETS_DIR/skybox.png" "$OUT_DIR/skybox.png"

# Bundle baked floor texture (pre-lit at build time by prebake tool).
test -f "$ASSETS_DIR/baked_floor.png" && cp -f "$ASSETS_DIR/baked_floor.png" "$OUT_DIR/baked_floor.png"

# Bundle raw hardwood floor textures (fallback if baked floor is missing).
test -f "$ASSETS_DIR/hardwood2_diffuse.png" && cp -f "$ASSETS_DIR/hardwood2_diffuse.png" "$OUT_DIR/hardwood2_diffuse.png"
test -f "$ASSETS_DIR/hardwood2_bump.png" && cp -f "$ASSETS_DIR/hardwood2_bump.png" "$OUT_DIR/hardwood2_bump.png"
test -f "$ASSETS_DIR/hardwood2_roughness.png" && cp -f "$ASSETS_DIR/hardwood2_roughness.png" "$OUT_DIR/hardwood2_roughness.png"

# Bundle MatCap texture for dice environment reflections.
test -f "$ASSETS_DIR/matcap.png" && cp -f "$ASSETS_DIR/matcap.png" "$OUT_DIR/matcap.png"

# Bundle scratch normal map for dice surface detail overlay.
test -f "$ASSETS_DIR/scratches.png" && cp -f "$ASSETS_DIR/scratches.png" "$OUT_DIR/scratches.png"

# Bundle dirt bump normal map for dice surface roughness overlay.
test -f "$ASSETS_DIR/dirt_bump.png" && cp -f "$ASSETS_DIR/dirt_bump.png" "$OUT_DIR/dirt_bump.png"

# Bundle MI GFX libs when available (for hardware blit on MMF).
if [ -d "$SRC_DIR/third_party/mi/lib" ]; then
  mkdir -p "$OUT_DIR/libs"
  cp -f "$SRC_DIR/third_party/mi/lib/"*.so "$OUT_DIR/libs/" 2>/dev/null || true
fi

echo "Build complete: $OUT_DIR/raylib-cube"

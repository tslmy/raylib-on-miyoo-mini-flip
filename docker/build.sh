#!/bin/sh
set -eu

RAYLIB_DIR=/build/raylib
SRC_DIR=/build/src
SYSROOT="${MMF_SYSROOT:-/build/sysroot}"
OUT_DIR="$SRC_DIR/dist"
ASSETS_DIR="$SRC_DIR/assets"

echo "============== Entered build script =============="

# Raylib sources (submodule) must be present.
test -d "$RAYLIB_DIR/src" || { echo "Raylib source not found at $RAYLIB_DIR/src" >&2; exit 1; }
# MMF sysroot provides device glibc to avoid version mismatches.
test -d "$SYSROOT" || { echo "MMF sysroot not found at $SYSROOT" >&2; exit 1; }

PLATFORM="${RAYLIB_PLATFORM:-PLATFORM_MEMORY}"
GRAPHICS="${RAYLIB_GRAPHICS:-GRAPHICS_API_OPENGL_11}"

TINYGL_DIR=/build/src/third_party/tinygl
BULLET_DIR=/build/src/third_party/bullet3
MMF_ARCH_FLAGS="-march=armv7-a -mfpu=neon-vfpv4"

# ---------- Build TinyGL ----------
echo "------ Building TinyGL ------"
rm -f "$TINYGL_DIR"/src/*.o "$TINYGL_DIR"/src/libTinyGL.a
# Compile all TinyGL .o files (skip the archive step — its Makefile hardcodes `ar`).
TINYGL_OBJS="api.o list.o vertex.o init.o matrix.o texture.o misc.o clear.o light.o clip.o select.o get.o zbuffer.o zline.o ztriangle.o zmath.o image_util.o msghandling.o arrays.o specbuf.o memory.o ztext.o zraster.o zpostprocess.o"
for obj in $TINYGL_OBJS; do
  src="$TINYGL_DIR/src/$(echo "$obj" | sed 's/\.o$/.c/')"
  $CC -Wall -O3 -std=c99 -pedantic -DNDEBUG -fno-stack-protector $MMF_ARCH_FLAGS -Wno-unused-function -I"$TINYGL_DIR/include" -c "$src" -o "$TINYGL_DIR/src/$obj"
done
(cd "$TINYGL_DIR/src" && rm -f libTinyGL.a && $AR rcs libTinyGL.a $TINYGL_OBJS)

# ---------- Build Bullet3 (unity build) ----------
echo "------ Building Bullet3 ------"
BULLET_BUILD_DIR="$BULLET_DIR/build_mmf"
mkdir -p "$BULLET_BUILD_DIR"
BULLET_CXXFLAGS="-O2 -fno-stack-protector -DNDEBUG -DBT_NO_PROFILE $MMF_ARCH_FLAGS -I$BULLET_DIR/src"
$CXX $BULLET_CXXFLAGS -c "$BULLET_DIR/src/btLinearMathAll.cpp"      -o "$BULLET_BUILD_DIR/btLinearMathAll.o"
$CXX $BULLET_CXXFLAGS -c "$BULLET_DIR/src/btBulletCollisionAll.cpp" -o "$BULLET_BUILD_DIR/btBulletCollisionAll.o"
$CXX $BULLET_CXXFLAGS -c "$BULLET_DIR/src/btBulletDynamicsAll.cpp"  -o "$BULLET_BUILD_DIR/btBulletDynamicsAll.o"
(cd "$BULLET_BUILD_DIR" && rm -f libBullet.a && $AR rcs libBullet.a btLinearMathAll.o btBulletCollisionAll.o btBulletDynamicsAll.o)

# ---------- Build raylib ----------
cd "$RAYLIB_DIR/src"
make clean || true
# TinyGL provides <GL/gl.h>; put its include path first so raylib picks it up.
RAYLIB_CFLAGS="-fno-stack-protector -DRAYLIB_MMF_FB -DRAYLIB_USE_TINYGL $MMF_ARCH_FLAGS -I$TINYGL_DIR/include"
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
CFLAGS="-I$RAYLIB_DIR/src -I$TINYGL_DIR/include -I$BULLET_DIR/src -I/usr/include -I/usr/include/arm-linux-gnueabihf -O2 -ffunction-sections -fdata-sections -fno-stack-protector $MMF_ARCH_FLAGS"
# Link against the MMF sysroot to keep glibc compatibility.
LDFLAGS="--sysroot=$SYSROOT $SYSROOT_LDFLAGS -Wl,--gc-sections $RPATH_LINKS"
LDFLAGS="$LDFLAGS -L$RAYLIB_DIR/src -L$TINYGL_DIR/src -L$BULLET_BUILD_DIR"

mkdir -p "$OUT_DIR"

# Build app + stat shim + TinyGL stubs (sysroot lacks libc_nonshared.a).
$CXX $CFLAGS -c -o "$OUT_DIR/main.o" "$SRC_DIR/src/main.cpp"
$CC $CFLAGS -c -o "$OUT_DIR/stat_shim.o" "$SRC_DIR/src/stat_shim.c"
$CC $CFLAGS -c -o "$OUT_DIR/tinygl_stubs.o" "$SRC_DIR/src/tinygl_stubs.c"

$CXX -o "$OUT_DIR/raylib-cube" \
  "$OUT_DIR/main.o" \
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

# Bundle MI GFX libs when available (for hardware blit on MMF).
if [ -d "$SRC_DIR/third_party/mi/lib" ]; then
  mkdir -p "$OUT_DIR/libs"
  cp -f "$SRC_DIR/third_party/mi/lib/"*.so "$OUT_DIR/libs/" 2>/dev/null || true
fi

echo "Build complete: $OUT_DIR/raylib-cube"

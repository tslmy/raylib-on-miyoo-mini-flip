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
GRAPHICS="${RAYLIB_GRAPHICS:-GRAPHICS_API_OPENGL_SOFTWARE}"

cd "$RAYLIB_DIR/src"
# Rebuild raylib with the software renderer + MMF framebuffer hooks.
make clean || true
RAYLIB_CFLAGS="-fno-stack-protector -DRAYLIB_MMF_FB"
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
CFLAGS="-I$RAYLIB_DIR/src -I/usr/include -I/usr/include/arm-linux-gnueabihf -O2 -ffunction-sections -fdata-sections -fno-stack-protector"
# Link against the MMF sysroot to keep glibc compatibility.
LDFLAGS="--sysroot=$SYSROOT $SYSROOT_LDFLAGS -Wl,--gc-sections $RPATH_LINKS"
LDFLAGS="$LDFLAGS -L$RAYLIB_DIR/src"
# Static raylib + standard libs.
LIBS="-lraylib -lm -lpthread -ldl -lc -lgcc_s -lgcc"

mkdir -p "$OUT_DIR"

# Build app + stat shim (sysroot lacks libc_nonshared.a).
$CC $CFLAGS -c -o "$OUT_DIR/main.o" "$SRC_DIR/src/main.c"
$CC $CFLAGS -c -o "$OUT_DIR/stat_shim.o" "$SRC_DIR/src/stat_shim.c"

$CC -o "$OUT_DIR/raylib-cube" \
  "$OUT_DIR/main.o" \
  "$OUT_DIR/stat_shim.o" \
  $LDFLAGS $LIBS

# Strip reduces binary size (optional).
if [ -n "${STRIP:-}" ]; then
  $STRIP "$OUT_DIR/raylib-cube" || true
fi

# Bundle OnionOS launcher metadata.
cp -f "$ASSETS_DIR/launch.sh" "$OUT_DIR/launch.sh"
cp -f "$ASSETS_DIR/config.json" "$OUT_DIR/config.json"

test -f "$ASSETS_DIR/icon.png" && cp -f "$ASSETS_DIR/icon.png" "$OUT_DIR/icon.png"

echo "Build complete: $OUT_DIR/raylib-cube"

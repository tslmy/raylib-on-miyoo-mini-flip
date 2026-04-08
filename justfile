set shell := ["zsh", "-c"]

# Fetch MMF sysroot libraries (glibc and friends) from device for compatibility.
sysroot-fetch host:
  rm -rf {{justfile_directory()}}/third_party/mmf-sysroot
  mkdir -p {{justfile_directory()}}/third_party/mmf-sysroot
  rsync -avzu --no-perms --no-owner --no-group onion@{{host}}:/lib {{justfile_directory()}}/third_party/mmf-sysroot/
  rsync -avzu --no-perms --no-owner --no-group onion@{{host}}:/usr/lib {{justfile_directory()}}/third_party/mmf-sysroot/usr/ || true
  rsync -avzu --no-perms --no-owner --no-group onion@{{host}}:/usr/lib32 {{justfile_directory()}}/third_party/mmf-sysroot/usr/ || true
  rsync -avzu --no-perms --no-owner --no-group onion@{{host}}:/customer/lib {{justfile_directory()}}/third_party/mmf-sysroot/ || true

# Cross-build for Miyoo Mini Flip (MMF) via Docker. (tested on macOS)
build:
  docker build -t raylib-mmf -f docker/Dockerfile docker
  mkdir -p \
    {{justfile_directory()}}/.cache/ccache \
    {{justfile_directory()}}/dist
  docker run --rm \
    -v {{justfile_directory()}}:/build/src \
    -v {{justfile_directory()}}/third_party/raylib:/build/raylib \
    -v {{justfile_directory()}}/third_party/mmf-sysroot:/build/sysroot \
    -v {{justfile_directory()}}/.cache/ccache:/build/ccache \
    -e MMF_SYSROOT=/build/sysroot \
    -e RAYLIB_PLATFORM \
    -e RAYLIB_GRAPHICS \
    raylib-mmf
  chmod +x {{justfile_directory()}}/dist/launch.sh

# Deploy built files to Miyoo Mini Flip (MMF) device over SSH.
# Requires `build` to be run first.
deploy host:
  rsync -avzu --no-perms --no-owner --no-group {{justfile_directory()}}/dist/ onion@{{host}}:/mnt/SDCARD/App/raylib-cube

clean:
  rm -rf {{justfile_directory()}}/dist

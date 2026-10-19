#!/bin/sh
# build-dawn-hdr.sh [WORK]: build Dawn with patches/dawn/hdr.patch (HDR10
# swapchains on Vulkan) as a package laid out like encounter/dawn's
# dawn-linux-x86_64.tar.gz, with the options of its release CI
# (.github/workflows/dawn-ci.cmake). The prebuilt package has no HDR on Linux:
# Dawn picks no colour space for a Vulkan swapchain. Run inside the edbuild
# container; about 25 minutes on a Steam Deck. Then point build-linux at it:
#   cmake -B build-linux -DAURORA_DAWN_PACKAGE_URL=file://WORK/dawn-linux-x86_64-hdr.tar.gz
set -e
HERE=$(dirname "$(readlink -f "$0")")
REPO=$(cd "$HERE/../.." && pwd)
WORK=${1:-$HOME/src/dawn-hdr}
# The commit the prebuilt package was made from (AURORA_DAWN_REF).
REF=266c1cf8de969a364afa4fa49311631fc99a881e

if [ ! -f "$WORK/CMakeLists.txt" ]; then
  mkdir -p "$WORK"
  curl -fL "https://github.com/encounter/dawn/archive/$REF.tar.gz" | tar -xzf - -C "$WORK" --strip-components=1
  patch -d "$WORK" -p1 < "$REPO/patches/dawn/hdr.patch"
fi
cmake -S "$WORK" -B "$WORK/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_LINKER_TYPE=LLD \
  -DDAWN_FETCH_DEPENDENCIES=ON -DDAWN_ENABLE_INSTALL=ON \
  -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_BENCHMARKS=OFF -DDAWN_BUILD_TESTS=OFF \
  -DDAWN_SUPPORTS_GLFW_FOR_WINDOWING=OFF -DDAWN_USE_GLFW=OFF -DDAWN_USE_WAYLAND=ON \
  -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF -DTINT_BUILD_IR_BINARY=OFF -DDAWN_BUILD_PROTOBUF=OFF
cmake --build "$WORK/build" --config Release
rm -rf "$WORK/install"
cmake --install "$WORK/build" --config Release --prefix "$WORK/install"
tar -czf "$WORK/dawn-linux-x86_64-hdr.tar.gz" -C "$WORK/install" .
ls -la "$WORK/dawn-linux-x86_64-hdr.tar.gz"

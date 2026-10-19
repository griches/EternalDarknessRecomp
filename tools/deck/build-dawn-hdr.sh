#!/bin/sh
# build-dawn-hdr.sh [WORK]: build Dawn with patches/dawn/hdr.patch (HDR10
# swapchains on Vulkan) as a package laid out like encounter/dawn's
# dawn-linux-x86_64.tar.gz, with the options of its release CI
# (.github/workflows/dawn-ci.cmake). The prebuilt package has no HDR on Linux:
# Dawn picks no colour space for a Vulkan swapchain. Run inside the edbuild
# container; about 25 minutes on a Steam Deck. CMakeLists.txt fetches the copy on
# this repo's releases (tag dawn-266c1cf8-hdr) by its SHA-256: after a rebuild,
# upload the new archive and update ED_DAWN_HDR_SHA256 there, or try it first with
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
cp "$WORK/LICENSE" "$WORK/install/LICENSE-dawn"
tar -czf "$WORK/dawn-linux-x86_64-hdr.tar.gz" -C "$WORK/install" .
sha256sum "$WORK/dawn-linux-x86_64-hdr.tar.gz"

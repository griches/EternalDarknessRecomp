#!/bin/sh
# make-mac-app.sh [OUT]: build "Eternal Darkness.app" for Apple silicon Macs.
# The app holds no game: the host (libedhost.dylib, every library the game
# needs), the disc translator (edprep, DolRecomp), a compiler (Zig) and the
# headers to build with. On first start its program (bootstrap.m) translates
# the player's own disc and builds the game from it.
#
# Needs the Xcode command line tools, CMake and Ninja; downloads Zig 0.14.1
# once into build/toolchains. Writes OUT (default build/mac-app): the .app
# and a zip of it.
set -e
SRC=$(cd "$(dirname "$0")/../.." && pwd)
HERE="$SRC/tools/mac"
B="$SRC/build-pkg-mac"
OUT=${1:-$SRC/build/mac-app}
ZIG_VERSION=0.14.1
ZIG_NAME=zig-aarch64-macos-$ZIG_VERSION
TOOLS="$SRC/build/toolchains"
MIN=13.0
# Assembled and signed outside ~/Documents: iCloud's file provider tags
# folders there with attributes codesign refuses.
STAGE=$(mktemp -d /tmp/ed-mac-app.XXXXXX)
APP="$STAGE/Eternal Darkness.app"
C="$APP/Contents"
R="$C/Resources"

# 1. The host library: every dependency static, built for macOS $MIN, nothing
#    from Homebrew.
if [ ! -f "$B/build.ninja" ]; then
  cmake -S "$SRC" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN -DAURORA_SDL3_PROVIDER=vendor -DAURORA_SDL3_LINKAGE=static \
    -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew -DPKG_CONFIG_EXECUTABLE=/nonexistent -DED_SPLIT_HOST=ON
fi
ninja -C "$B" edhost
if otool -L "$B/libedhost.dylib" | grep -q -e /opt/homebrew -e /usr/local; then
  echo "libedhost.dylib links a library outside macOS:" >&2
  otool -L "$B/libedhost.dylib" >&2
  exit 1
fi

# 2. The translator and the app's program.
mkdir -p "$OUT/obj"
clang++ -std=c++17 -O2 -mmacosx-version-min=$MIN "$SRC/tools/edprep.cpp" -o "$OUT/obj/edprep"
clang -fobjc-arc -O2 -mmacosx-version-min=$MIN "$HERE/bootstrap.m" -o "$OUT/obj/EternalDarkness" \
  -framework Cocoa -framework UniformTypeIdentifiers
cmake -S "$SRC/.deps/DolRecomp" -B "$OUT/obj/dolrecomp" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN > /dev/null
ninja -C "$OUT/obj/dolrecomp" dolrecomp > /dev/null

# 3. Zig, for `zig cc` to aarch64-macos only: its compiler, the macOS libc
#    headers and libSystem stub, clang's headers, and the std / compiler-rt
#    sources it builds its runtime from.
if [ ! -x "$TOOLS/$ZIG_NAME/zig" ]; then
  mkdir -p "$TOOLS"
  curl -sSfL "https://ziglang.org/download/$ZIG_VERSION/$ZIG_NAME.tar.xz" | tar -xJf - -C "$TOOLS"
fi

# 4. The bundle.
mkdir -p "$C/MacOS" "$C/Frameworks" "$R/tools" "$R/sdk/runtime/host" "$R/sdk/include" "$R/licenses"
cp "$OUT/obj/EternalDarkness" "$C/MacOS/EternalDarkness"
cp "$B/libedhost.dylib" "$C/Frameworks/"
cp "$OUT/obj/edprep" "$OUT/obj/dolrecomp/dolrecomp" "$R/tools/"
cp "$SRC/config/GEDE01/symbols.txt" "$R/tools/"
cp "$SRC"/tools/deck/hot-*.txt "$R/tools/"
cp -R "$SRC/res" "$R/res"
cp "$SRC/res/icon/AppIcon.icns" "$R/AppIcon.icns"
# What the player's build compiles: the host files that see the disc, the
# host and runtime headers, and the defines the rest of the host was built with.
for f in main.c hle.c audio.c sdk_map.c quickstate.c; do cp "$SRC/runtime/host/$f" "$R/sdk/runtime/host/"; done
cp "$SRC"/runtime/host/*.h "$R/sdk/runtime/host/"
cp -R "$SRC/.deps/GXRuntime/include/." "$R/sdk/include/"
ninja -C "$B" -t commands EternalDarkness | grep 'runtime/host/main.c.o ' | head -1 | tr ' ' '\n' |
  grep '^-D' | grep -v -e '^-DED_GAME_ID' -e '"' | sort -u | tr '\n' ' ' > "$R/sdk/cflags.txt"
cp -R "$TOOLS/$ZIG_NAME" "$R/zig"
Z="$R/zig/lib"
rm -rf "$R/zig/doc" "$Z/docs" "$Z/fuzzer" "$Z/tsan" "$Z/libcxx" "$Z/libcxxabi" "$Z/libunwind" \
       "$Z/libc/musl" "$Z/libc/mingw" "$Z/libc/wasi" "$Z/libc/glibc"
for d in "$Z"/libc/include/*; do
  [ "$(basename "$d")" = any-macos-any ] || rm -rf "$d"
done

# Licences: this project, its runtime and translator, Zig, and the libraries
# in libedhost.
cp "$SRC/LICENSE" "$R/licenses/EternalDarknessRecomp.txt"
cp "$SRC/.deps/DolRecomp/LICENSE" "$R/licenses/DolRecomp.txt"
cp "$SRC/.deps/GXRuntime/LICENSE" "$R/licenses/GXRuntime.txt"
cp "$TOOLS/$ZIG_NAME/LICENSE" "$R/licenses/Zig.txt"
for dep in "$B"/_deps/*-src; do
  name=$(basename "$dep" -src)
  for l in LICENSE LICENSE.txt LICENSE.md COPYING LICENSE.TXT docs/FTL.TXT; do
    if [ -f "$dep/$l" ]; then cp "$dep/$l" "$R/licenses/$name.txt"; break; fi
  done
done
for l in "$B"/_deps/dawn_prebuilt-src/LICENSE*; do [ -f "$l" ] && cp "$l" "$R/licenses/dawn.txt"; done

VERSION="$(date +%Y%m%d%H%M)-$(git -C "$SRC" rev-parse --short HEAD)"
echo "$VERSION" > "$R/VERSION"
sed -e "s/@VERSION@/$VERSION/" -e "s/@MIN@/$MIN/" "$HERE/Info.plist.in" > "$C/Info.plist"
cp "$HERE/README.txt" "$OUT/README.txt"

# 5. Sign (ad hoc: every binary in the bundle) and zip.
xattr -cr "$APP"  # copied files' Finder info and quarantine flags fail signing
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"
ZIP="$OUT/EternalDarkness-mac-$VERSION.zip"
rm -f "$OUT"/EternalDarkness-mac-*.zip
(cd "$STAGE" && ditto -c -k --keepParent "Eternal Darkness.app" "$ZIP" && zip -q -j "$ZIP" "$OUT/README.txt")
rm -rf "$OUT/Eternal Darkness.app"
ditto "$APP" "$OUT/Eternal Darkness.app"
echo "app: $OUT/Eternal Darkness.app ($(du -sh "$APP" | cut -f1)), zip: $ZIP ($(du -h "$ZIP" | cut -f1))"
rm -rf "$STAGE"

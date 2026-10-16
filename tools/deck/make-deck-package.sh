#!/bin/sh
# make-deck-package.sh [OUT]: assemble the Linux x86-64 / Steam Deck package from
# build-linux. It holds everything except the game: the runtime (prebuilt), the
# translator, a compiler (Zig), and play.sh, which translates the player's own
# disc and builds the game from it on first launch. Run inside the edbuild
# container (it needs the container's clang for the two launcher objects and
# DolRecomp).
set -e
SRC=/home/deck/src/PS5_EternalDarkness
B=$SRC/build-linux
HERE=$(dirname "$(readlink -f "$0")")
OUT=${1:-/home/deck/src/pkgtest/EternalDarknessRecomp}
ZIGDIR=/home/deck/src/toolchains/zig-x86_64-linux-0.14.1

rm -rf "$OUT"
mkdir -p "$OUT/lib/link" "$OUT/obj" "$OUT/include" "$OUT/runtime/host" "$OUT/tools" \
         "$OUT/config/GEDE01" "$OUT/.deps/DolRecomp/build" "$OUT/toolchain"

# Host sources that see the disc (game_data.h, generated.h, ED_GAME_ID): compiled
# on first launch. frontend.cpp only needs ED_GAME_ID, so it ships prebuilt for
# both discs.
DISC_SRC="main.c hle.c audio.c sdk_map.c quickstate.c"

cd "$B"
ninja -t commands EternalDarkness | tail -1 > /tmp/pkg-link.sh
ninja -t commands EternalDarkness | grep 'runtime/host/main.c.o' | head -1 > /tmp/pkg-cc-main.sh
ninja -t commands EternalDarkness | grep 'runtime/host/frontend.cpp.o' | head -1 > /tmp/pkg-cc-frontend.sh

# 1. Link inputs, in link order (lib/link.txt). Build-tree shared libraries ship
#    in lib/ under their SONAME (play.sh puts lib/ on LD_LIBRARY_PATH); system
#    libraries are link-time copies only (lib/link/), SteamOS has them at run time.
: > "$OUT/lib/link.txt"
for tok in $(cat /tmp/pkg-link.sh); do
  case "$tok" in
    CMakeFiles/EternalDarkness.dir/generated/*) ;;
    CMakeFiles/EternalDarkness.dir/runtime/host/*.o)
      f=$(basename "$tok" .o)
      case " $DISC_SRC frontend.cpp " in
        *" $f "*) ;;
        *) cp "$tok" "$OUT/obj/"; echo "obj/$(basename "$tok")" >> "$OUT/lib/link.txt" ;;
      esac ;;
    /usr/lib/*.so|/usr/lib/*.so.*)
      real=$(readlink -f "$tok"); soname=$(objdump -p "$real" | awk '/SONAME/ {print $2}')
      cp "$real" "$OUT/lib/link/$soname"; echo "lib/link/$soname" >> "$OUT/lib/link.txt" ;;
    *.a)
      b=$(basename "$tok")
      if [ -f "$OUT/lib/$b" ] && ! cmp -s "$tok" "$OUT/lib/$b"; then
        b=$(echo "$tok" | tr / _)   # same name, different archive
      fi
      [ -f "$OUT/lib/$b" ] || cp "$tok" "$OUT/lib/$b"
      echo "lib/$b" >> "$OUT/lib/link.txt" ;;
    *.so|*.so.*)
      real=$(readlink -f "$tok"); soname=$(objdump -p "$real" | awk '/SONAME/ {print $2}')
      cp "$real" "$OUT/lib/${soname:-$(basename "$tok")}"
      echo "lib/${soname:-$(basename "$tok")}" >> "$OUT/lib/link.txt" ;;
    -l*) echo "$tok" >> "$OUT/lib/link.txt" ;;
  esac
done
for l in libstdc++.so.6 libgcc_s.so.1; do
  cp -L "/usr/lib/x86_64-linux-gnu/$l" "$OUT/lib/link/$l"; echo "lib/link/$l" >> "$OUT/lib/link.txt"
done

# 2. The launcher (frontend.cpp) for each supported disc.
for id in GEDE01 GEDP01; do
  sed -e "s|-DED_GAME_ID=[^ ]*|-DED_GAME_ID=\\\\\"$id\\\\\"|" \
      -e "s| -o [^ ]*| -o $OUT/obj/frontend-$id.o|" \
      -e "s| -MD -MT [^ ]* -MF [^ ]*||" /tmp/pkg-cc-frontend.sh | sh
done

# 3. Compile flags for the first-launch host C files (defines only; play.sh adds
#    its own include paths and ED_GAME_ID).
tr ' ' '\n' < /tmp/pkg-cc-main.sh | grep '^-D' | grep -v '^-DED_GAME_ID' | sed 's/\\"/"/g' | tr '\n' ' ' > "$OUT/lib/host-cflags.txt"

# 4. Sources and headers those files need.
for f in $DISC_SRC; do cp "$SRC/runtime/host/$f" "$OUT/runtime/host/"; done
cp "$SRC"/runtime/host/*.h "$OUT/runtime/host/"
cp -r "$SRC/.deps/GXRuntime/include/." "$OUT/include/"

# 5. The translator: DolRecomp (patched, built here) and the generate scripts.
cmake -S "$SRC/.deps/DolRecomp" -B /tmp/pkg-dolrecomp -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang > /dev/null
ninja -C /tmp/pkg-dolrecomp dolrecomp > /dev/null
cp /tmp/pkg-dolrecomp/dolrecomp "$OUT/.deps/DolRecomp/build/dolrecomp"
for t in generate.py fix_generated.py symbols.py gamedata.py; do cp "$SRC/tools/$t" "$OUT/tools/"; done
cp "$SRC/config/GEDE01/symbols.txt" "$OUT/config/GEDE01/"
cp "$HERE"/hot-*.txt "$OUT/tools/" 2>/dev/null || true

# 6. Compiler, UI files, launcher script, licences.
cp -r "$ZIGDIR" "$OUT/toolchain/zig"
# Keep only what `zig cc` needs for x86_64-linux-gnu: glibc stubs and headers,
# clang's headers, and the Zig std/compiler_rt sources its cached runtime is
# checked against.
Z="$OUT/toolchain/zig/lib"
rm -rf "$OUT/toolchain/zig/doc" "$Z/docs" "$Z/fuzzer" "$Z/tsan" "$Z/libcxx" "$Z/libcxxabi" \
       "$Z/libc/musl" "$Z/libc/mingw" "$Z/libc/wasi" "$Z/libc/darwin"
for d in "$Z"/libc/include/*; do
  case "$(basename "$d")" in
    generic-glibc|x86_64-linux-gnu|x86-linux-any|any-linux-any) ;;
    *) rm -rf "$d" ;;
  esac
done
cp -r "$SRC/res" "$OUT/res"
cp "$HERE/play.sh" "$OUT/play.sh"; chmod +x "$OUT/play.sh"
cp "$HERE/README.txt" "$OUT/README.txt"
mkdir -p "$OUT/licenses"
cp "$SRC/LICENSE" "$OUT/licenses/EternalDarknessRecomp.txt"
cp "$SRC/.deps/DolRecomp/LICENSE" "$OUT/licenses/DolRecomp.txt"
cp "$SRC/.deps/GXRuntime/LICENSE" "$OUT/licenses/GXRuntime.txt"
cp "$ZIGDIR/LICENSE" "$OUT/licenses/Zig.txt"

# 7. Warm Zig's cache (its compiler-rt and glibc stubs for this target) so the
#    player's first link only links.
printf 'int main(void){return 0;}\n' > /tmp/pkg-warm.c
ZIG_GLOBAL_CACHE_DIR="$OUT/.cache/zig" ZIG_LOCAL_CACHE_DIR="$OUT/.cache/zig" \
  "$OUT/toolchain/zig/zig" cc -target x86_64-linux-gnu.2.41 -pthread /tmp/pkg-warm.c \
  "$OUT/lib/link/libstdc++.so.6" "$OUT/lib/link/libgcc_s.so.1" -lm -lrt -ldl -o /tmp/pkg-warm
date +%Y%m%d%H%M > "$OUT/VERSION"

echo "package: $OUT ($(du -sh "$OUT" | cut -f1))"

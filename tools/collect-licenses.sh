#!/bin/sh
# collect-licenses.sh OUT BUILD [ZIG]: the licence texts of everything a build
# of this project ships, into OUT (one file per component, and THIRD_PARTY.md
# as README.md). BUILD is the CMake build tree, whose _deps hold the libraries
# it fetched; ZIG is the Zig folder, for the packages that carry the compiler.
set -e
SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT=$1
B=$2
ZIG=$3
mkdir -p "$OUT"
cp "$SRC/THIRD_PARTY.md" "$OUT/README.md"
cp "$SRC/LICENSE" "$OUT/EternalDarknessRecomp.txt"
cp "$SRC/.deps/DolRecomp/LICENSE" "$OUT/DolRecomp.txt"
cp "$SRC/.deps/GXRuntime/LICENSE" "$OUT/GXRuntime.txt"
cp "$SRC/.deps/GXRuntime/graphics/aurora/LICENSE" "$OUT/Aurora.txt"
cp "$SRC"/licenses/*.txt "$OUT/"
cp "$SRC"/res/fonts/*-OFL.txt "$OUT/"
cp "$SRC/res/ui/glyphs/LICENSE-Kenney-CC0.txt" "$OUT/Kenney-Input-Prompts-CC0.txt"
# The libraries the build fetched (googletest only builds tests).
for dep in "$B"/_deps/*-src; do
  [ -d "$dep" ] || continue
  name=$(basename "$dep" -src)
  case $name in googletest|dawn_prebuilt) continue ;; esac
  for l in LICENSE LICENSE.txt LICENSE.TXT LICENSE.md COPYING; do
    if [ -f "$dep/$l" ]; then cp "$dep/$l" "$OUT/$name.txt"; break; fi
  done
done
# FreeType's LICENSE.TXT only names the choice of licences; the FTL is its own file.
[ -f "$B/_deps/freetype-src/docs/FTL.TXT" ] && cp "$B/_deps/freetype-src/docs/FTL.TXT" "$OUT/freetype-FTL.txt"
if [ -n "$ZIG" ]; then cp "$ZIG/LICENSE" "$OUT/Zig.txt"; fi

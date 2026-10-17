#!/bin/sh
# make-win-package.sh STAGE [OUTDIR]: zip the Windows (x64) package from a build
# staged on the PC by tools/win/stage-win-build.bat and copied here (STAGE holds
# bin/, lic/, sdl-licenses/ and build-commit, the commit the exe was built from).
#
# Unlike the Mac app and the Steam Deck folder, this package is ready-made: the
# exe holds the game's code translated from the European disc, as the PS5 title
# does (Gary's decision, 2026-10-17). The disc, main.dol, saves and settings
# never go in: the player puts the disc in dump\ and the first start reads
# main.dol from it.
#
#   tools/win/make-win-package.sh build/release/win-stage
#     -> build/release/EternalDarkness-windows-x64-<time>-<commit>.zip
set -e
SRC=$(cd "$(dirname "$0")/../.." && pwd)
HERE="$SRC/tools/win"
STAGE=$(cd "${1:?usage: make-win-package.sh STAGE [OUTDIR]}" && pwd)
OUTDIR=${2:-$SRC/build/release}
COMMIT=$(cat "$STAGE/build-commit")
STAMP=$(date +%Y%m%d%H%M)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
P="$work/EternalDarknessRecomp"
mkdir -p "$P/dump"

# The game, its DLLs and the Visual C++ runtime; the menus and fonts as they
# were at the exe's commit.
cp "$STAGE"/bin/EternalDarkness.exe "$STAGE"/bin/*.dll "$P/"
git -C "$SRC" archive "$COMMIT" res | tar -x -C "$P"

# Text files the player opens, with Windows line endings.
crlf() { sed 's/$/\r/' "$1" > "$2"; }
crlf "$HERE/play.bat" "$P/play.bat"
crlf "$HERE/README.txt" "$P/README.txt"
printf 'Put your own image of the European Eternal Darkness disc (GEDP01, .iso or\n.gcm) in this folder. Any file name works.\n' > "$work/disc.txt"
crlf "$work/disc.txt" "$P/dump/PUT YOUR DISC HERE.txt"

# Licences: the shared collector over the build's fetched libraries, then what
# only this package carries (SDL's prebuilt, the shader compiler, the runtime).
mkdir -p "$work/b/_deps"
for d in "$STAGE"/lic/*-src; do ln -s "$d" "$work/b/_deps/$(basename "$d")"; done
"$SRC/tools/collect-licenses.sh" "$P/licenses" "$work/b"
[ -f "$STAGE/lic/freetype-src/FTL.TXT" ] && cp "$STAGE/lic/freetype-src/FTL.TXT" "$P/licenses/freetype-FTL.txt"
cp "$STAGE/sdl-licenses/SDL3/LICENSE.txt" "$P/licenses/SDL3.txt"
cp "$HERE"/licenses/*.txt "$P/licenses/"
cat > "$P/licenses/Microsoft-Visual-C++-runtime.txt" <<'EOF'
msvcp140.dll, msvcp140_atomic_wait.dll, vcruntime140.dll and vcruntime140_1.dll
are Microsoft's Visual C++ runtime (x64, from Visual Studio's redistributable
files), shipped beside the game as Microsoft's licence terms for that
redistributable code allow. dxcompiler.dll and dxil.dll are Microsoft's
DirectX Shader Compiler (see the DirectXShaderCompiler-*.txt files).
EOF

found=$(find "$P" \( -iname '*.dol' -o -iname '*.iso' -o -iname '*.gcm' -o -iname '*.ciso' \
    -o -iname '*.dolcard' -o -iname '*.state' -o -name settings.ini -o -name env.txt \) -print)
[ -z "$found" ] || { echo "make-win-package.sh: game or player files in the package:" >&2; echo "$found" >&2; exit 2; }

mkdir -p "$OUTDIR"
zip_path="$OUTDIR/EternalDarkness-windows-x64-$STAMP-$COMMIT.zip"
rm -f "$zip_path"
(cd "$work" && COPYFILE_DISABLE=1 zip -q -r -X "$zip_path" EternalDarknessRecomp)
echo "$zip_path ($(du -h "$zip_path" | cut -f1))"

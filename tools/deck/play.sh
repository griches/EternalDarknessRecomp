#!/bin/sh
# Eternal Darkness Recomp (Linux x86-64 / Steam Deck).
#
# Put your own Eternal Darkness disc image (.iso or .gcm of the European disc,
# GEDP01; the USA disc is not supported) in this folder, then run this script, or add it to Steam. The first run
# translates the disc and builds the game from it (a few minutes: about 4 on
# a Steam Deck). After that the game starts straight away. Nothing from your disc
# leaves this folder.
cd "$(dirname "$(readlink -f "$0")")" || exit 1
HERE=$PWD
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
ZIG="$HERE/toolchain/zig/zig"
# Zig's cache writes Makefile-style dependency files, which a ":" in the
# cache's path breaks: every file failed with InvalidDepFile in a folder named
# "Eternal Darkness: Sanity's Requiem". There the cache is a temporary folder,
# deleted when the build is done, so nothing from the disc stays outside.
cache="$HERE/.cache/zig"
case "$HERE" in
*:*)
  cache=$(mktemp -d "${TMPDIR:-/tmp}/ed-zig.XXXXXX") || exit 1
  trap 'rm -rf "$cache"' EXIT
  ;;
esac
export ZIG_GLOBAL_CACHE_DIR="$cache" ZIG_LOCAL_CACHE_DIR="$cache"
JOBS=$(nproc 2>/dev/null || echo 4)
mkdir -p logs
# Three GX plan workers on 4-core / 8-thread CPUs like the Deck's (the best there
# in the study: 60 fps with the steadiest frame times).
[ "$JOBS" -le 8 ] && export DOL_GX_PLAN_WORKERS=${DOL_GX_PLAN_WORKERS:-3}

say() {  # a message the player sees: a dialog in the UI, else the terminal
  if command -v zenity >/dev/null 2>&1; then zenity --"$1" --title="Eternal Darkness" --width=420 --text="$2" 2>/dev/null
  else echo "$2" >&2; fi
}

find_disc() {
  for f in "$HERE"/*.iso "$HERE"/*.ISO "$HERE"/*.gcm "$HERE"/*.GCM "$HERE"/dump/*.iso "$HERE"/dump/*.gcm; do
    [ -f "$f" ] && { echo "$f"; return 0; }
  done
  return 1
}

disc=$(find_disc) || {
  say error "Put your Eternal Darkness disc image (.iso or .gcm) in this folder:\n\n$HERE\n\nthen start the game again."
  exit 1
}

# First run on this machine: start fullscreen at the screen's own resolution
# (the launcher's Options change these later).
[ -f settings.ini ] || printf 'fullscreen = 1\nrender_scale = 0\n' > settings.ini

stamp="$(stat -c '%n %s %Y' "$disc") $(cat VERSION 2>/dev/null)"

# Translate the disc and build the game: prints zenity progress lines.
build() {
  rm -f build.ok
  echo "5"; echo "# Reading your disc (only on the first start)..."
  python3 tools/generate.py "$disc" --out generated --jobs "$JOBS" >> logs/build.log 2>&1 || return 1
  id=$(cat generated/game_id.txt)

  # Game code: every chunk at -O1. 0.9.5 built only the 25 chunks hottest in a
  # few profiled scenes at -O1 and the rest at -O0, which ran two to three times
  # slower: busy rooms elsewhere fell to 23-42 fps on a Deck (the chapter 9
  # cathedral, issue #8). -O1 runs the game code as fast as -O3 for a fraction
  # of its compile time: about 4 minutes on a Deck, once. Biggest files
  # first, so the last ones to finish are short.
  rm -rf build; mkdir -p build/obj
  : > build/jobs
  for f in runtime/host/main.c runtime/host/hle.c runtime/host/audio.c runtime/host/sdk_map.c \
           runtime/host/quickstate.c; do
    echo "2 $(stat -c %s "$f") $f" >> build/jobs
  done
  ls -S generated/chunks/*.c generated/generated.c | while read -r f; do
    echo "1 $(stat -c %s "$f") $f" >> build/jobs
  done
  total=$(awk '{ s += $2 } END { print s }' build/jobs)

  echo "15"; echo "# Building the game from your disc. Only on the first start: a few minutes (about 4 on a Steam Deck)."
  # DOLRECOMP_STATIC_CPU and the CodeGenPrepare limit: see CMakeLists.txt.
  CFLAGS="-target x86_64-linux-gnu.2.41 -g0 -w -fno-sanitize=all $(cat lib/host-cflags.txt) -DED_GAME_ID=\"$id\" -DDOLRECOMP_STATIC_CPU=dolrecomp_cpu -mllvm -cgp-max-address-users-to-scan=100000 -Iruntime -Igenerated -Iinclude"
  export ZIG CFLAGS
  xargs -P "$JOBS" -L1 sh -c '"$ZIG" cc $CFLAGS -O$0 -Xclang -O$0 -c "$2" -o "build/obj/$(basename "$2").o" || echo "$2" >> build/failed' \
    < build/jobs >> logs/build.log 2>&1 &
  pid=$!
  # Progress by the size of the sources built, every 2 s (no subprocess per
  # file: the compiles have every core). The time left comes from the pace so
  # far, once a tenth is built, and is shown again only when it changes.
  start=$(date +%s); said=""
  while kill -0 $pid 2>/dev/null; do
    built=0
    while read -r o size f; do
      [ -f "build/obj/${f##*/}.o" ] && built=$((built + size))
    done < build/jobs
    echo $((15 + built * 75 / total))
    elapsed=$(($(date +%s) - start))
    if [ $((built * 10)) -ge "$total" ] && [ "$elapsed" -ge 10 ]; then
      left=$((elapsed * (total - built) / built))
      if [ "$left" -lt 60 ]; then text="less than a minute left"
      elif [ "$left" -lt 120 ]; then text="about a minute left"
      else text="about $(((left + 59) / 60)) minutes left"; fi
      [ "$text" != "$said" ] && echo "# Building the game from your disc (only on the first start): $text."
      said=$text
    fi
    sleep 2
  done
  wait $pid
  [ -f build/failed ] && return 1

  echo "92"; echo "# Linking..."
  "$ZIG" cc -target x86_64-linux-gnu.2.41 -pthread -o build/EternalDarkness \
    build/obj/*.o "obj/frontend-$id.o" $(cat lib/link.txt) >> logs/build.log 2>&1 || return 1
  mv build/EternalDarkness EternalDarkness
  echo "$stamp" > build.stamp
  touch build.ok
  echo "100"; echo "# Ready."
}

if [ ! -x EternalDarkness ] || [ "$(cat build.stamp 2>/dev/null)" != "$stamp" ]; then
  : > logs/build.log
  if command -v zenity >/dev/null 2>&1 && [ -n "$DISPLAY$WAYLAND_DISPLAY" ]; then
    build | zenity --progress --title="Eternal Darkness" --width=420 --auto-close --no-cancel \
      --text="Preparing your game..." 2>/dev/null
  else
    build | grep '^#' | sed 's/^# //'
  fi
  if [ ! -f build.ok ]; then
    say error "Building the game from your disc failed. The details are in\n$HERE/logs/build.log"
    exit 1
  fi
fi

# exec skips the EXIT trap: the temporary cache goes now.
[ "$cache" = "$HERE/.cache/zig" ] || rm -rf "$cache"
# The disc as the launcher's own folder scan names it ("x.iso", "dump/x.iso"),
# so it isn't listed twice.
exec ./EternalDarkness generated/main.dol --iso "${disc#"$HERE"/}" --max-blocks 1000000000000000 "$@" \
  > logs/game.log 2>&1

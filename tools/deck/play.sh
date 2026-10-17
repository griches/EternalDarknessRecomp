#!/bin/sh
# Eternal Darkness Recomp (Linux x86-64 / Steam Deck).
#
# Put your own Eternal Darkness disc image (.iso or .gcm of the European disc,
# GEDP01; the USA disc is not supported) in this folder, then run this script, or add it to Steam. The first run
# translates the disc and builds the game from it (about half a minute on a
# Steam Deck). After that the game starts straight away. Nothing from your disc
# leaves this folder.
cd "$(dirname "$(readlink -f "$0")")" || exit 1
HERE=$PWD
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
ZIG="$HERE/toolchain/zig/zig"
export ZIG_GLOBAL_CACHE_DIR="$HERE/.cache/zig" ZIG_LOCAL_CACHE_DIR="$HERE/.cache/zig"
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
  echo "5"; echo "# Reading your disc..."
  python3 tools/generate.py "$disc" --out generated --jobs "$JOBS" >> logs/build.log 2>&1 || return 1
  id=$(cat generated/game_id.txt)

  # Game code: the chunks hot in gameplay and in the movie decoder (profiled on a
  # Deck) get -O1, the rest -O0: a fraction of the compile time, and the rest is
  # a small share of a frame. Movies need the decoder at -O1 to keep up.
  rm -rf build; mkdir -p build/obj
  : > build/jobs
  for f in generated/chunks/*.c; do
    a=$(basename "$f" .c); a=${a##*_}
    if grep -qx "$a" "tools/hot-$id.txt" 2>/dev/null; then echo "1 $f" >> build/jobs.hot
    else echo "0 $f" >> build/jobs.cold; fi
  done
  echo "1 generated/generated.c" >> build/jobs.hot
  for f in main.c hle.c audio.c sdk_map.c quickstate.c; do echo "2 runtime/host/$f" >> build/jobs.hot; done
  cat build/jobs.hot build/jobs.cold > build/jobs 2>/dev/null; rm -f build/jobs.hot build/jobs.cold
  total=$(wc -l < build/jobs)

  echo "15"; echo "# Building the game from your disc (only the first time)..."
  # DOLRECOMP_STATIC_CPU and the CodeGenPrepare limit: see CMakeLists.txt.
  CFLAGS="-target x86_64-linux-gnu.2.41 -g0 -w -fno-sanitize=all $(cat lib/host-cflags.txt) -DED_GAME_ID=\"$id\" -DDOLRECOMP_STATIC_CPU=dolrecomp_cpu -mllvm -cgp-max-address-users-to-scan=100000 -Iruntime -Igenerated -Iinclude"
  export ZIG CFLAGS
  xargs -P "$JOBS" -L1 sh -c '"$ZIG" cc $CFLAGS -O$0 -Xclang -O$0 -c "$1" -o "build/obj/$(basename "$1").o" || echo "$1" >> build/failed' \
    < build/jobs >> logs/build.log 2>&1 &
  pid=$!
  while kill -0 $pid 2>/dev/null; do
    n=$(ls build/obj | wc -l)
    echo $((15 + n * 75 / total))
    sleep 1
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

exec ./EternalDarkness generated/main.dol --iso "$disc" --max-blocks 1000000000000000 "$@" \
  > logs/game.log 2>&1

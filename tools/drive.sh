#!/bin/sh
# drive.sh: play the windowed build by hand from scripts, one step at a time.
#   drive.sh start PREFIX [ENV=..]  launch (log build/runs/PREFIX.log); CARD=path
#                                   picks the card (default build/runs/test.dolcard)
#   drive.sh key KEY [SECONDS]      hold a key (default 0.15 s): j=A k=B
#                                   return=Start, w/a/s/d=stick, up/down/left/right
#   drive.sh shot NAME              screenshot to build/shots/NAME.png
#   drive.sh stop                   force-quit (the app ignores SIGTERM)
cd "$(dirname "$0")/.."
pidfile=build/runs/drive.pid
cmd=$1; shift
case $cmd in
start)
  prefix=$1; shift
  [ -f "$pidfile" ] && kill -9 "$(cat "$pidfile")" 2>/dev/null
  [ -f build/runs/test.dolcard ] || cp ed-GEDP01-slot-a.dolcard build/runs/test.dolcard
  env ED_SETTINGS=build/runs/test-settings.ini "$@" ./build-mac/EternalDarkness generated/main.dol --no-launcher --iso dump/GEDP01.iso \
      --card "${CARD:-build/runs/test.dolcard}" \
      --max-blocks 100000000000 > "build/runs/$prefix.log" 2>&1 &
  echo $! > "$pidfile"
  echo "started pid $!"
  ;;
key)
  k=$1; hold=${2:-0.15}
  case $k in
  return) code=36 ;; up) code=126 ;; down) code=125 ;; left) code=123 ;;
  right) code=124 ;; *) code= ;;
  esac
  pid=$(cat "$pidfile")
  if [ -n "$code" ]; then press="key down (key code $code)"; release="key up (key code $code)"
  else press="key down \"$k\""; release="key up \"$k\""; fi
  osascript -e "tell application \"System Events\"
    set frontmost of (first process whose unix id is $pid) to true
    delay 0.1
    $press
    delay $hold
    $release
  end tell"
  ;;
shot)
  ./build/winshot EternalDarkness "build/shots/$1.png"
  ;;
stop)
  [ -f "$pidfile" ] && kill -9 "$(cat "$pidfile")" 2>/dev/null
  rm -f "$pidfile"
  echo stopped
  ;;
*)
  sed -n 2,8p "$0"; exit 1 ;;
esac

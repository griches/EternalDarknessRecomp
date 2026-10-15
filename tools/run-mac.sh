#!/bin/sh
# run-mac.sh SECONDS SHOTS_PREFIX [env...]: run the windowed build, screenshot the
# window every SHOT_EVERY (default 8) s, then force-quit it (it ignores SIGTERM).
# Pass ED_MUTE=1 to silence a run, ED_SKIP_INTRO=1 to tap A through the intro
# movies (it stops when the opening movie ends). Runs use build/runs/test.dolcard
# (a copy of the real card, made once) so tests never write to the player's
# save; CARD=path picks another.
cd "$(dirname "$0")/.."
secs=$1; prefix=$2; shift 2
[ -f build/runs/test.dolcard ] || cp ed-GEDP01-slot-a.dolcard build/runs/test.dolcard 2>/dev/null
env "$@" ./build-mac/EternalDarkness generated/main.dol --iso dump/GEDP01.iso \
    --card "${CARD:-build/runs/test.dolcard}" \
    --max-blocks 100000000000 > "build/runs/$prefix.log" 2>&1 &
pid=$!
n=0; t=0
while [ $t -lt "$secs" ]; do
  perl -e 'select(undef,undef,undef,$ENV{SHOT_EVERY}||8)'; t=$((t + ${SHOT_EVERY:-8})); n=$((n + 1))
  ./build/winshot EternalDarkness "build/shots/$prefix-$n.png" > /dev/null 2>&1
done
kill -9 $pid 2>/dev/null
wait $pid 2>/dev/null
echo "done: build/runs/$prefix.log, $n shots"

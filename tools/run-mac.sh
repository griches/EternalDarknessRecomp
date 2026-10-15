#!/bin/sh
# run-mac.sh SECONDS SHOTS_PREFIX [env...]: run the windowed build, screenshot the
# window every SHOT_EVERY (default 8) s, then force-quit it (it ignores SIGTERM).
cd "$(dirname "$0")/.."
secs=$1; prefix=$2; shift 2
env "$@" ./build-mac/EternalDarkness generated/main.dol --iso dump/GEDP01.iso \
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

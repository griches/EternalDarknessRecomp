#!/bin/sh
# Play the Mac build: ./play.sh [disc image] (default dump/<game id>.iso).
# Keyboard: arrows = d-pad, J = A, K = B, U = X, I = Y, Q = Z, E/R = L/R,
# Enter = Start. A connected controller works too. Close the window to quit.
cd "$(dirname "$0")"
id=$(cat generated/game_id.txt 2>/dev/null || echo GEDP01)
iso=${1:-dump/$id.iso}
exec ./build-mac/EternalDarkness generated/main.dol --iso "$iso" --max-blocks 1000000000000000

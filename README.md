# Eternal Darkness: Sanity's Requiem (static recompilation)

A native build of the GameCube game made from **your own disc**: the game's
PowerPC code is translated to C by [DolRecomp](https://github.com/ExpansionPak/DolRecomp)
and runs on [GXRuntime](https://github.com/aharonahdoot/GXRuntime) with the
Aurora renderer. The Mac build comes first; a PS5 build is planned.

No game data is in this repository. `generated/` (code translated from your
disc) and `dump/` (your disc image) stay on your machine.

Status: boots, plays the Edgar Allan Poe opening with its streamed music and
moves on to the intro movie, which is still black and slow. Supported discs:
Europe `GEDP01` (tested) and USA `GEDE01`.

## Build (Mac)

```sh
tools/setup-deps.sh                       # pinned DolRecomp + GXRuntime, plus our GXRuntime patch
python3 tools/generate.py /path/to/disc.iso   # ISO or CISO; writes generated/
cmake -S . -B build-mac -G Ninja && ninja -C build-mac EternalDarkness
cp /path/to/disc.iso dump/GEDP01.iso      # or GEDE01.iso for the USA disc
./play.sh
```

Keyboard: arrows = d-pad, J = A, K = B, U = X, I = Y, Q = Z, E/R = L/R,
Enter = Start. Controllers work through SDL.

`./play.sh` opens the launcher: pick the disc (images in `dump/` or the
project folder; this build runs the disc it was generated from), Play, or
Options. In the game, Esc or the controller's Back/Select button pauses into a
menu with four save slots (`saves/`), the same options, and a way back to the
launcher. Menus work with the keyboard (arrows, Enter, Esc), a controller
(d-pad or stick, A, B) or the mouse.

Options (kept in `settings.ini`): skip intros, display (follow the game's
widescreen setting, 4:3, 16:9 or stretch), widescreen HUD, resolution (each
choice shows its size), fullscreen, on-screen FPS counter, music and
sound-effect volumes, audio buffer, and Controls: Original (GameCube
positions), Modern (attack RT/A, action B, aim LT, sprint and creep on the stick clicks
as toggles, reload X/Square, quickcast Y/Triangle) or
Custom (select an action, then press a button, trigger or key), Sprint, Creep
and Target as hold or toggle, stick deadzone and vibration. Bindings show
as Xbox, PlayStation or Switch button icons for the connected controller.

Quick save: 2 saves the whole machine to `build/runs/quick.state` and 1 loads
it back, in the same session or after a restart (`ED_QUICKSTATE=path` picks
another file). Memory-card saves are never touched by a load.

## Layout

- `runtime/host/`: the game's host layer (SDK intercepts, interrupts, disc,
  audio and disc-music streaming), derived from
  [StrikersRecomp](https://github.com/aharonahdoot/StrikersRecomp)'s host.
- `tools/`: disc reading and code generation (`generate.py`), the symbol map
  carried from the USA decomp to the PAL disc (`symbols.py`), per-build
  constants read from the game's code (`gamedata.py`), and debugging helpers.
- `config/GEDE01/symbols.txt`: function names from the
  [unending-occlusion](https://github.com/PattyTrish/unending-occlusion) decomp (CC0).
- `patches/gxruntime/`, `patches/dolrecomp/`: fixes to GXRuntime and the
  recompiler this game needs.
- `res/`: the launcher and in-game menu (RmlUi documents) and their fonts
  (Alegreya SC and Fira Sans, SIL Open Font License, licences alongside), and
  button icons from Kenney's Input Prompts (CC0).

## Licence

GPL-3.0-or-later (see `LICENSE`), as the GXRuntime and StrikersRecomp code it
builds on.

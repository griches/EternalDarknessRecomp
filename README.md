# Eternal Darkness Recomp

A static recompilation of **Eternal Darkness: Sanity's Requiem** (GameCube):
a native game built from **your own disc**. The game's PowerPC code is
translated to C by [DolRecomp](https://github.com/ExpansionPak/DolRecomp)
and runs on [GXRuntime](https://github.com/aharonahdoot/GXRuntime) with the
Aurora renderer.

No game data is in this repository. `generated/` (code translated from your
disc) and `dump/` (your disc image) stay on your machine.

Platforms: Mac (Apple silicon), Windows and Linux / Steam Deck, all from this
repository. Supported discs: Europe `GEDP01` and USA `GEDE01`. The game plays
through with its music, voices, movies and effects; widescreen, higher
resolutions, HD texture packs, sharper baked shadows and remappable controls
are in the launcher's Options. The PS5 port is a separate repository.

## Build

All platforms start the same way:

```sh
tools/setup-deps.sh                           # pinned DolRecomp + GXRuntime, plus our patches
python3 tools/generate.py /path/to/disc.iso   # ISO or CISO; writes generated/
cp /path/to/disc.iso dump/GEDP01.iso          # or GEDE01.iso for the USA disc
```

- **Mac**: `cmake -S . -B build-mac -G Ninja && ninja -C build-mac EternalDarkness`,
  then `./play.sh`. `tools/mac/make-mac-app.sh` builds a self-contained
  `Eternal Darkness.app` (beta) that asks for your disc and builds the game
  from it on first start (`tools/mac/README.txt`).
- **Windows**: configure with clang-cl and Ninja (`build-win`), build
  `EternalDarkness`, copy the DLLs from the build tree next to the exe, then
  `play.bat`.
- **Linux / Steam Deck**: build with clang and Ninja (`build-linux`);
  `tools/deck/make-deck-package.sh` makes a folder that builds the game from
  your disc on first start (`tools/deck/README.txt`).

Keyboard: arrows = d-pad, J = A, K = B, U = X, I = Y, Q = Z, E/R = L/R,
Enter = Start. Controllers work through SDL.

`./play.sh` opens the launcher. The first time, pick the disc (images in
`dump/` or the project folder; this build runs the disc it was generated from);
after that it lives in Options and the menu leads with Play. Play offers
Continue (the newest save slot), New game, or any used slot, so a save loads
without going through the game first. In the game, Esc or the controller's Back/Select button pauses into a
menu with four save slots (`saves/`), the same options, and a way back to the
launcher. Menus work with the keyboard (arrows, Enter, Esc), a controller
(d-pad or stick, A, B) or the mouse.

Options (kept in `settings.ini`): skip intros, aspect ratio (4:3 or 16:9, the
game's own two; it sets the game's widescreen option), widescreen HUD, resolution (each
choice shows its size), fullscreen, on-screen FPS counter, baked shadows
(Sharp: each room's light maps are upscaled 4x as it loads, with CMPR's blocks
evened out and edge-directed doubling, so window light and shadows on the
walls and floors have clean edges; Original: as on the disc), music and
sound-effect volumes, audio buffer, and Controls: Original (GameCube
positions), Modern (attack RT/A, action B, aim LT, sprint and creep on the stick clicks
as toggles, LB/RB as the GameCube L/R for paging the game's menus, reload
X/Square, quickcast Y/Triangle) or
Custom (select an action, then press a button, trigger or key), Sprint, Creep
and Target as hold or toggle, stick deadzone and vibration. Bindings show
as Xbox, PlayStation or Switch button icons for the connected controller, and
the game's own prompts ("B Examine", the inventory's L/R) are redrawn with the
same icons for the button that does each action (keyboard keys with no
controller).

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
- `res/icon/`: the Eternal Darkness Recomp emblem (window and Dock icon,
  `AppIcon.icns` for the Mac app).
- `res/`: the launcher and in-game menu (RmlUi documents) and their fonts
  (Alegreya SC and Fira Sans, SIL Open Font License, licences alongside), and
  button icons from Kenney's Input Prompts (CC0).

## Licence

GPL-3.0-or-later (see `LICENSE`), as the GXRuntime and StrikersRecomp code it
builds on.

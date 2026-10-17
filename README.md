# Eternal Darkness Recomp

**Eternal Darkness: Sanity's Requiem** (GameCube) as a native game for Mac,
Windows and Linux / Steam Deck, built from **your own disc**. Nothing from the
game is in this repository. The PS5 version is
[PS5_EternalDarkness](https://github.com/griches/PS5_EternalDarkness).

## Quick start

You need your own disc image (`.iso` or `.gcm`): Europe `GEDP01` or USA `GEDE01`.

**Mac (Apple silicon, macOS 13+)**
1. Unzip `EternalDarkness-mac-....zip` and move **Eternal Darkness.app** to Applications.
2. The first time, right-click the app, choose **Open**, then **Open** again (it is not notarised).
3. Drop your disc image on its window. It builds the game from it (a minute or two), then starts.

**Steam Deck / Linux**
1. Copy the game folder somewhere (for example `~/Games/EternalDarkness`) and put your disc image in it.
2. Run `play.sh`. On a Deck: Desktop Mode, Steam > Add a Non-Steam Game > `play.sh`, then play it from Game Mode.
   The first start builds the game (about half a minute).

**Windows**: build it from source (below), then run `play.bat`.

**HD textures**
1. Get a Dolphin HD texture pack for Eternal Darkness (a folder of `tex1_....png` files).
2. Copy those files (any subfolders are fine) into a folder called `textures`:
   - Mac app: `~/Library/Application Support/Eternal Darkness/textures`
   - Everywhere else: `textures/` in the game folder, next to `res/`
3. Start the game again. Options > HD textures turns the pack on and off.

**Playing**
- The launcher: **Play** (Continue, New game or a save slot) and **Options**.
- In the game, **Esc** or the controller's **Back / Select / Create** button opens the pause
  menu: four save slots, Options, Quit.
- Keyboard: arrows = d-pad, J = A, K = B, U = X, I = Y, Q = Z, E/R = L/R, Enter = Start.
  Controllers work as they are.
- Your saves stay in the game folder (the Mac app's is `~/Library/Application Support/Eternal Darkness`):
  `saves/` (the pause menu's slots) and `ed-GEDP01-slot-a.dolcard` (the game's memory card;
  `...-slot-b.dolcard` is slot B).

**If something goes wrong**: when the game stops responding, stops or crashes it
writes a report to `saves/reports/` in the game folder (for a hang, with a `.state`
of that moment). Send those files with a few words on what you were doing.

## About

The game's PowerPC code is translated to C by
[DolRecomp](https://github.com/ExpansionPak/DolRecomp) and runs on
[GXRuntime](https://github.com/aharonahdoot/GXRuntime) with the Aurora renderer.
`generated/` (code translated from your disc) and `dump/` (your disc image)
stay on your machine. The game plays through with its music, voices, movies and
effects; widescreen, higher resolutions, HD texture packs, sharper baked shadows
and remappable controls are in the launcher's Options.

## Building from source

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

Memory cards: slot A is `ed-GEDP01-slot-a.dolcard` (the disc's game ID in the
name) in the folder the game runs from. A card beside it named
`...-slot-b.dolcard`, or one given with `--card-b PATH` or `ED_CARD_B=path`,
goes in slot B, and the game's Load Game and Save screens offer it there.
The console's own settings sit beside slot A as `...-sram.bin`: the game takes
its Audio Mode from them at start, as on a GameCube, and writes them when you
change it. Without that file a new game starts in Stereo.
`tools/gci.py` lists a card and moves saves in and out as GCI files (as
Dolphin exports them; MaxDrive `.sav` and GameShark `.gcs` import too).

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

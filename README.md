# Eternal Darkness Recomp

**Eternal Darkness: Sanity's Requiem** (GameCube, 2002) as a native game for the Mac,
Windows and Linux / Steam Deck, built from **your own disc**. Nothing from the game
is in this repository. The PlayStation 5 version is
[PS5_EternalDarkness](https://github.com/griches/PS5_EternalDarkness).
Version 0.8.0.

> **European disc only.** You need your own image (`.iso` or `.gcm`) of the
> **European GameCube disc, `GEDP01`**. The USA disc (`GEDE01`) is not supported:
> the launcher does not offer it and the installers turn it away.

## Install

- **Mac** (Apple silicon, macOS 13 or later)
  1. Download `EternalDarkness-mac-....zip` from the release, unzip it and move
     **Eternal Darkness.app** to Applications.
  2. The first time, right-click the app, choose **Open**, then **Open** again
     (it is not notarised).
  3. Drop your European disc image on its window. It translates the disc and
     builds the game from it (a minute or two), then starts. Later starts are
     instant.
- **Steam Deck / Linux**
  1. Build the game folder (`tools/deck/make-deck-package.sh`, see below), copy it
     somewhere such as `~/Games/EternalDarkness` and put your disc image in it.
  2. Run `play.sh`. On a Deck: Desktop Mode, Steam > Add a Non-Steam Game >
     `play.sh`, then play it from Game Mode. The first start builds the game
     (about half a minute).
- **Windows**: build it from source (below), then run `play.bat`.

Your saves and settings stay in the game's folder (the Mac app's is
`~/Library/Application Support/Eternal Darkness`).

## HD texture pack

1. Get a Dolphin HD texture pack for Eternal Darkness: a folder of `tex1_....png`
   or `tex1_....dds` files. DDS packs (BC7) stay compressed on the graphics card,
   a quarter of the memory of PNG ones.
2. Copy those files (any subfolders are fine) into a folder called `textures`:
   - Mac app: `~/Library/Application Support/Eternal Darkness/textures`
   - everywhere else: `textures/` in the game's folder, next to `res/`
3. Start the game again: it finds the pack when it starts. **Options > Game >
   HD textures** turns it on and off while you play.

Packs can sit side by side in `textures`. Where two have the same texture, a
folder with *supplement* or *override* in its name wins, so a fix-up pack such
as the PAL supplement (the European disc's screens and words the main pack
misses) goes beside the main pack as it is.

## What is better than on the GameCube

- **Sharp picture at any resolution**: drawn at your screen's own resolution, up
  to 4K, or a fixed 720p, 1080p, 1440p or 4K, with full colour (no 6-bit banding).
- **Widescreen**: the game's own 16:9 mode (more of the room at the sides, not
  stretched) with the HUD and prompts at the screen's edges; 4:3 too.
- **New baked shadows** (below): the rooms' light and shadow redrawn sharp.
- **Crisp game text**: the game's lettering redrawn in two styles, *Sharp* or
  *Tome* (old typefaces), or *Original* as on the disc.
- **HD texture packs** in Dolphin's format.
- **Modern controls**, remapping, and button prompts that match your controller
  (Xbox, PlayStation or Switch icons), the game's own prompts included.
- **Save anywhere**: the pause menu has four save slots that hold the whole moment,
  besides the game's own memory-card saves (cards in slots A and B both work).
- **A launcher**: Continue, New game or any save slot straight from the start,
  and Options for everything here.
- **Problem reports**: if the game stops responding, stops or crashes, it says
  so on screen and writes a report to `saves/reports/` to send.

## Controls

A controller works as it is; the keyboard works too. Two layouts and your own:

| Action | Modern (default) | Original (GameCube positions) | Keyboard |
| --- | --- | --- | --- |
| Move | Left stick | Left stick | W A S D |
| Attack / Confirm | RT or A | A | J |
| Action / Cancel | B | B | K |
| Target / Aim | LT or RB | RT | R |
| Sprint | Left stick click (toggle), or hold LB | LT | E |
| Creep | Right stick click (toggle) | X | U |
| Reload | X | RB | Q |
| Quickcast | Y | Y | I |
| Spells | D-pad | D-pad | Arrow keys |
| Game's pause menu | Start / Menu | Start | Enter |
| This port's menu (saves, options) | Back / View / Select / Create | same | Esc |

In the game's own menus LB and RB page left and right (the GameCube's L and R).
**Options > Controls** switches layout, makes Sprint, Creep and Target hold or
toggle, sets the stick deadzone and vibration, and **Button mapping** rebinds any
action to a button, trigger or key (that layout is *Custom*).

## The new baked shadows

Eternal Darkness lights its rooms with pre-drawn light maps: the window light on
the floors, the shadows under the furniture, the glow of the lamps. On the disc
they are small and compressed, so at today's resolutions their edges break up
into blocks and smears. With **Options > Game > Baked shadows: Sharp** (the
default) each room's light maps are rebuilt four times larger as the room loads:
the compression blocks are evened out and edges are doubled along their own
direction, so light and shadow keep clean edges; the small maps on the walls are
smoothed instead. **Original** shows them as on the disc. The change shows as soon
as you switch.

## Playing

- The launcher's **Play** offers Continue (your newest save slot), New game or
  any slot. **Options** holds the display, sound, controls and game settings.
- In the game, the menu button (above) or **Esc** opens this port's pause menu:
  four save slots, Options, Return to title screen, Return to launcher, Quit.
- Saves: `saves/` (the pause menu's slots) and `ed-GEDP01-slot-a.dolcard` (the
  game's memory card; a `...-slot-b.dolcard` beside it is slot B).
- If something goes wrong, send the files in `saves/reports/` with a few words on
  what you were doing.

## How it works

The game's PowerPC code is translated to C by
[DolRecomp](https://github.com/ExpansionPak/DolRecomp) and compiled for your
machine; it runs on [GXRuntime](https://github.com/aharonahdoot/GXRuntime), which
supplies the GameCube's CPU state, devices and SDK services, and draws through its
gxcore renderer (the GameCube's GX graphics, after Dolphin's) on Aurora and Dawn
(Metal, Direct3D 12 or Vulkan). `generated/` (code translated from your disc) and
`dump/` (your disc image) stay on your machine.

## Building from source

All platforms start the same way:

```sh
tools/setup-deps.sh                           # pinned DolRecomp + GXRuntime, plus our patches
python3 tools/generate.py /path/to/disc.iso   # ISO or CISO; writes generated/
cp /path/to/disc.iso dump/GEDP01.iso          # the European disc
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

`./play.sh` opens the launcher. The first time, pick the disc (images in
`dump/` or the project folder; this build runs the disc it was generated from);
after that it lives in Options and the menu leads with Play. Play offers
Continue (the newest save slot), New game, or any used slot, so a save loads
without going through the game first. In the game, Esc or the controller's Back/Select button pauses into a
menu with four save slots (`saves/`), the same options, and a way back to the
launcher. Menus work with the keyboard (arrows, Enter, Esc), a controller
(d-pad or stick, A, B) or the mouse.

Options (kept in `settings.ini`): aspect ratio (4:3 or 16:9, the
game's own two; it sets the game's widescreen option), widescreen HUD, resolution (each
choice shows its size), fullscreen, on-screen FPS counter (off by default), baked shadows
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
builds on. `THIRD_PARTY.md` lists everything else a build holds and its
licence; every package carries their texts in `licenses/`.

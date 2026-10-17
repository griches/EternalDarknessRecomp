# Licences

Eternal Darkness Recomp is GPL-3.0-or-later (`LICENSE`). A build of it holds
the code and data below. Every package (the Mac app, the Linux / Steam Deck folder,
the Windows folder, the PS5 title) carries their licence texts in a `licenses/` folder, made by
`tools/collect-licenses.sh`.

No part of the game is in this repository. The Mac app and the Linux / Steam
Deck folder hold none of it either: they translate the game's code from the
player's own disc on their machine. The Windows folder and the PS5 title come
ready-made, with the game's code already translated; the game's graphics,
sound and music still come from the player's disc.

| Component | Licence | What it is here |
| --- | --- | --- |
| Eternal Darkness Recomp | GPL-3.0-or-later | This project: the host layer, launcher and tools |
| [DolRecomp](https://github.com/ExpansionPak/DolRecomp) | GPL-3.0-or-later | Translates the disc's PowerPC code to C |
| [GXRuntime](https://github.com/aharonahdoot/GXRuntime) | GPL-3.0-or-later | CPU, devices and the gxcore renderer (Dolphin's behaviour as its reference) |
| [StrikersRecomp](https://github.com/aharonahdoot/StrikersRecomp) | GPL-3.0-or-later | The host layer in `runtime/host/` started from its host |
| [Aurora](https://github.com/encounter/aurora) | MIT | Window, input and rendering layer (vendored in GXRuntime) |
| [Dawn](https://dawn.googlesource.com/dawn) | BSD-3-Clause | WebGPU implementation the renderer draws with (`licenses/Dawn.txt`) |
| [SDL 3](https://libsdl.org) | Zlib | Window, input, audio |
| [RmlUi](https://github.com/mikke89/RmlUi) | MIT | Launcher and in-game menus |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | FPS counter, debug overlays |
| [Abseil](https://abseil.io) | Apache-2.0 | Containers used by the renderer |
| [xxHash](https://github.com/Cyan4973/xxHash) | BSD-2-Clause | Texture and pipeline hashes |
| [{fmt}](https://github.com/fmtlib/fmt) | MIT | Text formatting |
| [Tracy](https://github.com/wolfpld/tracy) | BSD-3-Clause | Profiler hooks (off in normal builds) |
| [Zstandard](https://github.com/facebook/zstd) | BSD-3-Clause | Compression |
| [FreeType](https://freetype.org) | FreeType Licence (FTL) | Font rendering (where the build links its own) |
| [libpng](http://www.libpng.org/pub/png/libpng.html) | PNG Reference Library Licence v2 | HD texture packs (where the build links its own) |
| [SQLite](https://sqlite.org) | Public domain | Shader and pipeline caches (`licenses/SQLite.txt`) |
| [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) | NCSA / MIT; Microsoft's licence for `dxil.dll` | Shader compiler Dawn uses for Direct3D 12 (the Windows folder only; `tools/win/licenses/`) |
| Microsoft Visual C++ runtime | Visual Studio redistributable code | `msvcp140.dll`, `vcruntime140.dll` and friends beside the Windows game |
| [Zig](https://ziglang.org) | MIT | The compiler the Mac app and Deck folder build the game with (those packages only) |
| [Fira Sans](https://github.com/mozilla/Fira), [Alegreya SC](https://github.com/huertatipografica/Alegreya-SC), [IM Fell English](https://iginomarini.com/fell/) | SIL Open Font Licence 1.1 | Menu fonts (`res/fonts/*-OFL.txt`) |
| [Kenney Input Prompts](https://kenney.nl/assets/input-prompts) | CC0 1.0 | Controller and key icons |
| [unending-occlusion](https://github.com/PattyTrish/unending-occlusion) | CC0 1.0 | Function names (`config/GEDE01/symbols.txt`, `licenses/unending-occlusion.txt`) |

The PS5 title also holds the PS5 Vulkan stack (Mesa's RADV driver, MIT) from
[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan); its port repository
lists those.

The GPL components mean anyone who passes a build on must also offer its
source: this repository, DolRecomp and GXRuntime at the commits in
`tools/setup-deps.sh`, and the patches in `patches/`.

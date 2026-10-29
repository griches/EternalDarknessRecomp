#pragma once
// The recomp's own insanity effects (insanity.cpp): fake system screens of
// the machine the game runs on (a Mac's spinning wait cursor, a crash box...)
// and our menu's save states wiped. The game's own roll for an effect arms
// them (hle.c: some rolls come here instead of to the game's effects), and
// when one is over the game's own "This isn't happening!" follows.
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// hle.c, as the game rolls an insanity effect (sanity level `level`, 0 sane):
// true when one of ours takes the roll (the game then has none).
bool insanity_take_roll(int level);

// Once a game frame (frontend_frame): an armed effect's overlay, or the whole
// of a modal one (the game held while it plays, as the menu holds it).
void insanity_frame(void);

// Before ImGui's first frame (frontend_imgui_init): the system's own faces.
void insanity_imgui_init(void);

// The machine the game runs on, as the effects know it: "mac", "windows",
// "linux", "deck" or "ps5" (ED_INSANITY_PLATFORM shows another's).
const char* insanity_platform_name(void);

#ifdef __cplusplus
}

namespace insanity {

// The debug menu's list: ours for this machine (or ED_INSANITY_PLATFORM's).
int count();
const char* name(int effect);
const char* note(int effect);
void play(int effect);  // now, or the next time our menu opens for one shown there

// Our in-game menu: menu_frame once per menu frame (true when the menu's rows
// should be built again); slots_hidden while the save slots show as wiped;
// menu_closed as it closes.
bool menu_frame(bool in_game_menu);
bool slots_hidden();
void menu_closed();

}  // namespace insanity
#endif

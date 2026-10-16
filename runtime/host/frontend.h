#pragma once
// Launcher and in-game menu (RmlUi over the game, windowed build): choose the
// disc and options before boot; Esc or the controller's Back/Select button
// pauses the game into a menu with save slots and the same options.
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Remember the command line, for "return to launcher" (restarts the process).
void frontend_init(int argc, char** argv);

// Before boot: run the launcher until the player picks Play (true, the chosen
// disc image in iso_out) or quits (false).
bool frontend_run_launcher(const char* default_iso, char* iso_out,
                           size_t iso_cap);

// Once a game frame: open the in-game menu when asked for, and run it with
// the game paused until the player resumes.
void frontend_frame(void);

// The disc id this build runs ("GEDE01" or "GEDP01"; main.c).
const char* ed_game_id(void);

// For AuroraBackendConfig.imgui_init: the FPS counter's font into ImGui.
void frontend_imgui_init(void);

#ifdef __cplusplus
}
#include <string>
// The res folder (fonts, menus, icons).
std::string frontend_res_dir();
#endif

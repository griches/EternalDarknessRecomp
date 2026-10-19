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

// A problem page over the game (report.c): heading, the text, and Keep
// waiting (when can_wait) or Quit. True when the player chose to wait.
bool frontend_problem(const char* heading, const char* text, bool can_wait);

// The disc id this build runs ("GEDE01" or "GEDP01"; main.c).
const char* ed_game_id(void);

// For AuroraBackendConfig.imgui_init: the FPS counter's font into ImGui.
void frontend_imgui_init(void);

// Graphics enhancements (Options > Graphics enhancements), cycled in the game
// by holding R3 and pressing a button: 0 glow, 1 ambient occlusion, 2 HDR,
// 3 baked shadows, 4 HD textures; 5 all off (and the GameCube's own 528
// lines) and 6 all on (the defaults: HDR, Soft occlusion, sharp shadows, HD
// textures, no glow, the chosen resolution: Gary's before/after). Shows what
// changed on screen.
void frontend_cycle_enhancement(int which);

#ifdef __cplusplus
}
#include <string>
// The res folder (fonts, menus, icons).
std::string frontend_res_dir();
#endif

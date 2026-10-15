#pragma once
// Player settings shown in the launcher and the in-game menu, kept in
// settings.ini (ED_SETTINGS=path to use another file).
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool skip_intros;     // tap through the logo movies and the quote
    int aspect;           // 0 follow the game, 1 4:3, 2 16:9, 3 stretch
    bool widescreen_hud;  // move the game's widescreen HUD out to the edges
    int render_scale;     // 0 window pixels, 1-4 x the GameCube resolution
    bool fullscreen;
    bool fps_counter;     // frames per second in the window title
    int music_volume;     // disc-streamed music, 0-100
    int effects_volume;   // the game's own mix (effects, voices), 0-100
    int audio_buffer_ms;  // audio queued ahead of the output
    int controls_layout;  // 0 original, 1 modern, 2 custom
    bool sprint_toggle;   // press to start/stop sprinting (stops when you stop)
    bool creep_toggle;
    bool target_toggle;
    int stick_deadzone;   // percent of the stick's travel ignored, 5-25
    bool vibration;
    char controls_custom[512];  // custom bindings (controls.cpp format)
    char disc[1024];      // last disc image chosen in the launcher
} Settings;

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
// Push the current values to the renderer, window and audio (safe to call
// again after any change).
void settings_apply(void);

#ifdef __cplusplus
}
#endif

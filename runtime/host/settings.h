#pragma once
// Player settings shown in the launcher and the in-game menu, kept in
// settings.ini (ED_SETTINGS=path to use another file).
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int aspect;           // 1 4:3, 2 16:9 (the game's own widescreen view), 4 21:9 (that view
                          // widened); settings.ini's 0 (follow the game) and 3 (the old
                          // stretch) read as 16:9
    int render_scale;     // 0 window pixels, 1-4 = 720p, 1080p, 1440p, 4K
    int frame_rate;       // 60 the game's own; above it, frames in between at the screen's rate
    int game_font;        // the game's text: 0 sharp, 1 tome (default), 2 as on the disc
    bool hd_textures;     // use the texture pack in textures/ when there is one
    bool sharp_shadows;   // upscale the rooms' light maps as they load (lightmaps.cpp)
    int glow;             // light glows round lamps, flames, magick: 0 off, 1 soft, 2 strong
    bool hdr;             // HDR output where the display has it (light past SDR white)
    int ssao;             // ambient occlusion: 0 off, 1 soft, 2 strong
    // HDR calibration (Graphics enhancements > Calibrate HDR): the display's
    // peak and SDR white in nits, and the black level in steps (-5 sinks,
    // up to 10 lifts the darkest light).
    int hdr_peak;
    int hdr_paper;
    int hdr_black;
    bool fullscreen;
    bool fps_counter;     // frames per second in the window title
    int music_volume;     // disc-streamed music, 0-100
    int effects_volume;   // the game's own mix (effects, voices), 0-100
    int audio_buffer_ms;  // audio queued ahead of the output: 60 ms, not a
                          // setting (the renderer adds reserve if it runs dry)
    int controls_layout;  // 0 original, 1 modern, 2 custom
    // Hold or toggle, per controls layout (original, modern, custom).
    bool sprint_toggle[3];  // press to start/stop sprinting (stops when you stop)
    bool creep_toggle[3];
    bool target_toggle[3];
    int stick_deadzone[3];  // percent of the stick's travel ignored, per layout
    bool vibration;
    int camera;           // 0 the game's own, 2 third person, 3 first person (camera.h)
    char controls_custom[512];  // custom bindings (controls.cpp format)
    char disc[1024];      // last disc image chosen in the launcher
} Settings;

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
// Push the current values to the renderer, window and audio (safe to call
// again after any change).
void settings_apply(void);
// Glow, ambient occlusion and HDR only: safe from the game thread (the
// in-game shortcuts); baked shadows and HD textures are read where used.
void settings_apply_graphics(void);

#ifdef __cplusplus
}
#endif

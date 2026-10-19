// settings.ini: one `key = value` per line, unknown keys ignored, missing
// keys keep their defaults.
#include "host/settings.h"

#ifdef ED_AURORA
#include <aurora/gfx.h>
#include "gxruntime/aurora_backend.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Settings g_settings = {
    .aspect = 2,  // 16:9
    .widescreen_hud = true,
    .render_scale = 0,
    .frame_rate = 60,
    .game_font = 1,  // Tome
    .hd_textures = true,
    .sharp_shadows = true,
    .glow = 0,
    .hdr = true,  // only where the display has it
    .ssao = 1,
    .hdr_peak = 1000,
#if defined(__APPLE__)
    .hdr_paper = 100,  // macOS's own SDR white (EDR 1.0)
#else
    .hdr_paper = 140,  // the PS5's own SDR picture
#endif
    .hdr_black = 0,
    .fullscreen = true,
    .fps_counter = false,
    .music_volume = 100,
    .effects_volume = 100,
    .audio_buffer_ms = 60,
    .controls_layout = 1,  // Modern
    // Modern: sprint (left stick click) and creep (right stick click) toggle.
    .sprint_toggle = {false, true, false},
    .creep_toggle = {false, true, false},
    .target_toggle = {false, false, false},
    .stick_deadzone = {5, 5, 5},
    .vibration = true,
    .camera = 0,
    .controls_custom = "",
    .disc = "",
};

static const char* settings_path(void) {
    const char* path = getenv("ED_SETTINGS");
    return path != NULL && path[0] != '\0' ? path : "settings.ini";
}

static int clamp_int(int value, int lo, int hi) {
    return value < lo ? lo : value > hi ? hi : value;
}

static char* trim(char* s) {
    while (*s == ' ' || *s == '\t')
        s++;
    char* end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
                       end[-1] == '\r'))
        *--end = '\0';
    return s;
}

static const char* const kLayoutKeys[3] = {"original", "modern", "custom"};

// "sprint_toggle_modern = 1" into sprint_toggle[1].
static bool parse_toggle(const char* key, const char* prefix, bool values[3], int n) {
    const size_t len = strlen(prefix);
    if (strncmp(key, prefix, len) != 0)
        return false;
    for (int i = 0; i < 3; i++)
        if (strcmp(key + len, kLayoutKeys[i]) == 0) {
            values[i] = n != 0;
            return true;
        }
    return false;
}

void settings_load(void) {
    FILE* f = fopen(settings_path(), "r");
    if (f == NULL)
        return;
    char line[1200];
    while (fgets(line, sizeof line, f) != NULL) {
        char* eq = strchr(line, '=');
        if (line[0] == '#' || eq == NULL)
            continue;
        *eq = '\0';
        const char* key = trim(line);
        const char* value = trim(eq + 1);
        const int n = atoi(value);
        if (strcmp(key, "aspect") == 0)
            g_settings.aspect = n == 1 ? 1 : 2;
        else if (strcmp(key, "widescreen_hud") == 0)
            g_settings.widescreen_hud = n != 0;
        else if (strcmp(key, "render_scale") == 0)
            g_settings.render_scale = clamp_int(n, 0, 4);
        else if (strcmp(key, "frame_rate") == 0)
            g_settings.frame_rate = clamp_int(n, 60, 360);
        else if (strcmp(key, "game_font") == 0)
            g_settings.game_font = clamp_int(n, 0, 2);
        else if (strcmp(key, "hd_textures") == 0)
            g_settings.hd_textures = n != 0;
        else if (strcmp(key, "sharp_shadows") == 0)
            g_settings.sharp_shadows = n != 0;
        else if (strcmp(key, "glow") == 0)
            g_settings.glow = clamp_int(n, 0, 2);
        else if (strcmp(key, "hdr") == 0)
            g_settings.hdr = n != 0;
        else if (strcmp(key, "ssao") == 0)
            g_settings.ssao = clamp_int(n, 0, 2);
        else if (strcmp(key, "hdr_peak") == 0)
            g_settings.hdr_peak = clamp_int(n, 100, 4000);
        else if (strcmp(key, "hdr_paper") == 0)
            g_settings.hdr_paper = clamp_int(n, 60, 400);
        else if (strcmp(key, "hdr_black") == 0)
            g_settings.hdr_black = clamp_int(n, -5, 10);
        else if (strcmp(key, "fullscreen") == 0)
            g_settings.fullscreen = n != 0;
        else if (strcmp(key, "fps_counter") == 0)
            g_settings.fps_counter = n != 0;
        else if (strcmp(key, "music_volume") == 0)
            g_settings.music_volume = clamp_int(n, 0, 100);
        else if (strcmp(key, "effects_volume") == 0)
            g_settings.effects_volume = clamp_int(n, 0, 100);
        else if (strcmp(key, "controls_layout") == 0)
            g_settings.controls_layout = clamp_int(n, 0, 2);
        else if (parse_toggle(key, "sprint_toggle_", g_settings.sprint_toggle, n) ||
                 parse_toggle(key, "creep_toggle_", g_settings.creep_toggle, n) ||
                 parse_toggle(key, "target_toggle_", g_settings.target_toggle, n))
            ;
        else if (strncmp(key, "stick_deadzone_", 15) == 0) {
            for (int i = 0; i < 3; i++)
                if (strcmp(key + 15, kLayoutKeys[i]) == 0)
                    g_settings.stick_deadzone[i] = clamp_int(n, 5, 25);
        }
        else if (strcmp(key, "vibration") == 0)
            g_settings.vibration = n != 0;
        else if (strcmp(key, "camera") == 0)
            g_settings.camera = n == 1 ? 0 : clamp_int(n, 0, 3);  // 1 was a free look
        else if (strcmp(key, "controls_custom") == 0)
            snprintf(g_settings.controls_custom, sizeof g_settings.controls_custom, "%s", value);
        else if (strcmp(key, "disc") == 0)
            snprintf(g_settings.disc, sizeof g_settings.disc, "%s", value);
    }
    fclose(f);
}

void settings_save(void) {
    FILE* f = fopen(settings_path(), "w");
    if (f == NULL) {
        fprintf(stderr, "[settings] cannot write %s\n", settings_path());
        return;
    }
    fprintf(f, "# Eternal Darkness settings (written by the launcher)\n");
    fprintf(f, "aspect = %d\n", g_settings.aspect);
    fprintf(f, "widescreen_hud = %d\n", g_settings.widescreen_hud ? 1 : 0);
    fprintf(f, "render_scale = %d\n", g_settings.render_scale);
    fprintf(f, "frame_rate = %d\n", g_settings.frame_rate);
    fprintf(f, "game_font = %d\n", g_settings.game_font);
    fprintf(f, "hd_textures = %d\n", g_settings.hd_textures ? 1 : 0);
    fprintf(f, "sharp_shadows = %d\n", g_settings.sharp_shadows ? 1 : 0);
    fprintf(f, "glow = %d\n", g_settings.glow);
    fprintf(f, "hdr = %d\n", g_settings.hdr ? 1 : 0);
    fprintf(f, "ssao = %d\n", g_settings.ssao);
    fprintf(f, "hdr_peak = %d\n", g_settings.hdr_peak);
    fprintf(f, "hdr_paper = %d\n", g_settings.hdr_paper);
    fprintf(f, "hdr_black = %d\n", g_settings.hdr_black);
    fprintf(f, "fullscreen = %d\n", g_settings.fullscreen ? 1 : 0);
    fprintf(f, "fps_counter = %d\n", g_settings.fps_counter ? 1 : 0);
    fprintf(f, "music_volume = %d\n", g_settings.music_volume);
    fprintf(f, "effects_volume = %d\n", g_settings.effects_volume);
    fprintf(f, "controls_layout = %d\n", g_settings.controls_layout);
    for (int i = 0; i < 3; i++) {
        fprintf(f, "sprint_toggle_%s = %d\n", kLayoutKeys[i], g_settings.sprint_toggle[i] ? 1 : 0);
        fprintf(f, "creep_toggle_%s = %d\n", kLayoutKeys[i], g_settings.creep_toggle[i] ? 1 : 0);
        fprintf(f, "target_toggle_%s = %d\n", kLayoutKeys[i], g_settings.target_toggle[i] ? 1 : 0);
    }
    for (int i = 0; i < 3; i++)
        fprintf(f, "stick_deadzone_%s = %d\n", kLayoutKeys[i], g_settings.stick_deadzone[i]);
    fprintf(f, "vibration = %d\n", g_settings.vibration ? 1 : 0);
    fprintf(f, "camera = %d\n", g_settings.camera);
    fprintf(f, "controls_custom = %s\n", g_settings.controls_custom);
    fprintf(f, "disc = %s\n", g_settings.disc);
    fclose(f);
}

void settings_apply(void) {
#ifdef ED_AURORA
    dol_aurora_set_aspect_mode(g_settings.aspect);
    dol_aurora_set_widescreen_hud(g_settings.widescreen_hud);
    static const int kRenderLines[] = {0, 720, 1080, 1440, 2160};
    dol_aurora_set_render_height(kRenderLines[g_settings.render_scale]);
    dol_aurora_set_fullscreen(g_settings.fullscreen);
    dol_aurora_set_frame_rate(g_settings.frame_rate);
    dol_aurora_set_audio_buffer_ms(g_settings.audio_buffer_ms);
    dol_aurora_set_vibration(g_settings.vibration);
#endif
    settings_apply_graphics();
}

void settings_apply_graphics(void) {
#ifdef ED_AURORA
    static const float kGlow[] = {0.f, 0.6f, 1.0f};
    dol_aurora_set_glow(kGlow[g_settings.glow]);
    /* The living energy in the temples (the red doorways, the yellow floors):
     * one 256x256 C8 texture the game redraws every frame. With HDR on it
     * glows, at Soft when the glow setting is off. */
    aurora_glow_add_texture(0x010D6080u, 9u, 256u, 256u, kGlow[1]);
    /* The streaks of a magick effect (green and white), round a struck
     * enemy and the player. */
    aurora_glow_add_texture(0x012C4DA0u, 14u, 64u, 64u, kGlow[1]);
    aurora_glow_add_texture(0x012C5DA0u, 14u, 64u, 64u, kGlow[1]);
    dol_aurora_set_hdr(g_settings.hdr);
    static const float kSsao[] = {0.f, 1.3f, 2.4f};
    dol_aurora_set_ssao(kSsao[g_settings.ssao]);
    dol_aurora_set_hdr_levels((float)g_settings.hdr_paper, (float)g_settings.hdr_peak,
                              (float)g_settings.hdr_black * 0.002f);
    // Highlights up to 3.5x SDR white, 4x with ambient occlusion (which
    // darkens the room round them). The PS5 (HDR10, SDR white at 140 nits to
    // match the console's own SDR picture) goes to 5x and 6x: about 700 and
    // 850 nits, a TV's peak, where 3.5x at 200 nits only looked lighter.
#if defined(__PROSPERO__)
    dol_aurora_set_hdr_highlight(g_settings.ssao != 0 ? 5.0f : 4.0f);
#else
    dol_aurora_set_hdr_highlight(g_settings.ssao != 0 ? 3.0f : 2.5f);
#endif
#endif
}

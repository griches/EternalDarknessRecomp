// settings.ini: one `key = value` per line, unknown keys ignored, missing
// keys keep their defaults.
#include "host/settings.h"

#ifdef ED_AURORA
#include "gxruntime/aurora_backend.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Settings g_settings = {
    .skip_intros = false,
    .aspect = 0,
    .widescreen_hud = true,
    .render_scale = 0,
    .fullscreen = false,
    .fps_counter = true,
    .music_volume = 100,
    .effects_volume = 100,
    .audio_buffer_ms = 40,
    .controls_layout = 0,
    // Modern: sprint (left stick click) and creep (right stick click) toggle.
    .sprint_toggle = {false, true, false},
    .creep_toggle = {false, true, false},
    .target_toggle = {false, false, false},
    .stick_deadzone = 20,
    .vibration = true,
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
        if (strcmp(key, "skip_intros") == 0)
            g_settings.skip_intros = n != 0;
        else if (strcmp(key, "aspect") == 0)
            g_settings.aspect = clamp_int(n, 0, 3);
        else if (strcmp(key, "widescreen_hud") == 0)
            g_settings.widescreen_hud = n != 0;
        else if (strcmp(key, "render_scale") == 0)
            g_settings.render_scale = clamp_int(n, 0, 4);
        else if (strcmp(key, "fullscreen") == 0)
            g_settings.fullscreen = n != 0;
        else if (strcmp(key, "fps_counter") == 0)
            g_settings.fps_counter = n != 0;
        else if (strcmp(key, "music_volume") == 0)
            g_settings.music_volume = clamp_int(n, 0, 100);
        else if (strcmp(key, "effects_volume") == 0)
            g_settings.effects_volume = clamp_int(n, 0, 100);
        else if (strcmp(key, "audio_buffer_ms") == 0)
            g_settings.audio_buffer_ms = clamp_int(n, 20, 500);
        else if (strcmp(key, "controls_layout") == 0)
            g_settings.controls_layout = clamp_int(n, 0, 2);
        else if (parse_toggle(key, "sprint_toggle_", g_settings.sprint_toggle, n) ||
                 parse_toggle(key, "creep_toggle_", g_settings.creep_toggle, n) ||
                 parse_toggle(key, "target_toggle_", g_settings.target_toggle, n))
            ;
        else if (strcmp(key, "stick_deadzone") == 0)
            g_settings.stick_deadzone = clamp_int(n, 5, 25);
        else if (strcmp(key, "vibration") == 0)
            g_settings.vibration = n != 0;
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
    fprintf(f, "skip_intros = %d\n", g_settings.skip_intros ? 1 : 0);
    fprintf(f, "aspect = %d\n", g_settings.aspect);
    fprintf(f, "widescreen_hud = %d\n", g_settings.widescreen_hud ? 1 : 0);
    fprintf(f, "render_scale = %d\n", g_settings.render_scale);
    fprintf(f, "fullscreen = %d\n", g_settings.fullscreen ? 1 : 0);
    fprintf(f, "fps_counter = %d\n", g_settings.fps_counter ? 1 : 0);
    fprintf(f, "music_volume = %d\n", g_settings.music_volume);
    fprintf(f, "effects_volume = %d\n", g_settings.effects_volume);
    fprintf(f, "audio_buffer_ms = %d\n", g_settings.audio_buffer_ms);
    fprintf(f, "controls_layout = %d\n", g_settings.controls_layout);
    for (int i = 0; i < 3; i++) {
        fprintf(f, "sprint_toggle_%s = %d\n", kLayoutKeys[i], g_settings.sprint_toggle[i] ? 1 : 0);
        fprintf(f, "creep_toggle_%s = %d\n", kLayoutKeys[i], g_settings.creep_toggle[i] ? 1 : 0);
        fprintf(f, "target_toggle_%s = %d\n", kLayoutKeys[i], g_settings.target_toggle[i] ? 1 : 0);
    }
    fprintf(f, "stick_deadzone = %d\n", g_settings.stick_deadzone);
    fprintf(f, "vibration = %d\n", g_settings.vibration ? 1 : 0);
    fprintf(f, "controls_custom = %s\n", g_settings.controls_custom);
    fprintf(f, "disc = %s\n", g_settings.disc);
    fclose(f);
}

void settings_apply(void) {
#ifdef ED_AURORA
    dol_aurora_set_aspect_mode(g_settings.aspect);
    dol_aurora_set_widescreen_hud(g_settings.widescreen_hud);
    dol_aurora_set_render_scale(g_settings.render_scale);
    dol_aurora_set_fullscreen(g_settings.fullscreen);
    dol_aurora_set_audio_buffer_ms(g_settings.audio_buffer_ms);
    dol_aurora_set_vibration(g_settings.vibration);
#endif
}

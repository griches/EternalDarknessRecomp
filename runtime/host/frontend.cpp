// Launcher and in-game menu. One RmlUi document (res/ui/menu.rml) whose rows
// are rebuilt from C++ for each screen; selection, value changes and
// navigation are handled here rather than by RmlUi's focus, so the keyboard,
// a controller and the mouse all drive the same rows.
#include "host/frontend.h"

#include "gxruntime/aurora_backend.h"
#include "host/buttons.h"
#include "host/controls.h"
#include "host/report.h"
#include "host/settings.h"
#include "host/texpack.h"

#include <aurora/rmlui.hpp>
#include <imgui.h>

#include <RmlUi/Core.h>
#include <RmlUi_Platform_SDL.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include "host/compat.h"

#ifdef _WIN32
#include <process.h>
#endif

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <vector>

extern "C" {
void quickstate_request_save(const char* path);
void quickstate_request_load(const char* path);
const char* quickstate_boot_path(void);
void hle_set_skip_intro(bool on);
}

namespace {

// --- Input ------------------------------------------------------------------

enum Button { kUp, kDown, kLeft, kRight, kAccept, kBack, kMenu, kButtonCount };

SDL_Gamepad* g_pad = nullptr;

SDL_Gamepad* gamepad() {
    if (g_pad != nullptr && SDL_GamepadConnected(g_pad))
        return g_pad;
    g_pad = nullptr;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids != nullptr && count > 0)
        g_pad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
    return g_pad;
}

bool key(int scancode) { return dol_aurora_key_down(scancode); }

void read_buttons(bool held[kButtonCount]) {
    SDL_Gamepad* pad = gamepad();
    auto btn = [&](SDL_GamepadButton b) {
        return pad != nullptr && SDL_GetGamepadButton(pad, b);
    };
    const float lx = pad != nullptr ? SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.f : 0.f;
    const float ly = pad != nullptr ? SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.f : 0.f;
    // Controller only: the keyboard arrives as key events (below), which also
    // carry the system's key repeat.
    held[kUp] = btn(SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -0.5f;
    held[kDown] = btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > 0.5f;
    held[kLeft] = btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -0.5f;
    held[kRight] = btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > 0.5f;
    held[kAccept] = btn(SDL_GAMEPAD_BUTTON_SOUTH);
    held[kBack] = btn(SDL_GAMEPAD_BUTTON_EAST);
    // The GameCube pad has no Back/Select, so the game never sees it.
    held[kMenu] = btn(SDL_GAMEPAD_BUTTON_BACK);
}

// Keyboard presses since the last poll, from RmlUi key events.
bool g_key_pressed[kButtonCount] = {};

class KeyListener final : public Rml::EventListener {
public:
    void ProcessEvent(Rml::Event& event) override {
        using namespace Rml::Input;
        if (controls::capturing()) {
            // Rebinding: find the SDL scancode for this key.
            const int id = event.GetParameter<int>("key_identifier", 0);
            for (int sc = SDL_SCANCODE_A; sc < SDL_SCANCODE_COUNT; sc++) {
                const SDL_Keycode code = SDL_GetKeyFromScancode(
                    static_cast<SDL_Scancode>(sc), SDL_KMOD_NONE, false);
                if (code != SDLK_UNKNOWN && RmlSDL::ConvertKey(static_cast<int>(code)) == id) {
                    controls::capture_key(sc);
                    break;
                }
            }
            return;
        }
        static const bool log = std::getenv("ED_FRONTEND_LOG") != nullptr;
        if (log)
            std::fprintf(stderr, "[frontend] key %d phase=%d target=%s current=%s t=%llu\n",
                         event.GetParameter<int>("key_identifier", 0),
                         static_cast<int>(event.GetPhase()),
                         event.GetTargetElement() ? event.GetTargetElement()->GetTagName().c_str() : "-",
                         event.GetCurrentElement() ? event.GetCurrentElement()->GetTagName().c_str() : "-",
                         static_cast<unsigned long long>(SDL_GetTicks()));
        switch (event.GetParameter<int>("key_identifier", 0)) {
        case KI_UP: case KI_W: g_key_pressed[kUp] = true; break;
        case KI_DOWN: case KI_S: g_key_pressed[kDown] = true; break;
        case KI_LEFT: case KI_A: g_key_pressed[kLeft] = true; break;
        case KI_RIGHT: case KI_D: g_key_pressed[kRight] = true; break;
        case KI_RETURN: case KI_NUMPADENTER: case KI_SPACE: case KI_J:
            g_key_pressed[kAccept] = true;
            break;
        case KI_ESCAPE:
            g_key_pressed[kBack] = true;
            g_key_pressed[kMenu] = true;
            break;
        case KI_BACK: case KI_K: g_key_pressed[kBack] = true; break;
        default: break;
        }
    }
};
KeyListener g_key_listener;

// Presses this frame, with auto-repeat for the direction buttons.
struct InputState {
    bool held[kButtonCount] = {};
    Uint64 since[kButtonCount] = {};
    Uint64 repeat[kButtonCount] = {};

    void poll(bool pressed[kButtonCount]) {
        bool now[kButtonCount];
        read_buttons(now);
        const Uint64 t = SDL_GetTicks();
        for (int b = 0; b < kButtonCount; b++) {
            pressed[b] = false;
            if (now[b] && !held[b]) {
                pressed[b] = true;
                since[b] = t;
                repeat[b] = t + 380;
            } else if (now[b] && b <= kRight && t >= repeat[b]) {
                pressed[b] = true;
                repeat[b] = t + 90;
            }
            held[b] = now[b];
            if (g_key_pressed[b])
                pressed[b] = true;
            g_key_pressed[b] = false;
        }
    }

    bool any_held() const {
        for (bool h : held)
            if (h)
                return true;
        // Keys the game also reads (Enter is Start, J/K are A/B).
        for (int k : {SDL_SCANCODE_RETURN, SDL_SCANCODE_SPACE, SDL_SCANCODE_J,
                      SDL_SCANCODE_K, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_UP,
                      SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT})
            if (key(k))
                return true;
        return false;
    }
};

// --- Discs ------------------------------------------------------------------

struct Disc {
    std::string path;
    std::string id;    // game code + maker, e.g. GEDP01
    std::string name;  // title from the disc header
};

bool read_disc(const std::string& path, Disc* out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;
    unsigned char header[0x400];
    const size_t got = std::fread(header, 1, sizeof header, f);
    std::fclose(f);
    // GameCube discs carry this magic word at 0x1C.
    if (got != sizeof header || header[0x1C] != 0xC2 || header[0x1D] != 0x33 ||
        header[0x1E] != 0x9F || header[0x1F] != 0x3D)
        return false;
    out->path = path;
    out->id.assign(reinterpret_cast<const char*>(header), 6);
    out->name.assign(reinterpret_cast<const char*>(header + 0x20),
                     strnlen(reinterpret_cast<const char*>(header + 0x20), 0x3E0));
    return true;
}

std::vector<Disc> scan_discs(const char* default_iso) {
    std::vector<Disc> discs;
    auto add = [&](const std::string& path) {
        for (const Disc& d : discs)
            if (d.path == path)
                return;
        Disc d;
        if (read_disc(path, &d))
            discs.push_back(d);
    };
    if (default_iso != nullptr && default_iso[0] != '\0')
        add(default_iso);
    for (const char* dir : {"dump", "."}) {
        std::error_code ec;
        std::filesystem::directory_iterator it(dir, ec);
        if (ec)
            continue;
        std::vector<std::string> names;
        for (const auto& e : it) {
            std::string name = e.path().filename().string();
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.size() > 4 && (lower.ends_with(".iso") || lower.ends_with(".gcm")))
                names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        for (const std::string& name : names)
            add(std::string(dir) == "." ? name : std::string(dir) + "/" + name);
    }
    return discs;
}

std::string region_name(const std::string& id) {
    if (id.size() < 4)
        return "?";
    switch (id[3]) {
    case 'E': return "USA";
    case 'P': return "Europe";
    case 'J': return "Japan";
    default: return std::string("region ") + id[3];
    }
}

// --- Menu model -------------------------------------------------------------

enum class Screen { Launcher, Play, Options, Controls, Mapping, InGame, Problem };

struct Row {
    std::string label;
    std::string value;           // empty: an action row
    std::string value_rml;       // markup instead of value (button icons)
    bool enabled = true;
    bool browse = false;         // left/right still works while disabled (a slot picker on an empty slot)
    std::string note;            // shown under the menu while selected
    std::function<void(int)> change;  // left/right (-1/+1); null: no value
    std::function<void()> activate;   // A / click; null: same as change(+1)
    std::string section;         // starts a section with this heading
    bool gap = false;            // an ornament before it: a new group
    int page = 0;                // 1: the right-hand page of a spread
};

Rml::ElementDocument* g_doc = nullptr;
ImFont* g_fps_font = nullptr;  // the FPS counter's (frontend_imgui_init)
bool g_ui_ready = false;
Screen g_screen = Screen::Launcher;
Screen g_options_return = Screen::Launcher;
std::vector<Row> g_rows;
int g_selected = 0;
bool g_close = false;      // leave the current menu loop
bool g_play = false;       // launcher result
std::vector<Disc> g_discs;
int g_disc = 0;
int g_slot = 1;
bool g_disc_on_main = true;  // first run, or the chosen disc is missing
int g_load_slot = 0;         // launcher: boot into this save slot (0: none)
bool g_in_game = false;      // the game is running (menus open over it)
std::string g_running_disc;  // the disc this run plays (the launcher's choice)
int g_argc = 0;
char** g_argv = nullptr;
InputState g_input;

std::string escape(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else out += c;
    }
    return out;
}

std::string on_off(bool v) { return v ? "On" : "Off"; }

std::string slot_path(int slot) {
    return "saves/slot" + std::to_string(slot) + ".state";
}

bool slot_used(int slot) {
    struct stat st;
    return stat(slot_path(slot).c_str(), &st) == 0;
}

bool disc_playable() {
    return !g_discs.empty() && g_discs[g_disc].id == ed_game_id();
}

void build_rows();

void cycle(int& value, int count, int dir) {
    value = (value + dir + count) % count;
}

void changed() {
    settings_apply();
}

Row disc_row() {
    Row disc{"Disc"};
    if (g_discs.empty()) {
        disc.value = "None found";
        disc.enabled = false;
        disc.note = "Put your disc image (.iso) in the dump folder.";
        return disc;
    }
    const Disc& d = g_discs[g_disc];
    disc.value = d.name + " (" + region_name(d.id) + ")";
    if (g_discs.size() > 1)
        disc.value = "\u2039 " + disc.value + " \u203a";
    disc.note = d.path + "  |  " + d.id;
    if (d.id != ed_game_id())
        disc.note = d.id + ": this build runs " + std::string(ed_game_id()) + " only.";
    disc.change = [](int dir) {
        if (g_discs.size() > 1)
            cycle(g_disc, static_cast<int>(g_discs.size()), dir);
        if (disc_playable()) {
            snprintf(g_settings.disc, sizeof g_settings.disc, "%s",
                     g_discs[g_disc].path.c_str());
            settings_save();
        }
    };
    return disc;
}

// "Today 16:55" or "15 Oct 16:55", for a save slot's file time.
std::string slot_time(int slot) {
    struct stat st;
    if (stat(slot_path(slot).c_str(), &st) != 0)
        return "";
    char text[64];
    const time_t now = time(nullptr);
    struct tm when, today;
    localtime_r(&st.st_mtime, &when);
    localtime_r(&now, &today);
    if (when.tm_year == today.tm_year && when.tm_yday == today.tm_yday)
        strftime(text, sizeof text, "Today %H:%M", &when);
    else
        strftime(text, sizeof text, "%d %b %H:%M", &when);
    return text;
}

int newest_slot() {
    int best = 0;
    time_t best_time = 0;
    for (int slot = 1; slot <= 4; slot++) {
        struct stat st;
        if (stat(slot_path(slot).c_str(), &st) == 0 && st.st_mtime >= best_time) {
            best = slot;
            best_time = st.st_mtime;
        }
    }
    return best;
}

// Start this program again from the launcher (another disc, or no boot state
// to go back to): the window closes and reopens.
void restart_process() {
    settings_save();
    unsetenv("ED_QUICKLOAD");
    unsetenv("ED_SKIP_INTRO");
#ifdef _WIN32
    _execv(g_argv[0], g_argv);
#else
    execv(g_argv[0], g_argv);
#endif
    std::fprintf(stderr, "[frontend] restart failed\n");
}

void start_game(int slot) {
    snprintf(g_settings.disc, sizeof g_settings.disc, "%s", g_discs[g_disc].path.c_str());
    settings_save();
    if (g_in_game) {
        // The launcher over a running game: load the save, or the machine as
        // it booted for a new game, in place (the same disc only).
        if (!g_running_disc.empty() && g_discs[g_disc].path != g_running_disc) {
            restart_process();
            return;
        }
        if (slot != 0) {
            quickstate_request_load(slot_path(slot).c_str());
        } else {
            hle_set_skip_intro(false);
            quickstate_request_load(quickstate_boot_path());
        }
        g_close = true;
        return;
    }
    g_load_slot = slot;
    g_play = true;
    g_close = true;
}

void build_launcher() {
    if (g_disc_on_main)
        g_rows.push_back(disc_row());

    const int newest = newest_slot();
    if (newest != 0) {
        Row cont{"Continue", "Slot " + std::to_string(newest) + " \u00b7 " + slot_time(newest)};
        cont.enabled = disc_playable();
        cont.note = "Back where you saved last.";
        cont.activate = [newest] { start_game(newest); };
        g_rows.push_back(cont);
    }

    Row fresh{"New game"};
    fresh.enabled = disc_playable();
    fresh.note = "Start from the title screen (your memory card saves are there).";
    fresh.activate = [] { start_game(0); };
    g_rows.push_back(fresh);

    if (newest != 0) {
        Row load{"Load game"};
        load.enabled = disc_playable();
        load.note = "Choose one of your save slots.";
        load.activate = [] {
            g_screen = Screen::Play;
            g_selected = 0;
        };
        g_rows.push_back(load);
    }

    Row options{"Options"};
    options.gap = true;
    options.activate = [] {
        g_options_return = Screen::Launcher;
        g_screen = Screen::Options;
        g_selected = 0;
    };
    g_rows.push_back(options);

    Row quit{"Quit"};
    quit.gap = true;
    quit.activate = [] {
        if (g_in_game) {
            settings_save();
            dol_aurora_request_quit();
        }
        g_play = false;
        g_close = true;
    };
    g_rows.push_back(quit);
}

// Load game: the used save slots.
void build_play() {
    for (int slot = 1; slot <= 4; slot++) {
        if (!slot_used(slot))
            continue;
        Row r{"Slot " + std::to_string(slot), slot_time(slot)};
        r.note = "Load this save.";
        r.activate = [slot] { start_game(slot); };
        g_rows.push_back(r);
    }

    Row back{"Back"};
    back.gap = true;
    back.activate = [] {
        g_screen = Screen::Launcher;
        g_selected = 0;
    };
    g_rows.push_back(back);
}

Row layout_row() {
    Row layout{"Layout", "\u2039 " + std::string(controls::layout_name(controls::layout())) + " \u203a"};
    static const char* const kLayoutNotes[] = {
        "The GameCube's button positions.",
        "Attack RT/A, action B, aim LT, sprint L3, creep R3; LB/RB page menus.",
        "Your own buttons: change them in Button mapping."};
    layout.note = kLayoutNotes[controls::layout()];
    layout.change = [](int dir) {
        int l = controls::layout();
        cycle(l, controls::kLayoutCount, dir);
        controls::set_layout(l);
    };
    return layout;
}

Row vibration_row() {
    Row vibration{"Vibration", on_off(g_settings.vibration)};
    vibration.change = [](int) {
        g_settings.vibration = !g_settings.vibration;
        changed();
        settings_save();
    };
    return vibration;
}

void build_options() {
    static const char* const kAspect[] = {"16:9", "4:3", "16:9"};
    // Each resolution as the pixels it draws at, for the display shape in use:
    // the window's own, or a standard height at 4:3 or 16:9. The shape comes
    // from the choice itself: the renderer takes a new aspect at its next
    // present, after these rows are built, so its own value lags one change.
    const float display_aspect = g_settings.aspect == 1 ? 4.f / 3.f : 16.f / 9.f;
    uint32_t win_w = 0, win_h = 0;
    dol_aurora_get_display_info(nullptr, &win_w, &win_h);
    auto size_label = [&](int scale) {
        uint32_t w, h;
        if (scale == 0) {
            w = win_w;
            h = win_h;
            if (h != 0 && static_cast<float>(w) / h > display_aspect)
                w = static_cast<uint32_t>(h * display_aspect + 0.5f);
            else if (display_aspect > 0.f)
                h = static_cast<uint32_t>(w / display_aspect + 0.5f);
        } else {
            static const uint32_t kLines[] = {0, 720, 1080, 1440, 2160};
            h = kLines[scale];
            w = static_cast<uint32_t>(h * display_aspect + 0.5f);
        }
        return std::to_string(w) + "\u00d7" + std::to_string(h);
    };
#ifdef __PROSPERO__
    const char* const kNative = "Screen (";  // no window on a console
#else
    const char* const kNative = "Window (";
#endif
    const std::string kScale[] = {
        kNative + size_label(0) + ")", "720p (" + size_label(1) + ")",
        "1080p (" + size_label(2) + ")", "1440p (" + size_label(3) + ")",
        "4K (" + size_label(4) + ")"};

#ifndef __PROSPERO__
    Row full{"Fullscreen", on_off(g_settings.fullscreen)};
    full.section = "I \u00b7 Display";
    full.change = [](int) { g_settings.fullscreen = !g_settings.fullscreen; changed(); };
    g_rows.push_back(full);
#endif

    Row scale{"Resolution", kScale[g_settings.render_scale]};
#ifdef __PROSPERO__
    // The PS5 has no window: the picture always fills the screen, so no
    // Fullscreen row, and the section starts here.
    scale.section = "I \u00b7 Display";
#endif
#ifdef __PROSPERO__
    scale.note = "Screen draws at your TV's own resolution; the others draw at that size and scale to fit.";
#else
    scale.note = "Window draws at your screen's own resolution; the others draw at that size and scale to fit.";
#endif
    scale.change = [](int dir) { cycle(g_settings.render_scale, 5, dir); changed(); };
    g_rows.push_back(scale);

    Row aspect{"Aspect ratio", kAspect[g_settings.aspect]};
    aspect.note = "16:9 is the game's own widescreen view: more at the sides, not stretched. "
                  "Changing it in the game's options changes it here too.";
    aspect.change = [](int dir) {
        int choice = g_settings.aspect - 1;  // 4:3, 16:9
        cycle(choice, 2, dir);
        g_settings.aspect = choice + 1;
        changed();
    };
    g_rows.push_back(aspect);

    Row hud{"Widescreen HUD", g_settings.widescreen_hud ? "Screen edges" : "Centred (4:3)"};
    hud.note = "In the game's widescreen mode, where prompts like Examine sit.";
    hud.change = [](int) {
        g_settings.widescreen_hud = !g_settings.widescreen_hud;
        changed();
    };
    g_rows.push_back(hud);

    Row fps{"FPS counter", on_off(g_settings.fps_counter)};
    fps.note = "Frames per second in the corner of the screen.";
    fps.change = [](int) { g_settings.fps_counter = !g_settings.fps_counter; };
    g_rows.push_back(fps);

    Row music{"Music", std::to_string(g_settings.music_volume) + "%"};
    music.section = "II \u00b7 Sound";
    music.note = "Music streamed from the disc.";
    music.change = [](int dir) {
        g_settings.music_volume = std::clamp(g_settings.music_volume + dir * 10, 0, 100);
    };
    g_rows.push_back(music);

    Row effects{"Effects & voices", std::to_string(g_settings.effects_volume) + "%"};
    effects.note = "The game's own mix: effects, voices and some music.";
    effects.change = [](int dir) {
        g_settings.effects_volume = std::clamp(g_settings.effects_volume + dir * 10, 0, 100);
    };
    g_rows.push_back(effects);

    Row layout = layout_row();
    layout.section = "III \u00b7 Controls";
    layout.page = 1;
    g_rows.push_back(layout);

    Row vibration = vibration_row();
    vibration.page = 1;
    g_rows.push_back(vibration);

    Row more{"More controls"};
    more.note = "Hold or toggle, stick deadzone and button mapping.";
    more.page = 1;
    more.activate = [] {
        g_screen = Screen::Controls;
        g_selected = 0;
    };
    g_rows.push_back(more);

    static const char* const kGameFont[] = {"Sharp", "Tome", "Original"};
    Row font{"Game text", kGameFont[g_settings.game_font]};
    font.section = "IV \u00b7 Game";
    font.page = 1;
    font.note = "The game's own lettering: Sharp redraws it crisply, Tome in old "
                "typefaces, Original as on the disc.";
    font.change = [](int dir) { cycle(g_settings.game_font, 3, dir); };
    g_rows.push_back(font);

    Row hd{"HD textures", texpack_count() != 0 ? on_off(g_settings.hd_textures) : "No pack"};
    hd.page = 1;
    hd.enabled = texpack_count() != 0;
    hd.note = texpack_count() != 0
                  ? "The texture pack in your textures folder (" +
                        std::to_string(texpack_count()) + " images)."
                  : "Put a Dolphin-format texture pack (tex1_*.png) in the textures folder.";
    hd.change = [](int) { g_settings.hd_textures = !g_settings.hd_textures; };
    g_rows.push_back(hd);

    Row shadows{"Baked shadows", g_settings.sharp_shadows ? "Sharp" : "Original"};
    shadows.page = 1;
    shadows.note = "The rooms' light and shadow: Sharp upscales it as each room loads, "
                   "Original keeps the disc's soft, blocky maps.";
    shadows.change = [](int) { g_settings.sharp_shadows = !g_settings.sharp_shadows; };
    g_rows.push_back(shadows);

    if (g_options_return == Screen::Launcher && !g_disc_on_main) {
        Row disc = disc_row();
        disc.page = 1;
        g_rows.push_back(disc);
    }

    Row back{"Back"};
    back.gap = true;
    back.page = 1;
    back.activate = [] {
        settings_save();
        g_screen = g_options_return;
        g_selected = 0;
    };
    g_rows.push_back(back);
}

void build_controls() {
    g_rows.push_back(layout_row());

    Row pad{"Controller", controls::controller_name()};
    pad.enabled = false;
    g_rows.push_back(pad);

    auto hold_toggle = [](const char* label, bool* value, const char* note) {
        Row r{label, *value ? "Toggle" : "Hold"};
        r.note = note;
        r.change = [value](int) {
            *value = !*value;
            settings_save();
        };
        return r;
    };
    g_rows.push_back(hold_toggle("Sprint", &g_settings.sprint_toggle[controls::layout()],
                                 "Toggle: press to run, press again or stop moving to walk."));
    g_rows.push_back(hold_toggle("Creep", &g_settings.creep_toggle[controls::layout()],
                                 "Toggle: press to creep, press again (or sprint) to stop."));
    g_rows.push_back(hold_toggle("Target", &g_settings.target_toggle[controls::layout()],
                                 "Toggle: press to lock on, press again to release."));

    Row deadzone{"Stick deadzone",
                 std::to_string(g_settings.stick_deadzone[controls::layout()]) + "%"};
    deadzone.note = "Raise it if a worn stick drifts; lower is more sensitive.";
    deadzone.change = [](int dir) {
        int& dz = g_settings.stick_deadzone[controls::layout()];
        dz = std::clamp(dz + dir * 5, 5, 25);
        settings_save();
    };
    g_rows.push_back(deadzone);

    g_rows.push_back(vibration_row());

    Row mapping{"Button mapping"};
    mapping.gap = true;  // its own group, so it does not trail the settings
    mapping.note = "See and change which button or key does what.";
    mapping.activate = [] {
        g_screen = Screen::Mapping;
        g_selected = 0;
    };
    g_rows.push_back(mapping);

    Row back{"Back"};
    back.gap = true;
    back.activate = [] {
        g_screen = Screen::Options;
        g_selected = 0;
    };
    g_rows.push_back(back);
}

void build_mapping() {
    for (int a = 0; a < controls::kActionCount; a++) {
        Row r{controls::action_name(a)};
        if (controls::capturing() && controls::capture_action() == a) {
            r.value = "Press a button or key...";
            r.note = "Waiting for a controller button, trigger or key (Esc cancels).";
        } else {
            r.value_rml = controls::pad_glyphs(a) + "<span class=\"sep\"></span>" +
                          controls::key_glyph(a);
            r.note = std::string("Select to rebind (switches to Custom). Layout: ") +
                     controls::layout_name(controls::layout()) + ".";
        }
        r.activate = [a] { controls::begin_capture(a); };
        g_rows.push_back(r);
    }

    Row back{"Back"};
    back.gap = true;
    back.activate = [] {
        g_screen = Screen::Controls;
        g_selected = 0;
    };
    g_rows.push_back(back);
}

void build_in_game() {
    Row resume{"Resume"};
    resume.activate = [] { g_close = true; };
    g_rows.push_back(resume);

    Row save{"Save game", "\u2039 Slot " + std::to_string(g_slot) +
                              (slot_used(g_slot) ? " \u00b7 used \u203a" : " \u00b7 empty \u203a")};
    save.gap = true;
    save.note = "Saves everything, exactly where you are.";
    save.change = [](int dir) { g_slot = (g_slot - 1 + dir + 4) % 4 + 1; };
    save.activate = [] {
        quickstate_request_save(slot_path(g_slot).c_str());
        g_close = true;
    };
    g_rows.push_back(save);

    Row load{"Load game", "\u2039 Slot " + std::to_string(g_slot) +
                              (slot_used(g_slot) ? " \u203a" : " \u00b7 empty \u203a")};
    load.enabled = slot_used(g_slot);
    load.browse = true;
    load.change = [](int dir) { g_slot = (g_slot - 1 + dir + 4) % 4 + 1; };
    load.activate = [] {
        quickstate_request_load(slot_path(g_slot).c_str());
        g_close = true;
    };
    g_rows.push_back(load);

    Row options{"Options"};
    options.gap = true;
    options.activate = [] {
        g_options_return = Screen::InGame;
        g_screen = Screen::Options;
        g_selected = 0;
    };
    g_rows.push_back(options);

    Row launcher{"Return to launcher"};
    launcher.gap = true;
    launcher.note = "Unsaved progress is lost.";
    launcher.activate = [] {
        settings_save();
        // The launcher in this window, over the paused game; without a boot
        // state for New game, start the program again instead.
        if (quickstate_boot_path() == nullptr) {
            restart_process();
            return;
        }
        g_screen = Screen::Launcher;
        g_options_return = Screen::Launcher;
        g_selected = g_disc_on_main && disc_playable() ? 1 : 0;
    };
    g_rows.push_back(launcher);

    Row quit{"Quit"};
    quit.note = "Unsaved progress is lost.";
    quit.activate = [] {
        settings_save();
        dol_aurora_request_quit();
        g_close = true;
    };
    g_rows.push_back(quit);
}

// The problem page (frontend_problem).
std::string g_problem_heading;
std::string g_problem_text;
bool g_problem_can_wait = false;
bool g_problem_wait = false;

void build_problem() {
    if (g_problem_can_wait) {
        Row wait{"Keep waiting"};
        wait.note = g_problem_text;
        wait.activate = [] {
            g_problem_wait = true;
            g_close = true;
        };
        g_rows.push_back(wait);
    }
    Row quit{"Quit"};
    quit.note = g_problem_text;
    quit.activate = [] {
        g_problem_wait = false;
        g_close = true;
    };
    g_rows.push_back(quit);
}

void render() {
    if (g_doc == nullptr)
        return;
    const char* heading = g_screen == Screen::Launcher   ? ""
                          : g_screen == Screen::Play     ? "Load Game"
                          : g_screen == Screen::Options  ? "Options"
                          : g_screen == Screen::Controls ? "Controls"
                          : g_screen == Screen::Mapping  ? "Button Mapping"
                          : g_screen == Screen::Problem  ? g_problem_heading.c_str()
                                                         : "Paused";
    g_doc->GetElementById("heading")->SetInnerRML(heading);
    std::string rml[2];
    bool spread = false;
    for (size_t i = 0; i < g_rows.size(); i++) {
        const Row& r = g_rows[i];
        const int page = r.page == 1 ? 1 : 0;
        spread = spread || page == 1;
        std::string& out = rml[page];
        if (!r.section.empty())
            out += "<div class=\"section\">" + escape(r.section) + "</div>";
        else if (r.gap)
            out += "<div class=\"gap\"></div>";
        out += "<div class=\"row";
        if (static_cast<int>(i) == g_selected)
            out += " selected";
        if (!r.enabled)
            out += " disabled";
        out += "\" id=\"row" + std::to_string(i) +
               "\"><img class=\"mark\" src=\"tome/mark.png\"/><span class=\"label\">" +
               escape(r.label) + "</span>";
        if (!r.value_rml.empty())
            out += "<span class=\"leader\"></span><span class=\"value\">" + r.value_rml +
                   "</span>";
        else if (!r.value.empty())
            out += "<span class=\"leader\"></span><span class=\"value\">" +
                   escape(r.value) + "</span>";
        out += "</div>";
    }
    g_doc->GetElementById("menu0")->SetInnerRML(rml[0]);
    g_doc->GetElementById("menu1")->SetInnerRML(rml[1]);
    // The help note sits under the page holding the chosen row.
    const bool have = g_selected < static_cast<int>(g_rows.size());
    const std::string note = have ? escape(g_rows[g_selected].note) : "";
    const int note_page = have && g_rows[g_selected].page == 1 ? 1 : 0;
    g_doc->GetElementById("note0")->SetInnerRML(note_page == 0 ? note : "");
    g_doc->GetElementById("note1")->SetInnerRML(note_page == 1 ? note : "");
    g_doc->GetElementById("hint")->SetInnerRML(
        g_screen == Screen::InGame    ? "Up/Down choose &#183; Left/Right change &#183; A select &#183; B resume"
        : g_screen == Screen::Problem ? "Up/Down choose &#183; A select"
                                      : "Up/Down choose &#183; Left/Right change &#183; A select &#183; B back");
    g_doc->SetClass("spread", spread);
    // The main menu opens with the emblem instead of a heading.
    g_doc->SetClass("branded", g_screen == Screen::Launcher);
    // The mapping list is long: tighten the rows.
    g_doc->SetClass("compact", g_screen == Screen::Mapping);
    // Over the game the page lies on the dimmed, paused frame.
    g_doc->SetClass("overlay", g_screen == Screen::InGame || g_screen == Screen::Problem ||
                                   ((g_screen == Screen::Options ||
                                     g_screen == Screen::Controls ||
                                     g_screen == Screen::Mapping) &&
                                    g_options_return == Screen::InGame));
}

void build_rows() {
    g_rows.clear();
    switch (g_screen) {
    case Screen::Launcher: build_launcher(); break;
    case Screen::Play: build_play(); break;
    case Screen::Options: build_options(); break;
    case Screen::Controls: build_controls(); break;
    case Screen::Mapping: build_mapping(); break;
    case Screen::InGame: build_in_game(); break;
    case Screen::Problem: build_problem(); break;
    }
    if (g_selected >= static_cast<int>(g_rows.size()))
        g_selected = 0;
    render();
}

void move_selection(int dir) {
    const int n = static_cast<int>(g_rows.size());
    for (int step = 0; step < n; step++) {
        g_selected = (g_selected + dir + n) % n;
        if (g_rows[g_selected].enabled || g_rows[g_selected].change)
            return;
    }
}

void activate_selected() {
    if (g_selected >= static_cast<int>(g_rows.size()))
        return;
    Row& r = g_rows[g_selected];
    if (!r.enabled)
        return;
    if (r.activate)
        r.activate();
    else if (r.change)
        r.change(+1);
}

void go_back() {
    switch (g_screen) {
    case Screen::Play:
        g_screen = Screen::Launcher;
        g_selected = 0;
        break;
    case Screen::Controls:
        g_screen = Screen::Options;
        g_selected = 0;
        break;
    case Screen::Mapping:
        g_screen = Screen::Controls;
        g_selected = 0;
        break;
    case Screen::Options:
        settings_save();
        g_screen = g_options_return;
        g_selected = 0;
        break;
    case Screen::InGame:
        g_close = true;
        break;
    case Screen::Launcher:
    case Screen::Problem:  // only its rows close it (no quitting by accident)
        break;
    }
}

// Mouse: hovering selects a row, clicking activates it.
class MenuListener final : public Rml::EventListener {
public:
    void ProcessEvent(Rml::Event& event) override {
        Rml::Element* e = event.GetTargetElement();
        while (e != nullptr && e->GetId().rfind("row", 0) != 0)
            e = e->GetParentNode();
        if (e == nullptr)
            return;
        const int index = std::atoi(e->GetId().c_str() + 3);
        if (index < 0 || index >= static_cast<int>(g_rows.size()))
            return;
        if (event.GetId() == Rml::EventId::Mousemove) {
            // Only a real pointer movement selects: focus changes and rebuilt
            // rows also report the pointer, and must not steal the selection
            // from a controller or the keyboard.
            static int last_x = -1, last_y = -1;
            const int mx = event.GetParameter<int>("mouse_x", 0);
            const int my = event.GetParameter<int>("mouse_y", 0);
            const bool moved = last_x >= 0 && (mx != last_x || my != last_y);
            last_x = mx;
            last_y = my;
            if (moved && g_selected != index) {
                g_selected = index;
                render();
            }
        } else if (event.GetId() == Rml::EventId::Click) {
            g_selected = index;
            activate_selected();
            build_rows();
        }
    }
};
MenuListener g_listener;

// The res folder (fonts, menu documents): ED_RES_DIR, else res/ in the working
// directory (play.sh runs from the project), else next to the executable.
std::string res_dir() {
    auto has_menu = [](const std::string& dir) {
        struct stat st;
        return stat((dir + "/ui/menu.rml").c_str(), &st) == 0;
    };
    if (const char* env = std::getenv("ED_RES_DIR"); env != nullptr && has_menu(env))
        return env;
    std::error_code ec;
    const std::string cwd = std::filesystem::current_path(ec).string();
    if (!ec && has_menu(cwd + "/res"))
        return cwd + "/res";
    if (g_argv != nullptr && g_argv[0] != nullptr) {
        const std::filesystem::path exe = std::filesystem::absolute(g_argv[0], ec);
        if (!ec) {
            const std::string dir = exe.parent_path().string();
            for (const std::string& candidate : {dir + "/res", dir + "/../res"})
                if (has_menu(candidate))
                    return candidate;
        }
    }
    return "res";
}

bool ensure_ui() {
    if (g_ui_ready)
        return g_doc != nullptr;
    g_ui_ready = true;
    Rml::Context* context = aurora::rmlui::get_context();
    if (context == nullptr) {
        std::fprintf(stderr, "[frontend] no RmlUi context; menus disabled\n");
        return false;
    }
    const std::string res = res_dir();
    for (const char* font : {"FiraSans-Regular.ttf", "FiraSans-Bold.ttf",
                             "IMFellEnglish-Regular.ttf", "IMFellEnglish-Italic.ttf",
                             "IMFellEnglishSC-Regular.ttf"})
        if (!Rml::LoadFontFace(res + "/fonts/" + font))
            std::fprintf(stderr, "[frontend] font missing: %s/fonts/%s\n", res.c_str(), font);
    g_doc = context->LoadDocument(res + "/ui/menu.rml");
    if (g_doc == nullptr) {
        std::fprintf(stderr, "[frontend] %s/ui/menu.rml did not load; menus disabled\n",
                     res.c_str());
        return false;
    }
    context->GetRootElement()->AddEventListener(Rml::EventId::Keydown, &g_key_listener);
    Rml::Element* menu = g_doc->GetElementById("book");
    menu->AddEventListener(Rml::EventId::Click, &g_listener);
    // Mousemove, not mouseover: rebuilt rows would otherwise grab the
    // selection from the keyboard whenever the pointer rests over the menu.
    menu->AddEventListener(Rml::EventId::Mousemove, &g_listener);
    return true;
}

// The mouse pointer hides after two seconds without moving or clicking, in the
// game and the menus, and shows again as soon as the mouse moves.
void update_cursor() {
    static float last_x = -1.f, last_y = -1.f;
    static Uint64 moved_at = 0;
    static bool shown = true;
    float x = 0.f, y = 0.f;
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&x, &y);
    const Uint64 now = SDL_GetTicks();
    if (x != last_x || y != last_y || buttons != 0) {
        last_x = x;
        last_y = y;
        moved_at = now;
        if (!shown) {
            SDL_ShowCursor();
            shown = true;
        }
    } else if (shown && now - moved_at > 2000) {
        SDL_HideCursor();
        shown = false;
    }
}

// Run the current screen until g_close; false when the window was closed.
bool run_menu() {
    g_close = false;
    build_rows();
    g_doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
    // Ignore whatever is still held from before the menu opened.
    bool pressed[kButtonCount];
    g_input.poll(pressed);
    bool open = true;
    while (!g_close) {
        update_cursor();
        if (controls::capturing()) {
            const bool done = controls::poll_capture();
            if (done || !controls::capturing()) {
                // Swallow the press that bound the action.
                g_input.poll(pressed);
                std::fill(std::begin(g_key_pressed), std::end(g_key_pressed), false);
                build_rows();
            }
            if (!dol_aurora_ui_frame()) {
                open = false;
                break;
            }
            continue;
        }
        g_input.poll(pressed);
        bool dirty = false;
        if (pressed[kUp]) {
            move_selection(-1);
            dirty = true;
        }
        if (pressed[kDown]) {
            move_selection(+1);
            dirty = true;
        }
        if ((pressed[kLeft] || pressed[kRight]) && g_selected < static_cast<int>(g_rows.size())) {
            Row& r = g_rows[g_selected];
            if (r.change && (r.enabled || r.browse)) {
                r.change(pressed[kLeft] ? -1 : +1);
                dirty = true;
            }
        }
        if (pressed[kAccept]) {
            activate_selected();
            dirty = true;
        } else if (pressed[kBack] || (pressed[kMenu] && g_screen == Screen::InGame)) {
            go_back();
            dirty = true;
        }
        static const bool log = std::getenv("ED_FRONTEND_LOG") != nullptr;
        if (log && dirty)
            std::fprintf(stderr,
                         "[frontend] up=%d down=%d left=%d right=%d accept=%d back=%d "
                         "-> screen=%d row=%d close=%d\n",
                         pressed[kUp], pressed[kDown], pressed[kLeft], pressed[kRight],
                         pressed[kAccept], pressed[kBack], static_cast<int>(g_screen),
                         g_selected, g_close);
        if (g_close)
            break;
        static Uint64 refresh = 0;
        if ((g_screen == Screen::Controls || g_screen == Screen::Mapping) &&
            SDL_GetTicks() > refresh) {
            refresh = SDL_GetTicks() + 500;  // controller plugged in / out
            dirty = true;
        }
        if (dirty)
            build_rows();
        if (!dol_aurora_ui_frame()) {
            open = false;
            break;
        }
    }
    g_doc->Hide();
    // Wait for every button to be let go so the game does not see the press
    // that closed the menu.
    while (open && g_input.any_held()) {
        g_input.poll(pressed);
        if (!dol_aurora_ui_frame())
            open = false;
    }
    return open;
}

}  // namespace

std::string frontend_res_dir() { return res_dir(); }

// Before ImGui's first frame (its font atlas is locked during one): Fira Sans
// Bold, the menus' face, baked large so the counter stays sharp at any scale.
extern "C" void frontend_imgui_init(void) {
    const std::string path = res_dir() + "/fonts/FiraSans-Bold.ttf";
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        std::fprintf(stderr, "[frontend] font missing: %s (FPS counter in ImGui's own)\n", path.c_str());
        return;
    }
    g_fps_font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), 64.f);
}

extern "C" void frontend_init(int argc, char** argv) {
    g_argc = argc;
    g_argv = argv;
    // Load the menus now so their key listener hears Esc even when the
    // launcher is skipped (quick-load and test runs).
    ensure_ui();
    // The window and Dock icon: the Recomp emblem.
    std::vector<uint8_t> icon;
    uint32_t w = 0, h = 0;
    if (load_png_rgba(res_dir() + "/icon/window.png", &icon, &w, &h))
        dol_aurora_set_window_icon(icon.data(), w, h);
}

extern "C" bool frontend_run_launcher(const char* default_iso, char* iso_out,
                                      size_t iso_cap) {
    if (!ensure_ui()) {
        snprintf(iso_out, iso_cap, "%s", default_iso != nullptr ? default_iso : "");
        return true;
    }
    g_discs = scan_discs(g_settings.disc[0] != '\0' ? g_settings.disc : default_iso);
    // Start on the chosen disc, else the first one this build can run.
    g_disc = 0;
    bool found = false;
    for (size_t i = 0; i < g_discs.size() && !found; i++)
        if (g_discs[i].path == g_settings.disc && g_discs[i].id == ed_game_id()) {
            g_disc = static_cast<int>(i);
            found = true;
        }
    for (size_t i = 0; i < g_discs.size() && !found; i++)
        if (g_discs[i].id == ed_game_id()) {
            g_disc = static_cast<int>(i);
            found = true;
        }
    // Once a playable disc has been chosen, Play leads the main menu and the
    // disc choice lives in Options.
    g_disc_on_main = !(g_settings.disc[0] != '\0' && disc_playable() &&
                       g_discs[g_disc].path == g_settings.disc);
    g_screen = Screen::Launcher;
    g_selected = g_disc_on_main && disc_playable() ? 1 : 0;
    g_play = false;
    g_load_slot = 0;
    if (!run_menu() || !g_play)
        return false;
    snprintf(iso_out, iso_cap, "%s", g_discs[g_disc].path.c_str());
    g_running_disc = g_discs[g_disc].path;
    if (g_load_slot != 0) {
        // Boot straight into the save (the quick-state load at boot).
        setenv("ED_QUICKLOAD", slot_path(g_load_slot).c_str(), 1);
    }
    return true;
}

// On-screen frames per second (game frames, i.e. calls here), refreshed each
// second while the FPS counter option is on. Drawn through Aurora's ImGui,
// which covers the whole screen: the RmlUi context covers only the game
// picture, so in 4:3 (and in the game's letterboxed scenes) the counter sat
// inside the bars, not at the screen's corner.
void update_fps_counter() {
    static Uint64 mark = 0;
    static unsigned frames = 0;
    static int shown_fps = 0;
    if (!g_settings.fps_counter) {
        mark = 0;
        shown_fps = 0;
        return;
    }
    const Uint64 now = SDL_GetTicksNS();
    frames++;
    if (mark == 0) {
        mark = now;
        frames = 0;
    } else if (now - mark >= 1000000000ull) {
        shown_fps = static_cast<int>(frames * 1e9 / static_cast<double>(now - mark) + 0.5);
        mark = now;
        frames = 0;
    }
    if (shown_fps <= 0 || ImGui::GetCurrentContext() == nullptr)
        return;
    // dp in ImGui's units (window points): the display's content scale, as the
    // menus use (2 on a 4K PS5, 1 on a Retina Mac whose points are 2 pixels)
    float dp = 1.f;
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    if (windows != nullptr && count > 0) {
        const float density = SDL_GetWindowPixelDensity(windows[0]);
        const float display = SDL_GetWindowDisplayScale(windows[0]);
        if (density > 0.f && display > 0.f)
            dp = display / density;
    }
    SDL_free(windows);
    ImFont* font = g_fps_font != nullptr ? g_fps_font : ImGui::GetFont();
    char text[32];
    std::snprintf(text, sizeof text, "%d FPS", shown_fps);
    const float size = 18.f * dp;
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.f, text);
    const ImVec2 at{14.f * dp, 10.f * dp};
    const ImVec2 pad{8.f * dp, 3.f * dp};
    // The main viewport's list: the plain call goes through ImGui's current
    // window, which is null outside an ImGui frame (on quit, Aurora has
    // stopped starting them while the game's frames still end here).
    ImDrawList* draw = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    draw->AddRectFilled(at, ImVec2{at.x + extent.x + 2.f * pad.x, at.y + extent.y + 2.f * pad.y},
                        IM_COL32(0, 0, 0, 0x99), 4.f * dp);
    draw->AddText(font, size, ImVec2{at.x + pad.x, at.y + pad.y}, IM_COL32(0xf0, 0xe6, 0xc8, 0xff), text);
}

extern "C" void frontend_frame(void) {
    update_fps_counter();
    update_cursor();
    static bool menu_was_held = false;
    bool held[kButtonCount];
    read_buttons(held);
    const bool menu_held = held[kMenu] || key(SDL_SCANCODE_ESCAPE);
    const bool press = (menu_held && !menu_was_held) || g_key_pressed[kMenu];
    menu_was_held = menu_held;
    std::fill(std::begin(g_key_pressed), std::end(g_key_pressed), false);
    if (!press || !ensure_ui())
        return;
    g_in_game = true;
    g_screen = Screen::InGame;
    g_options_return = Screen::InGame;
    g_selected = 0;
    report_pause(true);
    if (!run_menu())
        dol_aurora_request_quit();  // window closed while paused
    report_pause(false);
    menu_was_held = false;
}

extern "C" bool frontend_problem(const char* heading, const char* text, bool can_wait) {
    if (!ensure_ui())
        return false;
    const Screen screen = g_screen;
    const int selected = g_selected;
    g_problem_heading = heading;
    g_problem_text = text;
    g_problem_can_wait = can_wait;
    g_problem_wait = false;
    g_screen = Screen::Problem;
    g_selected = 0;
    const bool open = run_menu();
    g_screen = screen;
    g_selected = selected;
    return open && g_problem_wait;
}

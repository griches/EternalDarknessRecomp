// Launcher and in-game menu. One RmlUi document (res/ui/menu.rml) whose rows
// are rebuilt from C++ for each screen; selection, value changes and
// navigation are handled here rather than by RmlUi's focus, so the keyboard,
// a controller and the mouse all drive the same rows.
#include "host/frontend.h"

#include "gxruntime/aurora_backend.h"
#include "host/controls.h"
#include "host/settings.h"

#include <aurora/rmlui.hpp>

#include <RmlUi/Core.h>
#include <RmlUi_Platform_SDL.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <vector>

extern "C" {
void quickstate_request_save(const char* path);
void quickstate_request_load(const char* path);
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
        DIR* d = opendir(dir);
        if (d == nullptr)
            continue;
        std::vector<std::string> names;
        while (dirent* e = readdir(d)) {
            std::string name = e->d_name;
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.size() > 4 && (lower.ends_with(".iso") || lower.ends_with(".gcm")))
                names.push_back(name);
        }
        closedir(d);
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

enum class Screen { Launcher, Options, Controls, Mapping, InGame };

struct Row {
    std::string label;
    std::string value;           // empty: an action row
    std::string value_rml;       // markup instead of value (button icons)
    bool enabled = true;
    std::string note;            // shown under the menu while selected
    std::function<void(int)> change;  // left/right (-1/+1); null: no value
    std::function<void()> activate;   // A / click; null: same as change(+1)
};

Rml::ElementDocument* g_doc = nullptr;
Rml::ElementDocument* g_hud = nullptr;  // FPS counter, over the game
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
    return !g_discs.empty() && g_discs[g_disc].id == ED_GAME_ID;
}

void build_rows();

void cycle(int& value, int count, int dir) {
    value = (value + dir + count) % count;
}

void changed() {
    settings_apply();
}

void build_launcher() {
    Row disc{"Disc"};
    if (g_discs.empty()) {
        disc.value = "None found";
        disc.enabled = false;
        disc.note = "Put your disc image (.iso) in the dump folder.";
    } else {
        const Disc& d = g_discs[g_disc];
        disc.value = d.name + " (" + region_name(d.id) + ")";
        if (g_discs.size() > 1)
            disc.value = "< " + disc.value + " >";
        disc.note = d.path + "  |  " + d.id;
        if (d.id != ED_GAME_ID)
            disc.note = d.id + ": this build runs " ED_GAME_ID " only.";
        disc.change = [](int dir) {
            if (g_discs.size() > 1)
                cycle(g_disc, static_cast<int>(g_discs.size()), dir);
        };
    }
    g_rows.push_back(disc);

    Row play{"Play"};
    play.enabled = disc_playable();
    play.activate = [] {
        snprintf(g_settings.disc, sizeof g_settings.disc, "%s",
                 g_discs[g_disc].path.c_str());
        settings_save();
        g_play = true;
        g_close = true;
    };
    g_rows.push_back(play);

    Row options{"Options"};
    options.activate = [] {
        g_options_return = Screen::Launcher;
        g_screen = Screen::Options;
        g_selected = 0;
    };
    g_rows.push_back(options);

    Row quit{"Quit"};
    quit.activate = [] {
        g_play = false;
        g_close = true;
    };
    g_rows.push_back(quit);
}

void build_options() {
    static const char* const kAspect[] = {"Follow the game", "4:3", "16:9", "Stretch"};
    // Each resolution as the pixels it draws at, for the display shape in use
    // (the GameCube's 528 lines times the scale, at 4:3 or 16:9).
    float display_aspect = 4.f / 3.f;
    uint32_t win_w = 0, win_h = 0;
    dol_aurora_get_display_info(&display_aspect, &win_w, &win_h);
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
            h = 528u * static_cast<uint32_t>(scale);
            w = static_cast<uint32_t>(h * display_aspect + 0.5f);
        }
        return std::to_string(w) + "x" + std::to_string(h);
    };
    const std::string kScale[] = {
        "Window (" + size_label(0) + ")", "1x (" + size_label(1) + ")",
        "2x (" + size_label(2) + ")", "3x (" + size_label(3) + ")",
        "4x (" + size_label(4) + ")"};
    static const int kBuffers[] = {40, 60, 80, 100, 150, 200};

    Row skip{"Skip intros", on_off(g_settings.skip_intros)};
    skip.note = "Skip the logo movies and the opening quote (next start).";
    skip.change = [](int) { g_settings.skip_intros = !g_settings.skip_intros; };
    g_rows.push_back(skip);

    Row aspect{"Display", kAspect[g_settings.aspect]};
    aspect.note = "Follow the game: 16:9 when its widescreen option is on.";
    aspect.change = [](int dir) { cycle(g_settings.aspect, 4, dir); changed(); };
    g_rows.push_back(aspect);

    Row hud{"Widescreen HUD", g_settings.widescreen_hud ? "Screen edges" : "Centred (4:3)"};
    hud.note = "In the game's widescreen mode, where prompts like Examine sit.";
    hud.change = [](int) {
        g_settings.widescreen_hud = !g_settings.widescreen_hud;
        changed();
    };
    g_rows.push_back(hud);

    Row scale{"Resolution", kScale[g_settings.render_scale]};
    scale.note = "Window draws at your screen's resolution.";
    scale.change = [](int dir) { cycle(g_settings.render_scale, 5, dir); changed(); };
    g_rows.push_back(scale);

    Row full{"Fullscreen", on_off(g_settings.fullscreen)};
    full.change = [](int) { g_settings.fullscreen = !g_settings.fullscreen; changed(); };
    g_rows.push_back(full);

    Row fps{"FPS counter", on_off(g_settings.fps_counter)};
    fps.note = "Frames per second in the window title.";
    fps.change = [](int) { g_settings.fps_counter = !g_settings.fps_counter; };
    g_rows.push_back(fps);

    Row music{"Music volume", std::to_string(g_settings.music_volume) + "%"};
    music.note = "Music streamed from the disc.";
    music.change = [](int dir) {
        g_settings.music_volume = std::clamp(g_settings.music_volume + dir * 10, 0, 100);
    };
    g_rows.push_back(music);

    Row effects{"Sound effects & voices", std::to_string(g_settings.effects_volume) + "%"};
    effects.note = "The game's own mix: effects, voices and some music.";
    effects.change = [](int dir) {
        g_settings.effects_volume = std::clamp(g_settings.effects_volume + dir * 10, 0, 100);
    };
    g_rows.push_back(effects);

    Row buffer{"Audio buffer", std::to_string(g_settings.audio_buffer_ms) + " ms"};
    buffer.note = "Raise it if the sound stutters; lower is more responsive.";
    buffer.change = [](int dir) {
        int i = 0;
        while (i < 5 && kBuffers[i] < g_settings.audio_buffer_ms)
            i++;
        i = std::clamp(i + dir, 0, 5);
        g_settings.audio_buffer_ms = kBuffers[i];
        changed();
    };
    g_rows.push_back(buffer);

    Row controls{"Controls", controls::layout_name(controls::layout())};
    controls.note = "Controller and keyboard layout: Original, Modern or Custom.";
    controls.activate = [] {
        g_screen = Screen::Controls;
        g_selected = 0;
    };
    g_rows.push_back(controls);

    Row back{"Back"};
    back.activate = [] {
        settings_save();
        g_screen = g_options_return;
        g_selected = 0;
    };
    g_rows.push_back(back);
}

void build_controls() {
    Row layout{"Layout", "< " + std::string(controls::layout_name(controls::layout())) + " >"};
    static const char* const kLayoutNotes[] = {
        "The GameCube's button positions.",
        "Attack RT/A, action B, aim LT, sprint L3, creep R3.",
        "Your own buttons: change them in Button mapping."};
    layout.note = kLayoutNotes[controls::layout()];
    layout.change = [](int dir) {
        int l = controls::layout();
        cycle(l, controls::kLayoutCount, dir);
        controls::set_layout(l);
    };
    g_rows.push_back(layout);

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

    Row deadzone{"Stick deadzone", std::to_string(g_settings.stick_deadzone) + "%"};
    deadzone.note = "Raise it if a worn stick drifts; lower is more sensitive.";
    deadzone.change = [](int dir) {
        g_settings.stick_deadzone = std::clamp(g_settings.stick_deadzone + dir * 5, 5, 25);
        settings_save();
    };
    g_rows.push_back(deadzone);

    Row vibration{"Vibration", on_off(g_settings.vibration)};
    vibration.change = [](int) {
        g_settings.vibration = !g_settings.vibration;
        changed();
        settings_save();
    };
    g_rows.push_back(vibration);

    Row mapping{"Button mapping"};
    mapping.note = "See and change which button or key does what.";
    mapping.activate = [] {
        g_screen = Screen::Mapping;
        g_selected = 0;
    };
    g_rows.push_back(mapping);

    Row back{"Back"};
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

    Row save{"Save to", "< Slot " + std::to_string(g_slot) + (slot_used(g_slot) ? " (used) >" : " (empty) >")};
    save.note = "Saves everything, exactly where you are.";
    save.change = [](int dir) { g_slot = (g_slot - 1 + dir + 4) % 4 + 1; };
    save.activate = [] {
        quickstate_request_save(slot_path(g_slot).c_str());
        g_close = true;
    };
    g_rows.push_back(save);

    Row load{"Load from", "< Slot " + std::to_string(g_slot) + (slot_used(g_slot) ? " >" : " (empty) >")};
    load.enabled = slot_used(g_slot);
    load.change = [](int dir) { g_slot = (g_slot - 1 + dir + 4) % 4 + 1; };
    load.activate = [] {
        quickstate_request_load(slot_path(g_slot).c_str());
        g_close = true;
    };
    g_rows.push_back(load);

    Row options{"Options"};
    options.activate = [] {
        g_options_return = Screen::InGame;
        g_screen = Screen::Options;
        g_selected = 0;
    };
    g_rows.push_back(options);

    Row launcher{"Return to launcher"};
    launcher.note = "Unsaved progress is lost.";
    launcher.activate = [] {
        settings_save();
        unsetenv("ED_QUICKLOAD");
        unsetenv("ED_SKIP_INTRO");
        execv(g_argv[0], g_argv);
        std::fprintf(stderr, "[frontend] restart failed\n");
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

void render() {
    if (g_doc == nullptr)
        return;
    const char* heading = g_screen == Screen::Launcher   ? "Main Menu"
                          : g_screen == Screen::Options  ? "Options"
                          : g_screen == Screen::Controls ? "Controls"
                          : g_screen == Screen::Mapping  ? "Button Mapping"
                                                         : "Paused";
    g_doc->GetElementById("heading")->SetInnerRML(heading);
    std::string rml;
    for (size_t i = 0; i < g_rows.size(); i++) {
        const Row& r = g_rows[i];
        rml += "<div class=\"row";
        if (static_cast<int>(i) == g_selected)
            rml += " selected";
        if (!r.enabled)
            rml += " disabled";
        rml += "\" id=\"row" + std::to_string(i) + "\"><span class=\"label\">" +
               escape(r.label) + "</span>";
        if (!r.value_rml.empty())
            rml += "<span class=\"value\">" + r.value_rml + "</span>";
        else if (!r.value.empty())
            rml += "<span class=\"value\">" + escape(r.value) + "</span>";
        rml += "</div>";
    }
    g_doc->GetElementById("menu")->SetInnerRML(rml);
    const std::string note =
        g_selected < static_cast<int>(g_rows.size()) ? g_rows[g_selected].note : "";
    g_doc->GetElementById("note")->SetInnerRML(escape(note));
    g_doc->GetElementById("hint")->SetInnerRML(
        g_screen == Screen::InGame
            ? "Up/Down choose &#183; Left/Right change &#183; A select &#183; B resume"
            : "Up/Down choose &#183; Left/Right change &#183; A select &#183; B back");
    // The controls list is long: drop the title and tighten the rows.
    g_doc->SetClass("compact", g_screen == Screen::Mapping);
    // Over the game the menu dims the paused frame; the launcher is opaque.
    g_doc->SetClass("overlay", g_screen == Screen::InGame ||
                                   ((g_screen == Screen::Options ||
                                     g_screen == Screen::Controls ||
                                     g_screen == Screen::Mapping) &&
                                    g_options_return == Screen::InGame));
}

void build_rows() {
    g_rows.clear();
    switch (g_screen) {
    case Screen::Launcher: build_launcher(); break;
    case Screen::Options: build_options(); break;
    case Screen::Controls: build_controls(); break;
    case Screen::Mapping: build_mapping(); break;
    case Screen::InGame: build_in_game(); break;
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
    char cwd[1024];
    if (getcwd(cwd, sizeof cwd) != nullptr && has_menu(std::string(cwd) + "/res"))
        return std::string(cwd) + "/res";
    if (g_argv != nullptr && g_argv[0] != nullptr) {
        char exe[1024];
        if (realpath(g_argv[0], exe) != nullptr) {
            std::string dir(exe);
            dir = dir.substr(0, dir.find_last_of('/'));
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
                             "AlegreyaSC-Regular.ttf", "AlegreyaSC-Bold.ttf"})
        if (!Rml::LoadFontFace(res + "/fonts/" + font))
            std::fprintf(stderr, "[frontend] font missing: %s/fonts/%s\n", res.c_str(), font);
    g_hud = context->LoadDocument(res + "/ui/hud.rml");
    g_doc = context->LoadDocument(res + "/ui/menu.rml");
    if (g_doc == nullptr) {
        std::fprintf(stderr, "[frontend] %s/ui/menu.rml did not load; menus disabled\n",
                     res.c_str());
        return false;
    }
    context->GetRootElement()->AddEventListener(Rml::EventId::Keydown, &g_key_listener);
    Rml::Element* menu = g_doc->GetElementById("menu");
    menu->AddEventListener(Rml::EventId::Click, &g_listener);
    // Mousemove, not mouseover: rebuilt rows would otherwise grab the
    // selection from the keyboard whenever the pointer rests over the menu.
    menu->AddEventListener(Rml::EventId::Mousemove, &g_listener);
    return true;
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
            if (r.change && r.enabled) {
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

extern "C" void frontend_init(int argc, char** argv) {
    g_argc = argc;
    g_argv = argv;
    // Load the menus now so their key listener hears Esc even when the
    // launcher is skipped (quick-load and test runs).
    ensure_ui();
}

extern "C" bool frontend_run_launcher(const char* default_iso, char* iso_out,
                                      size_t iso_cap) {
    if (!ensure_ui()) {
        snprintf(iso_out, iso_cap, "%s", default_iso != nullptr ? default_iso : "");
        return true;
    }
    g_discs = scan_discs(g_settings.disc[0] != '\0' ? g_settings.disc : default_iso);
    // Start on the first disc this build can run.
    for (size_t i = 0; i < g_discs.size(); i++)
        if (g_discs[i].id == ED_GAME_ID) {
            g_disc = static_cast<int>(i);
            break;
        }
    g_screen = Screen::Launcher;
    g_selected = disc_playable() ? 1 : 0;
    g_play = false;
    if (!run_menu() || !g_play)
        return false;
    snprintf(iso_out, iso_cap, "%s", g_discs[g_disc].path.c_str());
    if (g_settings.skip_intros)
        setenv("ED_SKIP_INTRO", "1", 1);
    return true;
}

// On-screen frames per second (game frames, i.e. calls here), refreshed each
// second while the FPS counter option is on.
void update_fps_counter() {
    static Uint64 mark = 0;
    static unsigned frames = 0;
    static bool shown = false;
    if (g_hud == nullptr)
        return;
    if (!g_settings.fps_counter) {
        if (shown) {
            g_hud->Hide();
            shown = false;
        }
        mark = 0;
        return;
    }
    const Uint64 now = SDL_GetTicksNS();
    frames++;
    if (mark == 0) {
        mark = now;
        frames = 0;
        return;
    }
    if (now - mark >= 1000000000ull) {
        const double fps = frames * 1e9 / static_cast<double>(now - mark);
        g_hud->GetElementById("fps")->SetInnerRML(std::to_string(static_cast<int>(fps + 0.5)) + " FPS");
        mark = now;
        frames = 0;
        if (!shown) {
            g_hud->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
            shown = true;
        }
    }
}

extern "C" void frontend_frame(void) {
    update_fps_counter();
    static bool menu_was_held = false;
    bool held[kButtonCount];
    read_buttons(held);
    const bool menu_held = held[kMenu] || key(SDL_SCANCODE_ESCAPE);
    const bool press = (menu_held && !menu_was_held) || g_key_pressed[kMenu];
    menu_was_held = menu_held;
    std::fill(std::begin(g_key_pressed), std::end(g_key_pressed), false);
    if (!press || !ensure_ui())
        return;
    g_screen = Screen::InGame;
    g_options_return = Screen::InGame;
    g_selected = 0;
    if (!run_menu())
        dol_aurora_request_quit();  // window closed while paused
    menu_was_held = false;
}

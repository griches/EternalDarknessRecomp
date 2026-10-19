// Launcher and in-game menu. One RmlUi document (res/ui/menu.rml) whose rows
// are rebuilt from C++ for each screen; selection, value changes and
// navigation are handled here rather than by RmlUi's focus, so the keyboard,
// a controller and the mouse all drive the same rows.
#include "host/frontend.h"

#include "gxruntime/aurora_backend.h"
#include "host/buttons.h"
#include "host/camera.h"
#include "host/controls.h"
#include "host/report.h"
#include "host/settings.h"
#include "host/texpack.h"

#include <aurora/gfx.h>
#include <aurora/imgui.h>
#include <aurora/rmlui.hpp>
#include <imgui.h>

#include <RmlUi/Core.h>
#include <RmlUi_Platform_SDL.h>
#include <SDL3/SDL_filesystem.h>
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
#include <array>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
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
        // Only the disc this build runs (the European disc is the one
        // supported): another image in the folder is not offered at all.
        Disc d;
        if (read_disc(path, &d) && d.id == ed_game_id())
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
        disc.note = std::string("Put your ") + region_name(ed_game_id()) + " disc image (" +
                    ed_game_id() + ", .iso) in the dump folder.";
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
        dol_aurora_measure_screen();
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
        "Attack RT/A, action B, aim LT, sprint L3, creep R3. The Tome: A B X Y, LB/RB page.",
        "Your own buttons (Button mapping). The Tome: A B X Y, LB/RB page."};
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

// Graphics enhancements: the menu's rows and the in-game shortcuts (hold R3
// and press a button, frontend_cycle_enhancement) change the same values,
// applied at once so they can be compared.
namespace enhancement {
enum { kGlow, kOcclusion, kHdr, kShadows, kHdTextures, kCount };
const char* const kNames[kCount] = {"Glow", "Ambient occlusion", "HDR", "Baked shadows", "HD textures"};
const char* const kLevels[] = {"Off", "Soft", "Strong"};

bool available(int which) {
    if (which == kHdr)
        return dol_aurora_hdr_available();
    if (which == kHdTextures)
        return texpack_count() != 0;
    return true;
}

std::string value(int which) {
    switch (which) {
    case kGlow: return kLevels[g_settings.glow];
    case kOcclusion: return kLevels[g_settings.ssao];
    case kHdr: return available(which) ? on_off(g_settings.hdr) : "Not on this display";
    case kShadows: return g_settings.sharp_shadows ? "Sharp" : "Original";
    case kHdTextures: return available(which) ? on_off(g_settings.hd_textures) : "No pack";
    default: return "";
    }
}

void change(int which, int dir) {
    switch (which) {
    case kGlow: cycle(g_settings.glow, 3, dir); break;
    case kOcclusion: cycle(g_settings.ssao, 3, dir); break;
    case kHdr: g_settings.hdr = !g_settings.hdr; break;
    case kShadows: g_settings.sharp_shadows = !g_settings.sharp_shadows; break;
    case kHdTextures: g_settings.hd_textures = !g_settings.hd_textures; break;
    default: return;
    }
    settings_apply_graphics();
}

const char* const kNotes[kCount] = {
    "Lamps, flames, magick and glowing runes give off a soft haze of light.",
    "Corners, creases and the ground round feet darken where light can't reach, worked out from depth each frame.",
    "On an HDR display, glowing light shines brighter than white.",
    "The rooms' light and shadow: Sharp upscales it as each room loads, Original keeps the disc's soft, blocky maps.",
    "The texture pack in your textures folder.",
};

// What changed, shown over the game for a moment (update_enhancement_notice).
std::mutex g_notice_mutex;
std::string g_notice;
Uint64 g_notice_at = 0;
}  // namespace enhancement

void run_hdr_calibration(int first_step);

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

    // Frames in between the game's own 60 a second (the renderer blends the
    // last two) on a faster screen. The choices are the screen's: its rate,
    // measured as the options open (so a refresh rate changed meanwhile shows
    // here), and in fullscreen on a PC every faster rate the display has,
    // which it is switched to.
    int choices[8];
    const int count = dol_aurora_frame_rate_choices(choices, 8);
    const int screen_hz = dol_aurora_screen_refresh();
    const bool faster = count > 1;
    int shown = 60;
    if (g_settings.frame_rate != 60 && faster) {
        shown = screen_hz >= 75 ? screen_hz : choices[count - 1];
        for (int i = 0; i < count; i++)
            if (choices[i] == g_settings.frame_rate)
                shown = g_settings.frame_rate;
    }
    Row rate{"Frame rate", std::to_string(shown)};
    rate.enabled = faster;
    if (faster) {
        rate.note = "Above 60, frames are drawn between the game's own, blended from the last two: "
                    "smoother motion, at most one game frame more delay.";
    } else {
#ifdef __PROSPERO__
        rate.note = "Your TV is running at 60 Hz. 120 needs a TV that takes 120 Hz, with Enable 120 Hz "
                    "Output on in the PS5's Screen and Video settings.";
#else
        rate.note = std::getenv("SteamDeck") != nullptr
                        ? std::string("Your screen is running at 60 Hz. On a Steam Deck OLED, set Refresh "
                                      "Rate to 90 in Steam's Performance menu, then open Options again.")
                        : "Your screen is running at " + std::to_string(screen_hz) +
                              " Hz: smoother motion needs a faster one.";
#endif
    }
    rate.change = [choices, count, shown](int dir) {
        int at = count - 1;
        for (int i = 0; i < count; i++)
            if (choices[i] == shown)
                at = i;
        cycle(at, count, dir);
        g_settings.frame_rate = choices[at];
        changed();
    };
    g_rows.push_back(rate);

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

    {
        using namespace enhancement;
        Row hdr{kNames[kHdr], value(kHdr)};
        hdr.enabled = available(kHdr);
        hdr.note = available(kHdr) ? std::string(kNotes[kHdr]) + " " + controls::enhancement_shortcut(kHdr) +
                                         " in the game to compare."
                                   : "Needs an HDR display (on a Mac, its XDR screen or an HDR monitor).";
        hdr.change = [](int dir) { change(kHdr, dir); };
        g_rows.push_back(hdr);
        // Only where HDR can be on: elsewhere it would be one more dead row.
        if (available(kHdr)) {
            Row calibrate{"Calibrate HDR"};
            calibrate.enabled = g_settings.hdr;
            calibrate.value = std::to_string(g_settings.hdr_peak) + " nits peak";
            calibrate.note = g_settings.hdr ? "Three steps for your display: the brightest it shows, how bright "
                                              "the picture is, and where black begins."
                                            : "Turn HDR on to calibrate it.";
            calibrate.activate = [] { run_hdr_calibration(0); };
            g_rows.push_back(calibrate);
        }
    }

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

    // Graphics: the enhancements, most visible first (HDR is with Display).
    {
        using namespace enhancement;
        static const int kOrder[] = {kHdTextures, kShadows, kOcclusion, kGlow};
        for (int which : kOrder) {
            Row r{kNames[which], value(which)};
            if (which == kHdTextures)
                r.section = "III \u00b7 Graphics";
            r.page = 1;
            r.enabled = available(which);
            r.note = kNotes[which];
            if (which == kHdTextures)
                r.note = available(which) ? "The texture pack in your textures folder (" +
                                                std::to_string(texpack_count()) + " images)."
                                          : "Put a Dolphin-format texture pack (tex1_*.png) in the textures folder.";
            if (available(which))
                r.note += " " + controls::enhancement_shortcut(which) + " in the game to compare.";
            r.change = [which](int dir) { change(which, dir); };
            g_rows.push_back(r);
        }
    }

    // Controls: the layout here, everything else (vibration, hold or toggle,
    // deadzone, mapping) a page of its own.
    Row controls_row = layout_row();
    controls_row.label = "Controls";
    controls_row.section = "IV \u00b7 Controls";
    controls_row.page = 1;
    controls_row.note = "Left and right: the button layout. A: vibration, hold or toggle, "
                        "stick deadzone and button mapping.";
    controls_row.activate = [] {
        g_screen = Screen::Controls;
        g_selected = 0;
    };
    g_rows.push_back(controls_row);

    static const char* const kCameraNotes[] = {
        "The game's own fixed cameras.",
        "The game's own fixed cameras.",
        "Behind Alex: the right stick turns it, up and down bring it closer or further. "
        "It holds still while you aim.",
        "Through Alex's eyes: the right stick looks around, and up on the left stick walks where you look. "
        "It holds still while you aim."};
    Row camera{"Camera", camera_mode_name(g_settings.camera)};
    camera.section = "V \u00b7 Game";
    camera.page = 1;
    camera.note = kCameraNotes[g_settings.camera];
    camera.change = [](int dir) {
        static const int kModes[] = {CAMERA_ORIGINAL, CAMERA_THIRD_PERSON, CAMERA_FIRST_PERSON};
        int at = 0;
        for (int i = 0; i < 3; i++)
            if (kModes[i] == g_settings.camera)
                at = i;
        cycle(at, 3, dir);
        g_settings.camera = kModes[at];
    };
    g_rows.push_back(camera);

    static const char* const kGameFont[] = {"Sharp", "Tome", "Original"};
    Row font{"Game text", kGameFont[g_settings.game_font]};
    font.page = 1;
    font.note = "The game's own lettering: Sharp redraws it crisply, Tome in old "
                "typefaces, Original as on the disc.";
    font.change = [](int dir) { cycle(g_settings.game_font, 3, dir); };
    g_rows.push_back(font);

    Row fps{"FPS counter", on_off(g_settings.fps_counter)};
    fps.page = 1;
    fps.note = "Frames per second in the corner of the screen.";
    fps.change = [](int) { g_settings.fps_counter = !g_settings.fps_counter; };
    g_rows.push_back(fps);

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
        dol_aurora_measure_screen();
        g_options_return = Screen::InGame;
        g_screen = Screen::Options;
        g_selected = 0;
    };
    g_rows.push_back(options);

    // A reset: the machine as it booted, so the game starts again through its
    // logos to the title screen (the launcher's New game, in place).
    Row title{"Return to title screen"};
    title.gap = true;
    title.note = "Unsaved progress is lost.";
    title.enabled = quickstate_boot_path() != nullptr;
    title.activate = [] {
        settings_save();
        hle_set_skip_intro(false);
        quickstate_request_load(quickstate_boot_path());
        g_close = true;
    };
    g_rows.push_back(title);

    Row launcher{"Return to launcher"};
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
// ED_MENU_TEST=after_s,hold_ms[,every_s]: open the in-game menu by itself
// after_s seconds in, close it hold_ms later, and again every every_s seconds
// (test runs without a controller).
static Uint64 g_test_close_at = 0;

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
        if (g_test_close_at != 0 && SDL_GetTicks() >= g_test_close_at) {
            g_test_close_at = 0;
            g_close = true;
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
    // ED_MENU_OPEN=options or controls[,row] (testing): the launcher opens on
    // that page, at that row.
    if (const char* page = std::getenv("ED_MENU_OPEN"); page != nullptr) {
        g_options_return = Screen::Launcher;
        g_screen = std::strncmp(page, "controls", 8) == 0 ? Screen::Controls : Screen::Options;
        const char* comma = std::strchr(page, ',');
        g_selected = comma != nullptr ? std::atoi(comma + 1) : 0;
    }
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

// dp in ImGui's units (window points) for the overlays drawn through ImGui:
// the display's content scale, as the menus use (2 on a 4K PS5, 1 on a Retina
// Mac whose points are 2 pixels).
float overlay_dp() {
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
    return dp;
}

// On-screen frames per second (game frames, i.e. calls here), refreshed each
// second while the FPS counter option is on. With frames in between, the
// screen's instead: every frame presented, which falls below the screen's
// rate only when frames in between are late and skipped. Drawn through Aurora's ImGui,
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
    const float dp = overlay_dp();
    ImFont* font = g_fps_font != nullptr ? g_fps_font : ImGui::GetFont();
    char text[32];
    std::snprintf(text, sizeof text, "%d FPS",
                  dol_aurora_frames_between() ? static_cast<int>(aurora_get_fps() + 0.5f) : shown_fps);
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

// While a cutscene plays, the fast-forward prompt at the screen's bottom right
// (in the letterbox bar): RB's icon (Tab's on the keyboard) and a
// fast-forward sign, which brightens while it is held. Through ImGui, as the FPS counter.
void update_fast_forward_prompt() {
    const std::string file = controls::fast_forward_glyph();
    if (file.empty() || ImGui::GetCurrentContext() == nullptr)
        return;
    static std::map<std::string, ImTextureID> icons;  // 0: the file did not load
    auto it = icons.find(file);
    if (it == icons.end()) {
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        ImTextureID id = 0;
        if (load_png_rgba(res_dir() + "/ui/glyphs/" + file, &rgba, &w, &h))
            id = aurora_imgui_add_texture(w, h, rgba.data());
        it = icons.emplace(file, id).first;
    }
    if (it->second == 0)
        return;
    const float dp = overlay_dp();
    const bool fast = controls::fast_forwarding();
    const ImVec2 screen = ImGui::GetMainViewport()->Size;
    const float icon = 44.f * dp;
    const float arrow = 14.f * dp;  // each triangle's width; height is icon / 2
    const float gap = 8.f * dp;
    const float right = screen.x - 24.f * dp;
    const float top = screen.y - 18.f * dp - icon;
    const float sign_left = right - 2.f * arrow;
    const float icon_left = sign_left - gap - icon;
    ImDrawList* draw = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    const ImU32 alpha = fast ? 0xff : 0x99;
    draw->AddImage(it->second, ImVec2{icon_left, top}, ImVec2{icon_left + icon, top + icon},
                   ImVec2{0, 0}, ImVec2{1, 1}, IM_COL32(0xff, 0xff, 0xff, alpha));
    const float mid = top + icon / 2.f;
    const ImU32 cream = IM_COL32(0xf0, 0xe6, 0xc8, alpha);
    for (int i = 0; i < 2; i++) {
        const float x = sign_left + i * arrow;
        draw->AddTriangleFilled(ImVec2{x, mid - icon / 4.f}, ImVec2{x + arrow, mid},
                                ImVec2{x, mid + icon / 4.f}, cream);
    }
}

// What a shortcut changed ("Glow: Strong"), top centre for two seconds, with
// the shortcuts under it. Through ImGui, as the FPS counter.
void update_enhancement_notice() {
    std::string text;
    Uint64 at;
    {
        std::lock_guard<std::mutex> lock(enhancement::g_notice_mutex);
        text = enhancement::g_notice;
        at = enhancement::g_notice_at;
    }
    const Uint64 now = SDL_GetTicks();
    if (text.empty() || now - at > 2500 || ImGui::GetCurrentContext() == nullptr)
        return;
    const float fade = now - at > 2000 ? 1.f - (now - at - 2000) / 500.f : 1.f;
    const float dp = overlay_dp();
    ImFont* font = g_fps_font != nullptr ? g_fps_font : ImGui::GetFont();
    static const std::string legend = controls::enhancement_legend();
    const float size = 26.f * dp, small = 15.f * dp;
    const ImVec2 big = font->CalcTextSizeA(size, FLT_MAX, 0.f, text.c_str());
    const ImVec2 line = font->CalcTextSizeA(small, FLT_MAX, 0.f, legend.c_str());
    const ImVec2 pad{16.f * dp, 9.f * dp};
    const float width = std::max(big.x, line.x) + 2.f * pad.x;
    const float height = big.y + 4.f * dp + line.y + 2.f * pad.y;
    const ImVec2 screen = ImGui::GetMainViewport()->Size;
    const ImVec2 at0{(screen.x - width) / 2.f, 28.f * dp};
    ImDrawList* draw = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    const auto a = [&](int v) { return static_cast<ImU32>(v * fade); };
    // Solid: with HDR on, a lamp behind it can be up to eight times
    // brighter than white and shone through even a nearly solid box.
    draw->AddRectFilled(at0, ImVec2{at0.x + width, at0.y + height}, IM_COL32(0, 0, 0, a(0xff)), 6.f * dp);
    draw->AddText(font, size, ImVec2{at0.x + (width - big.x) / 2.f, at0.y + pad.y}, IM_COL32(0xf0, 0xe6, 0xc8, a(0xff)),
                  text.c_str());
    draw->AddText(font, small, ImVec2{at0.x + (width - line.x) / 2.f, at0.y + pad.y + big.y + 4.f * dp},
                  IM_COL32(0xb8, 0xae, 0x98, a(0xff)), legend.c_str());
}

namespace {

// A button's picture for the controller in hand (or the key's), as an ImGui
// texture (0 when the file did not load).
ImTextureID glyph(const std::string& set, const std::string& name) {
    static std::map<std::string, ImTextureID> icons;
    const std::string file = set + "/" + name + ".png";
    auto it = icons.find(file);
    if (it == icons.end()) {
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        ImTextureID id = 0;
        if (load_png_rgba(res_dir() + "/ui/glyphs/" + file, &rgba, &w, &h))
            id = aurora_imgui_add_texture(w, h, rgba.data());
        it = icons.emplace(file, id).first;
    }
    return it->second;
}

// HDR calibration: the menu steps aside and the renderer draws each step's
// test pattern in place of the picture (dol_aurora_set_hdr_pattern), with
// this caption under it. 1 peak: the ring (the most a signal asks for) in a
// box at the chosen peak disappears once the box reaches what the display
// shows. 2 brightness: the game picture, SDR white to taste. 3 black: a ring
// a shade above black, set to only just visible.
void draw_hdr_calibration_caption(int step) {
    if (ImGui::GetCurrentContext() == nullptr)
        return;
    static const char* const kTitles[3] = {"1 / 3  Peak brightness", "2 / 3  Brightness", "3 / 3  Black level"};
    static const char* const kHelp[3] = {
        "Raise it until the ring in the box disappears, then go back one step.",
        "How bright the picture is: set it so the game looks right to you.",
        "Set it so the ring is only just visible against the black.",
    };
    char value[48];
    if (step == 0)
        std::snprintf(value, sizeof value, "%d nits", g_settings.hdr_peak);
    else if (step == 1)
        std::snprintf(value, sizeof value, "%d nits", g_settings.hdr_paper);
    else
        std::snprintf(value, sizeof value, "%+d", g_settings.hdr_black);
    const std::string head = std::string(kTitles[step]) + ":  " + value;
    // The buttons, as pictures for the controller in hand (or the keys).
    struct Hint {
        std::vector<std::string> names;
        const char* text;
    };
    const std::string set = controls::glyph_set();
    const bool kb = set == "kb";
    const std::vector<Hint> hints = {
        {kb ? std::vector<std::string>{"arrow_up", "arrow_down"} : std::vector<std::string>{"dup", "ddown"}, "Adjust"},
        {{kb ? "enter" : "south"}, "Next"},
        {{kb ? "escape" : "east"}, "Back"},
    };
    const float dp = overlay_dp();
    ImFont* font = g_fps_font != nullptr ? g_fps_font : ImGui::GetFont();
    const float big = 28.f * dp, small = 17.f * dp, icon = 28.f * dp, hint_gap = 22.f * dp;
    const ImVec2 a = font->CalcTextSizeA(big, FLT_MAX, 0.f, head.c_str());
    const ImVec2 b = font->CalcTextSizeA(small, FLT_MAX, 0.f, kHelp[step]);
    float hints_w = 0.f;
    for (const Hint& h : hints)
        hints_w += static_cast<float>(h.names.size()) * (icon + 2.f * dp) + 6.f * dp +
                   font->CalcTextSizeA(small, FLT_MAX, 0.f, h.text).x;
    hints_w += hint_gap * static_cast<float>(hints.size() - 1);
    const ImVec2 c{hints_w, icon};
    const float pad = 16.f * dp, gap = 8.f * dp;
    const float width = std::max({a.x, b.x, c.x}) + 2.f * pad;
    const float height = a.y + b.y + c.y + 2.f * gap + 2.f * pad;
    const ImVec2 screen = ImGui::GetMainViewport()->Size;
    const ImVec2 at{(screen.x - width) / 2.f, screen.y - height - 40.f * dp};
    ImDrawList* draw = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    draw->AddRectFilled(at, ImVec2{at.x + width, at.y + height}, IM_COL32(0, 0, 0, 0xff), 6.f * dp);
    float y = at.y + pad;
    draw->AddText(font, big, ImVec2{at.x + (width - a.x) / 2.f, y}, IM_COL32(0xf0, 0xe6, 0xc8, 0xff), head.c_str());
    y += a.y + gap;
    draw->AddText(font, small, ImVec2{at.x + (width - b.x) / 2.f, y}, IM_COL32(0xd8, 0xd0, 0xb8, 0xff), kHelp[step]);
    y += b.y + gap;
    float x = at.x + (width - c.x) / 2.f;
    for (const Hint& h : hints) {
        for (const std::string& name : h.names) {
            if (ImTextureID id = glyph(set, name); id != 0)
                draw->AddImage(id, ImVec2{x, y}, ImVec2{x + icon, y + icon});
            x += icon + 2.f * dp;
        }
        const ImVec2 t = font->CalcTextSizeA(small, FLT_MAX, 0.f, h.text);
        draw->AddText(font, small, ImVec2{x + 6.f * dp, y + (icon - t.y) / 2.f}, IM_COL32(0xa0, 0x98, 0x88, 0xff), h.text);
        x += 6.f * dp + t.x + hint_gap;
    }
}

// ED_HDR_CALIBRATION_TEST=seconds,step (testing): calibration opens on its
// own at that step after that many seconds and closes 4 s later.
Uint64 g_calibration_test_until = 0;

void run_hdr_calibration(int first_step = 0) {
    static const int kPatterns[3] = {1, 2, 3};
    g_doc->Hide();
    bool pressed[kButtonCount];
    g_input.poll(pressed);
    int step = first_step;
    bool open = true;
    while (open) {
        if (g_calibration_test_until != 0 && SDL_GetTicks() >= g_calibration_test_until) {
            g_calibration_test_until = 0;
            break;
        }
        dol_aurora_set_hdr_pattern(kPatterns[step]);
        g_input.poll(pressed);
        const int dir = pressed[kUp] ? 1 : pressed[kDown] ? -1 : 0;
        if (dir != 0) {
            if (step == 0)
                g_settings.hdr_peak = std::clamp(g_settings.hdr_peak + dir * 50, 100, 4000);
            else if (step == 1)
                g_settings.hdr_paper = std::clamp(g_settings.hdr_paper + dir * 10, 60, 400);
            else
                g_settings.hdr_black = std::clamp(g_settings.hdr_black + dir, -5, 10);
            settings_apply_graphics();
        }
        if (pressed[kAccept] && ++step == 3)
            break;
        if (pressed[kBack] && step-- == 0)
            break;
        draw_hdr_calibration_caption(step);
        open = dol_aurora_ui_frame();
    }
    dol_aurora_set_hdr_pattern(0);
    settings_save();
    if (open)
        g_doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
}

}  // namespace

extern "C" void frontend_cycle_enhancement(int which) {
    using namespace enhancement;
    if (which == kCount || which == kCount + 1) {
        // Before and after: off is the game as the GameCube showed it (its
        // own 528 lines, the disc's light maps and textures); on is Gary's set
        // (the defaults: HDR, Soft occlusion, sharp shadows, HD textures, no
        // glow) at the chosen resolution. The resolution is not saved.
        const bool on = which == kCount + 1;
        g_settings.hdr = on;
        g_settings.glow = 0;  // not in Gary's set either way
        g_settings.ssao = on ? 1 : 0;
        g_settings.hd_textures = on;
        g_settings.sharp_shadows = on;
        settings_apply_graphics();
        static const int kRenderLines[] = {0, 720, 1080, 1440, 2160};
        dol_aurora_set_render_height(on ? kRenderLines[g_settings.render_scale] : 528);
        settings_save();
        std::fprintf(stderr, "[frontend] enhancements %s\n", on ? "on" : "off");
        std::lock_guard<std::mutex> lock(g_notice_mutex);
        g_notice = on ? "Enhanced: HDR, soft occlusion, sharp shadows, HD textures"
                      : "Original: the GameCube's resolution, shadows and textures";
        g_notice_at = SDL_GetTicks();
        return;
    }
    if (which < 0 || which >= kCount)
        return;
    if (available(which)) {
        change(which, +1);
        settings_save();
    }
    std::fprintf(stderr, "[frontend] %s: %s\n", kNames[which], value(which).c_str());
    std::lock_guard<std::mutex> lock(g_notice_mutex);
    g_notice = std::string(kNames[which]) + ": " + value(which);
    g_notice_at = SDL_GetTicks();
}

// --- Photo mode ---------------------------------------------------------------
// Hold Back (P on the keyboard): the game pauses on its frame and a free camera
// looks round it (aurora_photo_*): the left stick moves, the right stick
// looks, LB/RB go down and up, LT/RT zoom, R3 puts the camera back. A HDR,
// B glow, X HD textures, Y shadows, L3 ambient occlusion; the D-pad's up and
// down the resolution, left and right the aspect. Start takes a photo (a PNG
// in Pictures/Eternal Darkness), a tap of Back hides the legend, holding it
// again leaves. Resolution, aspect, HD textures and shadows are drawn by the
// game: it runs one frame for them, with no input, and is held again.
extern "C" void audio_set_muted(bool muted);

namespace photo {
enum class State { Off, Waiting, On };
State g_state = State::Off;
// The free camera, level with the world: its heading (left positive) and
// elevation from the game camera's, a roll (R3 held), its position and zoom.
// g_up: the world's up in the game camera's view space (camera_view_up).
float g_heading = 0.f, g_elevation = 0.f, g_roll = 0.f, g_pos[3]{0.f, 0.f, 0.f}, g_zoom = 1.f;
float g_up[3]{0.f, 1.f, 0.f};
bool g_legend = true;
int g_resolution = 0;  // into kLines
int g_aspect = 0;      // into kAspects
std::string g_notice;
Uint64 g_notice_at = 0;
unsigned g_saved = 0;
bool g_saving = false;
std::string g_last_saved;
// The graphics settings as they were: photo mode's changes are its own, put
// back when it ends and never saved.
struct Saved {
    bool hdr, hd_textures, sharp_shadows;
    int glow, ssao;
};
Saved g_before{};
// HD textures were turned on or off: the pack reads its images on threads,
// the originals showing until they are ready, so the game draws until it is
// done (up to g_textures_until).
bool g_wait_textures = false;
Uint64 g_textures_until = 0;

constexpr int kLines[] = {720, 1080, 1440, 2160};
constexpr const char* kLineNames[] = {"720p", "1080p", "1440p", "4K"};
struct Aspect {
    const char* name;
    float value;  // 0: the game's own
};
constexpr Aspect kAspects[] = {{"Game", 0.f},        {"4:3", 4.f / 3.f}, {"16:9", 16.f / 9.f},
                               {"21:9", 21.f / 9.f}, {"1:1", 1.f},       {"4:5", 4.f / 5.f}};
constexpr int kAspectCount = sizeof kAspects / sizeof kAspects[0];

void notice(std::string text) {
    g_notice = std::move(text);
    g_notice_at = SDL_GetTicks();
}

// Pictures/Eternal Darkness (made if missing), else a folder beside the game.
std::string folder() {
    std::string dir;
    if (const char* pictures = SDL_GetUserFolder(SDL_FOLDER_PICTURES); pictures != nullptr && pictures[0] != '\0') {
        dir = std::string(pictures);
        if (dir.back() != '/' && dir.back() != '\\')
            dir += '/';
        dir += "Eternal Darkness";
    } else {
        dir = "Eternal Darkness Photos";
    }
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    return dir;
}

float g_elevation0 = 0.f;  // the game camera's own elevation

void reset_view() {
    camera_view_up(g_up);
    const float n = std::sqrt(g_up[0] * g_up[0] + g_up[1] * g_up[1] + g_up[2] * g_up[2]);
    if (n > 1e-4f)
        for (float& c : g_up)
            c /= n;
    else
        g_up[0] = 0.f, g_up[1] = 1.f, g_up[2] = 0.f;
    // Ahead is -z: its elevation is asin(ahead . up).
    g_elevation0 = std::asin(std::clamp(-g_up[2], -1.f, 1.f));
    g_heading = g_roll = 0.f;
    g_elevation = g_elevation0;
    g_pos[0] = g_pos[1] = g_pos[2] = 0.f;
    g_zoom = 1.f;
}

using V3 = std::array<float, 3>;
V3 add3(V3 a, V3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 mul3(V3 a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
float dot3(V3 a, V3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross3(V3 a, V3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
V3 unit3(V3 a) {
    const float n = std::sqrt(dot3(a, a));
    return n > 1e-6f ? mul3(a, 1.f / n) : a;
}

// The level frame: the game camera's ahead and right flattened onto the
// floor (h0, r0), and the world's up.
struct Frame {
    V3 up, h0, r0;
};
Frame level_frame() {
    const V3 up{g_up[0], g_up[1], g_up[2]};
    V3 h = add3(V3{0.f, 0.f, -1.f}, mul3(up, -dot3(V3{0.f, 0.f, -1.f}, up)));
    if (dot3(h, h) < 1e-6f)  // looking straight down or up: the camera's up flattened
        h = add3(V3{0.f, 1.f, 0.f}, mul3(up, -dot3(V3{0.f, 1.f, 0.f}, up)));
    h = unit3(h);
    return {up, h, unit3(cross3(h, up))};
}

int current_lines() {
    static const int kRenderLines[] = {0, 720, 1080, 1440, 2160};
    return kRenderLines[std::clamp(g_settings.render_scale, 0, 4)];
}

struct Row {
    std::vector<std::string> pad, keys;  // glyph files: the controller's, the keyboard's
    std::string text;
};

// See-through grey (with HDR on, the brightest lamps show through a little).
constexpr ImU32 kPanel = IM_COL32(0x22, 0x22, 0x24, 0xb8);

// The legend in the bottom-left corner (two columns of button pictures and
// what they do) and a notice over the top.
void draw() {
    if (ImGui::GetCurrentContext() == nullptr)
        return;
    const float dp = overlay_dp();
    ImFont* font = g_fps_font != nullptr ? g_fps_font : ImGui::GetFont();
    ImDrawList* list = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    const ImVec2 screen = ImGui::GetMainViewport()->Size;
    const ImU32 ink = IM_COL32(0xf0, 0xe6, 0xc8, 0xff), dim = IM_COL32(0xc8, 0xbe, 0xa4, 0xff);
    const Uint64 now = SDL_GetTicks();
    if (!g_notice.empty() && now - g_notice_at < 2500) {
        const float size = 22.f * dp;
        const ImVec2 t = font->CalcTextSizeA(size, FLT_MAX, 0.f, g_notice.c_str());
        const ImVec2 pad{14.f * dp, 8.f * dp};
        const ImVec2 at{(screen.x - t.x) / 2.f - pad.x, 24.f * dp};
        list->AddRectFilled(at, ImVec2{at.x + t.x + 2.f * pad.x, at.y + t.y + 2.f * pad.y}, kPanel, 6.f * dp);
        list->AddText(font, size, ImVec2{at.x + pad.x, at.y + pad.y}, ink, g_notice.c_str());
    }
    if (!g_legend)
        return;
    using namespace enhancement;
    const std::string set = controls::glyph_set();
    const bool kb = set == "kb";
    const auto on = [](bool b) { return std::string(b ? "On" : "Off"); };
    const std::vector<Row> settings = {
        {{"south"}, {"1"}, "HDR: " + (available(kHdr) ? on(g_settings.hdr) : std::string("Not on this display"))},
        {{"east"}, {"2"}, "Glow: " + value(kGlow)},
        {{"west"}, {"3"}, "HD textures: " + (available(kHdTextures) ? on(g_settings.hd_textures) : std::string("No pack"))},
        {{"north"}, {"4"}, "Shadows: " + value(kShadows)},
        {{"l3"}, {"5"}, "Ambient occlusion: " + value(kOcclusion)},
        {{"dup", "ddown"}, {"page_up", "page_down"}, std::string("Resolution: ") + kLineNames[g_resolution]},
        {{"dleft", "dright"}, {"home", "end"}, std::string("Ratio: ") + kAspects[g_aspect].name},
    };
    const std::vector<Row> camera = {
        {{"stick_l"}, {"w", "a", "s", "d"}, "Move"},
        {{"stick_r"}, {"arrows"}, "Look"},
        {{"lb", "rb"}, {"q", "e"}, "Down / up"},
        {{"lt", "rt"}, {"z", "c"}, "Zoom out / in"},
        {{"r3"}, {"r"}, "Reset \u00b7 hold + look to tilt"},
        {{"start"}, {"enter"}, "Take photo"},
        {{"back"}, {"h"}, kb ? "Hide this" : "Hide this \u00b7 hold to leave"},
    };
    std::vector<Row> right = camera;
    if (kb)
        right.push_back({{}, {"p"}, "Leave photo mode"});

    const float size = 18.f * dp, title = 22.f * dp, icon = 30.f * dp, gap = 4.f * dp, row_h = icon + gap;
    const ImVec2 pad{16.f * dp, 12.f * dp};
    const auto column_width = [&](const std::vector<Row>& rows) {
        float w = 0.f;
        for (const Row& r : rows) {
            const size_t n = kb ? r.keys.size() : r.pad.size();
            const float text = font->CalcTextSizeA(size, FLT_MAX, 0.f, r.text.c_str()).x;
            w = std::max(w, static_cast<float>(n) * (icon + 2.f * dp) + 8.f * dp + text);
        }
        return w;
    };
    const float left_w = column_width(settings), right_w = column_width(right), col_gap = 28.f * dp;
    const float head_h = font->CalcTextSizeA(title, FLT_MAX, 0.f, "Photo mode").y + 6.f * dp;
    const float body_h = static_cast<float>(std::max(settings.size(), right.size())) * row_h;
    const float width = left_w + col_gap + right_w + 2.f * pad.x, height = head_h + body_h + 2.f * pad.y;
    const ImVec2 at{24.f * dp, screen.y - 24.f * dp - height};
    list->AddRectFilled(at, ImVec2{at.x + width, at.y + height}, kPanel, 8.f * dp);
    list->AddText(font, title, ImVec2{at.x + pad.x, at.y + pad.y}, ink, "Photo mode");
    const auto column = [&](const std::vector<Row>& rows, float x) {
        float y = at.y + pad.y + head_h;
        for (const Row& r : rows) {
            float cx = x;
            for (const std::string& name : kb ? r.keys : r.pad) {
                if (ImTextureID id = glyph(set, name); id != 0)
                    list->AddImage(id, ImVec2{cx, y}, ImVec2{cx + icon, y + icon});
                cx += icon + 2.f * dp;
            }
            const float th = font->CalcTextSizeA(size, FLT_MAX, 0.f, r.text.c_str()).y;
            list->AddText(font, size, ImVec2{cx + 6.f * dp, y + (icon - th) / 2.f}, dim, r.text.c_str());
            y += row_h;
        }
    };
    column(settings, at.x + pad.x);
    column(right, at.x + pad.x + left_w + col_gap);
}

void begin() {
    if (g_state != State::Off)
        return;
    reset_view();
    g_legend = true;
    g_aspect = 0;
    const int lines = current_lines();
    g_resolution = 1;
    for (int i = 0; i < 4; i++)
        if (kLines[i] == lines)
            g_resolution = i;
    g_saved = aurora_photo_saved();
    g_saving = false;
    g_before = {g_settings.hdr, g_settings.hd_textures, g_settings.sharp_shadows, g_settings.glow, g_settings.ssao};
    g_state = State::Waiting;
    audio_set_muted(true);
    aurora_photo_begin();
    std::fprintf(stderr, "[photo] on\n");
}

void end() {
    aurora_photo_end();
    dol_aurora_set_photo_aspect(0.f);
    dol_aurora_set_render_height(current_lines());
    const bool changed = g_settings.hdr != g_before.hdr || g_settings.hd_textures != g_before.hd_textures ||
                         g_settings.sharp_shadows != g_before.sharp_shadows || g_settings.glow != g_before.glow ||
                         g_settings.ssao != g_before.ssao;
    g_settings.hdr = g_before.hdr;
    g_settings.hd_textures = g_before.hd_textures;
    g_settings.sharp_shadows = g_before.sharp_shadows;
    g_settings.glow = g_before.glow;
    g_settings.ssao = g_before.ssao;
    if (changed)
        settings_apply_graphics();
    audio_set_muted(false);
    g_state = State::Off;
    std::fprintf(stderr, "[photo] off\n");
}

// ED_PHOTO_TEST=<seconds>: photo mode by itself after that long (glow, move,
// look, a photo, 21:9, HD textures off and on, a photo, leave), for tests
// with no input.
struct Test {
    bool on = false;
    Uint64 enter_at = 0, began = 0;
    int step = 0;
};
Test g_test;

enum class Exit { Leave, Step, Quit };

constexpr int kKeys[] = {SDL_SCANCODE_RETURN, SDL_SCANCODE_H,      SDL_SCANCODE_P,        SDL_SCANCODE_1,
                         SDL_SCANCODE_2,      SDL_SCANCODE_3,      SDL_SCANCODE_4,        SDL_SCANCODE_5,
                         SDL_SCANCODE_R,      SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_HOME,
                         SDL_SCANCODE_END};

// Paused at the game's frame: the camera, the settings, the legend.
Exit run() {
    SDL_Gamepad* pad = gamepad();
    bool was[SDL_GAMEPAD_BUTTON_COUNT]{};
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
        was[b] = pad != nullptr && SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b));
    static bool key_was[SDL_SCANCODE_COUNT];
    for (int k : kKeys)
        key_was[k] = key(k);
    // Back held from before (the hold that opened it): wait for its release.
    bool back_armed = !(pad != nullptr && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK));
    Uint64 back_since = 0;
    Uint64 last = SDL_GetTicksNS();
    while (true) {
        pad = gamepad();
        const Uint64 now_ns = SDL_GetTicksNS();
        const float dt = std::min(static_cast<float>(now_ns - last) * 1e-9f, 0.1f);
        last = now_ns;
        const auto button = [&](SDL_GamepadButton b) { return pad != nullptr && SDL_GetGamepadButton(pad, b); };
        bool pressed[SDL_GAMEPAD_BUTTON_COUNT]{};
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
            const bool down = button(static_cast<SDL_GamepadButton>(b));
            pressed[b] = down && !was[b];
            was[b] = down;
        }
        bool key_pressed[SDL_SCANCODE_COUNT]{};
        for (int k : kKeys) {
            const bool down = key(k);
            key_pressed[k] = down && !key_was[k];
            key_was[k] = down;
        }
        const auto axis = [&](SDL_GamepadAxis a) {
            if (pad == nullptr)
                return 0.f;
            const float v = SDL_GetGamepadAxis(pad, a) / 32767.f;
            return std::fabs(v) < 0.15f ? 0.f : v;
        };
        float mx = axis(SDL_GAMEPAD_AXIS_LEFTX), mz = -axis(SDL_GAMEPAD_AXIS_LEFTY);
        float lx = axis(SDL_GAMEPAD_AXIS_RIGHTX), ly = -axis(SDL_GAMEPAD_AXIS_RIGHTY);
        float my = (button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ? 1.f : 0.f) -
                   (button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ? 1.f : 0.f);
        float zoom = std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0.f) -
                     std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0.f);
        // The keyboard: WASD move, arrows look, Q/E down/up, Z/C zoom.
        mx += (key(SDL_SCANCODE_D) ? 1.f : 0.f) - (key(SDL_SCANCODE_A) ? 1.f : 0.f);
        mz += (key(SDL_SCANCODE_W) ? 1.f : 0.f) - (key(SDL_SCANCODE_S) ? 1.f : 0.f);
        my += (key(SDL_SCANCODE_E) ? 1.f : 0.f) - (key(SDL_SCANCODE_Q) ? 1.f : 0.f);
        lx += (key(SDL_SCANCODE_RIGHT) ? 1.f : 0.f) - (key(SDL_SCANCODE_LEFT) ? 1.f : 0.f);
        ly += (key(SDL_SCANCODE_UP) ? 1.f : 0.f) - (key(SDL_SCANCODE_DOWN) ? 1.f : 0.f);
        zoom += (key(SDL_SCANCODE_C) ? 1.f : 0.f) - (key(SDL_SCANCODE_Z) ? 1.f : 0.f);

        bool take = pressed[SDL_GAMEPAD_BUTTON_START] || key_pressed[SDL_SCANCODE_RETURN];
        bool hdr = pressed[SDL_GAMEPAD_BUTTON_SOUTH] || key_pressed[SDL_SCANCODE_1];
        bool glow = pressed[SDL_GAMEPAD_BUTTON_EAST] || key_pressed[SDL_SCANCODE_2];
        bool textures = pressed[SDL_GAMEPAD_BUTTON_WEST] || key_pressed[SDL_SCANCODE_3];
        bool shadows = pressed[SDL_GAMEPAD_BUTTON_NORTH] || key_pressed[SDL_SCANCODE_4];
        bool occlusion = pressed[SDL_GAMEPAD_BUTTON_LEFT_STICK] || key_pressed[SDL_SCANCODE_5];
        // R3 (or R): a tap resets the camera; held, the look turns into a tilt.
        bool reset = false;
        const bool tilt_held = button(SDL_GAMEPAD_BUTTON_RIGHT_STICK) || key(SDL_SCANCODE_R);
        static bool tilt_was = false, tilted = false;
        if (tilt_held && !tilt_was)
            tilted = false;
        if (!tilt_held && tilt_was && !tilted)
            reset = true;
        tilt_was = tilt_held;
        int res = (pressed[SDL_GAMEPAD_BUTTON_DPAD_UP] || key_pressed[SDL_SCANCODE_PAGEUP] ? 1 : 0) -
                  (pressed[SDL_GAMEPAD_BUTTON_DPAD_DOWN] || key_pressed[SDL_SCANCODE_PAGEDOWN] ? 1 : 0);
        int ratio = (pressed[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] || key_pressed[SDL_SCANCODE_END] ? 1 : 0) -
                    (pressed[SDL_GAMEPAD_BUTTON_DPAD_LEFT] || key_pressed[SDL_SCANCODE_HOME] ? 1 : 0);
        int ratio_to = -1;
        bool hide = key_pressed[SDL_SCANCODE_H];
        bool leave = key_pressed[SDL_SCANCODE_P];
        // Back: a tap hides the legend, a hold leaves.
        const bool back = button(SDL_GAMEPAD_BUTTON_BACK);
        if (!back_armed) {
            back_armed = !back;
        } else if (back) {
            if (back_since == 0)
                back_since = SDL_GetTicks();
            if (SDL_GetTicks() - back_since >= 500)
                leave = true;
        } else if (back_since != 0) {
            hide = true;
            back_since = 0;
        }

        // The test script.
        if (g_test.on) {
            const Uint64 t = SDL_GetTicks() - g_test.began;
            const int step = g_test.step;
            if (step == 0 && t > 1000) {
                glow = true;  // for the photo only: put back at the end
                g_test.step++;
            } else if (step == 1 && t > 1500) {
                mz = 1.f;
                if (t > 2500)
                    g_test.step++;
            } else if (step == 2) {
                lx = 1.f;
                if (t > 3200)
                    g_test.step++;
            } else if (step == 3 && t > 3600) {
                take = true;
                g_test.step++;
            } else if (step == 4 && t > 5000) {
                ratio_to = 3;  // 21:9
                g_test.step++;
            } else if (step == 5 && t > 6500) {
                textures = true;  // off
                shadows = true;
                g_test.step++;
            } else if (step == 6 && t > 8000) {
                textures = true;  // on again: read back from the pack
                shadows = true;   // the sharp light maps made again
                g_test.step++;
            } else if (step == 7 && t > 9500) {
                take = true;
                g_test.step++;
            } else if (step == 8 && t > 10500) {
                leave = true;
                g_test.step++;
            }
        }

        using namespace enhancement;
        bool step = false;
        // For the photo only: put back as it was when photo mode ends (end),
        // never saved.
        const auto toggle = [](int which) {
            if (!available(which))
                return;
            change(which, +1);
            notice(std::string(kNames[which]) + ": " + value(which));
        };
        if (hdr)
            toggle(kHdr);
        if (glow)
            toggle(kGlow);
        if (occlusion)
            toggle(kOcclusion);
        if (textures) {
            toggle(kHdTextures);
            g_wait_textures = true;
            g_textures_until = 0;
            step = true;
        }
        if (shadows) {
            toggle(kShadows);
            g_wait_textures = true;  // the sharp light maps are made on a worker
            g_textures_until = 0;
            step = true;
        }
        if (reset) {
            reset_view();
            notice("Camera back to the game's");
        }
        if (hide)
            g_legend = !g_legend;
        if (res != 0) {
            g_resolution = std::clamp(g_resolution + res, 0, 3);
            dol_aurora_set_render_height(kLines[g_resolution]);
            notice(std::string("Resolution: ") + kLineNames[g_resolution]);
            step = true;
        }
        if (ratio != 0 || ratio_to >= 0) {
            g_aspect = ratio_to >= 0 ? ratio_to : (g_aspect + ratio + kAspectCount) % kAspectCount;
            dol_aurora_set_photo_aspect(kAspects[g_aspect].value);
            notice(std::string("Ratio: ") + kAspects[g_aspect].name);
            step = true;
        }
        if (take) {
            char stamp[64];
            const std::time_t now = std::time(nullptr);
            std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H.%M.%S", std::localtime(&now));
            g_last_saved = folder() + "/Eternal Darkness " + stamp + ".png";
            aurora_photo_save(g_last_saved.c_str());
            g_saving = true;
            notice("Taking photo...");
        }
        if (g_saving && aurora_photo_saved() != g_saved) {
            g_saved = aurora_photo_saved();
            g_saving = false;
            notice("Saved to Pictures/Eternal Darkness");
        }

        // The camera, level with the world: look turns about the world's up
        // and tips up and down (R3 held: tilts instead); the left stick moves
        // along the floor, LB/RB straight up and down (game units: about 100
        // a metre); zoom 0.3x to 4x.
        if (tilt_held) {
            if (lx != 0.f)
                tilted = true;
            g_roll -= lx * 1.2f * dt;
        } else {
            g_heading -= lx * 1.8f * dt;
            g_elevation = std::clamp(g_elevation + ly * 1.4f * dt, -1.5f, 1.5f);
        }
        g_zoom = std::clamp(g_zoom * std::exp(zoom * 1.2f * dt), 0.3f, 4.f);
        const Frame fr = level_frame();
        const float ch = std::cos(g_heading), sh = std::sin(g_heading);
        const V3 level_ahead = add3(mul3(fr.h0, ch), mul3(fr.r0, -sh));
        const V3 level_right = add3(mul3(fr.r0, ch), mul3(fr.h0, sh));
        const V3 ahead = add3(mul3(level_ahead, std::cos(g_elevation)), mul3(fr.up, std::sin(g_elevation)));
        const V3 up0 = cross3(level_right, ahead);
        const float cr = std::cos(g_roll), sr = std::sin(g_roll);
        const V3 right = add3(mul3(level_right, cr), mul3(up0, sr));
        const V3 up = add3(mul3(up0, cr), mul3(level_right, -sr));
        const float speed = 180.f / g_zoom;
        const V3 move = add3(add3(mul3(level_right, mx), mul3(level_ahead, mz)), mul3(fr.up, my));
        for (int i = 0; i < 3; i++)
            g_pos[i] += move[i] * speed * dt;
        // Row-major, columns right, up, back.
        const float rot[9]{right[0], up[0], -ahead[0], right[1], up[1], -ahead[1], right[2], up[2], -ahead[2]};
        aurora_photo_set_view(rot, g_pos[0], g_pos[1], g_pos[2], g_zoom, dol_aurora_display_aspect(), true);

        if (leave)
            return Exit::Leave;
        if (step) {
            // Two frames: a new size is only drawn from the frame after.
            aurora_photo_rehold(1);
            return Exit::Step;
        }
        draw();
        if (!dol_aurora_ui_frame())
            return Exit::Quit;
    }
}

// At each frame boundary: enter (once held), run, step or leave.
void frame(bool start) {
    static bool checked = false;
    if (!checked) {
        checked = true;
        if (const char* t = std::getenv("ED_PHOTO_TEST"); t != nullptr && std::atoi(t) > 0) {
            g_test.on = true;
            g_test.enter_at = SDL_GetTicks() + static_cast<Uint64>(std::atoi(t)) * 1000u;
        }
    }
    if (g_test.on && g_test.enter_at != 0 && SDL_GetTicks() >= g_test.enter_at) {
        g_test.enter_at = 0;
        start = true;
    }
    if (start && g_state == State::Off) {
        begin();
        return;  // the game draws the frame to hold
    }
    if (g_state != State::Waiting)
        return;
    // The held frame is finished on the render thread a moment after the
    // game hands it over.
    for (int i = 0; i < 50 && !aurora_photo_ready(); i++)
        SDL_Delay(2);
    if (!aurora_photo_ready())
        return;
    if (g_wait_textures) {
        static int frames = 0;
        if (g_textures_until == 0) {
            g_textures_until = SDL_GetTicks() + 3000;
            frames = 0;
            std::fprintf(stderr, "[photo] textures: %u waiting to upload, %u light maps being made%s\n",
                         aurora_texture_pack_pending(), aurora_texture_override_pending(),
                         texpack_busy() ? ", the pack still reading" : "");
        }
        const bool busy =
            texpack_busy() || aurora_texture_pack_pending() != 0 || aurora_texture_override_pending() != 0;
        if (busy && SDL_GetTicks() < g_textures_until) {
            frames++;
            aurora_photo_rehold(0);  // another frame, still black and silent
            return;
        }
        std::fprintf(stderr, "[photo] textures %s after %d more frame(s) (%u to upload, %u light maps%s)\n",
                     busy ? "still loading" : "loaded", frames, aurora_texture_pack_pending(),
                     aurora_texture_override_pending(), texpack_busy() ? ", the pack reading" : "");
        g_wait_textures = false;
    }
    if (g_test.on && g_test.began == 0)
        g_test.began = SDL_GetTicks();
    g_state = State::On;
    report_pause(true);
    const Exit exit = run();
    report_pause(false);
    if (exit == Exit::Step) {
        g_state = State::Waiting;
        return;
    }
    end();
    if (exit == Exit::Quit)
        dol_aurora_request_quit();
}
}  // namespace photo

extern "C" bool frontend_photo_active(void) { return photo::g_state != photo::State::Off; }

extern "C" void frontend_frame(void) {
    static Uint64 calibration_test_at = 0;
    static int calibration_test_step = 0;
    static bool calibration_test_checked = false;
    if (!calibration_test_checked) {
        calibration_test_checked = true;
        unsigned long after = 0;
        int step = 0;
        if (const char* t = std::getenv("ED_HDR_CALIBRATION_TEST"); t && std::sscanf(t, "%lu,%d", &after, &step) >= 1) {
            calibration_test_at = SDL_GetTicks() + after * 1000u;
            calibration_test_step = std::clamp(step, 0, 2);
        }
    }
    if (calibration_test_at != 0 && SDL_GetTicks() >= calibration_test_at && ensure_ui()) {
        calibration_test_at = 0;
        g_calibration_test_until = SDL_GetTicks() + 4000;
        run_hdr_calibration(calibration_test_step);
    }
    update_fps_counter();
    update_enhancement_notice();
    update_fast_forward_prompt();
    update_cursor();
    static bool menu_was_held = false;
    bool held[kButtonCount];
    read_buttons(held);
    // Back: a tap opens the menu (on its release), held half a second photo
    // mode; Escape opens the menu at once, P photo mode.
    static Uint64 back_since = 0;
    static bool back_photo = false;
    const bool back = held[kMenu];
    bool photo_start = false, back_tap = false;
    if (back && back_since == 0) {
        back_since = SDL_GetTicks();
        back_photo = false;
    }
    if (back && !back_photo && SDL_GetTicks() - back_since >= 500) {
        back_photo = true;
        photo_start = true;
    }
    if (!back && back_since != 0) {
        back_tap = !back_photo;
        back_since = 0;
    }
    static bool p_was = false;
    const bool p = key(SDL_SCANCODE_P);
    if (p && !p_was)
        photo_start = true;
    p_was = p;
    photo::frame(photo_start && photo::g_state == photo::State::Off);
    if (photo::g_state != photo::State::Off) {
        menu_was_held = true;
        std::fill(std::begin(g_key_pressed), std::end(g_key_pressed), false);
        return;
    }
    const bool menu_held = key(SDL_SCANCODE_ESCAPE);
    const bool press = (menu_held && !menu_was_held) || g_key_pressed[kMenu] || back_tap;
    menu_was_held = menu_held;
    std::fill(std::begin(g_key_pressed), std::end(g_key_pressed), false);
    static Uint64 test_open_at = 0, test_hold = 0, test_every = 0;
    static bool test_checked = false;
    if (!test_checked) {
        test_checked = true;
        unsigned long after = 0, hold = 0, every = 0;
        if (const char* t = std::getenv("ED_MENU_TEST"); t && std::sscanf(t, "%lu,%lu,%lu", &after, &hold, &every) >= 2) {
            test_open_at = SDL_GetTicks() + after * 1000u;
            test_hold = hold;
            test_every = every * 1000u;
        }
    }
    bool test_press = false;
    if (test_open_at != 0 && SDL_GetTicks() >= test_open_at) {
        test_open_at = test_every != 0 ? SDL_GetTicks() + test_hold + test_every : 0;
        test_press = true;
        g_test_close_at = SDL_GetTicks() + test_hold;
        std::fprintf(stderr, "[frontend] test menu open for %llu ms\n", (unsigned long long)test_hold);
    }
    if ((!press && !test_press) || !ensure_ui())
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

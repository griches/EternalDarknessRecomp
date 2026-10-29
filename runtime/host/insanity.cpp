// The recomp's own insanity effects (insanity.h): fake system screens of the
// machine the game runs on, drawn through ImGui over the game as the FPS
// counter is, and our menu's save states wiped. A modal one holds the game
// while it plays, as our menu does (dol_aurora_ui_frame shows the held
// picture under it); the game's own "This isn't happening!" follows each.
#include "host/insanity.h"

#include "gxruntime/aurora_backend.h"
#include "host/buttons.h"
#include "host/frontend.h"
#include "host/report.h"

#include <aurora/imgui.h>
#include <imgui.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern "C" void hle_insanity_recovered(void);
extern "C" int hle_game_mode(void);
// audio.c: a sound of ours mixed into the game's as it goes out.
extern "C" void audio_play_host_sound(const int16_t* samples, uint32_t count, uint32_t rate, float volume);
// audio.c: the last of the game's sound, stuck and repeating (a hung machine).
extern "C" void audio_play_stuck(double loop_seconds, double seconds);

namespace insanity {
namespace {

constexpr float kPi = 3.14159265f;

// --- The machine -------------------------------------------------------------

enum Platform : unsigned { kMac = 1u, kWindows = 2u, kLinux = 4u, kDeck = 8u, kPS5 = 16u, kEvery = 31u };

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// The machine the game runs on; ED_INSANITY_PLATFORM (mac, windows, linux,
// deck, ps5) shows another's effects, to see them on this one.
Platform detect_platform() {
    if (const char* p = std::getenv("ED_INSANITY_PLATFORM"); p != nullptr) {
        static const std::pair<const char*, Platform> kNames[] = {
            {"mac", kMac}, {"windows", kWindows}, {"linux", kLinux}, {"deck", kDeck}, {"ps5", kPS5}};
        for (const auto& [name, platform] : kNames)
            if (std::strcmp(p, name) == 0)
                return platform;
    }
#if defined(__PROSPERO__)
    return kPS5;
#elif defined(__APPLE__)
    return kMac;
#elif defined(_WIN32)
    return kWindows;
#else
    // SteamOS (a Deck, or Steam's own session) or a desktop's Linux.
    if (const char* deck = std::getenv("SteamDeck"); deck != nullptr && std::strcmp(deck, "1") == 0)
        return kDeck;
    if (FILE* f = std::fopen("/etc/os-release", "r")) {
        char line[256];
        bool steamos = false;
        while (std::fgets(line, sizeof line, f) != nullptr)
            if (std::strncmp(line, "ID=steamos", 10) == 0)
                steamos = true;
        std::fclose(f);
        if (steamos)
            return kDeck;
    }
    return kLinux;
#endif
}

Platform platform() {
    static const Platform p = detect_platform();
    return p;
}

// --- Faces -------------------------------------------------------------------

// The system's own, where it has them (the boxes look like its own), else the
// menus' Fira Sans; mono for console text.
struct Faces {
    ImFont* ui = nullptr;
    ImFont* light = nullptr;    // the PS5's thin system type
    ImFont* regular = nullptr;  // and its body text
    ImFont* bold = nullptr;
    ImFont* mono = nullptr;
    ImFont* menu = nullptr;  // our menu's faces (menu.rcss), for the save states
    ImFont* menu_title = nullptr;
} g_faces;

// Our screens are laid out for 1080 lines and drawn at that times the screen's
// height (2 at 4K), so the faces are rasterized that much finer too, not
// stretched from 1080p's: the output's height in pixels over 1080, 1 to 4.
float raster_density() {
    float h = 1080.f;
    if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay()); m != nullptr)
        h = static_cast<float>(m->h) * (m->pixel_density > 0.f ? m->pixel_density : 1.f);
    return std::clamp(std::ceil(h / 1080.f), 1.f, 4.f);
}

ImFont* add_face(const std::vector<std::pair<std::string, int>>& choices, float size) {
    static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x2122, 0x2122, 0x2190, 0x2193, 0};
    static const float density = raster_density();
    for (const auto& [path, index] : choices)
        if (exists(path)) {
            ImFontConfig config;
            config.FontNo = index;
            config.RasterizerDensity = density;
            config.OversampleH = density >= 2.f ? 1 : 2;
            if (ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), size, &config, kRanges))
                return font;
        }
    return nullptr;
}

std::string home() {
    const char* h = std::getenv("HOME");
    return h != nullptr ? h : "";
}

// --- Input -------------------------------------------------------------------

// Any button or key that would answer a box, on its press.
bool any_press() {
    static bool was = false;
    bool now = dol_aurora_key_down(SDL_SCANCODE_RETURN) || dol_aurora_key_down(SDL_SCANCODE_SPACE) ||
               dol_aurora_key_down(SDL_SCANCODE_ESCAPE) || dol_aurora_key_down(SDL_SCANCODE_KP_ENTER);
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count); ids != nullptr) {
        static const SDL_GamepadButton kButtons[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                                     SDL_GAMEPAD_BUTTON_WEST,  SDL_GAMEPAD_BUTTON_NORTH,
                                                     SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK};
        for (int i = 0; i < count; i++)
            if (SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]); pad != nullptr)
                for (SDL_GamepadButton b : kButtons)
                    now = now || SDL_GetGamepadButton(pad, b);
        SDL_free(ids);
    }
    const bool press = now && !was;
    was = now;
    return press;
}

// Left or right, as pressed this frame (-1, +1, 0): the d-pad, the left stick,
// the arrow keys.
int g_test_sideways = 0;  // a test's left or right (run_modal), taken once

int sideways_press() {
    if (const int test = g_test_sideways; test != 0) {
        g_test_sideways = 0;
        return test;
    }
    static int was = 0;
    int now = 0;
    if (dol_aurora_key_down(SDL_SCANCODE_LEFT))
        now = -1;
    if (dol_aurora_key_down(SDL_SCANCODE_RIGHT))
        now = 1;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count); ids != nullptr) {
        for (int i = 0; i < count; i++)
            if (SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]); pad != nullptr) {
                const float x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.f;
                if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -0.5f)
                    now = -1;
                if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > 0.5f)
                    now = 1;
            }
        SDL_free(ids);
    }
    const int press = now != was ? now : 0;
    was = now;
    return press;
}

// --- Drawing -----------------------------------------------------------------

float dp() {
    float scale = 1.f;
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    if (windows != nullptr && count > 0) {
        const float density = SDL_GetWindowPixelDensity(windows[0]);
        const float display = SDL_GetWindowDisplayScale(windows[0]);
        if (density > 0.f && display > 0.f)
            scale = display / density;
    }
    SDL_free(windows);
    return scale;
}

ImDrawList* canvas() { return ImGui::GetForegroundDrawList(ImGui::GetMainViewport()); }
ImVec2 screen() { return ImGui::GetMainViewport()->Size; }

ImU32 rgba(int r, int g, int b, float a) {
    return IM_COL32(r, g, b, static_cast<int>(std::lround(std::clamp(a, 0.f, 1.f) * 255.f)));
}

// A window's soft shadow: rounded rings, fainter outwards.
void shadow(ImDrawList* d, ImVec2 a, ImVec2 b, float round, float spread, float alpha) {
    for (int i = 8; i >= 1; i--) {
        const float grow = spread * i / 8.f;
        d->AddRectFilled(ImVec2{a.x - grow, a.y - grow + spread * 0.3f}, ImVec2{b.x + grow, b.y + grow + spread * 0.3f},
                         rgba(0, 0, 0, alpha / 8.f), round + grow);
    }
}

ImVec2 text_size(ImFont* f, float size, const char* text, float wrap = 0.f) {
    return f->CalcTextSizeA(size, FLT_MAX, wrap, text);
}

// An image from res (0 when it did not load).
ImTextureID picture(const std::string& file) {
    static std::vector<std::pair<std::string, ImTextureID>> loaded;
    for (const auto& [name, id] : loaded)
        if (name == file)
            return id;
    std::vector<uint8_t> pixels;
    uint32_t w = 0, h = 0;
    ImTextureID id = 0;
    if (load_png_rgba(frontend_res_dir() + "/" + file, &pixels, &w, &h))
        id = aurora_imgui_add_texture(w, h, pixels.data());
    loaded.emplace_back(file, id);
    return id;
}

// Where the pointer is, if it is over the window; else the middle.
ImVec2 pointer() {
    float x = 0.f, y = 0.f;
    SDL_GetMouseState(&x, &y);
    const ImVec2 s = screen();
    if (x > 0.f && y > 0.f && x < s.x && y < s.y)
        return ImVec2{x, y};
    return ImVec2{s.x * 0.5f, s.y * 0.5f};
}

// --- Mac ---------------------------------------------------------------------

// The spinning wait cursor: a glossy disc of colour bands turning, the
// picture held still under it, as an app that has stopped answering.
void draw_beach_ball(ImDrawList* d, ImVec2 c, float r, float turn) {
    static const ImU32 kBands[] = {IM_COL32(0x2f, 0x8c, 0xf2, 0xff), IM_COL32(0x2c, 0xc1, 0xd8, 0xff),
                                   IM_COL32(0x5c, 0xc8, 0x3c, 0xff), IM_COL32(0xf6, 0xd3, 0x2b, 0xff),
                                   IM_COL32(0xf5, 0x8f, 0x23, 0xff), IM_COL32(0xea, 0x3e, 0x3e, 0xff),
                                   IM_COL32(0xc2, 0x3f, 0xb8, 0xff), IM_COL32(0x77, 0x4b, 0xd8, 0xff)};
    constexpr int kCount = sizeof kBands / sizeof kBands[0];
    constexpr int kSteps = 10;  // each band's edge as a curve: the swirl
    d->AddCircleFilled(ImVec2{c.x + r * 0.08f, c.y + r * 0.14f}, r * 1.08f, IM_COL32(0, 0, 0, 0x50), 48);
    for (int band = 0; band < kCount; band++) {
        const float from = turn + band * 2.f * kPi / kCount;
        const float to = from + 2.f * kPi / kCount;
        // Each band bends as it goes out from the middle.
        d->PathLineTo(c);
        for (int s = 0; s <= kSteps; s++) {
            const float k = static_cast<float>(s) / kSteps;
            const float a = from + 0.9f * k;
            d->PathLineTo(ImVec2{c.x + std::cos(a) * r * k, c.y + std::sin(a) * r * k});
        }
        for (int s = 0; s <= kSteps; s++) {
            const float a = from + 0.9f + (to - from) * s / kSteps;
            d->PathLineTo(ImVec2{c.x + std::cos(a) * r, c.y + std::sin(a) * r});
        }
        for (int s = kSteps; s >= 0; s--) {
            const float k = static_cast<float>(s) / kSteps;
            const float a = to + 0.9f * k;
            d->PathLineTo(ImVec2{c.x + std::cos(a) * r * k, c.y + std::sin(a) * r * k});
        }
        d->PathFillConcave(kBands[band]);
    }
    // Gloss: a soft light up and left, a darker rim.
    for (int i = 6; i >= 1; i--)
        d->AddCircleFilled(ImVec2{c.x - r * 0.28f, c.y - r * 0.32f}, r * 0.12f * i,
                           IM_COL32(0xff, 0xff, 0xff, 0x0c), 32);
    d->AddCircle(c, r, IM_COL32(0, 0, 0, 0x60), 48, std::max(1.f, r * 0.06f));
}

struct Step {
    double t;      // seconds since it began
    bool first;    // its first frame
    bool press;    // a button or key, pressed this frame
};

bool mac_beach_ball(const Step& s) {
    const float r = 14.f * dp();
    draw_beach_ball(canvas(), pointer(), r, static_cast<float>(s.t * 2.0 * kPi * 0.9));
    return s.t < 5.5;
}

// A push button as the Mac draws one in dark mode (the default one blue).
void mac_button(ImDrawList* d, ImVec2 at, float w, const char* label, bool primary, bool down, float k) {
    const float h = 22.f * k;
    const ImU32 fill = primary ? (down ? rgba(0x00, 0x62, 0xd6, 1.f) : rgba(0x0a, 0x84, 0xff, 1.f))
                               : rgba(0xff, 0xff, 0xff, down ? 0.28f : 0.17f);
    d->AddRectFilled(at, ImVec2{at.x + w, at.y + h}, fill, 5.f * k);
    if (!primary)
        d->AddRect(at, ImVec2{at.x + w, at.y + h}, rgba(0xff, 0xff, 0xff, 0.06f), 5.f * k);
    ImFont* f = g_faces.ui != nullptr ? g_faces.ui : ImGui::GetFont();
    const float size = 13.f * k;
    const ImVec2 ts = text_size(f, size, label);
    d->AddText(f, size, ImVec2{at.x + (w - ts.x) * 0.5f, at.y + (h - ts.y) * 0.5f}, rgba(0xff, 0xff, 0xff, 0.95f),
               label);
}

// The app gone (the picture black, as its full-screen space closes) and the
// crash box: Ignore, Report..., Reopen. Any button: Reopen, and it is back.
bool mac_quit_unexpectedly(const Step& s) {
    static double answered = -1.0;
    if (s.first)
        answered = -1.0;
    ImDrawList* d = canvas();
    const ImVec2 sc = screen();
    d->AddRectFilled(ImVec2{0, 0}, sc, IM_COL32(0, 0, 0, 0xff));
    if (answered < 0.0 && ((s.press && s.t > 1.6) || s.t > 20.0))
        answered = s.t;
    if (answered >= 0.0 && s.t > answered + 0.15)
        return s.t < answered + 0.9;  // the box gone, black a moment, then back
    if (s.t < 0.7)
        return true;
    const float k = dp();
    const float w = 486.f * k, h = 158.f * k;
    const ImVec2 a{(sc.x - w) * 0.5f, sc.y * 0.32f - h * 0.5f};
    const ImVec2 b{a.x + w, a.y + h};
    const float round = 10.f * k;
    shadow(d, a, b, round, 22.f * k, 0.55f);
    d->AddRectFilled(a, b, rgba(0x2b, 0x2b, 0x2d, 1.f), round);
    d->AddRect(a, b, rgba(0xff, 0xff, 0xff, 0.13f), round, 0, 1.f);
    const float icon = 64.f * k;
    if (ImTextureID id = picture("icon/window.png"); id != 0)
        d->AddImage(id, ImVec2{a.x + 22.f * k, a.y + 20.f * k}, ImVec2{a.x + 22.f * k + icon, a.y + 20.f * k + icon});
    ImFont* bold = g_faces.bold != nullptr ? g_faces.bold : ImGui::GetFont();
    ImFont* ui = g_faces.ui != nullptr ? g_faces.ui : ImGui::GetFont();
    const float left = a.x + 22.f * k + icon + 18.f * k;
    const float wrap = b.x - 22.f * k - left;
    d->AddText(bold, 13.f * k, ImVec2{left, a.y + 22.f * k}, rgba(0xff, 0xff, 0xff, 0.92f),
               "Eternal Darkness quit unexpectedly.");
    d->AddText(ui, 11.f * k, ImVec2{left, a.y + 46.f * k}, rgba(0xff, 0xff, 0xff, 0.72f),
               "Click Reopen to open the application again. Click Report to see more detailed information and "
               "send a report to Apple.",
               nullptr, wrap);
    const float bw = 82.f * k, gap = 12.f * k, by = b.y - 22.f * k - 20.f * k;
    const bool down = answered >= 0.0;
    mac_button(d, ImVec2{b.x - 20.f * k - bw, by}, bw, "Reopen", true, down, k);
    mac_button(d, ImVec2{b.x - 20.f * k - 2.f * bw - gap, by}, bw, "Report\u2026", false, false, k);
    mac_button(d, ImVec2{left, by}, bw, "Ignore", false, false, k);
    return true;
}

// --- Windows -------------------------------------------------------------------

// A notification as Windows 11 shows one in its dark look, sliding in at the
// screen's bottom right: the app's line, a bold title, the text, and buttons.
// `glyph` draws the app's little icon at a point, `size` across.
void win_toast(double t, double shown_for, const char* app, void (*glyph)(ImDrawList*, ImVec2, float),
               const char* title, const char* body, const char* left_button, const char* right_button) {
    ImDrawList* d = canvas();
    const ImVec2 sc = screen();
    const float k = dp();
    const float w = 364.f * k;
    ImFont* ui = g_faces.ui != nullptr ? g_faces.ui : ImGui::GetFont();
    ImFont* bold = g_faces.bold != nullptr ? g_faces.bold : ImGui::GetFont();
    const float pad = 16.f * k, wrap = w - 2.f * pad;
    const ImVec2 body_size = text_size(ui, 14.f * k, body, wrap);
    const float buttons = left_button != nullptr ? 32.f * k + 12.f * k : 0.f;
    const float h = 40.f * k + 22.f * k + body_size.y + 16.f * k + buttons;
    // In over a fifth of a second, out the same way.
    const double in = std::min(1.0, t / 0.22), out = std::min(1.0, std::max(0.0, (shown_for - t) / 0.22));
    const float slide = static_cast<float>(1.0 - std::pow(std::min(in, out), 3.0));
    const ImVec2 a{sc.x - w - 12.f * k + slide * (w + 24.f * k), sc.y - h - 12.f * k};
    const ImVec2 b{a.x + w, a.y + h};
    const float round = 8.f * k;
    shadow(d, a, b, round, 14.f * k, 0.45f);
    d->AddRectFilled(a, b, rgba(0x2b, 0x2b, 0x2b, 0.98f), round);
    d->AddRect(a, b, rgba(0xff, 0xff, 0xff, 0.09f), round, 0, 1.f);
    glyph(d, ImVec2{a.x + pad + 8.f * k, a.y + 20.f * k}, 16.f * k);
    d->AddText(ui, 12.f * k, ImVec2{a.x + pad + 24.f * k, a.y + 13.f * k}, rgba(0xff, 0xff, 0xff, 0.78f), app);
    // The "..." and close marks at the top right.
    const ImU32 mark = rgba(0xff, 0xff, 0xff, 0.7f);
    const ImVec2 x{b.x - pad - 4.f * k, a.y + 20.f * k};
    const float arm = 4.5f * k;
    d->AddLine(ImVec2{x.x - arm, x.y - arm}, ImVec2{x.x + arm, x.y + arm}, mark, 1.f * k);
    d->AddLine(ImVec2{x.x - arm, x.y + arm}, ImVec2{x.x + arm, x.y - arm}, mark, 1.f * k);
    for (int i = -1; i <= 1; i++)
        d->AddCircleFilled(ImVec2{x.x - 28.f * k + i * 5.f * k, x.y}, 1.2f * k, mark);
    d->AddText(bold, 14.f * k, ImVec2{a.x + pad, a.y + 40.f * k}, rgba(0xff, 0xff, 0xff, 0.96f), title);
    d->AddText(ui, 14.f * k, ImVec2{a.x + pad, a.y + 62.f * k}, rgba(0xff, 0xff, 0xff, 0.78f), body, nullptr,
               wrap);
    if (left_button != nullptr) {
        const float bw = (w - 2.f * pad - 8.f * k) * 0.5f, by = b.y - pad - 32.f * k;
        const char* labels[2] = {left_button, right_button};
        for (int i = 0; i < 2; i++) {
            const ImVec2 ba{a.x + pad + i * (bw + 8.f * k), by}, bb{ba.x + bw, by + 32.f * k};
            d->AddRectFilled(ba, bb, rgba(0xff, 0xff, 0xff, 0.08f), 4.f * k);
            d->AddRect(ba, bb, rgba(0xff, 0xff, 0xff, 0.07f), 4.f * k);
            const ImVec2 ls = text_size(ui, 14.f * k, labels[i]);
            d->AddText(ui, 14.f * k, ImVec2{ba.x + (bw - ls.x) * 0.5f, ba.y + (32.f * k - ls.y) * 0.5f},
                       rgba(0xff, 0xff, 0xff, 0.92f), labels[i]);
        }
    }
}

// Windows Update's icon: two arrows chasing round a circle, in the accent blue.
void glyph_update(ImDrawList* d, ImVec2 c, float size) {
    const ImU32 blue = IM_COL32(0x4c, 0xc2, 0xff, 0xff);
    const float r = size * 0.38f, w = size * 0.11f;
    for (int half = 0; half < 2; half++) {
        const float from = half * kPi + 0.35f, to = from + kPi - 0.7f;
        d->PathArcTo(c, r, from, to, 12);
        d->PathStroke(blue, 0, w);
        const ImVec2 tip{c.x + std::cos(to) * r, c.y + std::sin(to) * r};
        const ImVec2 along{-std::sin(to), std::cos(to)}, out{std::cos(to), std::sin(to)};
        const float s = size * 0.16f;
        d->AddTriangleFilled(ImVec2{tip.x + along.x * s, tip.y + along.y * s},
                             ImVec2{tip.x + out.x * s, tip.y + out.y * s}, ImVec2{tip.x - out.x * s, tip.y - out.y * s},
                             blue);
    }
}

// A display: the screen and its stand, for the driver's message.
void glyph_display(ImDrawList* d, ImVec2 c, float size) {
    const ImU32 white = IM_COL32(0xe8, 0xe8, 0xe8, 0xff);
    const float w = size * 0.42f, h = size * 0.28f;
    d->AddRect(ImVec2{c.x - w, c.y - h - size * 0.06f}, ImVec2{c.x + w, c.y + h - size * 0.06f}, white,
               size * 0.06f, 0, size * 0.08f);
    d->AddLine(ImVec2{c.x - w * 0.5f, c.y + h + size * 0.12f}, ImVec2{c.x + w * 0.5f, c.y + h + size * 0.12f}, white,
               size * 0.08f);
}

// Windows' warning sign: a yellow triangle with a black "!".
void glyph_warning(ImDrawList* d, ImVec2 c, float size) {
    const float h = size * 0.86f, w = size * 0.5f;
    const ImVec2 top{c.x, c.y - h * 0.5f}, left{c.x - w, c.y + h * 0.5f}, right{c.x + w, c.y + h * 0.5f};
    d->AddTriangleFilled(top, right, left, IM_COL32(0xfc, 0xe1, 0x00, 0xff));
    const ImU32 ink = IM_COL32(0x1a, 0x1a, 0x1a, 0xff);
    d->AddLine(ImVec2{c.x, c.y - h * 0.14f}, ImVec2{c.x, c.y + h * 0.2f}, ink, size * 0.1f);
    d->AddCircleFilled(ImVec2{c.x, c.y + h * 0.34f}, size * 0.06f, ink);
}

// The kernel driver Windows names when it resets one: the card's own maker's
// (from the display adapter's name), else NVIDIA's.
const char* display_driver() {
#ifdef _WIN32
    DISPLAY_DEVICEA device{};
    device.cb = sizeof device;
    for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &device, 0); i++) {
        if ((device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) == 0)
            continue;
        const std::string name = device.DeviceString;
        if (name.find("AMD") != std::string::npos || name.find("Radeon") != std::string::npos)
            return "amdkmdag";
        if (name.find("Intel") != std::string::npos)
            return "igfxn";
        break;
    }
#endif
    return "nvlddmkm";
}

// The driver gives out: the picture hangs with the sound stuck in a buzz,
// then black for a few long seconds, the frozen picture flashing back torn
// between, until the driver is back.
bool win_driver_reset(const Step& s) {
    if (s.first)
        audio_play_stuck(0.04, 1.0);
    static const double kBlack[][2] = {{1.0, 1.12}, {1.2, 4.4}, {4.48, 5.3}};
    static const double kTorn[][2] = {{1.12, 1.2}, {4.4, 4.48}};
    ImDrawList* d = canvas();
    const ImVec2 sc = screen();
    for (const auto& span : kBlack)
        if (s.t >= span[0] && s.t < span[1])
            d->AddRectFilled(ImVec2{0, 0}, sc, IM_COL32(0, 0, 0, 0xff));
    for (int i = 0; i < 2; i++) {
        if (s.t < kTorn[i][0] || s.t >= kTorn[i][1])
            continue;
        // Bands of the picture missing, as a half-sent frame.
        static const float kBands[2][3][2] = {{{0.08f, 0.21f}, {0.47f, 0.52f}, {0.71f, 1.f}},
                                              {{0.f, 0.33f}, {0.58f, 0.66f}, {0.86f, 0.9f}}};
        for (const auto& band : kBands[i])
            d->AddRectFilled(ImVec2{0, sc.y * band[0]}, ImVec2{sc.x, sc.y * band[1]}, IM_COL32(0, 0, 0, 0xff));
    }
    return s.t < 5.3;
}

// Then, over the game again: the screen coming back slowly, Windows' notice
// in its own words (with the warning sign), and a flicker as it shows that
// all is not well.
bool win_driver_toast(const Step& s) {
    constexpr double kIn = 0.9, kShown = 9.0;
    static std::string body;
    if (s.first)
        body = std::string("Display driver ") + display_driver() + " stopped responding and has successfully recovered.";
    ImDrawList* d = canvas();
    const ImVec2 sc = screen();
    if (s.t < 0.7) {
        const double dip = s.t > 0.18 && s.t < 0.26 ? 0.5 : 0.0;
        const float black = static_cast<float>(std::min(1.0, 1.0 - s.t / 0.7 + dip));
        d->AddRectFilled(ImVec2{0, 0}, sc, rgba(0, 0, 0, black));
    }
    if ((s.t >= 4.0 && s.t < 4.1) || (s.t >= 4.18 && s.t < 4.24))
        d->AddRectFilled(ImVec2{0, 0}, sc, IM_COL32(0, 0, 0, 0xff));
    if (s.t >= kIn)
        win_toast(s.t - kIn, kShown, "Windows", glyph_warning, "Display driver stopped responding and has recovered",
                  body.c_str(), nullptr, nullptr);
    return s.t < kIn + kShown;
}

bool win_update_toast(const Step& s) {
    constexpr double kShown = 9.0;
    win_toast(s.t, kShown, "Windows Update", glyph_update, "Restart required",
              "Your device will restart in 1 minute to finish installing updates.", "Restart now", "Pick a time");
    return s.t < kShown;
}

// Windows' arrow pointer, white edged in black, `scale` times its pixels.
void win_arrow(ImDrawList* d, ImVec2 at, float scale, float alpha) {
    static const ImVec2 kShape[] = {{0, 0}, {0, 17}, {4, 13}, {7, 19.5f}, {9.5f, 18.5f}, {6.5f, 12}, {12, 12}};
    ImVec2 points[7];
    for (int i = 0; i < 7; i++)
        points[i] = ImVec2{at.x + kShape[i].x * scale, at.y + kShape[i].y * scale};
    d->AddConcavePolyFilled(points, 7, rgba(0xff, 0xff, 0xff, alpha));
    d->AddPolyline(points, 7, rgba(0, 0, 0, alpha), ImDrawFlags_Closed, std::max(1.f, scale * 0.9f));
}

// Someone else's pointer: it appears, wanders over the picture as if a hand
// were on a mouse, stops, "clicks", and is gone.
bool win_pointer(const Step& s) {
    constexpr double kFor = 10.0;
    static float seed = 0.f;
    if (s.first)
        seed = static_cast<float>(std::rand() % 1000) / 100.f;
    const ImVec2 sc = screen();
    const float t = static_cast<float>(s.t);
    // Slow and uneven: a wander, with a pause in the middle.
    const float walk = t < 4.f ? t : t < 6.f ? 4.f + (t - 4.f) * 0.08f : 4.16f + (t - 6.f);
    const ImVec2 at{sc.x * (0.62f + 0.22f * std::sin(walk * 0.55f + seed) + 0.03f * std::sin(walk * 2.3f)),
                    sc.y * (0.45f + 0.25f * std::sin(walk * 0.41f + seed * 1.7f) + 0.02f * std::cos(walk * 3.1f))};
    const float alpha = t < 0.15f ? t / 0.15f : t > kFor - 0.1f ? 0.f : 1.f;
    const bool clicking = t > kFor - 1.2f && t < kFor - 1.0f;
    win_arrow(canvas(), at, (clicking ? 1.1f : 1.25f) * dp(), alpha);
    return s.t < kFor;
}

// --- PC and Deck: the screen failing ----------------------------------------------

// As you play, a few thin lines of pure colour appear down the screen, as a
// failing panel's dead columns do: one, a while later another, then two side
// by side, one only part of the way down; now and then one flickers; then
// they go, one by one.
bool screen_lines(const Step& s) {
    struct Line {
        float x;      // across, 0..1
        int width;    // in pixels (of a 1080-line screen)
        float from;   // down, 0..1, where it starts
        ImU32 colour;
        double at, until;
        bool flickers;
    };
    static std::vector<Line> lines;
    if (s.first) {
        const float x = 0.12f + 0.76f * static_cast<float>(std::rand() % 1000) / 1000.f;
        const float spread = static_cast<float>(std::rand() % 1000) / 1000.f;
        lines = {
            {x, 1, 0.f, IM_COL32(0xff, 0x2e, 0xd2, 0xff), 0.0, 12.6, false},
            {std::fmod(x + 0.31f + 0.2f * spread, 1.f), 1, 0.f, IM_COL32(0x3c, 0xff, 0x5a, 0xff), 2.6, 13.4, true},
            {std::fmod(x + 0.57f, 1.f), 1, 0.f, IM_COL32(0x40, 0xf0, 0xff, 0xff), 5.1, 11.8, false},
            {std::fmod(x + 0.57f, 1.f), 1, 0.f, IM_COL32(0xff, 0xf2, 0x3c, 0xff), 5.1, 11.8, false},
            {std::fmod(x + 0.13f + 0.1f * spread, 1.f), 2, 0.46f, IM_COL32(0xff, 0x2e, 0xd2, 0xff), 7.4, 13.9, false},
        };
    }
    const ImVec2 sc = screen();
    const float scale = ImGui::GetIO().DisplayFramebufferScale.x;
    const float px = scale > 0.f ? 1.f / scale : 1.f;
    const float unit = std::max(1.f, std::round(sc.y / px / 1080.f)) * px;
    const unsigned frame = static_cast<unsigned>(s.t * 60.0);
    ImDrawList* d = canvas();
    for (size_t i = 0; i < lines.size(); i++) {
        const Line& l = lines[i];
        if (s.t < l.at || s.t >= l.until)
            continue;
        // Catching as it comes and as it goes: a few frames on and off.
        const bool edge = s.t < l.at + 0.1 || s.t > l.until - 0.12;
        if (edge && (frame / 2u + i) % 2u)
            continue;
        // Now and then gone for a moment.
        if (l.flickers && (frame % 97u) < 3u)
            continue;
        float x = std::floor(l.x * sc.x / px) * px;
        if (i == 3)
            x += unit;  // beside the one before
        d->AddRectFilled(ImVec2{x, sc.y * l.from}, ImVec2{x + l.width * unit, sc.y}, l.colour);
    }
    return s.t < 14.0;
}

// --- Steam Deck and Linux -------------------------------------------------------

// Steam's own "not responding" box in its Deck look (dark slate, the focused
// button light), over the held picture, with its button hints along the
// bottom. Any button: Wait, and the game goes on.
bool deck_not_responding(const Step& s) {
    static double answered = -1.0;
    if (s.first)
        answered = -1.0;
    if (s.t < 2.5)
        return true;  // held: the game has stopped answering
    if (answered < 0.0 && ((s.press && s.t > 3.2) || s.t > 25.0))
        answered = s.t;
    if (answered >= 0.0 && s.t > answered + 0.12)
        return false;
    ImDrawList* d = canvas();
    const ImVec2 sc = screen();
    const float k = dp();
    const float fade = static_cast<float>(std::min(1.0, (s.t - 2.5) / 0.15));
    d->AddRectFilled(ImVec2{0, 0}, sc, rgba(0x0e, 0x14, 0x1b, 0.72f * fade));
    const float w = std::min(640.f * k, sc.x - 48.f * k), h = 236.f * k;
    const ImVec2 a{(sc.x - w) * 0.5f, (sc.y - h) * 0.5f - 20.f * k}, b{a.x + w, a.y + h};
    shadow(d, a, b, 4.f * k, 24.f * k, 0.6f * fade);
    d->AddRectFilledMultiColor(a, b, rgba(0x2a, 0x2e, 0x36, fade), rgba(0x2a, 0x2e, 0x36, fade),
                               rgba(0x1d, 0x20, 0x26, fade), rgba(0x1d, 0x20, 0x26, fade));
    ImFont* bold = g_faces.bold != nullptr ? g_faces.bold : ImGui::GetFont();
    ImFont* ui = g_faces.ui != nullptr ? g_faces.ui : ImGui::GetFont();
    const float pad = 32.f * k;
    d->AddText(bold, 26.f * k, ImVec2{a.x + pad, a.y + 28.f * k}, rgba(0xff, 0xff, 0xff, fade),
               "Eternal Darkness is not responding");
    d->AddText(ui, 17.f * k, ImVec2{a.x + pad, a.y + 72.f * k}, rgba(0xb8, 0xbc, 0xbf, fade),
               "You can wait for it to respond, or force it to quit. Any progress since you last saved will be lost.",
               nullptr, w - 2.f * pad);
    const float bh = 44.f * k, bw = (w - 2.f * pad - 12.f * k) * 0.5f, by = b.y - pad - bh;
    const char* labels[2] = {"Wait", "Force Quit"};
    for (int i = 0; i < 2; i++) {
        const ImVec2 ba{a.x + pad + i * (bw + 12.f * k), by}, bb{ba.x + bw, by + bh};
        const bool focus = i == 0;
        const bool down = focus && answered >= 0.0;
        d->AddRectFilled(ba, bb, focus ? rgba(down ? 0xb0 : 0xdc, down ? 0xb2 : 0xde, down ? 0xb4 : 0xdf, fade)
                                       : rgba(0x3d, 0x44, 0x50, fade),
                         2.f * k);
        const ImVec2 ls = text_size(ui, 17.f * k, labels[i]);
        d->AddText(focus ? bold : ui, 17.f * k, ImVec2{ba.x + (bw - ls.x) * 0.5f, ba.y + (bh - ls.y) * 0.5f},
                   focus ? rgba(0x0e, 0x14, 0x1b, fade) : rgba(0xdc, 0xde, 0xdf, fade), labels[i]);
    }
    // The hints bar: A Select, B Back.
    const float bar = 44.f * k;
    d->AddRectFilled(ImVec2{0, sc.y - bar}, sc, rgba(0x0e, 0x14, 0x1b, 0.92f * fade));
    float x = sc.x - 24.f * k;
    const std::pair<const char*, const char*> kHints[] = {{"east", "Back"}, {"south", "Select"}};
    for (const auto& [button, label] : kHints) {
        const ImVec2 ls = text_size(ui, 15.f * k, label);
        x -= ls.x;
        d->AddText(ui, 15.f * k, ImVec2{x, sc.y - bar + (bar - ls.y) * 0.5f}, rgba(0xdc, 0xde, 0xdf, fade), label);
        const float icon = 24.f * k;
        x -= icon + 8.f * k;
        if (ImTextureID id = picture(std::string("ui/glyphs/xbox/") + button + ".png"); id != 0)
            d->AddImage(id, ImVec2{x, sc.y - bar + (bar - icon) * 0.5f}, ImVec2{x + icon, sc.y - bar + (bar + icon) * 0.5f},
                        ImVec2{0, 0}, ImVec2{1, 1}, rgba(0xff, 0xff, 0xff, fade));
        x -= 28.f * k;
    }
    return true;
}

// The game falls over and takes Linux with it: the picture holds, then the
// console's text, a segfault and an oops scrolling up to the kernel's panic
// (its call trace through the game's gods), then nothing; then the game
// again, as if none of it happened.
bool linux_kernel_panic(const Step& s) {
    static std::vector<std::string> lines;
    if (s.first) {
        const bool deck = platform() == kDeck;
        const char* uname = deck ? "6.5.0-valve22-1-neptune-65" : "6.8.0-45-generic";
        const char* hardware =
            deck ? "Valve Jupiter/Jupiter, BIOS F7A0131 01/30/2024" : "To Be Filled By O.E.M. To Be Filled By O.E.M./B550M, BIOS P2.20 07/27/2021";
        const std::string text[] = {
            "EternalDarkness[4183]: segfault at 48 ip 000055d0c2b1e4a1 sp 00007ffd3c1e9a50 error 4 in "
            "EternalDarkness[55d0c2a00000+4c2000] likely on CPU 6 (core 3, socket 0)",
            "BUG: kernel NULL pointer dereference, address: 0000000000000048",
            "#PF: supervisor read access in kernel mode",
            "#PF: error_code(0x0000) - not-present page",
            "PGD 0 P4D 0",
            "Oops: 0000 [#1] PREEMPT SMP NOPTI",
            std::string("CPU: 6 PID: 4183 Comm: EternalDarkness Not tainted ") + uname + " #1",
            std::string("Hardware name: ") + hardware,
            "RIP: 0010:sanity_drain+0x4e/0x1d0",
            "Code: 48 8b 47 48 48 85 c0 74 2e 8b 50 10 85 d2 7e 27 <48> 8b 40 48 48 85 c0 74 1e 0f b7 50 0a",
            "RSP: 0018:ffffb3c4c2a37d18 EFLAGS: 00010246",
            "RAX: 0000000000000000 RBX: ffff9a5e0b2d8000 RCX: 0000000000000000",
            "RDX: 0000000000000001 RSI: 0000000000000000 RDI: ffff9a5e0b2d8000",
            "RBP: ffffb3c4c2a37d40 R08: 0000000000000000 R09: 0000000000000000",
            "R10: 0000000000000000 R11: 0000000000000000 R12: ffff9a5e1e6f4c00",
            "R13: 0000000000000000 R14: 0000000000000000 R15: 0000000000000000",
            "FS:  00007f3c2d7fe6c0(0000) GS:ffff9a612ef80000(0000) knlGS:0000000000000000",
            "CS:  0010 DS: 0000 ES: 0000 CR0: 0000000080050033",
            "CR2: 0000000000000048 CR3: 000000011b5d2000 CR4: 0000000000350ee0",
            "Call Trace:",
            " <TASK>",
            " ? __die+0x23/0x70",
            " ? page_fault_oops+0x171/0x4e0",
            " ? exc_page_fault+0x7f/0x180",
            " ? asm_exc_page_fault+0x26/0x30",
            " ? sanity_drain+0x4e/0x1d0",
            " mantorok_whisper+0x1b2/0x3a0",
            " ? ulyaoth_reach+0x9c/0x140",
            " ? xellotath_dream+0x51/0x120",
            " chattur_gha_bind+0x2e5/0x610",
            " pious_augustus_ritual+0x41/0x90",
            " __x64_sys_ioctl+0x94/0xd0",
            " do_syscall_64+0x5d/0x90",
            " entry_SYSCALL_64_after_hwframe+0x6e/0xd8",
            " </TASK>",
            "Modules linked in: amdgpu snd_hda_intel snd_intel_dspcfg snd_hda_codec drm_buddy gpu_sched "
            "drm_display_helper hid_steam bluetooth cfg80211 nvme ccp xhci_pci",
            "CR2: 0000000000000048",
            "---[ end trace 0000000000000000 ]---",
            "Kernel panic - not syncing: Fatal exception in interrupt",
            "Kernel Offset: 0x2c000000 from 0xffffffff81000000 (relocation range: 0xffffffff80000000-0xffffffffbfffffff)",
            "---[ end Kernel panic - not syncing: Fatal exception in interrupt ]---",
        };
        lines.clear();
        double stamp = 4127.881203;
        for (const std::string& line : text) {
            char at[32];
            std::snprintf(at, sizeof at, "[%11.6f] ", stamp);
            lines.push_back(at + line);
            stamp += 0.000005 + (std::rand() % 9) * 0.000001;
            if (line.rfind("---[ end trace", 0) == 0)
                stamp += 0.0208;
        }
    }
    constexpr double kHeld = 1.2, kPerLine = 0.045, kStay = 6.0;
    const double printed = (s.t - kHeld) / kPerLine;
    const double end = kHeld + lines.size() * kPerLine + kStay;
    if (s.t < kHeld)
        return true;
    ImDrawList* d = canvas();
    const ImVec2 sc = screen();
    d->AddRectFilled(ImVec2{0, 0}, sc, IM_COL32(0, 0, 0, 0xff));
    if (s.t > end - 0.6)
        return s.t < end;  // a moment of nothing before the game is back
    ImFont* mono = g_faces.mono != nullptr ? g_faces.mono : ImGui::GetFont();
    const float size = std::max(10.f, sc.y / 46.f);
    const float line_h = size * 1.12f;
    const int shown = std::min(static_cast<int>(lines.size()), static_cast<int>(printed) + 1);
    // Wrapped as the console wraps, oldest scrolled off the top.
    std::vector<std::pair<const char*, const char*>> rows;
    const float width = sc.x - 2.f * size * 0.5f;
    const float column = mono->CalcTextSizeA(size, FLT_MAX, 0.f, "M").x;
    const size_t per_row = std::max<size_t>(20, static_cast<size_t>(width / column));
    for (int i = 0; i < shown; i++)
        for (size_t at = 0; at < lines[i].size(); at += per_row)
            rows.emplace_back(lines[i].c_str() + at, lines[i].c_str() + std::min(lines[i].size(), at + per_row));
    const int fit = static_cast<int>((sc.y - size) / line_h);
    const int from = std::max(0, static_cast<int>(rows.size()) - fit);
    for (int r = from; r < static_cast<int>(rows.size()); r++)
        d->AddText(mono, size, ImVec2{size * 0.5f, size * 0.5f + (r - from) * line_h}, IM_COL32(0xaa, 0xaa, 0xaa, 0xff),
                   rows[r].first, rows[r].second);
    return true;
}

// --- PS5 ---------------------------------------------------------------------

// The PS5's own system screens, laid out as it lays them out: every size and
// place below is measured from its screens at 1080 lines (k = one of those
// lines), and text is sized to the width the PS5's own makes, whatever face
// draws it here.
// k, and where the 16:9 frame the PS5 draws in sits on this screen (all of a
// PS5's; the middle of a wider or taller window).
float ps5_k() { return std::min(screen().x / 1920.f, screen().y / 1080.f); }

// The black before a PS5 screen, as the game gives way to the system's.
constexpr double kGiveWay = 0.5;
float ps5_ox() { return (screen().x - 1920.f * ps5_k()) * 0.5f; }
float ps5_oy() { return (screen().y - 1080.f * ps5_k()) * 0.5f; }

ImFont* ps5_face() {
    return g_faces.light != nullptr ? g_faces.light : g_faces.ui != nullptr ? g_faces.ui : ImGui::GetFont();
}
ImFont* ps5_medium() { return g_faces.ui != nullptr ? g_faces.ui : ps5_face(); }
ImFont* ps5_regular() { return g_faces.regular != nullptr ? g_faces.regular : ps5_face(); }

// Its backdrops, as 32 x 18 colours averaged from its own screens (a soft
// room, so stretched they are it): the dim room lit from the top left behind
// its notices, and the plum one behind an update's install.
const uint32_t kPs5Room[32 * 18] = {
    0x1f1e26, 0x312c31, 0x504648, 0x7b6969, 0x8d7877, 0x96817e, 0x7f6d6c, 0x655758,
    0x4c4245, 0x3a3438, 0x29282e, 0x212129, 0x1e1e28, 0x1c1c26, 0x1b1b26, 0x1b1b25,
    0x1a1a24, 0x191923, 0x191923, 0x181a23, 0x161822, 0x151722, 0x141622, 0x131521,
    0x131521, 0x141420, 0x141420, 0x13131f, 0x12121e, 0x12121e, 0x11111d, 0x111020,
    0x232129, 0x332d33, 0x4d4345, 0x6e5e5f, 0x7c6a6a, 0x847171, 0x766565, 0x625456,
    0x4d4346, 0x3f383c, 0x302d33, 0x2a282f, 0x23222b, 0x1f1f29, 0x1d1d27, 0x1c1c27,
    0x1b1b25, 0x1a1a24, 0x191923, 0x191923, 0x171a23, 0x151822, 0x141622, 0x141622,
    0x131521, 0x141521, 0x141420, 0x141420, 0x13131f, 0x12121e, 0x11111d, 0x11101f,
    0x27252c, 0x342e34, 0x494042, 0x615455, 0x6b5c5d, 0x726263, 0x6c5e5e, 0x5e5253,
    0x4f4548, 0x443d40, 0x373237, 0x332e36, 0x29282f, 0x24232b, 0x202029, 0x1e1e28,
    0x1c1c26, 0x1b1b25, 0x1a1a24, 0x191923, 0x181923, 0x161922, 0x151722, 0x141622,
    0x131521, 0x131521, 0x141420, 0x141420, 0x13131f, 0x12121e, 0x11111d, 0x11101e,
    0x2a272e, 0x363035, 0x453d40, 0x54494b, 0x5a4e50, 0x605355, 0x635657, 0x5b4f51,
    0x514749, 0x494144, 0x3e383c, 0x373138, 0x2f2d33, 0x29282e, 0x25242b, 0x20202a,
    0x1e1e28, 0x1c1c26, 0x1b1b25, 0x1a1a24, 0x191923, 0x171a23, 0x151822, 0x141622,
    0x141622, 0x131521, 0x141420, 0x141420, 0x141420, 0x12121e, 0x12121e, 0x11111d,
    0x2a282f, 0x362f35, 0x423a3d, 0x4d4346, 0x51474a, 0x554b4d, 0x5b5051, 0x564c4d,
    0x4e4548, 0x494043, 0x413b3e, 0x393239, 0x312e35, 0x2d2b31, 0x28272e, 0x25242b,
    0x21202a, 0x1e1e28, 0x1b1b26, 0x1a1a24, 0x191923, 0x181923, 0x161922, 0x151723,
    0x141622, 0x131521, 0x131521, 0x141420, 0x141420, 0x12121e, 0x12121e, 0x11111d,
    0x2a292f, 0x342e35, 0x3e383b, 0x473f42, 0x4b4245, 0x4d4447, 0x534a4b, 0x52484a,
    0x4c4447, 0x453e41, 0x413b3e, 0x3c363b, 0x333037, 0x2f2d33, 0x2b2930, 0x27262d,
    0x24232b, 0x21202a, 0x1d1d27, 0x1b1b25, 0x1a1a24, 0x191923, 0x171922, 0x151722,
    0x141622, 0x131521, 0x131521, 0x141420, 0x141420, 0x13131f, 0x12121e, 0x11111d,
    0x2a292f, 0x332d34, 0x3c363a, 0x423c3e, 0x453e41, 0x484043, 0x4c4447, 0x4d4548,
    0x494245, 0x443e40, 0x403a3e, 0x3c383b, 0x363239, 0x302e34, 0x2c2b31, 0x29282e,
    0x26252c, 0x24232a, 0x201f29, 0x1d1d26, 0x1b1b25, 0x1a1a24, 0x191923, 0x161822,
    0x141622, 0x141622, 0x131521, 0x131420, 0x141420, 0x13131f, 0x12121e, 0x11111d,
    0x2a292f, 0x322d35, 0x3a333a, 0x3f393c, 0x413b3e, 0x433c3f, 0x464042, 0x484244,
    0x474143, 0x433d40, 0x3f393d, 0x3b383b, 0x37333b, 0x322f36, 0x2d2c32, 0x2a292f,
    0x27262d, 0x25242b, 0x22212a, 0x1f1f28, 0x1c1c26, 0x1a1a24, 0x191923, 0x171922,
    0x151722, 0x141622, 0x131521, 0x141420, 0x141420, 0x13131f, 0x12121e, 0x11111d,
    0x29282e, 0x312d34, 0x383139, 0x3c363b, 0x3e383c, 0x403a3e, 0x3f383d, 0x413b3f,
    0x423b3e, 0x40393f, 0x3a373b, 0x3c3940, 0x3b383f, 0x3a373e, 0x3a373e, 0x38353c,
    0x36343b, 0x323137, 0x302e35, 0x302f36, 0x2a292f, 0x201f28, 0x1e1d25, 0x1b1a25,
    0x191923, 0x171722, 0x161622, 0x141521, 0x141420, 0x141420, 0x12121e, 0x11111d,
    0x29282e, 0x2f2c33, 0x363038, 0x39333a, 0x3c373b, 0x3d383c, 0x3a373c, 0x3d383d,
    0x3f393f, 0x3f383d, 0x39363d, 0x36343a, 0x333037, 0x312f35, 0x2f2e34, 0x2f2c33,
    0x2d2c32, 0x2b2a30, 0x28272d, 0x26252b, 0x242329, 0x222029, 0x1f1e26, 0x1d1c24,
    0x1b1a24, 0x191923, 0x171722, 0x151521, 0x141420, 0x141420, 0x12121e, 0x12121e,
    0x29282e, 0x2e2c33, 0x333037, 0x383139, 0x37343b, 0x39373c, 0x38363c, 0x39363d,
    0x3c373d, 0x3b373e, 0x39363d, 0x36343a, 0x333137, 0x302f35, 0x2e2d33, 0x2d2c32,
    0x2c2b31, 0x2b2a30, 0x28272d, 0x26252b, 0x25232a, 0x222128, 0x201f27, 0x1e1d25,
    0x1d1b24, 0x1b1924, 0x181822, 0x151521, 0x141420, 0x141420, 0x13131f, 0x12121e,
    0x29282e, 0x2d2c32, 0x322f36, 0x353138, 0x36333a, 0x37353c, 0x38373d, 0x3c383e,
    0x3f3840, 0x3d383e, 0x3a373e, 0x37363c, 0x353239, 0x323037, 0x312e35, 0x2e2d33,
    0x2a292f, 0x27262d, 0x25242b, 0x24222a, 0x212029, 0x201e27, 0x1d1d26, 0x1b1b25,
    0x191923, 0x181822, 0x171722, 0x161622, 0x151521, 0x141420, 0x13131f, 0x12121e,
    0x2b2a30, 0x2f2d34, 0x323037, 0x343239, 0x35343a, 0x37363c, 0x38373d, 0x3a383e,
    0x3b383f, 0x3b383f, 0x39373e, 0x37363c, 0x35333a, 0x323137, 0x302f35, 0x2e2d33,
    0x2b2a30, 0x28272e, 0x26252c, 0x24232a, 0x222129, 0x201f29, 0x1e1d26, 0x1c1b25,
    0x1a1a24, 0x191923, 0x181822, 0x161622, 0x151521, 0x141420, 0x141420, 0x12121e,
    0x2f2e34, 0x323137, 0x35343a, 0x37363c, 0x39383e, 0x3a393f, 0x3b3a40, 0x3c3b41,
    0x3d3c42, 0x3d3b42, 0x3b3a40, 0x39383e, 0x37363c, 0x343339, 0x313036, 0x2f2e34,
    0x2d2c32, 0x2a292f, 0x27262d, 0x25242b, 0x23222a, 0x211f29, 0x1f1e27, 0x1d1c25,
    0x1b1b25, 0x1a1a24, 0x191923, 0x171721, 0x161622, 0x151521, 0x141420, 0x141420,
    0x36373d, 0x3a3b41, 0x3f3f44, 0x414147, 0x44444a, 0x45454b, 0x47464c, 0x47464c,
    0x47464c, 0x45444a, 0x434248, 0x403f45, 0x3c3b40, 0x38373d, 0x35343a, 0x323137,
    0x2f2e35, 0x2d2c32, 0x2a292f, 0x27262d, 0x25242b, 0x23222a, 0x211f29, 0x1f1e27,
    0x1c1c26, 0x1b1b25, 0x1a1a24, 0x191923, 0x171721, 0x161622, 0x151521, 0x141420,
    0x3f4046, 0x44444a, 0x48484e, 0x4b4c52, 0x4e4f54, 0x505156, 0x505157, 0x505056,
    0x4f5055, 0x4c4d52, 0x4a494f, 0x45454b, 0x404146, 0x3d3c42, 0x38383e, 0x34353a,
    0x323237, 0x302f35, 0x2d2c32, 0x29292f, 0x26262d, 0x25242b, 0x23222a, 0x211f29,
    0x1f1f28, 0x1d1d26, 0x1b1b25, 0x1a1a24, 0x191923, 0x181822, 0x161622, 0x151521,
    0x403f45, 0x434148, 0x45434a, 0x46454b, 0x47474d, 0x48484e, 0x48484e, 0x46484d,
    0x46474c, 0x45464b, 0x434349, 0x404146, 0x3d3e43, 0x3a3b40, 0x37383d, 0x34353a,
    0x313237, 0x302f35, 0x2e2d33, 0x2a2b30, 0x28282f, 0x25252d, 0x24242c, 0x23212b,
    0x212029, 0x1f1f28, 0x1d1d26, 0x1b1b25, 0x1a1a24, 0x191923, 0x191923, 0x171722,
    0x38373d, 0x39383e, 0x3a393f, 0x3b3a40, 0x3b3a41, 0x3c3b41, 0x3c3b41, 0x3c3b41,
    0x3c3b41, 0x3c3b41, 0x3b3a40, 0x3a393f, 0x38373d, 0x36353b, 0x333339, 0x313036,
    0x2f2e34, 0x2e2d33, 0x2c2b31, 0x2a2a2f, 0x27282e, 0x26252d, 0x25242b, 0x24222c,
    0x222029, 0x201f29, 0x1e1e27, 0x1d1d26, 0x1b1b25, 0x1a1a24, 0x1a1a24, 0x191923,
};

const uint32_t kPs5Install[32 * 18] = {
    0x22222c, 0x22222c, 0x22222c, 0x22222c, 0x22222c, 0x22222c, 0x23222c, 0x24232b,
    0x24232b, 0x24232b, 0x24222d, 0x24232b, 0x25242a, 0x25242c, 0x26252b, 0x28272f,
    0x29282e, 0x2a292f, 0x2a292f, 0x2e2930, 0x2d2a31, 0x2f2c32, 0x342f35, 0x383339,
    0x3b353a, 0x3b363c, 0x3f393d, 0x3f3a3e, 0x3f3a3e, 0x3f3a3e, 0x3f3a3e, 0x3f3a3e,
    0x22222c, 0x22222c, 0x22222c, 0x22222c, 0x22222b, 0x22222b, 0x24222d, 0x24232b,
    0x24232b, 0x24232b, 0x24222d, 0x24232b, 0x25242a, 0x25242b, 0x26252b, 0x27262e,
    0x29282e, 0x2a292f, 0x2a282f, 0x2e2930, 0x2d2a31, 0x2f2d32, 0x342f35, 0x383439,
    0x39343a, 0x3b363c, 0x3e383c, 0x3f393d, 0x3f393d, 0x3f393d, 0x3f393d, 0x3f393d,
    0x22222c, 0x22222c, 0x22222c, 0x22222c, 0x22222c, 0x22222c, 0x22212b, 0x23212b,
    0x24222d, 0x24232b, 0x24232b, 0x25242c, 0x25242b, 0x26252b, 0x27262e, 0x29282e,
    0x29282e, 0x2a292f, 0x2a282f, 0x2d2930, 0x2f2b32, 0x342f35, 0x363238, 0x39343a,
    0x39343a, 0x3b363c, 0x3c373b, 0x3e393d, 0x3e393d, 0x3e393d, 0x3e393d, 0x3e393d,
    0x22222c, 0x22222c, 0x22222c, 0x22222c, 0x22222c, 0x22222c, 0x22222c, 0x24222d,
    0x24232b, 0x24232b, 0x25242c, 0x25242b, 0x26252b, 0x27262e, 0x27262e, 0x29282e,
    0x29282e, 0x2a292f, 0x2d2930, 0x2f2a31, 0x322e33, 0x353036, 0x363137, 0x373138,
    0x39343a, 0x39343a, 0x3c373d, 0x3c373b, 0x3c373b, 0x3c373b, 0x3c373b, 0x3c373b,
    0x21212b, 0x21212b, 0x21212b, 0x22222c, 0x22222c, 0x22222c, 0x22222c, 0x24232b,
    0x24232b, 0x25242a, 0x25242c, 0x26252b, 0x26252b, 0x28272f, 0x28272f, 0x29282e,
    0x29282e, 0x2a292f, 0x2f2a31, 0x302d33, 0x342f35, 0x342f35, 0x342f35, 0x363137,
    0x383339, 0x39343a, 0x3b363c, 0x3c373b, 0x3c373b, 0x3c373b, 0x3c373b, 0x3c373b,
    0x21212b, 0x21212b, 0x21212b, 0x22222c, 0x22222c, 0x22212c, 0x24232b, 0x24232b,
    0x25242c, 0x25242c, 0x25242b, 0x26252b, 0x26252b, 0x27262e, 0x27262e, 0x29282e,
    0x2a292f, 0x2d2a31, 0x2e2b32, 0x322e33, 0x342f35, 0x342f35, 0x342f35, 0x343237,
    0x383138, 0x39343a, 0x3b363c, 0x3b363c, 0x3b363c, 0x3b363c, 0x3b363c, 0x3b363c,
    0x21212b, 0x21212b, 0x22222c, 0x22222c, 0x22222c, 0x24232b, 0x24232b, 0x25232e,
    0x25242c, 0x25242c, 0x25242a, 0x26252b, 0x26252b, 0x27262e, 0x28262e, 0x29282e,
    0x2d2a30, 0x2f2a31, 0x2f2d32, 0x302e33, 0x302e33, 0x342f35, 0x342f35, 0x353137,
    0x373238, 0x39343a, 0x3a353b, 0x3b363c, 0x3b363c, 0x3b363c, 0x3b363c, 0x3b363c,
    0x21212b, 0x21212b, 0x22222c, 0x22222c, 0x24222d, 0x24232b, 0x24222d, 0x25232c,
    0x25242c, 0x25242c, 0x25242c, 0x25242b, 0x26252b, 0x27262e, 0x29282e, 0x2a2930,
    0x2e2a31, 0x302b32, 0x2f2d32, 0x2f2d32, 0x2f2d32, 0x322e33, 0x342f35, 0x342f35,
    0x363137, 0x383339, 0x39343a, 0x39343a, 0x39343a, 0x39343a, 0x39343a, 0x39343a,
    0x22222c, 0x22222c, 0x22222c, 0x22222c, 0x23212c, 0x24232b, 0x24222d, 0x24232b,
    0x24232b, 0x25242c, 0x25242c, 0x25242b, 0x27262e, 0x29282e, 0x2a282f, 0x2c282f,
    0x2f2a31, 0x2f2b31, 0x302b31, 0x302b31, 0x2f2d32, 0x302e33, 0x342f35, 0x342f35,
    0x343137, 0x383138, 0x39343a, 0x39343a, 0x39343a, 0x39343a, 0x39343a, 0x39343a,
    0x22222b, 0x22222c, 0x22222c, 0x22212c, 0x23212c, 0x23212b, 0x24232b, 0x24232b,
    0x24232b, 0x25242c, 0x25242c, 0x26252b, 0x27262e, 0x29282e, 0x2b282f, 0x2d2930,
    0x2f2a31, 0x2f2a31, 0x2f2a31, 0x302b32, 0x2f2d32, 0x302e33, 0x322e33, 0x342f35,
    0x343036, 0x373138, 0x383339, 0x38333a, 0x38333a, 0x38333a, 0x38333a, 0x38333a,
    0x23222b, 0x23222b, 0x24222d, 0x24232b, 0x24232b, 0x24232b, 0x24232b, 0x24232b,
    0x24222c, 0x25242c, 0x25242c, 0x27262e, 0x29282e, 0x2a292f, 0x2d2930, 0x2d2930,
    0x2e2930, 0x2d2930, 0x2f2a31, 0x2e2b32, 0x302b31, 0x2f2d32, 0x322d33, 0x342f35,
    0x342f35, 0x363137, 0x373138, 0x373238, 0x373238, 0x373238, 0x373238, 0x373238,
    0x1f202a, 0x1f2029, 0x21202a, 0x21212b, 0x24222b, 0x24232b, 0x25242c, 0x25242c,
    0x26252c, 0x26252d, 0x28272d, 0x28272f, 0x29282e, 0x2a292f, 0x2d2930, 0x2c282f,
    0x2d2930, 0x2e2930, 0x2e2a31, 0x2e2b32, 0x2f2c32, 0x302d32, 0x332e34, 0x332f35,
    0x373338, 0x3a3438, 0x353136, 0x363137, 0x363137, 0x363137, 0x363137, 0x363137,
    0x1f1f29, 0x1f1f29, 0x201f29, 0x211f29, 0x201e29, 0x21202a, 0x22212b, 0x24232b,
    0x25242b, 0x27262e, 0x29282e, 0x29282e, 0x2a282f, 0x2c282f, 0x2b282f, 0x2d282f,
    0x2b282f, 0x2e2830, 0x2c282f, 0x2f2a31, 0x2e2b32, 0x2f2b32, 0x302d32, 0x322e34,
    0x332e34, 0x343035, 0x342f35, 0x343036, 0x343036, 0x343036, 0x343036, 0x343036,
    0x21222a, 0x25202a, 0x222029, 0x21202a, 0x211f2a, 0x22202b, 0x22202b, 0x22212b,
    0x24232b, 0x25242b, 0x27262e, 0x29282e, 0x2b282f, 0x2b282f, 0x2d282f, 0x2e2930,
    0x2d2930, 0x2c2930, 0x2d2a31, 0x2d2a31, 0x2d2a31, 0x2e2b32, 0x2f2d32, 0x322d33,
    0x332e34, 0x332e34, 0x342f35, 0x342f35, 0x342f35, 0x342f35, 0x342f35, 0x342f35,
    0x22202a, 0x22202b, 0x26222a, 0x28232a, 0x362f2e, 0x25222b, 0x26222b, 0x27222b,
    0x25242b, 0x27242b, 0x26242b, 0x25242a, 0x27242b, 0x28252c, 0x28272e, 0x2a272f,
    0x2b282f, 0x2c2930, 0x2d2a31, 0x2e2a31, 0x2f2b32, 0x2e2b32, 0x323134, 0x322d33,
    0x302e33, 0x342f34, 0x342f35, 0x342f35, 0x342f35, 0x342f35, 0x342f35, 0x342f35,
    0x22212b, 0x23212b, 0x23222a, 0x25232a, 0x24232a, 0x26232a, 0x26232a, 0x27242b,
    0x28252c, 0x28252e, 0x28252c, 0x28252e, 0x28252c, 0x28252c, 0x28252e, 0x28252e,
    0x28252d, 0x2a272e, 0x2b282f, 0x2d282f, 0x2d2a31, 0x302c32, 0x2f2d32, 0x322e33,
    0x322d33, 0x322e34, 0x342f35, 0x332e34, 0x332e34, 0x332e34, 0x332e34, 0x332e34,
    0x24222c, 0x24222b, 0x24222a, 0x24222b, 0x252229, 0x27242b, 0x28252c, 0x28252c,
    0x28252e, 0x29262d, 0x28252d, 0x28252c, 0x28252c, 0x28252d, 0x28252c, 0x28252c,
    0x28252d, 0x2a272e, 0x2a272e, 0x2b282f, 0x2c2930, 0x2f2a31, 0x302c32, 0x302e33,
    0x302e33, 0x322e33, 0x332e34, 0x342f35, 0x342f35, 0x342f35, 0x342f35, 0x342f35,
    0x24222c, 0x24222b, 0x24222a, 0x24222b, 0x252229, 0x27242b, 0x28252c, 0x28252c,
    0x28252e, 0x29262d, 0x28252d, 0x28252c, 0x28252c, 0x28252d, 0x28252c, 0x28252c,
    0x28252d, 0x2a272e, 0x2a272e, 0x2b282f, 0x2c2930, 0x2f2a31, 0x302c32, 0x302e33,
    0x302e33, 0x322e33, 0x332e34, 0x342f35, 0x342f35, 0x342f35, 0x342f35, 0x342f35,
};

void ps5_room(ImDrawList* d, const uint32_t* grid, float alpha) {
    static std::vector<std::pair<const uint32_t*, ImTextureID>> made;
    ImTextureID id = 0;
    for (const auto& [g, t] : made)
        if (g == grid)
            id = t;
    if (id == 0) {
        std::vector<uint8_t> pixels(32 * 18 * 4);
        for (int i = 0; i < 32 * 18; i++) {
            pixels[i * 4] = static_cast<uint8_t>(grid[i] >> 16);
            pixels[i * 4 + 1] = static_cast<uint8_t>(grid[i] >> 8);
            pixels[i * 4 + 2] = static_cast<uint8_t>(grid[i]);
            pixels[i * 4 + 3] = 0xff;
        }
        id = aurora_imgui_add_texture(32, 18, pixels.data());
        made.emplace_back(grid, id);
    }
    // Texel centres at the screen's edges, so nothing smears in from beyond.
    d->AddImage(id, ImVec2{0, 0}, screen(), ImVec2{0.5f / 32.f, 0.5f / 18.f}, ImVec2{31.5f / 32.f, 17.5f / 18.f},
                rgba(0xff, 0xff, 0xff, alpha));
}

// Gold motes over the install's room, thickest in the lower corners, some
// near and soft, most far and small, drifting slowly up.
void ps5_motes(ImDrawList* d, double t, float alpha) {
    const ImVec2 sc = screen();
    const float k = ps5_k();
    for (int i = 0; i < 520; i++) {
        const float u = std::fmod(i * 0.61803f, 1.f), v = std::fmod(i * 0.41421f + 0.13f, 1.f);
        const float w = std::fmod(i * 0.27183f + 0.5f, 1.f);
        const bool left = i % 5 < 3;
        const float x = left ? sc.x * 0.36f * u * u : sc.x * (1.f - 0.24f * u * u);
        const float drift = static_cast<float>(std::fmod(t * (3.f + 5.f * v) * k, 260.f * k));
        const float y = sc.y * (0.55f + 0.45f * std::sqrt(v)) - drift + 120.f * k * w;
        const bool near = i % 8 == 0;
        const float r = near ? (5.f + 9.f * w) * k : (1.f + 3.2f * w * w) * k;
        const float twinkle = 0.55f + 0.45f * std::sin(static_cast<float>(t) * (0.4f + v) + i);
        const float a = alpha * twinkle * (near ? 0.16f : 0.55f + 0.35f * w);
        if (near)
            for (int ring = 3; ring >= 1; ring--)
                d->AddCircleFilled(ImVec2{x, y}, r * ring / 3.f, rgba(0xd6, 0xa8, 0x5c, a * 0.5f), 20);
        else
            d->AddCircleFilled(ImVec2{x, y}, r, rgba(0xe0, 0xb4, 0x6a, a), 10);
    }
}

// Text at `size` so that `fit` (the PS5's own line, measured) spans `width`
// and its capitals stand `cap` high, as near as this face's proportions let
// both be (the size between the two).
float ps5_size(ImFont* f, const char* fit, float width, float cap = 0.f) {
    const float at_one = f->CalcTextSizeA(1.f, FLT_MAX, 0.f, fit).x;
    const float by_width = at_one > 0.f ? width / at_one : 30.f * ps5_k();
    const ImFontGlyph* h = f->FindGlyph('H');
    if (cap <= 0.f || h == nullptr || h->Y1 <= h->Y0)
        return by_width;
    const float by_cap = cap * f->FontSize / (h->Y1 - h->Y0);
    return std::sqrt(by_width * by_cap);
}

// The top of the capitals at `size`, below the top of the line.
float cap_offset(ImFont* f, float size) {
    const ImFontGlyph* h = f->FindGlyph('H');
    return h != nullptr ? h->Y0 * size / f->FontSize : size * 0.2f;
}

// Text with its capitals' top at y, its left at x.
void ps5_text(ImDrawList* d, ImFont* f, float size, float x, float cap_top, ImU32 colour, const char* text) {
    d->AddText(f, size, ImVec2{x, cap_top - cap_offset(f, size)}, colour, text);
}

float ps5_width(ImFont* f, float size, const char* text) { return f->CalcTextSizeA(size, FLT_MAX, 0.f, text).x; }

// The screen's name, top left: 44 lines high in its face, at 98, 86.
void ps5_title(ImDrawList* d, const char* title, float alpha) {
    const float k = ps5_k();
    ImFont* f = ps5_face();
    const float size = ps5_size(f, "System Software Update", 492.f * k, 35.f * k);
    ps5_text(d, f, size, ps5_ox() + 98.f * k, ps5_oy() + 86.f * k, rgba(0xf4, 0xf2, 0xf4, alpha), title);
}

// A PS5 button, centred: 70 high; 331 wide, faint glass, white text without
// the focus; 389 wide, near white, dark text, a thin ring a little way out,
// with it.
void ps5_button(ImDrawList* d, ImVec2 centre, const char* label, bool focus, bool down, float alpha,
                float width = 0.f) {
    const float k = ps5_k();
    const float h = 70.f * k, w = width > 0.f ? width : (focus ? 389.f : 331.f) * k;
    const ImVec2 a{centre.x - w * 0.5f, centre.y - h * 0.5f}, b{centre.x + w * 0.5f, centre.y + h * 0.5f};
    if (focus) {
        const float gap = 4.8f * k;
        d->AddRect(ImVec2{a.x - gap, a.y - gap}, ImVec2{b.x + gap, b.y + gap}, rgba(0xff, 0xff, 0xff, 0.42f * alpha),
                   h * 0.5f + gap, 0, 2.2f * k);
        d->AddRectFilled(a, b, down ? rgba(0xc8, 0xc8, 0xd0, alpha) : rgba(0xf7, 0xf7, 0xff, alpha), h * 0.5f);
    } else {
        d->AddRectFilled(a, b, rgba(0xff, 0xff, 0xff, 0.085f * alpha), h * 0.5f);
    }
    // Bold on the glass, regular on the focus, as the PS5 sets them.
    ImFont* f = focus ? ps5_regular() : ps5_medium();
    const float size = ps5_size(ps5_medium(), "View Details", 168.f * k, 20.5f * k);
    const float tw = ps5_width(f, size, label);
    const float cap = f->FindGlyph('H') != nullptr ? (f->FindGlyph('H')->Y1 - f->FindGlyph('H')->Y0) * size / f->FontSize
                                                   : size * 0.7f;
    ps5_text(d, f, size, centre.x - tw * 0.5f, centre.y - cap * 0.5f,
             focus ? rgba(0x3a, 0x39, 0x40, alpha) : rgba(0xff, 0xff, 0xff, alpha), label);
}

// The no-entry sign before "Don't turn off your PS5".
void ps5_no_sign(ImDrawList* d, ImVec2 c, float r, ImU32 colour) {
    d->AddCircle(c, r - r * 0.1f, colour, 32, r * 0.2f);
    d->AddLine(ImVec2{c.x - r * 0.62f, c.y - r * 0.62f}, ImVec2{c.x + r * 0.62f, c.y + r * 0.62f}, colour, r * 0.2f);
}

// The body's size: as the PS5's "The PS5 system software will be updated."
// (548 wide, capitals 20.5).
float ps5_body_size(ImFont* f) {
    return ps5_size(f, "The PS5 system software will be updated.", 548.f * ps5_k(), 20.5f * ps5_k());
}

// The body: lines left-aligned as a block, the block centred, 61 apart.
void ps5_body(ImDrawList* d, const char* const* lines, int count, float first_cap_top, float alpha) {
    const ImVec2 sc = screen();
    const float k = ps5_k();
    ImFont* f = ps5_regular();
    const float size = ps5_body_size(f);
    float widest = 0.f;
    for (int i = 0; i < count; i++)
        widest = std::max(widest, ps5_width(f, size, lines[i]));
    for (int i = 0; i < count; i++)
        ps5_text(d, f, size, (sc.x - widest) * 0.5f, first_cap_top + i * 61.f * k, rgba(0xe7, 0xe6, 0xed, alpha),
                 lines[i]);
}

// https://www.youtube.com/watch?v=OrOYvVf6tIM, level H (macOS CIQRCodeGenerator), 1 = a module.
const char* const kUpdateQr[37] = {
    "1111111000110110010000001010001111111",
    "1000001000111101111010001110001000001",
    "1011101011111001001100101100101011101",
    "1011101010101110010101101110101011101",
    "1011101001100001000010000000001011101",
    "1000001001111100011111011010001000001",
    "1111111010101010101010101010101111111",
    "0000000000010101010010011111100000000",
    "0001101101011001110011010110000001100",
    "1100110110010101111101101111011110100",
    "0110111111101011001111101101011001101",
    "0111100000111110110110000011011101100",
    "0100001001101000100111010001101101100",
    "1010100010100010000110001101110010100",
    "1001111100000111101111011111011011101",
    "0010000010111001110011011001111111110",
    "1001111010010101100100011111001100101",
    "1100100011101000101011111101000101010",
    "1100011011101010010001110001011001001",
    "0011000011100110110111000100111001010",
    "0101001111000111000001110011111010000",
    "0101010001010011001110011101110111010",
    "1111101110110110111111101011001010001",
    "1011100111110010010110010011110100100",
    "1001001110001011011000101100001001000",
    "1101000001001000101101000111000011100",
    "1111001010101010100000011011101000011",
    "1011010110010111000110110001010001110",
    "1011101011001011110010001101111110011",
    "0000000011000100111111110110100011100",
    "1111111011100110110000010000101011101",
    "1000001001000110000101100111100011010",
    "1011101011110111001011011011111110011",
    "1011101011000110011010001010101100000",
    "1011101000100100011100010100000110011",
    "1000001001101110011001110001001111111",
    "1111111001011101100101100010111001001",
};

// The QR code as the PS5 shows it: light modules in a dark rounded square,
// the PlayStation shapes on a dark patch in its middle (the code's error
// correction reads through it).
void ps5_qr(ImDrawList* d, ImVec2 at, float box, float alpha) {
    const float k = ps5_k();
    d->AddRectFilled(at, ImVec2{at.x + box, at.y + box}, rgba(0x14, 0x14, 0x1a, 0.62f * alpha), 6.f * k);
    const float inset = 16.f * k, m = (box - 2.f * inset) / 37.f;
    const ImU32 light = rgba(0xf2, 0xf2, 0xf4, alpha);
    for (int y = 0; y < 37; y++)
        for (int x = 0; x < 37; x++)
            if (kUpdateQr[y][x] == '1' && !(x >= 14 && x <= 22 && y >= 14 && y <= 22))
                d->AddRectFilled(ImVec2{at.x + inset + x * m, at.y + inset + y * m},
                                 ImVec2{at.x + inset + (x + 1) * m, at.y + inset + (y + 1) * m}, light);
    // The shapes: triangle and circle above, cross and square below.
    const ImVec2 c{at.x + inset + 18.5f * m, at.y + inset + 18.5f * m};
    const float r = m * 1.15f, w = std::max(1.f, m * 0.38f), s2 = m * 1.55f;
    const ImVec2 tri{c.x - s2, c.y - s2}, cir{c.x + s2, c.y - s2}, crs{c.x - s2, c.y + s2}, sq{c.x + s2, c.y + s2};
    d->AddTriangle(ImVec2{tri.x, tri.y - r}, ImVec2{tri.x + r * 0.9f, tri.y + r * 0.6f},
                   ImVec2{tri.x - r * 0.9f, tri.y + r * 0.6f}, light, w);
    d->AddCircle(cir, r * 0.85f, light, 16, w);
    d->AddLine(ImVec2{crs.x - r * 0.75f, crs.y - r * 0.75f}, ImVec2{crs.x + r * 0.75f, crs.y + r * 0.75f}, light, w);
    d->AddLine(ImVec2{crs.x - r * 0.75f, crs.y + r * 0.75f}, ImVec2{crs.x + r * 0.75f, crs.y - r * 0.75f}, light, w);
    d->AddRect(ImVec2{sq.x - r * 0.75f, sq.y - r * 0.75f}, ImVec2{sq.x + r * 0.75f, sq.y + r * 0.75f}, light, 0.f, 0,
               w);
}

// A notice in the PS5's manner (Temperature Alert, an update that failed):
// the room, its name, the message, OK. Any button presses OK (after
// `min_s`); `pressed` keeps when.
void ps5_notice(const Step& s, double shown_at, const char* title, const char* const* lines, int count,
                double min_s, double* pressed) {
    ImDrawList* d = canvas();
    const ImVec2 sc = screen();
    const float k = ps5_k();
    d->AddRectFilled(ImVec2{0, 0}, sc, IM_COL32(0, 0, 0, 0xff));
    ps5_room(d, kPs5Room, 1.f);
    ps5_title(d, title, 1.f);
    ps5_body(d, lines, count, ps5_oy() + 520.f * k - (count - 1) * 31.f * k, 1.f);
    if (*pressed < 0.0 && s.press && s.t - shown_at > min_s)
        *pressed = s.t;
    const bool down = *pressed >= 0.0 && s.t - *pressed < 0.18;
    ps5_button(d, ImVec2{sc.x * 0.5f, ps5_oy() + 953.f * k}, "OK", true, down, 1.f);
}

// Temperature Alert, as the PS5 shows it: OK (any button, or in a while by
// itself) and the PS5 turns off: the screen fades to black and stays dark a
// few seconds before the game comes back.
bool ps5_too_hot(const Step& s) {
    static double pressed = -1.0;
    if (s.first)
        pressed = -1.0;
    static const char* const kLines[] = {
        "Your PS5 is too hot. Turn off your PS5, and wait until the temperature goes down."};
    if (s.t < kGiveWay) {
        canvas()->AddRectFilled(ImVec2{0, 0}, screen(), IM_COL32(0, 0, 0, 0xff));
        return true;
    }
    if (pressed < 0.0 && s.t > 20.0)
        pressed = s.t;
    if (pressed < 0.0 || s.t - pressed < 0.25) {
        ps5_notice(s, 0.0, "Temperature Alert", kLines, 1, 1.0, &pressed);
        return true;
    }
    const double off = s.t - pressed - 0.25;
    ImDrawList* d = canvas();
    d->AddRectFilled(ImVec2{0, 0}, screen(), IM_COL32(0, 0, 0, 0xff));
    if (off < 0.9) {
        ps5_room(d, kPs5Room, static_cast<float>(1.0 - off / 0.9));
        return true;
    }
    return off < 0.9 + 3.5;
}

// The notice's tone: ours, a system alert in the PS5's manner (two soft
// glassy notes, falling, over a breath of air), made here, not copied.
std::vector<int16_t> notice_tone(int rate) {
    const int n = static_cast<int>(rate * 1.1);
    std::vector<float> mix(n, 0.f);
    struct Note {
        double at, freq, decay, gain;
    };
    static const Note kNotes[] = {{0.000, 1318.5, 0.16, 0.50},  // E6
                                  {0.000, 2637.0, 0.07, 0.10},  // its octave, a touch
                                  {0.120, 987.8, 0.34, 0.55},   // B5, left ringing
                                  {0.120, 1975.5, 0.12, 0.08}};
    for (const Note& note : kNotes)
        for (int i = static_cast<int>(note.at * rate); i < n; i++) {
            const double t = static_cast<double>(i) / rate - note.at;
            const double env = (1.0 - std::exp(-t / 0.004)) * std::exp(-t / note.decay);
            const double tone = std::sin(2.0 * kPi * note.freq * t) + 0.3 * std::sin(2.0 * kPi * (note.freq + 1.9) * t);
            mix[i] += static_cast<float>(note.gain * env * tone);
        }
    // Air: noise, smoothed, swelling and gone in a sixth of a second.
    float smooth = 0.f;
    std::srand(4321);
    for (int i = 0; i < static_cast<int>(rate * 0.16); i++) {
        const double t = static_cast<double>(i) / rate;
        const float noise = static_cast<float>(std::rand() % 2001 - 1000) / 1000.f;
        smooth += (noise - smooth) * 0.15f;
        mix[i] += static_cast<float>(0.07 * std::sin(kPi * t / 0.16) * smooth);
    }
    float peak = 0.001f;
    for (float v : mix)
        peak = std::max(peak, std::fabs(v));
    std::vector<int16_t> out(n);
    for (int i = 0; i < n; i++)
        out[i] = static_cast<int16_t>(std::lround(mix[i] / peak * 0.8f * 32767.f));
    return out;
}

// A system notice, as the PS5 slides one in at the top right (where its
// trophies come: 660 wide, 24 in from the right, 38 down): an alert's
// picture, a line in white and what it means in grey under it; its tone.
bool ps5_modified_notice(const Step& s) {
    constexpr double kShown = 7.5;
    if (s.first) {
        static const std::vector<int16_t> tone = notice_tone(48000);
        audio_play_host_sound(tone.data(), static_cast<uint32_t>(tone.size()), 48000u, 0.5f);
    }
    static const char* const kTitle = "Modified system software detected";
    static const char* const kBody =
        "This PS5\xE2\x84\xA2 console has been identified as modified. Some features may be restricted.";
    ImDrawList* d = canvas();
    const float k = ps5_k();
    const double in = std::min(1.0, s.t / 0.25), out = std::min(1.0, std::max(0.0, (kShown - s.t) / 0.35));
    const float a = static_cast<float>(std::min(in, out));
    ImFont* f = ps5_medium();
    // The trophy's two sizes (measured from the PS5's): its name's, its line's.
    const float title_size = ps5_size(f, "1 down, lots more to go!", 330.f * k);
    ImFont* body = ps5_regular();
    const float body_size = ps5_size(body, "Trophy earned!", 168.f * k);
    const float w = 660.f * k, pic = 66.f * k;
    const float left = 22.f * k + pic + 22.f * k, wrap = w - left - 26.f * k;
    const float body_h = body->CalcTextSizeA(body_size, FLT_MAX, wrap, kBody).y;
    const float h = std::max(110.f * k, 30.f * k + title_size + 4.f * k + body_h + 22.f * k);
    const ImVec2 at{ps5_ox() + 1920.f * k - w - 24.f * k, ps5_oy() + 38.f * k - (1.f - static_cast<float>(in)) * 10.f * k};
    d->AddRectFilled(at, ImVec2{at.x + w, at.y + h}, rgba(0x22, 0x22, 0x25, 0.92f * a), 10.f * k);
    // The alert: a white "!" in a ring on the system's blue.
    const ImVec2 pa{at.x + 22.f * k, at.y + 22.f * k};
    d->AddRectFilled(pa, ImVec2{pa.x + pic, pa.y + pic}, rgba(0x00, 0x5e, 0xc4, a), 12.f * k);
    const ImVec2 c{pa.x + pic * 0.5f, pa.y + pic * 0.5f};
    d->AddCircle(c, pic * 0.3f, rgba(0xff, 0xff, 0xff, a), 32, 3.2f * k);
    d->AddRectFilled(ImVec2{c.x - 2.2f * k, c.y - pic * 0.17f}, ImVec2{c.x + 2.2f * k, c.y + pic * 0.05f},
                     rgba(0xff, 0xff, 0xff, a), 1.5f * k);
    d->AddCircleFilled(ImVec2{c.x, c.y + pic * 0.14f}, 2.8f * k, rgba(0xff, 0xff, 0xff, a), 12);
    ps5_text(d, f, title_size, at.x + left, at.y + 30.f * k, rgba(0xf4, 0xf4, 0xf6, a), kTitle);
    d->AddText(body, body_size, ImVec2{at.x + left, at.y + 30.f * k + title_size + 4.f * k - cap_offset(body, body_size)},
               rgba(0xa4, 0xa4, 0xaa, a), kBody, nullptr, wrap);
    return s.t < kShown;
}

// System software 14.20, as the PS5 asks today: the two lines, the update's
// details behind a QR code, Cancel and Update. Whichever is pressed, it
// restarts into the install; the bar climbs to 100%, stays a couple of
// seconds, and the game is back, as if none of it was real.
bool ps5_update(const Step& s) {
    constexpr double kDark = 1.6, kClimb = 11.0, kFull = 2.2;
    static double chosen = -1.0;
    static int focus = 1;
    if (s.first) {
        chosen = -1.0;
        focus = 1;
    }
    ImDrawList* d = canvas();
    const ImVec2 sc = screen();
    const float k = ps5_k(), oy = ps5_oy();
    // Half a second of black first, as the game gives way to the system.
    d->AddRectFilled(ImVec2{0, 0}, sc, IM_COL32(0, 0, 0, 0xff));
    if (s.t < kGiveWay)
        return true;
    const double t = s.t - kGiveWay;
    if (chosen < 0.0 && ((s.press && t > 0.6) || t > 45.0))
        chosen = t;
    else if (chosen < 0.0)
        focus = std::clamp(focus + sideways_press(), 0, 1);
    const double asked = chosen < 0.0 ? 1e9 : chosen + 0.22;
    d->AddRectFilled(ImVec2{0, 0}, sc, IM_COL32(0, 0, 0, 0xff));
    if (t < asked) {
        // Measured from the PS5's own (1080 lines).
        ps5_room(d, kPs5Room, 1.f);
        ps5_title(d, "System Software Update", 1.f);
        static const char* const kLines[] = {"The PS5 system software will be updated.",
                                             "Your PS5 will restart and start the update."};
        ps5_body(d, kLines, 2, oy + 318.f * k, 1.f);
        // The details: two lines beside the code, the pair centred.
        ImFont* f = ps5_regular();
        const float size = ps5_body_size(f);
        const char* q1 = "To check the update details, scan the";
        const char* q2 = "QR code.";
        const float box = 214.f * k, gap = 34.f * k;
        const float left = (sc.x - (ps5_width(f, size, q1) + gap + box)) * 0.5f;
        const ImU32 text = rgba(0xe7, 0xe6, 0xed, 1.f);
        ps5_text(d, f, size, left, oy + 675.f * k, text, q1);
        ps5_text(d, f, size, left, oy + 718.f * k, text, q2);
        ps5_qr(d, ImVec2{left + ps5_width(f, size, q1) + gap, oy + 603.f * k}, box, 1.f);
        static const char* const kButtons[] = {"Cancel", "Update"};
        for (int i = 0; i < 2; i++)
            ps5_button(d, ImVec2{sc.x * 0.5f + (i == 0 ? -196.f : 196.f) * k, oy + 951.f * k}, kButtons[i], i == focus,
                       i == focus && chosen >= 0.0, 1.f, 378.f * k);
        return true;
    }
    if (t < asked + kDark)
        return true;  // dark, restarting
    const double u = t - asked - kDark;
    if (u < kClimb + kFull) {
        const float in = static_cast<float>(std::min(1.0, u / 0.6));
        ps5_room(d, kPs5Install, in);
        ps5_motes(d, t, in);
        ps5_title(d, "System Software Update", in);
        ImFont* f = ps5_face();
        const float size = ps5_size(f, "Installing...", 212.f * k, 25.f * k);
        const ImU32 text = rgba(0xd2, 0xd0, 0xd6, in);
        auto centred = [&](float cap_top, const char* line) {
            ps5_text(d, f, size, (sc.x - ps5_width(f, size, line)) * 0.5f, cap_top, text, line);
        };
        centred(oy + 383.f * k, "Installing...");
        // Quick at first, then slower: 100 at the end.
        const double share = 1.0 - std::pow(1.0 - std::min(1.0, u / kClimb), 1.8);
        char percent[16];
        std::snprintf(percent, sizeof percent, "%d%%", static_cast<int>(share * 100.0 + 1e-6));
        centred(oy + 466.f * k, percent);
        const float bw = 1068.f * k, by = oy + 538.f * k;
        const float bx = (sc.x - bw) * 0.5f;
        d->AddRectFilled(ImVec2{bx, by - 1.5f * k}, ImVec2{bx + bw, by + 1.5f * k}, rgba(0xff, 0xff, 0xff, 0.38f * in));
        d->AddRectFilled(ImVec2{bx, by - 2.6f * k}, ImVec2{bx + bw * static_cast<float>(share), by + 2.6f * k},
                         rgba(0xf4, 0xf4, 0xf6, in), 2.6f * k);
        const char* warn = "Don't turn off your PS5. Don't insert or eject discs.";
        const float icon = 41.f * k, igap = 17.f * k;
        const float ww = ps5_width(f, size, warn), wx = (sc.x - ww - icon - igap) * 0.5f;
        const ImFontGlyph* hg = f->FindGlyph('H');
        const float cap = hg != nullptr ? (hg->Y1 - hg->Y0) * size / f->FontSize : size * 0.7f;
        ps5_no_sign(d, ImVec2{wx + icon * 0.5f, oy + 676.f * k + cap * 0.5f}, icon * 0.5f, rgba(0xf0, 0xf0, 0xf2, in));
        ps5_text(d, f, size, wx + icon + igap, oy + 676.f * k, text, warn);
        return true;
    }
    return false;
}

// --- DualSense -------------------------------------------------------------------

// The DualSense in hand (a PS5's own, or one on a computer): its light bar
// bleeds to red, a heartbeat in its motors, and its triggers go stiff.
SDL_Gamepad* dualsense() {
    int count = 0;
    SDL_Gamepad* found = nullptr;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count); ids != nullptr) {
        for (int i = 0; i < count && found == nullptr; i++)
            if (SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
                pad != nullptr && (platform() == kPS5 || SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_PS5))
                found = pad;
        SDL_free(ids);
    }
    return found;
}

bool dualsense_present() { return dualsense() != nullptr; }

// SDL's DualSense output report (SDL_hidapi_ps5.c DS5EffectsState_t): the
// triggers' effects, 11 bytes each from byte 10.
void dualsense_triggers(SDL_Gamepad* pad, bool stiff) {
    Uint8 report[47] = {};
    report[0] = 0x04 | 0x08;  // right and left trigger effects
    // 0x01: resistance from `start` with `force`; 0x05: off.
    const Uint8 on[3] = {0x01, 0x00, 0xff}, off[3] = {0x05, 0x00, 0x00};
    for (int side = 0; side < 2; side++)
        std::memcpy(report + 10 + side * 11, stiff ? on : off, 3);
    SDL_SendGamepadEffect(pad, report, sizeof report);
}

bool dualsense_dread(const Step& s) {
    constexpr double kFor = 12.0;
    SDL_Gamepad* pad = dualsense();
    static bool stiff = false;
    if (s.first)
        stiff = false;
    if (pad == nullptr)
        return false;
    const double t = s.t;
    if (t >= kFor) {
        SDL_SetGamepadLED(pad, 0x00, 0x00, 0x40);  // SDL's first player blue
        SDL_RumbleGamepad(pad, 0, 0, 0);
        if (stiff)
            dualsense_triggers(pad, false);
        return false;
    }
    // Red creeps in over six seconds, pulsing with the heart.
    const double beat = std::fmod(t * (1.0 + t / kFor), 1.0);  // quickening
    const double lub = beat < 0.12 ? 1.0 : beat > 0.24 && beat < 0.34 ? 0.7 : 0.0;
    const double red = std::min(1.0, t / 6.0);
    const double glow = 0.55 + 0.45 * lub;
    SDL_SetGamepadLED(pad, static_cast<Uint8>(255 * red * glow), 0, static_cast<Uint8>(64 * (1.0 - red) * glow));
    SDL_RumbleGamepad(pad, lub > 0.0 ? static_cast<Uint16>(0xffff * (0.35 + 0.4 * red) * lub) : 0, 0, 120);
    // The triggers stiffen for the middle four seconds.
    const bool want = t > 4.0 && t < 8.0;
    if (want != stiff) {
        stiff = want;
        dualsense_triggers(pad, stiff);
    }
    return true;
}

// --- The effects ---------------------------------------------------------------

// Modal: the game held while it plays; Overlay: over the game as it plays;
// Menu: in our menu, the next time it opens.
enum class Kind { Modal, Overlay, Menu };

struct Effect {
    const char* name;
    const char* note;
    unsigned platforms;
    Kind kind;
    bool (*step)(const Step&);            // one frame of it; false once it is over
    bool (*after)(const Step&) = nullptr;  // a Modal's sequel over the game (a notification)
    bool (*available)() = nullptr;         // only while this holds (a DualSense in hand)
    // It ends on a screen of its own (black, a system's): the game's picture
    // stays covered until its "This isn't happening!" has begun, so not one
    // frame of it shows between the two.
    bool covers_after = false;
};

const Effect kEffects[] = {
    {"Save states wiped", "Next time our menu opens, its save slots are deleted one by one. They come back.",
     kEvery, Kind::Menu, nullptr},
    {"Mac: spinning wait cursor", "The game stops answering: the picture holds, the beach ball spins.", kMac,
     Kind::Modal, mac_beach_ball},
    {"Mac: quit unexpectedly", "The game is gone and the Mac's crash box asks to reopen it.", kMac, Kind::Modal,
     mac_quit_unexpectedly, nullptr, nullptr, true},
    {"PC: display driver crash",
     "The picture hangs with the sound stuck, goes black for seconds, comes back slowly and flickers; Windows "
     "warns its display driver stopped responding.",
     kWindows, Kind::Modal, win_driver_reset, win_driver_toast},
    {"PC: restart for updates", "Windows says it will restart in a minute to install updates.", kWindows,
     Kind::Overlay, win_update_toast},
    {"PC: someone else's pointer", "A mouse pointer wanders across the picture by itself, clicks, and is gone.",
     kWindows, Kind::Overlay, win_pointer},
    {"PC: screen lines", "As you play, a few thin coloured lines appear down the screen, flicker, and go.",
     kWindows, Kind::Overlay, screen_lines},
    {"Deck: not responding", "The game stops answering and Steam offers to force it to quit. Any button waits.",
     kDeck | kLinux, Kind::Modal, deck_not_responding},
    {"Deck: screen lines", "As you play, a few thin coloured lines appear down the screen, flicker, and go.",
     kDeck | kLinux, Kind::Overlay, screen_lines},
    {"Linux: kernel panic", "The game crashes and takes Linux down with it: the console scrolls to a kernel panic.",
     kDeck | kLinux, Kind::Modal, linux_kernel_panic, nullptr, nullptr, true},
    {"PS5: too hot", "The PS5 warns it is too hot, says it is turning off, and fades to black.", kPS5, Kind::Modal,
     ps5_too_hot, nullptr, nullptr, true},
    {"PS5: modified console", "A system notice slides in: this PS5 has been identified as modified.", kPS5,
     Kind::Overlay, ps5_modified_notice},
    {"PS5: system update 14.20", "The PS5 asks to update to 14.20, either button installs it to 100%, then the game.",
     kPS5, Kind::Modal,
     ps5_update, nullptr, nullptr, true},
    {"DualSense: dread", "The controller's light bleeds red, a heartbeat in your hands, the triggers go stiff.",
     kEvery, Kind::Overlay, dualsense_dread, nullptr, dualsense_present},
};
constexpr int kEffectCount = sizeof kEffects / sizeof kEffects[0];

// This machine's effects, as indexes into kEffects.
const std::vector<int>& mine() {
    static std::vector<int> list;
    list.clear();
    for (int i = 0; i < kEffectCount; i++)
        if ((kEffects[i].platforms & platform()) && (kEffects[i].available == nullptr || kEffects[i].available()))
            list.push_back(i);
    return list;
}

int g_armed = -1;  // the kEffects entry waiting to play

// The save states: deleting (from g_wipe_start), then shown wiped until our
// menu closes.
Uint64 g_wipe_start = 0;
bool g_wiped = false;

void run_modal(const Effect& e) {
    std::fprintf(stderr, "[insanity] %s\n", e.name);
    // The game held, as our menu holds it: no frames, but no hang either.
    report_pause(true);
    // ED_OWN_INSANITY_PRESS=s1,s2,... (testing): a press that many seconds in.
    std::vector<double> presses;
    if (const char* p = std::getenv("ED_OWN_INSANITY_PRESS"); p != nullptr)
        for (const char* at = p; *at != '\0';) {
            char* end = nullptr;
            presses.push_back(std::strtod(at, &end));
            at = *end == ',' ? end + 1 : end;
            if (end == at && *at != '\0')
                break;
        }
    // ED_OWN_INSANITY_KEYS=s:left,s:right,s:press,... (testing): those, then.
    std::vector<std::pair<double, int>> keys;
    if (const char* p = std::getenv("ED_OWN_INSANITY_KEYS"); p != nullptr) {
        std::string all = p;
        for (size_t at = 0; at < all.size();) {
            size_t end = all.find(',', at);
            if (end == std::string::npos)
                end = all.size();
            const std::string item = all.substr(at, end - at);
            const size_t colon = item.find(':');
            if (colon != std::string::npos) {
                const std::string what = item.substr(colon + 1);
                keys.emplace_back(std::atof(item.substr(0, colon).c_str()),
                                  what == "left" ? -1 : what == "right" ? 1 : 0);
            }
            at = end + 1;
        }
    }
    size_t next_press = 0, next_key = 0;
    const Uint64 start = SDL_GetTicksNS();
    for (bool first = true;; first = false) {
        const double t = static_cast<double>(SDL_GetTicksNS() - start) / 1e9;
        bool press = any_press();
        if (next_press < presses.size() && t >= presses[next_press]) {
            press = true;
            next_press++;
        }
        if (next_key < keys.size() && t >= keys[next_key].first) {
            if (keys[next_key].second == 0)
                press = true;
            else
                g_test_sideways = keys[next_key].second;
            next_key++;
        }
        const Step s{t, first, press};
        const bool more = e.step(s);
        if (!dol_aurora_ui_frame() || !more)
            break;
    }
    report_pause(false);
}

}  // namespace

int count() { return static_cast<int>(mine().size()); }
const char* name(int effect) { return kEffects[mine()[effect]].name; }
const char* note(int effect) { return kEffects[mine()[effect]].note; }

void play(int effect) {
    if (effect >= 0 && effect < count())
        g_armed = mine()[effect];
}

bool menu_frame(bool in_game_menu) {
    if (g_armed < 0 || kEffects[g_armed].kind != Kind::Menu || !in_game_menu)
        return false;
    constexpr double kDeleting = 4.5;
    const Uint64 now = SDL_GetTicks();
    if (g_wipe_start == 0 && !g_wiped) {
        g_wipe_start = now;
        std::fprintf(stderr, "[insanity] %s\n", kEffects[g_armed].name);
    }
    if (g_wiped)
        return false;
    const double t = (now - g_wipe_start) / 1000.0;
    if (t >= kDeleting) {
        g_wiped = true;
        return true;
    }
    // A box in our menu's own look (menu.rcss: parchment, ink, the red of
    // its titles) over it: each slot deleted in turn.
    ImDrawList* d = canvas();
    const ImVec2 sc = screen();
    const float k = dp();
    const float w = 420.f * k, h = 150.f * k;
    const ImVec2 a{(sc.x - w) * 0.5f, (sc.y - h) * 0.5f}, b{a.x + w, a.y + h};
    const ImU32 ink = IM_COL32(0x2a, 0x18, 0x10, 0xff), red = IM_COL32(0x8e, 0x1b, 0x12, 0xff);
    const ImU32 brown = IM_COL32(0x5e, 0x3f, 0x1c, 0xff);
    shadow(d, a, b, 2.f * k, 16.f * k, 0.6f);
    d->AddRectFilled(a, b, IM_COL32(0xe8, 0xdb, 0xbd, 0xff), 2.f * k);
    d->AddRect(a, b, brown, 2.f * k, 0, 2.f * k);
    d->AddRect(ImVec2{a.x + 7.f * k, a.y + 7.f * k}, ImVec2{b.x - 7.f * k, b.y - 7.f * k},
               IM_COL32(0x8e, 0x1b, 0x12, 0x80), 0.f, 0, 1.f * k);
    ImFont* title = g_faces.menu_title != nullptr ? g_faces.menu_title : ImGui::GetFont();
    ImFont* face = g_faces.menu != nullptr ? g_faces.menu : ImGui::GetFont();
    const double share = t / kDeleting;
    const int slot = std::min(4, 1 + static_cast<int>(share * 4.0));
    const char* heading = "Deleting save states\u2026";
    const float title_size = 30.f * k;
    const ImVec2 ts = text_size(title, title_size, heading);
    d->AddText(title, title_size, ImVec2{(sc.x - ts.x) * 0.5f, a.y + 24.f * k}, red, heading);
    char line[64];
    std::snprintf(line, sizeof line, "Slot %d of 4", slot);
    const float line_size = 22.f * k;
    const ImVec2 ls = text_size(face, line_size, line);
    d->AddText(face, line_size, ImVec2{(sc.x - ls.x) * 0.5f, a.y + 64.f * k}, ink, line);
    const ImVec2 bar_a{a.x + 40.f * k, b.y - 38.f * k}, bar_b{b.x - 40.f * k, b.y - 30.f * k};
    d->AddRectFilled(bar_a, bar_b, IM_COL32(0xd2, 0xbf, 0x98, 0xff));
    d->AddRectFilled(bar_a, ImVec2{bar_a.x + (bar_b.x - bar_a.x) * static_cast<float>(share), bar_b.y}, brown);
    return false;
}

bool slots_hidden() { return g_wiped; }

void menu_closed() {
    if (g_armed < 0 || kEffects[g_armed].kind != Kind::Menu || (g_wipe_start == 0 && !g_wiped))
        return;
    g_armed = -1;
    g_wipe_start = 0;
    g_wiped = false;
    hle_insanity_recovered();
}

}  // namespace insanity

using namespace insanity;

extern "C" bool insanity_take_roll(int level) {
    // One roll in three, while none of ours is waiting. Only below half
    // sanity (level 2 and down): the game says "This isn't happening!" only
    // there (0x80113E04 drops the request otherwise), and each of ours ends
    // with it.
    if (level < 2 || g_armed >= 0 || mine().empty() || std::rand() % 3 != 0)
        return false;
    g_armed = mine()[std::rand() % mine().size()];
    std::fprintf(stderr, "[insanity] the game's roll (sanity level %d): %s\n", level, kEffects[g_armed].name);
    return true;
}

extern "C" void insanity_frame(void) {
    // ED_OWN_INSANITY_TEST=effect,seconds (testing): this machine's effect
    // (its place in the debug menu's list) that long after the first frame.
    static Uint64 test_at = 0;
    static int test_effect = -1;
    static bool test_read = false;
    if (!test_read) {
        test_read = true;
        int seconds = 0;
        if (const char* t = std::getenv("ED_OWN_INSANITY_TEST"); t != nullptr &&
                                                             std::sscanf(t, "%d,%d", &test_effect, &seconds) == 2)
            test_at = SDL_GetTicks() + static_cast<Uint64>(seconds) * 1000u;
    }
    if (test_at != 0 && SDL_GetTicks() >= test_at) {
        test_at = 0;
        play(test_effect);
        std::fprintf(stderr, "[insanity] test: effect %d of %d (%s)\n", test_effect, count(),
                     g_armed >= 0 ? kEffects[g_armed].name : "none");
    }
    if (ImGui::GetCurrentContext() == nullptr)
        return;
    static bool atlas_logged = false;
    if (!atlas_logged && ImGui::GetIO().Fonts->TexHeight > 0) {
        atlas_logged = true;
        std::fprintf(stderr, "[insanity] faces rasterized at %gx (font atlas %dx%d)\n", raster_density(),
                     ImGui::GetIO().Fonts->TexWidth, ImGui::GetIO().Fonts->TexHeight);
    }
    // One drawn over play: a frame of it each game frame.
    static bool (*over)(const Step&) = nullptr;
    static Uint64 over_start = 0;
    // Black over the game after one that ends on a screen of its own, until
    // the game's own "This isn't happening!" has begun (or a second and a
    // half, if it does not come).
    static Uint64 cover_from = 0;
    if (cover_from != 0) {
        if (hle_game_mode() == 12 || SDL_GetTicksNS() - cover_from > 1500000000ull)
            cover_from = 0;
        else
            canvas()->AddRectFilled(ImVec2{0, 0}, screen(), IM_COL32(0, 0, 0, 0xff));
    }
    if (over == nullptr && g_armed >= 0 && kEffects[g_armed].kind != Kind::Menu) {
        const Effect& e = kEffects[g_armed];
        g_armed = -1;
        if (e.kind == Kind::Modal) {
            run_modal(e);
            if (e.covers_after) {
                // This frame too: it is the game's next, drawn as the effect ends.
                cover_from = SDL_GetTicksNS();
                canvas()->AddRectFilled(ImVec2{0, 0}, screen(), IM_COL32(0, 0, 0, 0xff));
            }
        } else {
            std::fprintf(stderr, "[insanity] %s\n", e.name);
        }
        over = e.kind == Kind::Overlay ? e.step : e.after;
        over_start = 0;
        if (over == nullptr) {
            hle_insanity_recovered();
            return;
        }
    }
    if (over == nullptr)
        return;
    const bool first = over_start == 0;
    if (first)
        over_start = SDL_GetTicksNS();
    const Step s{static_cast<double>(SDL_GetTicksNS() - over_start) / 1e9, first, false};
    if (!over(s)) {
        over = nullptr;
        hle_insanity_recovered();
    }
}

extern "C" void insanity_imgui_init(void) {
    const std::string res = frontend_res_dir() + "/fonts/";
    std::vector<std::pair<std::string, int>> ui, bold, mono;
    switch (platform()) {
    case kMac:
        ui = {{"/System/Library/Fonts/SFNS.ttf", 0}};
        bold = {{"/Library/Fonts/SF-Pro-Text-Bold.otf", 0}, {"/System/Library/Fonts/HelveticaNeue.ttc", 1}};
        break;
    case kWindows:
        ui = {{"C:/Windows/Fonts/segoeui.ttf", 0}};
        bold = {{"C:/Windows/Fonts/seguisb.ttf", 0}, {"C:/Windows/Fonts/segoeuib.ttf", 0}};
        mono = {{"C:/Windows/Fonts/consola.ttf", 0}};
        break;
    case kDeck:
    case kLinux:
        ui = {{home() + "/.local/share/Steam/clientui/fonts/GoNotoKurrent-Regular.ttf", 0},
              {"/usr/share/fonts/noto/NotoSans-Regular.ttf", 0}};
        bold = {{home() + "/.local/share/Steam/clientui/fonts/GoNotoKurrent-Bold.ttf", 0},
                {"/usr/share/fonts/noto/NotoSans-Bold.ttf", 0}};
        break;
    case kPS5:
        // The PS5's own SST where it can be read; else, for a look at them
        // on a Mac, the nearest it has: Seravek (Light for text, Medium for
        // buttons), another humanist sans much like it.
        ui = {{"/System/Library/Fonts/Supplemental/Seravek.ttc", 3},
              {"/System/Library/Fonts/HelveticaNeue.ttc", 10}};
        bold = {{"/System/Library/Fonts/Supplemental/Seravek.ttc", 9}};
        break;
    default:
        break;
    }
    // Console text: the system's mono, else any this machine has.
    for (const auto& m : std::vector<std::pair<std::string, int>>{
             {"/usr/share/fonts/TTF/DejaVuSansMono.ttf", 0},
             {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 0},
             {"/System/Library/Fonts/Menlo.ttc", 0},
             {"C:/Windows/Fonts/consola.ttf", 0}})
        mono.push_back(m);
    ui.push_back({res + "FiraSans-Regular.ttf", 0});
    bold.push_back({res + "FiraSans-Bold.ttf", 0});
    // Light: the PS5's own where it is (its port), else the nearest this
    // machine has.
    std::vector<std::pair<std::string, int>> light = {{"/System/Library/Fonts/HelveticaNeue.ttc", 7},
                                                      {"C:/Windows/Fonts/segoeuil.ttf", 0}};
    if (platform() == kPS5)
        light.insert(light.begin(), {"/System/Library/Fonts/Supplemental/Seravek.ttc", 5});
    light.insert(light.end(), ui.begin(), ui.end());
    g_faces.light = add_face(light, 36.f);
    std::vector<std::pair<std::string, int>> regular;
    if (platform() == kPS5)
        regular.push_back({"/System/Library/Fonts/Supplemental/Seravek.ttc", 0});
    regular.insert(regular.end(), ui.begin(), ui.end());
    g_faces.regular = add_face(regular, 36.f);
    g_faces.ui = add_face(ui, 32.f);
    g_faces.bold = add_face(bold, 32.f);
    g_faces.mono = add_face(mono, 32.f);
    g_faces.menu = add_face({{res + "IMFellEnglish-Regular.ttf", 0}}, 40.f);
    g_faces.menu_title = add_face({{res + "IMFellEnglishSC-Regular.ttf", 0}}, 40.f);
}

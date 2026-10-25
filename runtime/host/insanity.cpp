// The recomp's own insanity effects (insanity.h): fake system screens of the
// machine the game runs on, drawn through ImGui over the game as the FPS
// counter is, and our menu's save states wiped. A modal one holds the game
// while it plays, as our menu does (dol_aurora_ui_frame shows the held
// picture under it); the game's own "This isn't happening!" follows each.
#include "host/insanity.h"

#include "gxruntime/aurora_backend.h"
#include "host/buttons.h"
#include "host/frontend.h"

#include <aurora/imgui.h>
#include <imgui.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

extern "C" void hle_insanity_recovered(void);

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
    ImFont* bold = nullptr;
    ImFont* mono = nullptr;
    ImFont* menu = nullptr;  // our menu's faces (menu.rcss), for the save states
    ImFont* menu_title = nullptr;
} g_faces;

ImFont* add_face(const std::vector<std::pair<std::string, int>>& choices, float size) {
    static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x2190, 0x2193, 0};
    for (const auto& [path, index] : choices)
        if (exists(path)) {
            ImFontConfig config;
            config.FontNo = index;
            config.OversampleH = 2;
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

// The picture drops out, comes back, drops out again as the driver resets;
// then Windows says so.
bool win_driver_reset(const Step& s) {
    static const double kBlack[][2] = {{0.0, 0.3}, {0.42, 1.7}, {1.82, 2.6}};
    for (const auto& span : kBlack)
        if (s.t >= span[0] && s.t < span[1])
            canvas()->AddRectFilled(ImVec2{0, 0}, screen(), IM_COL32(0, 0, 0, 0xff));
    return s.t < 2.6;
}

bool win_driver_toast(const Step& s) {
    constexpr double kShown = 7.0;
    win_toast(s.t, kShown, "Windows", glyph_display, "Display driver stopped responding and has recovered",
              "Display driver stopped responding and has successfully recovered.", nullptr, nullptr);
    return s.t < kShown;
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

// The PS5's system look: white on near-black, light and airy type.
const ImU32 kPs5White = IM_COL32(0xff, 0xff, 0xff, 0xff);
const ImU32 kPs5Grey = IM_COL32(0xb4, 0xb4, 0xb4, 0xff);

ImVec2 centred(ImDrawList* d, ImFont* f, float size, float y, ImU32 colour, const char* text, float wrap = 0.f) {
    const ImVec2 sc = screen();
    const ImVec2 ts = text_size(f, size, text, wrap);
    const float x = wrap > 0.f ? (sc.x - wrap) * 0.5f : (sc.x - ts.x) * 0.5f;
    if (wrap > 0.f) {
        // Each wrapped line centred on its own.
        const char* at = text;
        const char* const text_end = text + std::strlen(text);
        float line_y = y;
        while (*at != '\0') {
            const char* end = f->CalcWordWrapPositionA(size / f->FontSize, at, text_end, wrap);
            if (end == at)
                end = at + 1;
            const ImVec2 ls = f->CalcTextSizeA(size, FLT_MAX, 0.f, at, end);
            d->AddText(f, size, ImVec2{(sc.x - ls.x) * 0.5f, line_y}, colour, at, end);
            line_y += size * 1.3f;
            at = end;
            while (*at == ' ')
                at++;
        }
        return ImVec2{x, line_y};
    }
    d->AddText(f, size, ImVec2{x, y}, colour, text);
    return ImVec2{x, y + ts.y};
}

// The PS5 warns that it is too hot, says it is turning off, and the screen
// fades to black as it would; after a few seconds dark, the game again.
bool ps5_too_hot(const Step& s) {
    constexpr double kWarn = 6.5, kTurning = 2.5, kFade = 1.2, kDark = 3.0;
    ImDrawList* d = canvas();
    const ImVec2 sc = screen();
    const float k = sc.y / 1080.f;  // the PS5 draws its notices for the screen, not a window
    d->AddRectFilled(ImVec2{0, 0}, sc, IM_COL32(0x08, 0x08, 0x0a, 0xff));
    const double t = s.t;
    if (t > kWarn + kTurning + kFade) {
        d->AddRectFilled(ImVec2{0, 0}, sc, IM_COL32(0, 0, 0, 0xff));  // off
        return t < kWarn + kTurning + kFade + kDark;
    }
    ImFont* ui = g_faces.ui != nullptr ? g_faces.ui : ImGui::GetFont();
    const float dim = t > kWarn + kTurning ? static_cast<float>(1.0 - (t - kWarn - kTurning) / kFade) : 1.f;
    const ImU32 white = rgba(0xff, 0xff, 0xff, dim), grey = rgba(0xb4, 0xb4, 0xb4, dim);
    if (t < kWarn) {
        // The warning sign: a rounded triangle with its mark.
        const ImVec2 c{sc.x * 0.5f, sc.y * 0.5f - 150.f * k};
        const float r = 40.f * k;
        d->AddTriangleFilled(ImVec2{c.x, c.y - r}, ImVec2{c.x + r * 1.1f, c.y + r * 0.8f}, ImVec2{c.x - r * 1.1f, c.y + r * 0.8f},
                             white);
        d->AddRectFilled(ImVec2{c.x - 3.5f * k, c.y - r * 0.45f}, ImVec2{c.x + 3.5f * k, c.y + r * 0.3f},
                         IM_COL32(0x08, 0x08, 0x0a, 0xff), 2.f * k);
        d->AddCircleFilled(ImVec2{c.x, c.y + r * 0.55f}, 4.5f * k, IM_COL32(0x08, 0x08, 0x0a, 0xff));
        centred(d, ui, 44.f * k, sc.y * 0.5f - 70.f * k, white, "Your PS5 is too hot.");
        centred(d, ui, 28.f * k, sc.y * 0.5f + 4.f * k, grey,
                "Turn off your PS5 and wait until it cools down. Make sure there's enough space around your PS5 for "
                "air to flow, and don't place it in a hot location.",
                900.f * k);
    } else {
        centred(d, ui, 36.f * k, sc.y * 0.5f - 24.f * k, white, "Turning off your PS5…");
    }
    return true;
}

// A trophy, as the PS5 announces one at the screen's top right: its picture
// (the game's), the bronze cup, and its name.
bool ps5_trophy(const Step& s) {
    constexpr double kShown = 6.5;
    ImDrawList* d = canvas();
    const ImVec2 sc = screen();
    const float k = sc.y / 1080.f;
    const double in = std::min(1.0, s.t / 0.3), out = std::min(1.0, std::max(0.0, (kShown - s.t) / 0.3));
    const float a = static_cast<float>(std::min(in, out));
    const float w = 470.f * k, h = 110.f * k;
    const ImVec2 at{sc.x - w - 52.f * k + (1.f - a) * 30.f * k, 46.f * k};
    const ImVec2 to{at.x + w, at.y + h};
    shadow(d, at, to, 14.f * k, 18.f * k, 0.5f * a);
    d->AddRectFilled(at, to, rgba(0x24, 0x25, 0x29, 0.96f * a), 14.f * k);
    const float pic = 78.f * k;
    const ImVec2 pa{at.x + 16.f * k, at.y + (h - pic) * 0.5f};
    if (ImTextureID id = picture("icon/window.png"); id != 0)
        d->AddImageRounded(id, pa, ImVec2{pa.x + pic, pa.y + pic}, ImVec2{0, 0}, ImVec2{1, 1}, rgba(0xff, 0xff, 0xff, a),
                           8.f * k);
    // The bronze cup: bowl, stem, foot.
    const ImU32 bronze = rgba(0xc9, 0x86, 0x4f, a);
    const ImVec2 cup{pa.x + pic + 22.f * k, at.y + 42.f * k};
    const float cw = 9.f * k;
    d->PathArcTo(ImVec2{cup.x, cup.y - 4.f * k}, cw, 0.f, kPi, 12);
    d->PathLineTo(ImVec2{cup.x - cw, cup.y - 10.f * k});
    d->PathLineTo(ImVec2{cup.x + cw, cup.y - 10.f * k});
    d->PathFillConvex(bronze);
    d->AddRectFilled(ImVec2{cup.x - 1.5f * k, cup.y + 4.f * k}, ImVec2{cup.x + 1.5f * k, cup.y + 9.f * k}, bronze);
    d->AddRectFilled(ImVec2{cup.x - 6.f * k, cup.y + 9.f * k}, ImVec2{cup.x + 6.f * k, cup.y + 12.f * k}, bronze,
                     1.f * k);
    ImFont* ui = g_faces.ui != nullptr ? g_faces.ui : ImGui::GetFont();
    ImFont* bold = g_faces.bold != nullptr ? g_faces.bold : ImGui::GetFont();
    d->AddText(ui, 22.f * k, ImVec2{cup.x + 18.f * k, at.y + 21.f * k}, rgba(0xc9, 0x86, 0x4f, a), "Bronze");
    d->AddText(bold, 27.f * k, ImVec2{pa.x + pic + 13.f * k, at.y + 56.f * k}, rgba(0xff, 0xff, 0xff, a),
               "This Isn't Happening");
    return s.t < kShown;
}

// A system software update to 14.20: the install screen, its bar creeping
// up to 99%, and there it stops... then an error, and the game, as if none
// of it was real.
bool ps5_update(const Step& s) {
    constexpr double kClimb = 11.0, kStuck = 4.0, kError = 3.5;
    ImDrawList* d = canvas();
    const ImVec2 sc = screen();
    const float k = sc.y / 1080.f;
    d->AddRectFilled(ImVec2{0, 0}, sc, IM_COL32(0x00, 0x00, 0x00, 0xff));
    if (s.t < 0.8)
        return true;  // dark, as it restarts into the update
    ImFont* ui = g_faces.ui != nullptr ? g_faces.ui : ImGui::GetFont();
    const double t = s.t - 0.8;
    if (t > kClimb + kStuck) {
        if (t > kClimb + kStuck + kError)
            return false;
        centred(d, ui, 40.f * k, sc.y * 0.5f - 70.f * k, kPs5White, "Can't update the system software.");
        centred(d, ui, 28.f * k, sc.y * 0.5f, kPs5Grey, "An error has occurred. (CE-108255-1)");
        return true;
    }
    centred(d, ui, 40.f * k, sc.y * 0.5f - 120.f * k, kPs5White, "Updating System Software");
    centred(d, ui, 26.f * k, sc.y * 0.5f - 62.f * k, kPs5Grey, "Version 14.20");
    // Quick at first, then slower and slower: 99 and no further.
    const double share = std::min(0.99, 0.99 * (1.0 - std::pow(1.0 - std::min(1.0, t / kClimb), 2.2)));
    const float bw = 760.f * k, bh = 6.f * k;
    const ImVec2 ba{(sc.x - bw) * 0.5f, sc.y * 0.5f + 10.f * k};
    d->AddRectFilled(ba, ImVec2{ba.x + bw, ba.y + bh}, IM_COL32(0x3a, 0x3a, 0x3d, 0xff), bh * 0.5f);
    d->AddRectFilled(ba, ImVec2{ba.x + bw * static_cast<float>(share), ba.y + bh}, kPs5White, bh * 0.5f);
    char line[64];
    std::snprintf(line, sizeof line, "Installing update file… (%d/2)", share < 0.42 ? 1 : 2);
    d->AddText(ui, 24.f * k, ImVec2{ba.x, ba.y + 22.f * k}, kPs5Grey, line);
    std::snprintf(line, sizeof line, "%d%%", static_cast<int>(share * 100.0));
    const ImVec2 ps = text_size(ui, 24.f * k, line);
    d->AddText(ui, 24.f * k, ImVec2{ba.x + bw - ps.x, ba.y + 22.f * k}, kPs5Grey, line);
    centred(d, ui, 22.f * k, sc.y - 120.f * k, kPs5Grey,
            "Don't turn off your PS5 or unplug the AC power cord while the update is being installed.");
    return true;
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
};

const Effect kEffects[] = {
    {"Save states wiped", "Next time our menu opens, its save slots are deleted one by one. They come back.",
     kEvery, Kind::Menu, nullptr},
    {"Mac: spinning wait cursor", "The game stops answering: the picture holds, the beach ball spins.", kMac,
     Kind::Modal, mac_beach_ball},
    {"Mac: quit unexpectedly", "The game is gone and the Mac's crash box asks to reopen it.", kMac, Kind::Modal,
     mac_quit_unexpectedly},
    {"PC: display driver crash", "The picture drops out twice, then Windows says its display driver recovered.",
     kWindows, Kind::Modal, win_driver_reset, win_driver_toast},
    {"PC: restart for updates", "Windows says it will restart in a minute to install updates.", kWindows,
     Kind::Overlay, win_update_toast},
    {"PC: someone else's pointer", "A mouse pointer wanders across the picture by itself, clicks, and is gone.",
     kWindows, Kind::Overlay, win_pointer},
    {"Deck: not responding", "The game stops answering and Steam offers to force it to quit. Any button waits.",
     kDeck | kLinux, Kind::Modal, deck_not_responding},
    {"Linux: kernel panic", "The game crashes and takes Linux down with it: the console scrolls to a kernel panic.",
     kDeck | kLinux, Kind::Modal, linux_kernel_panic},
    {"PS5: too hot", "The PS5 warns it is too hot, says it is turning off, and fades to black.", kPS5, Kind::Modal,
     ps5_too_hot},
    {"PS5: trophy", "A bronze trophy pops up: This Isn't Happening.", kPS5, Kind::Overlay, ps5_trophy},
    {"PS5: system update 14.20", "The PS5 installs system software 14.20... to 99%, then fails.", kPS5, Kind::Modal,
     ps5_update},
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
    const Uint64 start = SDL_GetTicksNS();
    for (bool first = true;; first = false) {
        const Step s{static_cast<double>(SDL_GetTicksNS() - start) / 1e9, first, any_press()};
        const bool more = e.step(s);
        if (!dol_aurora_ui_frame() || !more)
            break;
    }
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
    // One roll in three, while none of ours is waiting.
    if (g_armed >= 0 || mine().empty() || std::rand() % 3 != 0)
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
    // One drawn over play: a frame of it each game frame.
    static bool (*over)(const Step&) = nullptr;
    static Uint64 over_start = 0;
    if (over == nullptr && g_armed >= 0 && kEffects[g_armed].kind != Kind::Menu) {
        const Effect& e = kEffects[g_armed];
        g_armed = -1;
        if (e.kind == Kind::Modal)
            run_modal(e);
        else
            std::fprintf(stderr, "[insanity] %s\n", e.name);
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
    g_faces.ui = add_face(ui, 32.f);
    g_faces.bold = add_face(bold, 32.f);
    g_faces.mono = add_face(mono, 32.f);
    g_faces.menu = add_face({{res + "IMFellEnglish-Regular.ttf", 0}}, 40.f);
    g_faces.menu_title = add_face({{res + "IMFellEnglishSC-Regular.ttf", 0}}, 40.f);
}

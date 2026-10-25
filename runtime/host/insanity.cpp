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

// --- The effects ---------------------------------------------------------------

enum class Kind { Modal, Menu };

struct Effect {
    const char* name;
    const char* note;
    unsigned platforms;
    Kind kind;
    bool (*step)(const Step&);  // Modal: one frame of it; false once it is over
};

const Effect kEffects[] = {
    {"Save states wiped", "Next time our menu opens, its save slots are deleted one by one. They come back.",
     kEvery, Kind::Menu, nullptr},
    {"Mac: spinning wait cursor", "The game stops answering: the picture holds, the beach ball spins.", kMac,
     Kind::Modal, mac_beach_ball},
    {"Mac: quit unexpectedly", "The game is gone and the Mac's crash box asks to reopen it.", kMac, Kind::Modal,
     mac_quit_unexpectedly},
};
constexpr int kEffectCount = sizeof kEffects / sizeof kEffects[0];

// This machine's effects, as indexes into kEffects.
const std::vector<int>& mine() {
    static const std::vector<int> list = [] {
        std::vector<int> out;
        for (int i = 0; i < kEffectCount; i++)
            if (kEffects[i].platforms & platform())
                out.push_back(i);
        return out;
    }();
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
    if (g_armed < 0 || kEffects[g_armed].kind != Kind::Modal || ImGui::GetCurrentContext() == nullptr)
        return;
    const Effect& e = kEffects[g_armed];
    g_armed = -1;
    run_modal(e);
    hle_insanity_recovered();
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

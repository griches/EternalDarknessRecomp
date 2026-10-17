// The game's text redrawn sharp. Eternal Darkness draws its text from two
// 512x512 CMPR font sheets (UFonts.tpl: the plain font and the handwritten
// one), 32 pixels to a letter, soft at today's resolutions. When gxcore
// decodes either sheet it is rebuilt at 2048x2048: the letters from
// res/fonts/game/<style>-<sheet>.png (drawn by tools/ui/make_game_fonts.py,
// each fitted to the original letter's box so the game's spacing holds), and
// every cell those do not cover, the button icons first, from the game's own
// sheet, smoothly enlarged. The sheets are recognised by the signature of
// their transparency (res/fonts/game/signatures.txt).
//
// A rebuilt sheet is 16 MB and its letters a 2048x2048 PNG: made on the
// renderer's thread at the first text after a load, that held a frame for
// ~80 ms on the Steam Deck (the game thread waited, and its audio ran dry).
// The letters load on a worker at start, each sheet is built on a worker,
// and the game's own sheet shows for the frame or two that takes.
//
// The handwritten sheet's GameCube button icons (codes 0x01-0x1F, used in the
// game's hints and pages) are drawn with the icons of the buttons that do the
// same thing in the layout in use, as the prompt sheet is (buttons.cpp). Its
// two latest builds are kept, so the pause menu's own buttons opening and
// closing it do not rebuild it each time.
#include "host/fonts.h"

#include "host/buttons.h"
#include "host/controls.h"
#include "host/frontend.h"
#include "host/settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kCmpr = 14;
constexpr uint32_t kSheet = 512, kCell = 32, kScale = 4, kOut = kSheet * kScale;
const char* const kStyles[] = {"sharp", "tome"};  // settings game_font 0, 1

struct Loaded {
    std::once_flag once;
    std::vector<uint8_t> rgba;  // kOut x kOut, or empty
    std::string cells;          // '1' per character redrawn
};

uint64_t g_signature[2] = {0, 0};
std::once_flag g_signatures_once;

void read_signatures_now() {
    const std::string path = frontend_res_dir() + "/fonts/game/signatures.txt";
    FILE* f = std::fopen(path.c_str(), "r");
    if (f == nullptr) {
        std::fprintf(stderr, "[fonts] %s missing: game text stays as on the disc\n",
                     path.c_str());
        return;
    }
    int index;
    unsigned long long value;
    while (std::fscanf(f, "%d %llx", &index, &value) == 2)
        if (index >= 0 && index < 2)
            g_signature[index] = value;
    std::fclose(f);
}

void read_signatures() { std::call_once(g_signatures_once, read_signatures_now); }

// FNV-1a over the opaque/transparent mask (twin of the generator's).
uint64_t signature(const uint8_t* rgba) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < kSheet * kSheet; i++) {
        h ^= rgba[i * 4 + 3] >= 128 ? 1u : 0u;
        h *= 0x100000001B3ull;
    }
    return h;
}

// Loads once, on whichever worker asks first (the others wait for it).
void load_style_sheet(Loaded& l, int style, int sheet) {
    const std::string base =
        frontend_res_dir() + "/fonts/game/" + kStyles[style] + "-" + std::to_string(sheet);
    uint32_t w = 0, h = 0;
    if (!load_png_rgba(base + ".png", &l.rgba, &w, &h) || w != kOut || h != kOut) {
        std::fprintf(stderr, "[fonts] %s.png missing or not %ux%u\n", base.c_str(), kOut, kOut);
        l.rgba.clear();
        return;
    }
    if (FILE* f = std::fopen((base + ".cells").c_str(), "r")) {
        char line[300] = {};
        if (std::fgets(line, sizeof line, f) != nullptr)
            l.cells = line;
        std::fclose(f);
    }
    if (l.cells.size() < 256) {
        std::fprintf(stderr, "[fonts] %s.cells missing\n", base.c_str());
        l.rgba.clear();
    }
}

const Loaded& style_sheet(int style, int sheet) {
    static Loaded loaded[2][2];
    Loaded& l = loaded[style][sheet];
    std::call_once(l.once, [&] { load_style_sheet(l, style, sheet); });
    return l;
}

// One cell of the game's sheet, enlarged bilinearly into the output.
void enlarge_cell(const uint8_t* src, uint32_t col, uint32_t row, uint8_t* out) {
    for (uint32_t y = 0; y < kCell * kScale; y++) {
        const float fy = (y + 0.5f) / kScale - 0.5f;
        const int y0 = std::max(0, static_cast<int>(fy));
        const int y1 = std::min(static_cast<int>(kCell) - 1, y0 + 1);
        const float ty = std::min(std::max(fy - y0, 0.f), 1.f);
        for (uint32_t x = 0; x < kCell * kScale; x++) {
            const float fx = (x + 0.5f) / kScale - 0.5f;
            const int x0 = std::max(0, static_cast<int>(fx));
            const int x1 = std::min(static_cast<int>(kCell) - 1, x0 + 1);
            const float tx = std::min(std::max(fx - x0, 0.f), 1.f);
            auto at = [&](int px, int py) {
                return src + ((row * kCell + py) * kSheet + col * kCell + px) * 4;
            };
            const uint8_t *a = at(x0, y0), *b = at(x1, y0), *c = at(x0, y1), *d = at(x1, y1);
            uint8_t* o = out + ((row * kCell * kScale + y) * kOut + col * kCell * kScale + x) * 4;
            for (int k = 0; k < 4; k++) {
                const float top = a[k] + (b[k] - a[k]) * tx;
                const float bottom = c[k] + (d[k] - c[k]) * tx;
                o[k] = static_cast<uint8_t>(top + (bottom - top) * ty + 0.5f);
            }
        }
    }
}

// The handwritten sheet's button icons: the action a GameCube button does
// (and which of its buttons to show), or a stick's icon (and its key).
struct IconCell {
    int code;
    int action;      // controls::Action, or -1 for a stick
    int preference;  // controls::GlyphPreference
    const char* stick;
    const char* key;
};
const IconCell kIconCells[] = {
    {0x01, controls::kPause, controls::kAnyButton, nullptr, nullptr},
    {0x02, controls::kSprint, controls::kShoulderButton, nullptr, nullptr},
    {0x03, controls::kReload, controls::kAnyButton, nullptr, nullptr},
    {0x04, controls::kTarget, controls::kShoulderButton, nullptr, nullptr},
    {0x0B, controls::kSpellUp, controls::kAnyButton, nullptr, nullptr},
    {0x0C, controls::kSpellDown, controls::kAnyButton, nullptr, nullptr},
    {0x0D, controls::kSpellRight, controls::kAnyButton, nullptr, nullptr},
    {0x0E, controls::kSpellLeft, controls::kAnyButton, nullptr, nullptr},
    {0x10, -1, 0, "stick_l", nullptr},
    {0x11, -1, 0, "stick_l_right", "kb/d.png"},
    {0x12, -1, 0, "stick_l_left", "kb/a.png"},
    {0x13, -1, 0, "stick_l_up", "kb/w.png"},
    {0x14, -1, 0, "stick_l_down", "kb/s.png"},
    {0x15, -1, 0, "stick_l", nullptr},
    {0x18, -1, 0, "stick_r_up", "kb/t.png"},
    {0x19, -1, 0, "stick_r_down", "kb/g.png"},
    {0x1A, -1, 0, "stick_r_right", "kb/h.png"},
    {0x1B, -1, 0, "stick_r_left", "kb/f.png"},
    {0x1E, controls::kAttack, controls::kFaceButton, nullptr, nullptr},
    {0x1F, controls::kAction, controls::kFaceButton, nullptr, nullptr},
};
constexpr int kPairCode = 0x0F;  // a small B and A side by side

// The icon file for each cell now ("" keeps the game's), the pair's last.
std::vector<std::string> icon_files() {
    const std::string set = controls::glyph_set();
    std::vector<std::string> files;
    for (const IconCell& cell : kIconCells) {
        if (cell.action >= 0)
            files.push_back(controls::action_glyph(cell.action, cell.preference));
        else if (set == "kb")
            files.push_back(cell.key != nullptr ? cell.key : "");
        else
            files.push_back(set + "/" + cell.stick + ".png");
    }
    files.push_back(controls::action_glyph(controls::kAction, controls::kFaceButton));
    files.push_back(controls::action_glyph(controls::kAttack, controls::kFaceButton));
    return files;
}

uint64_t hash_files(const std::vector<std::string>& files) {
    uint64_t h = 1469598103934665603ull;
    for (const std::string& f : files)
        for (unsigned char c : f + "|")
            h = (h ^ c) * 1099511628211ull;
    return h;
}

// Worked out again only when the icons could have changed.
uint64_t icons_signature() {
    static std::mutex mutex;
    static uint64_t glyphs = ~0ull, last = 0;
    const uint64_t now = controls::glyph_signature();
    std::lock_guard<std::mutex> lock(mutex);
    if (now != glyphs) {
        last = hash_files(icon_files());
        glyphs = now;
    }
    return last;
}

// An icon drawn into part of a cell (x, y, size in the game sheet's pixels),
// box-filtered from its 128-pixel source and upside down, as the sheet is.
void draw_icon(uint8_t* out, int code, const std::string& file, float x, float y, float size) {
    std::vector<uint8_t> icon;
    uint32_t iw = 0, ih = 0;
    if (file.empty() || !load_png_rgba(frontend_res_dir() + "/ui/glyphs/" + file, &icon, &iw, &ih))
        return;
    const uint32_t col = static_cast<uint32_t>(code) >> 4;
    const uint32_t row = 15u - (static_cast<uint32_t>(code) & 15u);
    const int n = static_cast<int>(size * kScale);
    const int ox = static_cast<int>(x * kScale), oy = static_cast<int>(y * kScale);
    for (int py = 0; py < n; py++)
        for (int px = 0; px < n; px++) {
            const uint32_t sx0 = px * iw / n, sx1 = std::max(sx0 + 1, (px + 1) * iw / n);
            const uint32_t sy0 = py * ih / n, sy1 = std::max(sy0 + 1, (py + 1) * ih / n);
            uint32_t acc[4] = {0, 0, 0, 0}, cnt = 0;
            for (uint32_t sy = sy0; sy < sy1; sy++)
                for (uint32_t sx = sx0; sx < sx1; sx++) {
                    const uint8_t* q = &icon[(static_cast<size_t>(sy) * iw + sx) * 4];
                    for (int k = 0; k < 3; k++)
                        acc[k] += q[k] * q[3];
                    acc[3] += q[3];
                    cnt++;
                }
            const int cx = ox + px, cy = oy + py;
            if (cx < 0 || cy < 0 || cx >= static_cast<int>(kCell * kScale) ||
                cy >= static_cast<int>(kCell * kScale))
                continue;
            uint8_t* d = out + ((row * kCell * kScale + (kCell * kScale - 1 - cy)) * kOut +
                                col * kCell * kScale + cx) * 4;
            const float a = acc[3] / static_cast<float>(cnt) / 255.f;
            for (int k = 0; k < 3; k++) {
                const float v = acc[3] != 0 ? static_cast<float>(acc[k]) / acc[3] : 0.f;
                d[k] = static_cast<uint8_t>(d[k] + (v - d[k]) * a + 0.5f);
            }
            d[3] = static_cast<uint8_t>(d[3] + (255 - d[3]) * a + 0.5f);
        }
}

void clear_cell(uint8_t* out, int code) {
    const uint32_t col = static_cast<uint32_t>(code) >> 4;
    const uint32_t row = 15u - (static_cast<uint32_t>(code) & 15u);
    for (uint32_t y = 0; y < kCell * kScale; y++)
        std::memset(out + ((row * kCell * kScale + y) * kOut + col * kCell * kScale) * 4, 0,
                    kCell * kScale * 4);
}

// One sheet for one style, built on a worker from a copy of the game's sheet;
// the latest two builds are kept, by their icons' signature.
struct Build {
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> done;  // newest last
    bool running = false;
    bool failed = false;  // the letters are missing: keep the game's sheet
};
std::mutex g_build_mutex;
Build g_build[2][2];

void build_sheet(int style, int sheet, std::vector<uint8_t> game, std::vector<std::string> icons,
                 uint64_t icons_sig) {
    const Loaded& letters = style_sheet(style, sheet);
    uint8_t* out = letters.rgba.empty()
                       ? nullptr
                       : static_cast<uint8_t*>(std::malloc(static_cast<size_t>(kOut) * kOut * 4));
    if (out != nullptr) {
        // Character c sits in column c>>4, row 15-(c&15) of the sheet as stored.
        for (int c = 0; c < 256; c++) {
            const uint32_t col = static_cast<uint32_t>(c) >> 4;
            const uint32_t row = 15u - (static_cast<uint32_t>(c) & 15u);
            if (letters.cells[c] == '1') {
                for (uint32_t y = 0; y < kCell * kScale; y++) {
                    const size_t at =
                        ((row * kCell * kScale + y) * kOut + col * kCell * kScale) * 4;
                    std::memcpy(out + at, letters.rgba.data() + at, kCell * kScale * 4);
                }
            } else {
                enlarge_cell(game.data(), col, row, out);
            }
        }
        // The button icons, in the handwritten sheet only.
        for (size_t i = 0; i < std::size(kIconCells) && i < icons.size(); i++)
            if (!icons[i].empty()) {
                clear_cell(out, kIconCells[i].code);
                draw_icon(out, kIconCells[i].code, icons[i], 1, 1, 30);
            }
        if (icons.size() == std::size(kIconCells) + 2) {
            clear_cell(out, kPairCode);
            draw_icon(out, kPairCode, icons[icons.size() - 2], 1, 16, 14);
            draw_icon(out, kPairCode, icons[icons.size() - 1], 17, 16, 14);
        }
    }
    std::lock_guard<std::mutex> lock(g_build_mutex);
    Build& b = g_build[style][sheet];
    b.running = false;
    b.failed = out == nullptr;
    if (out != nullptr) {
        b.done.emplace_back(icons_sig, std::vector<uint8_t>(out, out + static_cast<size_t>(kOut) * kOut * 4));
        if (b.done.size() > 2)
            b.done.erase(b.done.begin());
    }
    std::free(out);
}

}  // namespace

bool fonts_claims(uint32_t format, uint32_t width, uint32_t height) {
    return g_settings.game_font != kGameFontOriginal && format == kCmpr && width == kSheet &&
           height == kSheet;
}

int fonts_apply(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t** out_rgba,
                uint32_t* out_width, uint32_t* out_height) {
    const int style = g_settings.game_font;
    if (style == kGameFontOriginal || width != kSheet || height != kSheet)
        return 0;
    read_signatures();
    const uint64_t sig = signature(rgba);
    const int sheet = sig == g_signature[0] ? 0 : sig == g_signature[1] ? 1 : -1;
    if (sheet < 0)
        return 0;  // another 512x512 texture
    // Only the handwritten sheet has button icons.
    std::vector<std::string> icons = sheet == 1 ? icon_files() : std::vector<std::string>{};
    const uint64_t icons_sig = sheet == 1 ? hash_files(icons) : 0;
    std::lock_guard<std::mutex> lock(g_build_mutex);
    Build& b = g_build[style][sheet];
    for (const auto& [sig_done, pixels] : b.done)
        if (sig_done == icons_sig) {
            *out_rgba = static_cast<uint8_t*>(std::malloc(pixels.size()));
            if (*out_rgba == nullptr)
                return 0;
            std::memcpy(*out_rgba, pixels.data(), pixels.size());
            *out_width = kOut;
            *out_height = kOut;
            return 1;
        }
    if (b.failed)
        return 0;
    if (!b.running) {
        b.running = true;
        std::thread(build_sheet, style, sheet,
                    std::vector<uint8_t>(rgba, rgba + static_cast<size_t>(kSheet) * kSheet * 4),
                    std::move(icons), icons_sig)
            .detach();
    }
    return 2;
}

void fonts_preload() {
    const int style = g_settings.game_font;
    if (style == kGameFontOriginal)
        return;
    std::thread([style] {
        read_signatures();
        style_sheet(style, 0);
        style_sheet(style, 1);
    }).detach();
}

uint64_t fonts_generation() {
    return static_cast<uint64_t>(g_settings.game_font) ^ (icons_signature() << 2);
}

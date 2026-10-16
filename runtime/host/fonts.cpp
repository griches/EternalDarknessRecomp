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
#include "host/fonts.h"

#include "host/buttons.h"
#include "host/frontend.h"
#include "host/settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// One sheet for one style, built on a worker from a copy of the game's sheet.
struct Build {
    uint8_t* rgba = nullptr;  // malloc'd kOut x kOut when ready, until handed over
    bool running = false;
    bool failed = false;  // the letters are missing: keep the game's sheet
};
std::mutex g_build_mutex;
Build g_build[2][2];

void build_sheet(int style, int sheet, std::vector<uint8_t> game) {
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
    }
    std::lock_guard<std::mutex> lock(g_build_mutex);
    Build& b = g_build[style][sheet];
    b.running = false;
    b.rgba = out;
    b.failed = out == nullptr;
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
    std::lock_guard<std::mutex> lock(g_build_mutex);
    Build& b = g_build[style][sheet];
    if (b.rgba != nullptr) {
        // The renderer keeps the texture; asked again (a new style or a
        // reload), the sheet is built again.
        *out_rgba = b.rgba;
        b.rgba = nullptr;
        *out_width = kOut;
        *out_height = kOut;
        return 1;
    }
    if (b.failed)
        return 0;
    if (!b.running) {
        b.running = true;
        std::thread(build_sheet, style, sheet,
                    std::vector<uint8_t>(rgba, rgba + static_cast<size_t>(kSheet) * kSheet * 4))
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

uint64_t fonts_generation() { return static_cast<uint64_t>(g_settings.game_font); }

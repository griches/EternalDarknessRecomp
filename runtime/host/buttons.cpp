// The game's button icons drawn for the player's controller. Eternal Darkness
// keeps its prompts' icons in one 512x64 CMPR sheet of 32x32 cells (GameCube
// L, R, Y, X, A, B, the main stick and C-stick with direction arrows, Z, and
// arrow tiles; the rest are magick runes). When gxcore decodes that sheet it
// is redrawn at 2048x256 with the icons for the action each GameCube button
// performs in the current controls layout (res/ui/glyphs), so "B Examine"
// shows the button that examines, and the text boxes' scroll arrows drawn
// smooth. The sheet is stored upside down, so the icons are drawn flipped.
#include "host/buttons.h"

#include "gxruntime/aurora_backend.h"
#include "host/controls.h"
#include "host/fonts.h"
#include "host/glyph_art.h"
#include "host/lightmaps.h"
#include "host/tome.h"
#include "host/frontend.h"

#include <png.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kCmpr = 14;
constexpr uint32_t kSheetW = 512, kSheetH = 64, kCell = 32, kScale = 4;

struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

const Image* load_icon(const std::string& file) {
    static std::map<std::string, Image> cache;
    auto it = cache.find(file);
    if (it != cache.end())
        return it->second.rgba.empty() ? nullptr : &it->second;
    Image& img = cache[file];
    const std::string path = frontend_res_dir() + "/ui/glyphs/" + file;
    if (!load_png_rgba(path, &img.rgba, &img.w, &img.h))
        img.rgba.clear();
    if (img.rgba.empty())
        std::fprintf(stderr, "[buttons] icon missing: %s\n", path.c_str());
    return img.rgba.empty() ? nullptr : &img;
}

// Mean colour of a cell's middle, to recognise the sheet by its green A and
// red B (the format and size alone could match another texture).
void cell_colour(const uint8_t* rgba, uint32_t col, uint32_t row, int out[3]) {
    long sum[3] = {0, 0, 0};
    int n = 0;
    for (uint32_t y = row * kCell + 10; y < row * kCell + 22; y++)
        for (uint32_t x = col * kCell + 10; x < col * kCell + 22; x++) {
            const uint8_t* p = rgba + (y * kSheetW + x) * 4;
            for (int c = 0; c < 3; c++)
                sum[c] += p[c];
            n++;
        }
    for (int c = 0; c < 3; c++)
        out[c] = static_cast<int>(sum[c] / n);
}

bool is_button_sheet(const uint8_t* rgba) {
    int a[3], b[3];
    cell_colour(rgba, 4, 0, a);
    cell_colour(rgba, 5, 0, b);
    return a[1] > a[0] + 20 && a[1] > a[2] + 20 && b[0] > b[1] + 40 && b[0] > b[2] + 40;
}

// One cell of the output: clear it and draw the icon, flipped, box-filtered
// down to the cell. dim darkens it (the sheet's greyed-out B).
void draw_cell(std::vector<uint8_t>& out, uint32_t col, uint32_t row, const Image& icon,
               bool dim) {
    const uint32_t size = kCell * kScale, ow = kSheetW * kScale;
    const uint32_t x0 = col * size, y0 = row * size;
    for (uint32_t y = 0; y < size; y++)
        for (uint32_t x = 0; x < size; x++) {
            // Source rectangle for this output texel.
            const uint32_t sx0 = x * icon.w / size, sx1 = std::max(sx0 + 1, (x + 1) * icon.w / size);
            const uint32_t sy0 = y * icon.h / size, sy1 = std::max(sy0 + 1, (y + 1) * icon.h / size);
            uint32_t acc[4] = {0, 0, 0, 0}, n = 0;
            for (uint32_t sy = sy0; sy < sy1; sy++)
                for (uint32_t sx = sx0; sx < sx1; sx++) {
                    const uint8_t* p = icon.rgba.data() + (sy * icon.w + sx) * 4;
                    // Premultiply so transparent edges do not darken.
                    acc[0] += p[0] * p[3];
                    acc[1] += p[1] * p[3];
                    acc[2] += p[2] * p[3];
                    acc[3] += p[3];
                    n++;
                }
            uint8_t* d = out.data() + ((y0 + (size - 1 - y)) * ow + x0 + x) * 4;
            const uint32_t alpha = acc[3] / n;
            for (int c = 0; c < 3; c++) {
                uint32_t v = acc[3] != 0 ? acc[c] / acc[3] : 0;
                if (dim)
                    v = v * 55 / 100;
                d[c] = static_cast<uint8_t>(v);
            }
            d[3] = static_cast<uint8_t>(alpha);
        }
}

// The text boxes' scroll arrows (more above, more below): a white triangle
// with a one-texel dark outline, alone in its cell, in hard 1-bit alpha.
// Redrawn from its own shape: the white texels' box and the outline's, the
// base on the wider side, each edge sampled 4x4 per output texel.
void smooth_arrow(const uint8_t* rgba, std::vector<uint8_t>& out, uint32_t col, uint32_t row) {
    int wx0 = kCell, wx1 = -1, wy0 = kCell, wy1 = -1, ox0 = kCell, ox1 = -1, oy0 = kCell, oy1 = -1;
    for (int y = 0; y < static_cast<int>(kCell); y++)
        for (int x = 0; x < static_cast<int>(kCell); x++) {
            const uint8_t* p = rgba + ((row * kCell + y) * kSheetW + col * kCell + x) * 4;
            if (p[3] == 0)
                continue;
            ox0 = std::min(ox0, x), ox1 = std::max(ox1, x), oy0 = std::min(oy0, y), oy1 = std::max(oy1, y);
            if (p[0] > 200 && p[1] > 200 && p[2] > 200)
                wx0 = std::min(wx0, x), wx1 = std::max(wx1, x), wy0 = std::min(wy0, y), wy1 = std::max(wy1, y);
        }
    if (wx1 < wx0 + 4 || wy1 < wy0 + 3)
        return;  // not a triangle here
    auto row_width = [&](int y) {
        int n = 0;
        for (int x = wx0; x <= wx1; x++) {
            const uint8_t* p = rgba + ((row * kCell + y) * kSheetW + col * kCell + x) * 4;
            n += p[3] != 0 && p[0] > 200;
        }
        return n;
    };
    const bool base_low = row_width(wy1) > row_width(wy0);  // base at the larger y
    struct Tri {
        float ax, ay, bx, by, cx, cy;
    };
    auto make = [&](int x0, int x1, int y0, int y1) {
        const float l = static_cast<float>(x0), r = static_cast<float>(x1 + 1);
        const float top = static_cast<float>(y0), bottom = static_cast<float>(y1 + 1);
        const float mid = (l + r) * 0.5f;
        return base_low ? Tri{mid, top, r, bottom, l, bottom} : Tri{mid, bottom, l, top, r, top};
    };
    const Tri outline = make(ox0, ox1, oy0, oy1), white = make(wx0, wx1, wy0, wy1);
    auto inside = [](const Tri& t, float x, float y) {
        const float e0 = (t.bx - t.ax) * (y - t.ay) - (t.by - t.ay) * (x - t.ax);
        const float e1 = (t.cx - t.bx) * (y - t.by) - (t.cy - t.by) * (x - t.bx);
        const float e2 = (t.ax - t.cx) * (y - t.cy) - (t.ay - t.cy) * (x - t.cx);
        return (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
    };
    const uint32_t size = kCell * kScale, ow = kSheetW * kScale;
    for (uint32_t y = 0; y < size; y++)
        for (uint32_t x = 0; x < size; x++) {
            int in_outline = 0, in_white = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    const float fx = (static_cast<float>(x) + (sx + 0.5f) / 4.0f) / kScale;
                    const float fy = (static_cast<float>(y) + (sy + 0.5f) / 4.0f) / kScale;
                    in_outline += inside(outline, fx, fy);
                    in_white += inside(white, fx, fy);
                }
            uint8_t* d = out.data() + ((row * size + y) * ow + col * size + x) * 4;
            // White over the dark outline, by coverage.
            const int a = std::max(in_outline, in_white);
            const int v = a != 0 ? (24 * (a - in_white) + 255 * in_white) / a : 0;
            d[0] = d[1] = d[2] = static_cast<uint8_t>(v);
            d[3] = static_cast<uint8_t>(a * 255 / 16);
        }
}

struct Cell {
    uint32_t col, row;
    int action;          // controls::Action, or -1 for a fixed icon
    int preference;      // controls::GlyphPreference
    const char* fixed;   // stick icon name (stick_l_left...) when action < 0
    const char* kb_key;  // keyboard icon for a stick cell with no controller
    bool dim;
};

const Cell kCells[] = {
    {0, 0, controls::kSprint, controls::kShoulderButton, nullptr, nullptr, false},
    {1, 0, controls::kTarget, controls::kShoulderButton, nullptr, nullptr, false},
    {2, 0, controls::kQuickcast, controls::kFaceButton, nullptr, nullptr, false},
    {3, 0, controls::kCreep, controls::kFaceButton, nullptr, nullptr, false},
    {4, 0, controls::kAttack, controls::kFaceButton, nullptr, nullptr, false},
    {5, 0, controls::kAction, controls::kFaceButton, nullptr, nullptr, false},
    {6, 1, controls::kAction, controls::kFaceButton, nullptr, nullptr, true},
    {15, 1, controls::kReload, controls::kShoulderButton, nullptr, nullptr, false},
    {1, 1, -1, 0, "stick_l", nullptr, false},
    {2, 1, -1, 0, "stick_l_left", "kb/a.png", false},
    {3, 1, -1, 0, "stick_l_right", "kb/d.png", false},
    {4, 1, -1, 0, "stick_l_up", "kb/w.png", false},
    {5, 1, -1, 0, "stick_l_down", "kb/s.png", false},
    {7, 1, -1, 0, "stick_r_left", "kb/f.png", false},
    {8, 1, -1, 0, "stick_r_right", "kb/h.png", false},
    {9, 1, -1, 0, "stick_r_up", "kb/t.png", false},
    {10, 1, -1, 0, "stick_r_down", "kb/g.png", false},
    {11, 1, controls::kSpellLeft, controls::kAnyButton, nullptr, nullptr, false},
    {12, 1, controls::kSpellRight, controls::kAnyButton, nullptr, nullptr, false},
    {13, 1, controls::kSpellUp, controls::kAnyButton, nullptr, nullptr, false},
    {14, 1, controls::kSpellDown, controls::kAnyButton, nullptr, nullptr, false},
};

bool claims(uint32_t format, uint32_t width, uint32_t height) {
    return format == kCmpr && width == kSheetW && height == kSheetH;
}

int apply(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t** out_rgba,
          uint32_t* out_width, uint32_t* out_height) {
    if (width != kSheetW || height != kSheetH || !is_button_sheet(rgba))
        return 0;
    const uint32_t ow = width * kScale, oh = height * kScale;
    std::vector<uint8_t> out(static_cast<size_t>(ow) * oh * 4);
    // The original, doubled (runes and markers stay as they were).
    for (uint32_t y = 0; y < oh; y++)
        for (uint32_t x = 0; x < ow; x++)
            std::memcpy(&out[(y * ow + x) * 4], rgba + ((y / kScale) * width + x / kScale) * 4, 4);
    smooth_arrow(rgba, out, 6, 0);
    smooth_arrow(rgba, out, 15, 0);
    const std::string set = controls::glyph_set();
    for (const Cell& cell : kCells) {
        std::string file;
        if (cell.action >= 0)
            file = controls::action_glyph(cell.action, cell.preference);
        else if (set == "kb")
            file = cell.kb_key != nullptr ? cell.kb_key : "";
        else
            file = set + "/" + cell.fixed + ".png";
        if (file.empty())
            continue;  // unbound or no icon: keep the GameCube one
        if (const Image* icon = load_icon(file))
            draw_cell(out, cell.col, cell.row, *icon, cell.dim);
    }
    // ED_BUTTONS_DUMP=path.png: the redrawn sheet, as stored (upside down).
    if (const char* dump = std::getenv("ED_BUTTONS_DUMP")) {
        png_image png{};
        png.version = PNG_IMAGE_VERSION;
        png.width = ow;
        png.height = oh;
        png.format = PNG_FORMAT_RGBA;
        png_image_write_to_file(&png, dump, 0, out.data(), 0, nullptr);
    }
    *out_rgba = static_cast<uint8_t*>(std::malloc(out.size()));
    if (*out_rgba == nullptr)
        return 0;
    std::memcpy(*out_rgba, out.data(), out.size());
    *out_width = ow;
    *out_height = oh;
    return 1;
}

uint64_t generation() { return controls::glyph_signature(); }

}  // namespace

bool load_png_rgba(const std::string& path, std::vector<uint8_t>* rgba, uint32_t* width,
                   uint32_t* height) {
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&png, path.c_str()))
        return false;
    png.format = PNG_FORMAT_RGBA;
    rgba->resize(PNG_IMAGE_SIZE(png));
    if (!png_image_finish_read(&png, nullptr, rgba->data(), 0, nullptr)) {
        rgba->clear();
        return false;
    }
    *width = png.width;
    *height = png.height;
    return true;
}

// The renderer takes one override: the prompt sheet here, the font sheets in
// fonts.cpp, the Tome's controls page in tome.cpp, the other pictures of
// buttons in glyph_art.cpp, and (claimed by how a draw samples them) the light
// maps in lightmaps.cpp.
bool claims_any(uint32_t format, uint32_t width, uint32_t height) {
    return claims(format, width, height) || fonts_claims(format, width, height) ||
           tome_claims(format, width, height) || art_claims(format, width, height);
}

// Of the textures the size of a redrawn one, only that one: the rest keep the
// HD pack's versions.
bool claims_hash_any(uint32_t format, uint32_t width, uint32_t height, uint64_t hash) {
    if (!tome_claims(format, width, height) && !art_claims(format, width, height))
        return true;
    return (tome_claims(format, width, height) && tome_claims_hash(hash)) ||
           (art_claims(format, width, height) && art_claims_hash(hash));
}

int apply_any(const uint8_t* rgba, uint32_t width, uint32_t height, uint64_t hash,
              uint8_t** out_rgba, uint32_t* out_width, uint32_t* out_height) {
    if (claims(kCmpr, width, height))
        return apply(rgba, width, height, out_rgba, out_width, out_height);
    if (fonts_claims(kCmpr, width, height))
        return fonts_apply(rgba, width, height, out_rgba, out_width, out_height);
    if (art_claims(kCmpr, width, height) && art_claims_hash(hash))
        return art_apply(rgba, width, height, hash, out_rgba, out_width, out_height);
    if (tome_claims(kCmpr, width, height) && tome_claims_hash(hash))
        return tome_apply(rgba, width, height, out_rgba, out_width, out_height);
    return lightmaps_apply(rgba, width, height, out_rgba, out_width, out_height);
}

// Each sheet keys on its own settings, so redrawing the icons (the pause menu
// opening) leaves the font sheets in the cache.
uint64_t generation_any(uint32_t format, uint32_t width, uint32_t height) {
    if (claims(format, width, height))
        return generation();
    if (fonts_claims(format, width, height))
        return fonts_generation();
    if (tome_claims(format, width, height) || art_claims(format, width, height))
        return art_generation();
    return lightmaps_generation();
}

extern "C" void buttons_init(void) {
    fonts_preload();
    static const DolAuroraTextureOverride ops = {claims_any,           nullptr,
                                                 generation_any,       lightmaps_claims_uv2,
                                                 claims_hash_any,      apply_any};
    dol_aurora_set_texture_override(&ops);
}

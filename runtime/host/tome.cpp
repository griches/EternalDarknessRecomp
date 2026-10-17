// The Tome's controls page for the controller in use. The game's page (in
// UBookPak.bin, a 432x480 CMPR texture) draws a GameCube controller with its
// buttons labelled; other controllers have other buttons, and a layout moves
// them. When gxcore decodes that page it is redrawn at 4x as a list: each
// control's button icons (or keys) in the layout in use, then what it does,
// in the Tome's hand (La Belle Aurore, the game font's handwriting), on
// parchment with the colour and shading of the game's page but none of its
// writing. The targeting note keeps its place at the top with the player's
// Target button, and "Rumble Feature :" its place at the bottom, where the
// game writes On or Off after it. Only the English pages are redrawn.
#include "host/tome.h"

#include "host/buttons.h"
#include "host/controls.h"
#include "host/frontend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "imstb_truetype.h"

namespace {

constexpr uint32_t kCmpr = 14;
constexpr uint32_t kPageW = 432, kPageH = 480, kScale = 4;
constexpr uint32_t kW = kPageW * kScale, kH = kPageH * kScale;
// Dolphin's name hashes of the English controls page (the pack's and the
// disc's: UBookPak.bin image 31, and its twin in UBook.tpl).
constexpr uint64_t kPages[] = {0x8680aee4cb0d0a97ull, 0x7ba7ca0b431bba6aull};
const float kInk[3] = {38.f, 27.f, 17.f};
constexpr float kInkAlpha = 0.86f;

struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    bool mixed = false;  // white and coloured parts (a d-pad with its lit direction)
};

struct Row {
    // Files under res/ui/glyphs ("a|b": the icons laid over each other), or
    // "=text" for a key without an icon.
    std::vector<std::string> icons;
    std::string text;
};

struct Page {
    std::string target_icon;  // the Target button for the note at the top
    std::vector<Row> rows;
};

// An upright RGB canvas of floats (0-255), written out as RGBA8 stored upside
// down, as the game's texture is.
struct Canvas {
    std::vector<float> px = std::vector<float>(static_cast<size_t>(kW) * kH * 3);
    float* at(int x, int y) { return &px[(static_cast<size_t>(y) * kW + x) * 3]; }
    void blend(int x, int y, const float rgb[3], float a) {
        if (x < 0 || y < 0 || x >= static_cast<int>(kW) || y >= static_cast<int>(kH) || a <= 0.f)
            return;
        float* p = at(x, y);
        for (int c = 0; c < 3; c++)
            p[c] += (rgb[c] - p[c]) * a;
    }
};

// ---- Parchment -------------------------------------------------------------

float hash01(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xFFFFu) / 65535.f;
}

// Smooth noise in -1..1 with features about cell pixels across.
float value_noise(float x, float y, float cell, uint32_t seed) {
    const float fx = x / cell, fy = y / cell;
    const uint32_t ix = static_cast<uint32_t>(fx), iy = static_cast<uint32_t>(fy);
    float tx = fx - ix, ty = fy - iy;
    tx = tx * tx * (3.f - 2.f * tx);
    ty = ty * ty * (3.f - 2.f * ty);
    const float a = hash01(ix, iy, seed), b = hash01(ix + 1, iy, seed);
    const float c = hash01(ix, iy + 1, seed), d = hash01(ix + 1, iy + 1, seed);
    return ((a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty) * 2.f - 1.f;
}

// The page's parchment without its writing: the median colour of each 8x8
// block, a smooth (cubic) surface fitted to the blocks with the outliers
// (writing, the drawing) left out, then mottling and grain as faint as the
// page's own.
void parchment(const uint8_t* game, Canvas& out) {
    constexpr int kBlock = 8, kBW = kPageW / kBlock, kBH = kPageH / kBlock, kTerms = 10;
    std::vector<float> block(static_cast<size_t>(kBW) * kBH * 3);
    for (int by = 0; by < kBH; by++)
        for (int bx = 0; bx < kBW; bx++)
            for (int c = 0; c < 3; c++) {
                float v[kBlock * kBlock];
                int n = 0;
                for (int y = 0; y < kBlock; y++)
                    for (int x = 0; x < kBlock; x++) {
                        // Stored upside down: upright row r is stored row H-1-r.
                        const int sy = kPageH - 1 - (by * kBlock + y);
                        v[n++] = game[(static_cast<size_t>(sy) * kPageW + bx * kBlock + x) * 4 + c];
                    }
                std::nth_element(v, v + n / 2, v + n);
                block[(static_cast<size_t>(by) * kBW + bx) * 3 + c] = v[n / 2];
            }
    auto terms = [](float x, float y, double t[kTerms]) {
        t[0] = 1; t[1] = x; t[2] = y; t[3] = x * x; t[4] = x * y; t[5] = y * y;
        t[6] = x * x * x; t[7] = x * x * y; t[8] = x * y * y; t[9] = y * y * y;
    };
    std::vector<char> keep(static_cast<size_t>(kBW) * kBH, 1);
    double coef[3][kTerms] = {};
    float sigma = 2.f;
    for (int pass = 0; pass < 5; pass++) {
        for (int c = 0; c < 3; c++) {
            double m[kTerms][kTerms + 1] = {};
            for (int by = 0; by < kBH; by++)
                for (int bx = 0; bx < kBW; bx++) {
                    if (!keep[by * kBW + bx])
                        continue;
                    double t[kTerms];
                    terms((bx + 0.5f) / kBW * 2 - 1, (by + 0.5f) / kBH * 2 - 1, t);
                    const double v = block[(static_cast<size_t>(by) * kBW + bx) * 3 + c];
                    for (int i = 0; i < kTerms; i++) {
                        for (int j = 0; j < kTerms; j++)
                            m[i][j] += t[i] * t[j];
                        m[i][kTerms] += t[i] * v;
                    }
                }
            // Gaussian elimination with partial pivoting.
            for (int i = 0; i < kTerms; i++) {
                int pivot = i;
                for (int r = i + 1; r < kTerms; r++)
                    if (std::fabs(m[r][i]) > std::fabs(m[pivot][i]))
                        pivot = r;
                for (int k = 0; k <= kTerms; k++)
                    std::swap(m[i][k], m[pivot][k]);
                if (std::fabs(m[i][i]) < 1e-9)
                    continue;
                for (int r = 0; r < kTerms; r++) {
                    if (r == i)
                        continue;
                    const double f = m[r][i] / m[i][i];
                    for (int k = i; k <= kTerms; k++)
                        m[r][k] -= f * m[i][k];
                }
            }
            for (int i = 0; i < kTerms; i++)
                coef[c][i] = std::fabs(m[i][i]) < 1e-9 ? 0 : m[i][kTerms] / m[i][i];
        }
        // Residuals (summed over the channels); keep blocks within 2.5 sigma.
        std::vector<float> res(keep.size());
        std::vector<float> kept;
        for (int by = 0; by < kBH; by++)
            for (int bx = 0; bx < kBW; bx++) {
                double t[kTerms];
                terms((bx + 0.5f) / kBW * 2 - 1, (by + 0.5f) / kBH * 2 - 1, t);
                float r = 0;
                for (int c = 0; c < 3; c++) {
                    double p = 0;
                    for (int i = 0; i < kTerms; i++)
                        p += coef[c][i] * t[i];
                    r += std::fabs(static_cast<float>(p) - block[(static_cast<size_t>(by) * kBW + bx) * 3 + c]);
                }
                res[by * kBW + bx] = r / 3;
                if (keep[by * kBW + bx])
                    kept.push_back(r / 3);
            }
        if (kept.empty())
            break;
        std::nth_element(kept.begin(), kept.begin() + kept.size() / 2, kept.end());
        sigma = std::max(0.5f, kept[kept.size() / 2] * 1.4826f);
        for (size_t i = 0; i < keep.size(); i++)
            keep[i] = res[i] < 2.5f * sigma;
    }
    // The page's own variation sets the mottling's strength.
    const float mottle = std::min(std::max(sigma * 1.6f, 3.f), 7.f);
    for (uint32_t y = 0; y < kH; y++) {
        const float fy = (y + 0.5f) / kH * 2 - 1;
        for (uint32_t x = 0; x < kW; x++) {
            const float fx = (x + 0.5f) / kW * 2 - 1;
            double t[kTerms];
            terms(fx, fy, t);
            const float n = mottle * (0.5f * value_noise(x, y, 170.f, 1) +
                                      0.3f * value_noise(x, y, 48.f, 2) +
                                      0.2f * value_noise(x, y, 12.f, 4)) +
                            2.5f * (hash01(x, y, 3) * 2 - 1);
            float* p = out.at(x, y);
            double base[3];
            for (int c = 0; c < 3; c++) {
                base[c] = 0;
                for (int i = 0; i < kTerms; i++)
                    base[c] += coef[c][i] * t[i];
            }
            const double luma = std::max(1.0, (base[0] + base[1] + base[2]) / 3);
            for (int c = 0; c < 3; c++)
                p[c] = static_cast<float>(base[c] + n * base[c] / luma);
        }
    }
}

// ---- Writing ---------------------------------------------------------------

struct Font {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ok = false;
};

const Font& hand() {
    static Font font;
    static std::once_flag once;
    std::call_once(once, [] {
        const std::string path = frontend_res_dir() + "/fonts/LaBelleAurore.ttf";
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            font.data.resize(static_cast<size_t>(std::max(0L, std::ftell(f))));
            std::fseek(f, 0, SEEK_SET);
            if (std::fread(font.data.data(), 1, font.data.size(), f) == font.data.size())
                font.ok = stbtt_InitFont(&font.info, font.data.data(), 0) != 0;
            std::fclose(f);
        }
        if (!font.ok)
            std::fprintf(stderr, "[tome] %s missing: the controls page stays as on the disc\n",
                         path.c_str());
    });
    return font;
}

constexpr float kTextPx = 116.f;  // the hand's size, ascender to descender
constexpr int kBold = 3;  // strokes thickened by this many pixels: the font is
                          // finer than the page's pen

float text_width(const std::string& s, float px) {
    const Font& f = hand();
    const float scale = stbtt_ScaleForPixelHeight(&f.info, px);
    float w = 0;
    for (size_t i = 0; i < s.size(); i++) {
        int adv = 0, lsb = 0;
        const int ch = static_cast<unsigned char>(s[i]);
        stbtt_GetCodepointHMetrics(&f.info, ch, &adv, &lsb);
        w += adv * scale;
        if (i + 1 < s.size())
            w += stbtt_GetCodepointKernAdvance(&f.info, ch, static_cast<unsigned char>(s[i + 1])) *
                 scale;
    }
    return w;
}

// Writes s with its baseline at y; returns the x after it. The letters'
// coverage is merged first, so joined strokes do not darken where they meet.
float write(Canvas& page, const std::string& s, float x, float baseline, float px) {
    const Font& f = hand();
    const float scale = stbtt_ScaleForPixelHeight(&f.info, px);
    const int w = static_cast<int>(text_width(s, px)) + static_cast<int>(px);
    const int h = static_cast<int>(px * 1.6f);
    const int top = static_cast<int>(baseline - px * 1.1f);
    std::vector<unsigned char> cover(static_cast<size_t>(w) * h, 0);
    float pen = px * 0.25f;
    for (size_t i = 0; i < s.size(); i++) {
        const int ch = static_cast<unsigned char>(s[i]);
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f.info, ch, &adv, &lsb);
        int x0, y0, x1, y1;
        const float shift = pen - std::floor(pen);
        stbtt_GetCodepointBitmapBoxSubpixel(&f.info, ch, scale, scale, shift, 0, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            std::vector<unsigned char> glyph(static_cast<size_t>(gw) * gh);
            stbtt_MakeCodepointBitmapSubpixel(&f.info, glyph.data(), gw, gh, gw, scale, scale, shift,
                                              0, ch);
            const int ox = static_cast<int>(std::floor(pen)) + x0;
            const int oy = static_cast<int>(baseline) - top + y0;
            for (int gy = 0; gy < gh; gy++)
                for (int gx = 0; gx < gw; gx++) {
                    const int cx = ox + gx, cy = oy + gy;
                    if (cx >= 0 && cy >= 0 && cx < w && cy < h) {
                        unsigned char& d = cover[static_cast<size_t>(cy) * w + cx];
                        d = std::max(d, glyph[static_cast<size_t>(gy) * gw + gx]);
                    }
                }
        }
        pen += adv * scale;
        if (i + 1 < s.size())
            pen += stbtt_GetCodepointKernAdvance(&f.info, ch, static_cast<unsigned char>(s[i + 1])) *
                   scale;
    }
    // Thicken: the most coverage within kBold pixels (a round pen).
    std::vector<unsigned char> bold(cover.size(), 0);
    for (int cy = 0; cy < h; cy++)
        for (int cx = 0; cx < w; cx++) {
            unsigned char m = 0;
            for (int dy = -kBold; dy <= kBold; dy++)
                for (int dx = -kBold; dx <= kBold; dx++) {
                    const int sx = cx + dx, sy = cy + dy;
                    if (dx * dx + dy * dy <= kBold * kBold && sx >= 0 && sy >= 0 && sx < w && sy < h)
                        m = std::max(m, cover[static_cast<size_t>(sy) * w + sx]);
                }
            bold[static_cast<size_t>(cy) * w + cx] = m;
        }
    const int left = static_cast<int>(x - px * 0.25f);
    for (int cy = 0; cy < h; cy++)
        for (int cx = 0; cx < w; cx++)
            if (const unsigned char v = bold[static_cast<size_t>(cy) * w + cx])
                page.blend(left + cx, top + cy, kInk, v / 255.f * kInkAlpha);
    return x + pen - px * 0.25f;
}

// ---- Icons -----------------------------------------------------------------

constexpr int kIcon = 116;  // icon size on the page

bool saturated(const uint8_t* p) {
    return std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]}) > 40;
}

const Image* icon(const std::string& file) {
    static std::mutex mutex;
    static std::map<std::string, Image> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(file);
    if (it == cache.end()) {
        Image img;
        if (!load_png_rgba(frontend_res_dir() + "/ui/glyphs/" + file, &img.rgba, &img.w, &img.h))
            std::fprintf(stderr, "[tome] icon missing: %s\n", file.c_str());
        bool white = false, coloured = false;
        for (size_t i = 0; i < img.rgba.size(); i += 4)
            if (img.rgba[i + 3] >= 128)
                (saturated(&img.rgba[i]) ? coloured : white) = true;
        img.mixed = white && coloured;
        it = cache.emplace(file, std::move(img)).first;
    }
    return it->second.rgba.empty() ? nullptr : &it->second;
}

// Draws icons laid over each other (one, or the d-pad's four directions as a
// whole d-pad), kIcon pixels square with the left edge at x, centred on cy,
// as a stamp in the page's ink: its shape, with the letters cut out of it left
// as paper. In an icon of white and coloured parts (a d-pad and its lit
// direction) the white is a lighter wash, so the coloured part stands out.
void draw_icon(Canvas& page, const std::vector<const Image*>& layers, int x, int cy) {
    const int y = cy - kIcon / 2;
    for (int oy = 0; oy < kIcon; oy++)
        for (int ox = 0; ox < kIcon; ox++) {
            float best = 0;
            for (const Image* img : layers) {
                const uint32_t sx0 = ox * img->w / kIcon,
                               sx1 = std::max(sx0 + 1, (ox + 1) * img->w / kIcon);
                const uint32_t sy0 = oy * img->h / kIcon,
                               sy1 = std::max(sy0 + 1, (oy + 1) * img->h / kIcon);
                float ink = 0;
                int n = 0;
                for (uint32_t sy = sy0; sy < sy1; sy++)
                    for (uint32_t sx = sx0; sx < sx1; sx++) {
                        const uint8_t* p = &img->rgba[(static_cast<size_t>(sy) * img->w + sx) * 4];
                        ink += p[3] * (img->mixed && !saturated(p) ? 0.3f : 1.f);
                        n++;
                    }
                best = std::max(best, ink / n);
            }
            page.blend(x + ox, y + oy, kInk, best / 255.f * kInkAlpha);
        }
}

// Icons (or a key's name) left to right from x, centred on cy; returns the x
// after them.
int draw_icons(Canvas& page, const std::vector<std::string>& icons, int x, int cy) {
    for (const std::string& file : icons) {
        if (file.rfind("=", 0) == 0) {
            x = static_cast<int>(write(page, file.substr(1), static_cast<float>(x),
                                       cy + kTextPx * 0.28f, kTextPx)) + 16;
        } else {
            std::vector<const Image*> layers;
            for (size_t at = 0; at <= file.size();) {
                size_t end = file.find('|', at);
                if (end == std::string::npos)
                    end = file.size();
                if (const Image* img = icon(file.substr(at, end - at)))
                    layers.push_back(img);
                at = end + 1;
            }
            if (!layers.empty()) {
                draw_icon(page, layers, x, cy);
                x += kIcon + 10;
            }
        }
    }
    return x;
}

int icons_width(const std::vector<std::string>& icons) {
    int w = 0;
    for (const std::string& file : icons)
        w += file.rfind("=", 0) == 0 ? static_cast<int>(text_width(file.substr(1), kTextPx)) + 16
                                     : kIcon + 10;
    return w > 0 ? w - 10 : 0;
}

// ---- The page --------------------------------------------------------------

std::vector<uint8_t> compose(const std::vector<uint8_t>& game, const Page& content) {
    Canvas page;
    parchment(game.data(), page);
    // The targeting note, centred as the game's was.
    const std::string note1 = "To target an enemy, press and hold";
    const std::string note2 = "Release and hold again to select the next enemy.";
    const std::vector<std::string> target{content.target_icon};
    const float w1 = text_width(note1, kTextPx) + 24 + icons_width(target);
    float x = write(page, note1, kW / 2.f - w1 / 2, 250, kTextPx);
    draw_icons(page, target, static_cast<int>(x + 24), static_cast<int>(250 - kTextPx * 0.28f));
    write(page, note2, kW / 2.f - text_width(note2, kTextPx) / 2, 375, kTextPx);
    // The controls: icons right-aligned to a column, then what they do.
    constexpr int kColumn = 720, kFirst = 530, kStep = 112;
    int y = kFirst;
    for (const Row& row : content.rows) {
        const int icons_w = icons_width(row.icons);
        draw_icons(page, row.icons, kColumn - icons_w, y);
        float px = kTextPx;
        // A long line is written smaller to fit the page.
        const float room = kW - (kColumn + 50) - 110;
        if (text_width(row.text, px) > room)
            px *= room / text_width(row.text, px);
        write(page, row.text, kColumn + 50, y + px * 0.28f, px);
        y += kStep;
    }
    // "Rumble Feature :" where the game's was (its On / Off follows).
    const std::string rumble = "Rumble Feature :";
    write(page, rumble, 960 - text_width(rumble, kTextPx), 1658, kTextPx);
    // Out as the game's texture: RGBA8, stored upside down.
    std::vector<uint8_t> out(static_cast<size_t>(kW) * kH * 4);
    for (uint32_t yy = 0; yy < kH; yy++)
        for (uint32_t xx = 0; xx < kW; xx++) {
            const float* p = page.at(xx, yy);
            uint8_t* o = &out[(static_cast<size_t>(kH - 1 - yy) * kW + xx) * 4];
            for (int c = 0; c < 3; c++)
                o[c] = static_cast<uint8_t>(std::min(255.f, std::max(0.f, p[c])) + 0.5f);
            o[3] = 255;
        }
    return out;
}

// What the page lists, from the layout in use (the buttons that work in play,
// not the Tome's own).
Page content_now() {
    using namespace controls;
    const std::string set = glyph_set();
    const bool keys = set == "kb";
    auto inputs = [&](int action) {
        std::vector<std::string> out;
        if (keys) {
            const std::string file = key_glyph_file(action);
            if (!file.empty())
                out.push_back(file + ".png");
            else if (key_label(action) != "-")
                out.push_back("=" + key_label(action));
        } else {
            for (const std::string& id : pad_input_ids(action))
                out.push_back(set + "/" + id + ".png");
        }
        if (out.empty())
            out.push_back("=-");
        return out;
    };
    Page page;
    // Target: a trigger in play when it has one, else a bumper, else the first.
    const std::vector<std::string> target = inputs(kTarget);
    page.target_icon = target.front();
    auto ends = [](const std::string& s, const std::string& end) {
        return s.size() > end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
    };
    for (const char* pick : {"/lb.png", "/rb.png", "/lt.png", "/rt.png"})
        for (const std::string& file : target)
            if (ends(file, pick))
                page.target_icon = file;
    page.rows.push_back({keys ? std::vector<std::string>{"kb/w.png", "kb/a.png", "kb/s.png",
                                                         "kb/d.png"}
                              : std::vector<std::string>{set + "/stick_l.png"},
                         "Move Character"});
    page.rows.push_back({inputs(kAttack), "Attack / Select"});
    page.rows.push_back({inputs(kAction), "Perform Action"});
    page.rows.push_back({inputs(kSprint), "Run"});
    page.rows.push_back({inputs(kCreep), "Sneak"});
    page.rows.push_back({inputs(kQuickcast), "Quick Spell #1"});
    std::vector<std::string> spells;
    for (int a : {kSpellUp, kSpellDown, kSpellLeft, kSpellRight})
        for (const std::string& file : inputs(a))
            spells.push_back(file);
    // All four on the d-pad (as the layouts have them): one whole d-pad.
    const std::vector<std::string> dpad{set + "/dup.png", set + "/ddown.png", set + "/dleft.png",
                                        set + "/dright.png"};
    if (spells == dpad)
        spells = {dpad[0] + "|" + dpad[1] + "|" + dpad[2] + "|" + dpad[3]};
    page.rows.push_back({spells, "Quick Spell #2-5"});
    page.rows.push_back({inputs(kReload), "Reload (hold to load all rounds)"});
    page.rows.push_back({inputs(kPause), "Access / Return from Book"});
    return page;
}

// Built on a worker (a 1728x1920 page takes a moment); the game's page shows
// until it is ready.
struct Build {
    std::vector<uint8_t> rgba;
    uint64_t signature = 0;
    bool running = false, ready = false;
};
std::mutex g_mutex;
Build g_build;

void build(std::vector<uint8_t> game, Page content, uint64_t signature) {
    std::vector<uint8_t> rgba = compose(game, content);
    std::fprintf(stderr, "[tome] controls page drawn (%zu rows, %s)\n", content.rows.size(),
                 controls::glyph_set().c_str());
    std::lock_guard<std::mutex> lock(g_mutex);
    g_build.rgba = std::move(rgba);
    g_build.signature = signature;
    g_build.running = false;
    g_build.ready = true;
}

}  // namespace

bool tome_claims(uint32_t format, uint32_t width, uint32_t height) {
    return format == kCmpr && width == kPageW && height == kPageH;
}

bool tome_claims_hash(uint64_t hash) {
    return std::find(std::begin(kPages), std::end(kPages), hash) != std::end(kPages) && hand().ok;
}

int tome_apply(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t** out_rgba,
               uint32_t* out_width, uint32_t* out_height) {
    if (width != kPageW || height != kPageH)
        return 0;
    const uint64_t signature = controls::bindings_signature();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_build.ready && g_build.signature == signature) {
        *out_rgba = static_cast<uint8_t*>(std::malloc(g_build.rgba.size()));
        if (*out_rgba == nullptr)
            return 0;
        std::memcpy(*out_rgba, g_build.rgba.data(), g_build.rgba.size());
        *out_width = kW;
        *out_height = kH;
        return 1;
    }
    if (!g_build.running) {
        g_build.running = true;
        g_build.ready = false;
        std::thread(build, std::vector<uint8_t>(rgba, rgba + static_cast<size_t>(width) * height * 4),
                    content_now(), signature)
            .detach();
    }
    return 2;
}

uint64_t tome_generation() { return controls::bindings_signature(); }

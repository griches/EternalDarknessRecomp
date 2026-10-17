// The game's pictures of GameCube buttons, redrawn with the player's. Besides
// the prompt sheet (buttons.cpp), the text icons (fonts.cpp) and the Tome's
// controls page (tome.cpp), a few textures have a button painted in: "Press A
// to Continue", the language screen's "Press A to continue", the brightness
// screen's sticks, the Tome map's "Y Button: Up Floor" lines, the quick spell
// slots, a card of the face buttons, and the organ's notes and sheet music.
// Each is claimed by its name hash, taken from the HD pack's image when there
// is one (else the game's, enlarged 4x), and has its buttons rubbed out
// (filled from around them) and the icons of the buttons that do the same
// thing drawn in their place, in the game's colours on a dark screen or as
// ink on parchment.
#include "host/glyph_art.h"

#include "host/buttons.h"
#include "host/controls.h"
#include "host/frontend.h"
#include "host/settings.h"
#include "host/texpack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kCmpr = 14;
constexpr uint32_t kScale = 4;  // the game's texture without a pack image, enlarged

// The textures, by Dolphin's name hash (the English ones where they have words).
enum Art {
    kMap, kPressA, kLanguage, kBrightness, kQuickY, kQuickDpad, kFaceCard,
    kNoteA, kNoteY, kNoteB, kNoteX, kSheet, kSheetTorn, kSheetTornLarge,
};
struct Texture {
    uint64_t hash;
    uint32_t width, height;
    Art art;
};
const Texture kTextures[] = {
    {0x09e293e32e372edeull, 432, 480, kMap},       {0x81611b2a3fc494ebull, 432, 480, kMap},
    {0x069eb3996db5fb97ull, 266, 48, kPressA},     {0x7910715fce98efbfull, 850, 480, kLanguage},
    {0x9e51063f6b37567aull, 640, 480, kBrightness}, {0x5b2ecb5d1ba4b71aull, 75, 46, kQuickY},
    {0x1e01f71343d270a4ull, 72, 76, kQuickDpad},   {0x145356da7aa6f360ull, 143, 128, kFaceCard},
    {0x643354e7d16f12f6ull, 32, 64, kNoteA},       {0x66baf1041997e1f1ull, 32, 64, kNoteY},
    {0x8832b7241bbb1cf3ull, 32, 64, kNoteB},       {0x90abe76e0509b0f1ull, 32, 64, kNoteX},
    {0x723d679a56921b95ull, 192, 96, kSheet},      {0x552207ae4f48978dull, 192, 96, kSheetTorn},
    {0x7ae79958862c8f69ull, 349, 160, kSheetTornLarge},
};

const Texture* find(uint64_t hash) {
    for (const Texture& t : kTextures)
        if (t.hash == hash)
            return &t;
    return nullptr;
}

// ---- Canvas ------------------------------------------------------------------

// Upright RGBA floats (0-255); u converts the game texture's pixels to its own.
struct Canvas {
    int w = 0, h = 0;
    float u = 1;
    std::vector<float> px;
    float* at(int x, int y) { return &px[(static_cast<size_t>(y) * w + x) * 4]; }
    const float* at(int x, int y) const { return &px[(static_cast<size_t>(y) * w + x) * 4]; }
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
};

float luma(const float* p) { return 0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2]; }

// Dark red: the blood across the torn sheet music, kept over what is drawn.
bool blood(const float* p) { return p[0] > 45 && p[0] > p[1] * 1.6f && p[0] - p[1] > 20; }

// Fills the masked pixels from around them (push-pull: averages up a pyramid
// of the unmasked ones, then down into the holes), with a little grain.
void fill(Canvas& c, const std::vector<uint8_t>& hole, int x0, int y0, int x1, int y1) {
    x0 = std::max(0, x0 - 24);
    y0 = std::max(0, y0 - 24);
    x1 = std::min(c.w, x1 + 24);
    y1 = std::min(c.h, y1 + 24);
    struct Level {
        int w, h;
        std::vector<float> v;  // weighted RGBA sums
        std::vector<float> wt;
    };
    std::vector<Level> levels(1);
    Level& l0 = levels[0];
    l0.w = x1 - x0;
    l0.h = y1 - y0;
    if (l0.w <= 0 || l0.h <= 0)
        return;
    l0.v.assign(static_cast<size_t>(l0.w) * l0.h * 4, 0);
    l0.wt.assign(static_cast<size_t>(l0.w) * l0.h, 0);
    for (int y = 0; y < l0.h; y++)
        for (int x = 0; x < l0.w; x++) {
            if (hole[static_cast<size_t>(y + y0) * c.w + x + x0])
                continue;
            const float* p = c.at(x + x0, y + y0);
            for (int k = 0; k < 4; k++)
                l0.v[(static_cast<size_t>(y) * l0.w + x) * 4 + k] = p[k];
            l0.wt[static_cast<size_t>(y) * l0.w + x] = 1;
        }
    levels.reserve(16);
    while (levels.back().w > 1 || levels.back().h > 1) {
        const Level& a = levels.back();
        Level b;
        b.w = (a.w + 1) / 2;
        b.h = (a.h + 1) / 2;
        b.v.assign(static_cast<size_t>(b.w) * b.h * 4, 0);
        b.wt.assign(static_cast<size_t>(b.w) * b.h, 0);
        for (int y = 0; y < a.h; y++)
            for (int x = 0; x < a.w; x++) {
                const size_t s = static_cast<size_t>(y) * a.w + x, d = static_cast<size_t>(y / 2) * b.w + x / 2;
                for (int k = 0; k < 4; k++)
                    b.v[d * 4 + k] += a.v[s * 4 + k];
                b.wt[d] += a.wt[s];
            }
        levels.push_back(std::move(b));
    }
    // Down the pyramid: a pixel without weight takes its parents' average,
    // bilinear between the four nearest (nearest alone leaves blocks).
    levels.back().wt[0] = std::max(levels.back().wt[0], 1e-6f);
    for (int li = static_cast<int>(levels.size()) - 2; li >= 0; li--) {
        Level& a = levels[li];
        const Level& b = levels[li + 1];
        for (int y = 0; y < a.h; y++)
            for (int x = 0; x < a.w; x++) {
                const size_t s = static_cast<size_t>(y) * a.w + x;
                if (a.wt[s] >= 1)
                    continue;
                const float fx = std::min(std::max((x + 0.5f) / 2 - 0.5f, 0.f), b.w - 1.f);
                const float fy = std::min(std::max((y + 0.5f) / 2 - 0.5f, 0.f), b.h - 1.f);
                const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
                const int jx = std::min(ix + 1, b.w - 1), jy = std::min(iy + 1, b.h - 1);
                const float tx = fx - ix, ty = fy - iy;
                float acc[4] = {0, 0, 0, 0}, wsum = 0;
                const int qx[4] = {ix, jx, ix, jx}, qy[4] = {iy, iy, jy, jy};
                const float qw[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
                for (int q = 0; q < 4; q++) {
                    const size_t d = static_cast<size_t>(qy[q]) * b.w + qx[q];
                    if (b.wt[d] <= 0 || qw[q] <= 0)
                        continue;
                    for (int k = 0; k < 4; k++)
                        acc[k] += b.v[d * 4 + k] / b.wt[d] * qw[q];
                    wsum += qw[q];
                }
                if (wsum <= 0)
                    continue;
                for (int k = 0; k < 4; k++)
                    a.v[s * 4 + k] = acc[k] / wsum;
                a.wt[s] = 1;
            }
        // Normalise this level's sums for the next one down.
        for (size_t s = 0; s < a.wt.size(); s++)
            if (a.wt[s] > 0) {
                for (int k = 0; k < 4; k++)
                    a.v[s * 4 + k] /= a.wt[s];
                a.wt[s] = 1;
            }
    }
    const Level& top = levels[0];  // levels grew: l0 no longer points into it
    for (int y = 0; y < top.h; y++)
        for (int x = 0; x < top.w; x++) {
            if (!hole[static_cast<size_t>(y + y0) * c.w + x + x0])
                continue;
            float* p = c.at(x + x0, y + y0);
            const uint32_t hsh = static_cast<uint32_t>(x * 374761393u + y * 668265263u);
            const float grain = (static_cast<float>(((hsh ^ (hsh >> 13)) * 1274126177u) >> 16 & 0xFF) / 255.f - 0.5f) * 3.f;
            for (int k = 0; k < 4; k++)
                p[k] = top.v[(static_cast<size_t>(y) * top.w + x) * 4 + k] + (k < 3 ? grain : 0);
        }
}

// Rubs out an ellipse (centre, radii and angle in the game texture's pixels),
// filled from around it; clear makes it transparent instead. Blood stays.
void rub_out(Canvas& c, float cx, float cy, float rx, float ry, float degrees, bool clear,
             bool keep_blood = false) {
    std::vector<uint8_t> hole(static_cast<size_t>(c.w) * c.h, 0);
    const float ca = std::cos(degrees * 3.14159265f / 180), sa = std::sin(degrees * 3.14159265f / 180);
    const float r = std::max(rx, ry) * c.u;
    const int x0 = std::max(0, static_cast<int>(cx * c.u - r) - 1), x1 = std::min(c.w, static_cast<int>(cx * c.u + r) + 2);
    const int y0 = std::max(0, static_cast<int>(cy * c.u - r) - 1), y1 = std::min(c.h, static_cast<int>(cy * c.u + r) + 2);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const float dx = (x + 0.5f) / c.u - cx, dy = (y + 0.5f) / c.u - cy;
            const float ex = (dx * ca + dy * sa) / rx, ey = (-dx * sa + dy * ca) / ry;
            if (ex * ex + ey * ey <= 1 && !(keep_blood && blood(c.at(x, y))))
                hole[static_cast<size_t>(y) * c.w + x] = 1;
        }
    fill(c, hole, x0, y0, x1, y1);
    if (clear)
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++)
                if (hole[static_cast<size_t>(y) * c.w + x])
                    c.at(x, y)[3] = 0;
}

// ---- Icons -------------------------------------------------------------------

bool saturated(const uint8_t* p) {
    return std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]}) > 40;
}

struct Icon {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    std::vector<uint8_t> solid;  // alpha with the letters cut out of it filled in
    bool mixed = false;          // white and coloured parts (a PlayStation symbol, a d-pad)
    bool dpad = false;           // a d-pad and its lit direction
};

// How much ink a pixel of an icon puts down as a stamp: its shape, but a white
// symbol on a coloured button (PlayStation's) stays paper like a cut-out
// letter, and a d-pad's unlit arms are a light wash under the lit one.
float ink_of(const Icon& ic, const uint8_t* q) {
    if (!ic.mixed || saturated(q))
        return q[3];
    return ic.dpad ? q[3] * 0.3f : 0.f;
}

const Icon* icon(const std::string& file) {
    static std::mutex mutex;
    static std::map<std::string, Icon> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(file);
    if (it != cache.end())
        return it->second.rgba.empty() ? nullptr : &it->second;
    Icon& ic = cache[file];
    if (file.empty() ||
        !load_png_rgba(frontend_res_dir() + "/ui/glyphs/" + file, &ic.rgba, &ic.w, &ic.h)) {
        if (!file.empty())
            std::fprintf(stderr, "[glyph-art] icon missing: %s\n", file.c_str());
        ic.rgba.clear();
        return nullptr;
    }
    // The outside: transparent pixels reached from the border.
    const int w = static_cast<int>(ic.w), h = static_cast<int>(ic.h);
    std::vector<uint8_t> outside(static_cast<size_t>(w) * h, 0);
    std::vector<int> stack;
    auto push = [&](int x, int y) {
        const size_t i = static_cast<size_t>(y) * w + x;
        if (!outside[i] && ic.rgba[i * 4 + 3] < 128) {
            outside[i] = 1;
            stack.push_back(static_cast<int>(i));
        }
    };
    for (int x = 0; x < w; x++) {
        push(x, 0);
        push(x, h - 1);
    }
    for (int y = 0; y < h; y++) {
        push(0, y);
        push(w - 1, y);
    }
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        const int x = i % w, y = i / w;
        if (x > 0) push(x - 1, y);
        if (x + 1 < w) push(x + 1, y);
        if (y > 0) push(x, y - 1);
        if (y + 1 < h) push(x, y + 1);
    }
    ic.solid.resize(static_cast<size_t>(w) * h);
    bool white = false, coloured = false;
    for (size_t i = 0; i < ic.solid.size(); i++) {
        ic.solid[i] = outside[i] ? ic.rgba[i * 4 + 3] : 255;
        if (ic.rgba[i * 4 + 3] >= 128)
            (saturated(&ic.rgba[i * 4]) ? coloured : white) = true;
    }
    ic.mixed = white && coloured;
    for (const char* d : {"/dup.", "/ddown.", "/dleft.", "/dright."})
        ic.dpad = ic.dpad || file.find(d) != std::string::npos;
    return &ic;
}

struct Style {
    bool colour = true;        // the icon's own colours
    float ink[3] = {0, 0, 0};  // else its shape in this colour,
    bool backed = false;       // over its outline filled with
    float paper[3] = {0, 0, 0};
    float alpha = 1;
    bool keep_blood = false;
};

Style colour() { return Style{}; }

Style stamp(float r, float g, float b, float alpha = 0.9f) {
    Style s;
    s.colour = false;
    s.ink[0] = r;
    s.ink[1] = g;
    s.ink[2] = b;
    s.alpha = alpha;
    return s;
}

Style backed(Style s, float r, float g, float b) {
    s.backed = true;
    s.paper[0] = r;
    s.paper[1] = g;
    s.paper[2] = b;
    return s;
}

void over(float* p, const float rgb[3], float a) {
    if (a <= 0)
        return;
    for (int k = 0; k < 3; k++)
        p[k] += (rgb[k] - p[k]) * a;
    p[3] += (255 - p[3]) * a;
}

// Draws icons laid over each other, size pixels of the game texture square,
// centred on (cx, cy).
void draw(Canvas& c, const std::vector<std::string>& files, float cx, float cy, float size,
          const Style& st) {
    std::vector<const Icon*> layers;
    for (const std::string& f : files)
        if (const Icon* ic = icon(f))
            layers.push_back(ic);
    if (layers.empty())
        return;
    const int n = std::max(1, static_cast<int>(size * c.u + 0.5f));
    const int ox = static_cast<int>(cx * c.u - n / 2.f + 0.5f), oy = static_cast<int>(cy * c.u - n / 2.f + 0.5f);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            if (!c.inside(ox + x, oy + y))
                continue;
            float* p = c.at(ox + x, oy + y);
            if (st.keep_blood && blood(p))
                continue;
            float rgb[3] = {0, 0, 0}, alpha = 0, ink = 0, solid = 0;
            for (const Icon* ic : layers) {
                const uint32_t sx0 = x * ic->w / n, sx1 = std::max(sx0 + 1, (x + 1) * ic->w / n);
                const uint32_t sy0 = y * ic->h / n, sy1 = std::max(sy0 + 1, (y + 1) * ic->h / n);
                float acc[3] = {0, 0, 0}, a = 0, k = 0, s = 0;
                int cnt = 0;
                for (uint32_t sy = sy0; sy < sy1; sy++)
                    for (uint32_t sx = sx0; sx < sx1; sx++) {
                        const size_t i = static_cast<size_t>(sy) * ic->w + sx;
                        const uint8_t* q = &ic->rgba[i * 4];
                        for (int j = 0; j < 3; j++)
                            acc[j] += q[j] * q[3];
                        a += q[3];
                        k += ink_of(*ic, q);
                        s += ic->solid[i];
                        cnt++;
                    }
                if (a / cnt > alpha) {
                    alpha = a / cnt;
                    for (int j = 0; j < 3; j++)
                        rgb[j] = a > 0 ? acc[j] / a : 0;
                }
                ink = std::max(ink, k / cnt);
                solid = std::max(solid, s / cnt);
            }
            if (st.colour) {
                over(p, rgb, alpha / 255 * st.alpha);
            } else {
                if (st.backed)
                    over(p, st.paper, solid / 255 * st.alpha);
                over(p, st.ink, ink / 255 * st.alpha);
            }
        }
}

// ---- The textures --------------------------------------------------------------

// The icon file for an action, or one file per input for the d-pad slots.
struct Glyphs {
    std::string set;  // xbox, ps, switch or kb
    std::string play[controls::kActionCount];  // face buttons first
    std::string tome[controls::kActionCount];
    std::vector<std::string> spells;  // the four quick spell slots, up down left right
    bool spells_on_dpad = false;
};

Glyphs glyphs_now() {
    using namespace controls;
    Glyphs g;
    g.set = glyph_set();
    for (int a = 0; a < kActionCount; a++) {
        g.play[a] = glyph_for(a, kFaceButton, false);
        g.tome[a] = glyph_for(a, a == kReload ? kAnyButton : kFaceButton, true);
    }
    for (int a : {kSpellUp, kSpellDown, kSpellLeft, kSpellRight})
        g.spells.push_back(glyph_for(a, kAnyButton, false));
    g.spells_on_dpad = g.set != "kb" && g.spells[0] == g.set + "/dup.png" &&
                       g.spells[1] == g.set + "/ddown.png" && g.spells[2] == g.set + "/dleft.png" &&
                       g.spells[3] == g.set + "/dright.png";
    return g;
}

// The Tome map's "Y Button: Up Floor" lines: "Y Button" becomes the icon and
// the rest of each line moves left after it. The writing is lifted off the
// parchment by how much darker it is than the parchment filled in under it.
void map_page(Canvas& c, const Glyphs& g) {
    const Canvas before = c;
    // The four lines span x 13-192, y 41-127; their words from the colon at 86.5.
    std::vector<uint8_t> hole(static_cast<size_t>(c.w) * c.h, 0);
    const int x0 = static_cast<int>(13 * c.u), x1 = static_cast<int>(194 * c.u);
    const int y0 = static_cast<int>(41 * c.u), y1 = static_cast<int>(127 * c.u);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            hole[static_cast<size_t>(y) * c.w + x] = 1;
    // The parchment under the writing, for telling the writing from it...
    fill(c, hole, x0, y0, x1, y1);
    const Canvas paper = c;
    // ...then only the writing (and a little around it) is filled, so the
    // parchment's own grain stays everywhere else.
    c = before;
    std::fill(hole.begin(), hole.end(), 0);
    const int grow = static_cast<int>(2 * c.u);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (luma(paper.at(x, y)) - luma(before.at(x, y)) > 7)
                for (int dy = -grow; dy <= grow; dy++)
                    for (int dx = -grow; dx <= grow; dx++)
                        if (c.inside(x + dx, y + dy) && dx * dx + dy * dy <= grow * grow)
                            hole[static_cast<size_t>(y + dy) * c.w + x + dx] = 1;
    fill(c, hole, x0 - grow, y0 - grow, x1 + grow, y1 + grow);
    const int shift = static_cast<int>(-49 * c.u);
    float ink[3] = {40, 28, 20};
    for (int y = y0; y < y1; y++) {
        // The last line's colon sits a little further right.
        const int from = static_cast<int>((y < 105 * c.u ? 86.5f : 87.8f) * c.u);
        for (int x = from; x < x1; x++) {
            const float* o = before.at(x, y);
            const float* b = paper.at(x, y);
            const float d = luma(b) - luma(o);
            const float full = std::max(20.f, (luma(b) - 40) * 0.8f);
            const float a = std::min(1.f, std::max(0.f, (d - 8) / (full - 8)));
            if (a > 0 && c.inside(x + shift, y))
                over(c.at(x + shift, y), ink, a);
        }
    }
    const Style st = stamp(40, 28, 20, 0.92f);
    const float size = 17;
    draw(c, {g.tome[controls::kQuickcast]}, 26, 52, size, st);
    draw(c, {g.tome[controls::kReload]}, 26, 73, size, st);
    draw(c, {g.tome[controls::kAttack]}, 26, 94.5f, size, st);
    draw(c, {g.tome[controls::kCreep]}, 26, 116, size, st);
}

struct Head {
    float x, y;
    int action;
    bool bloodied;  // the blood runs over it (on the torn sheets): it stays on top
};

void notes(Canvas& c, const std::vector<Head>& heads, float r, bool torn, const Glyphs& g) {
    for (const Head& h : heads) {
        const int px = static_cast<int>(h.x * c.u), py = static_cast<int>(h.y * c.u);
        if (!c.inside(px, py) || c.at(px, py)[3] < 128)
            continue;  // torn away
        // Torn away into the dark behind the page (the small torn sheet keeps
        // that dark in the picture): the parchment just around the head says.
        float ring = 0;
        int count = 0;
        for (int i = 0; i < 24; i++) {
            const float a = i * 3.14159265f / 12;
            const int rx = static_cast<int>((h.x + std::cos(a) * (r + 2)) * c.u);
            const int ry = static_cast<int>((h.y + std::sin(a) * (r + 2)) * c.u);
            if (c.inside(rx, ry) && c.at(rx, ry)[3] >= 128 && !blood(c.at(rx, ry))) {
                ring += luma(c.at(rx, ry));
                count++;
            }
        }
        if (count < 8 || ring / count < 50)
            continue;
        // The head itself: the middle of the pixels near where it was
        // measured that are well darker than the parchment around it, so the
        // whole of the old head is rubbed out (a sliver left would show
        // through the new letter).
        const float paper = ring / count;
        float sx = 0, sy = 0;
        int dark = 0;
        const int reach = static_cast<int>((r + 3) * c.u);
        for (int dy = -reach; dy <= reach; dy++)
            for (int dx = -reach; dx <= reach; dx++) {
                if (dx * dx + dy * dy > reach * reach || !c.inside(px + dx, py + dy))
                    continue;
                const float* q = c.at(px + dx, py + dy);
                if (q[3] >= 128 && !blood(q) && luma(q) < paper - 25) {
                    sx += px + dx + 0.5f;
                    sy += py + dy + 0.5f;
                    dark++;
                }
            }
        float x = h.x, y = h.y;
        if (dark > reach * reach) {  // most of a head's worth: trust it
            x = sx / dark / c.u;
            y = sy / dark / c.u;
        }
        // The notes' heads are as dark a red as the blood: only where it
        // crosses them is anything kept.
        Style st = stamp(58, 42, 30, 0.95f);
        st.keep_blood = torn && h.bloodied;
        rub_out(c, x, y, r + 0.9f, r + 0.9f, 0, false, st.keep_blood);
        draw(c, {g.play[h.action]}, x, y, r * 2.3f, st);
    }
}

void redraw(Art art, Canvas& c, const Glyphs& g) {
    using namespace controls;
    const std::string sticks[2] = {g.set == "kb" ? "kb/w.png" : g.set + "/stick_l_up.png",
                                   g.set == "kb" ? "kb/s.png" : g.set + "/stick_l_down.png"};
    switch (art) {
    case kMap:
        map_page(c, g);
        break;
    case kPressA:
        rub_out(c, 109.5f, 28, 16.5f, 16.5f, 0, false);
        draw(c, {g.play[kAttack]}, 110, 28, 27, colour());
        break;
    case kLanguage:
        rub_out(c, 394, 408, 14.5f, 14.5f, 0, false);
        draw(c, {g.play[kAttack]}, 394, 408, 27, colour());
        break;
    case kBrightness:
        for (float x : {97.f, 545.f}) {
            rub_out(c, x, 223, 18, 18, 0, false);
            rub_out(c, x, 261, 18, 18, 0, false);
            draw(c, {sticks[0]}, x, 222, 28, colour());
            draw(c, {sticks[1]}, x, 260, 28, colour());
        }
        break;
    case kQuickY:
        rub_out(c, 27, 23, 31, 30, 0, true);
        draw(c, {g.play[kQuickcast]}, 27, 24, 36, backed(stamp(70, 48, 38, 1), 150, 126, 106));
        break;
    case kQuickDpad:
        if (!g.spells_on_dpad) {
            const float at[4][2] = {{36, 14}, {36, 61}, {13, 37}, {60, 37}};
            for (int i = 0; i < 4; i++)
                draw(c, {g.spells[i]}, at[i][0], at[i][1], 20,
                     backed(stamp(60, 40, 30, 1), 170, 150, 130));
        }
        break;
    case kFaceCard:
        rub_out(c, 45, 31, 25, 14, -33, false);
        rub_out(c, 106, 46, 26, 14, 58, false);
        rub_out(c, 69, 68, 27.5f, 27.5f, 0, false);
        rub_out(c, 31, 100, 16.5f, 16.5f, 0, false);
        draw(c, {g.play[kQuickcast]}, 45, 31, 28, colour());
        draw(c, {g.play[kCreep]}, 106, 47, 28, colour());
        draw(c, {g.play[kAttack]}, 69, 68, 46, colour());
        draw(c, {g.play[kAction]}, 31, 100, 28, colour());
        break;
    case kNoteA: case kNoteY: case kNoteB: case kNoteX: {
        const int action = art == kNoteA ? kAttack : art == kNoteY ? kQuickcast
                         : art == kNoteB ? kAction : kCreep;
        rub_out(c, 11, 47, 11, 11, 0, true);
        draw(c, {g.play[action]}, 11, 47, 21, backed(stamp(62, 44, 30, 1), 168, 138, 96));
        break;
    }
    case kSheet:
    case kSheetTorn:
        notes(c, {{54, 82.6f, kAttack, false}, {72.1f, 72.1f, kAction, false},
                  {89.6f, 78.3f, kQuickcast, false}, {107.4f, 65, kCreep, true},
                  {125.4f, 71.4f, kAction, true}, {142.9f, 78.3f, kQuickcast, false},
                  {161.4f, 83.6f, kAttack, false}},
              5.8f, art == kSheetTorn, g);
        break;
    case kSheetTornLarge:
        notes(c, {{98, 137, kAttack, false}, {130, 121.7f, kAction, false},
                  {162, 129.5f, kQuickcast, false}, {195.5f, 110, kCreep, true},
                  {227.5f, 117.5f, kAction, true}, {261.5f, 130.4f, kQuickcast, false},
                  {295.5f, 136, kAttack, false}},
              10.5f, true, g);
        break;
    }
}

// The pack's image (else the game's, enlarged), upright.
Canvas base(const Texture& t, const std::vector<uint8_t>& game) {
    Canvas c;
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (!texpack_read_rgba(kCmpr, t.width, t.height, t.hash, &rgba, &w, &h)) {
        w = t.width * kScale;
        h = t.height * kScale;
        rgba.resize(static_cast<size_t>(w) * h * 4);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                const float fx = std::min(std::max((x + 0.5f) / kScale - 0.5f, 0.f), t.width - 1.f);
                const float fy = std::min(std::max((y + 0.5f) / kScale - 0.5f, 0.f), t.height - 1.f);
                const uint32_t ix = static_cast<uint32_t>(fx), iy = static_cast<uint32_t>(fy);
                const uint32_t jx = std::min(ix + 1, t.width - 1), jy = std::min(iy + 1, t.height - 1);
                const float tx = fx - ix, ty = fy - iy;
                for (int k = 0; k < 4; k++) {
                    auto px = [&](uint32_t xx, uint32_t yy) {
                        return static_cast<float>(game[(static_cast<size_t>(yy) * t.width + xx) * 4 + k]);
                    };
                    const float top = px(ix, iy) + (px(jx, iy) - px(ix, iy)) * tx;
                    const float bottom = px(ix, jy) + (px(jx, jy) - px(ix, jy)) * tx;
                    rgba[(static_cast<size_t>(y) * w + x) * 4 + k] =
                        static_cast<uint8_t>(top + (bottom - top) * ty + 0.5f);
                }
            }
    }
    c.w = static_cast<int>(w);
    c.h = static_cast<int>(h);
    c.u = static_cast<float>(w) / t.width;
    c.px.resize(static_cast<size_t>(w) * h * 4);
    // Stored upside down, as the game's textures are.
    for (int y = 0; y < c.h; y++)
        for (int x = 0; x < c.w; x++)
            for (int k = 0; k < 4; k++)
                c.at(x, y)[k] = rgba[(static_cast<size_t>(c.h - 1 - y) * w + x) * 4 + k];
    return c;
}

// The redrawn texture as RGBA8, stored upside down again.
struct Picture {
    std::vector<uint8_t> rgba;
    uint32_t width = 0, height = 0;
};

Picture compose(const Texture& t, const std::vector<uint8_t>& game, const Glyphs& g) {
    Canvas c = base(t, game);
    redraw(t.art, c, g);
    Picture out;
    out.width = static_cast<uint32_t>(c.w);
    out.height = static_cast<uint32_t>(c.h);
    out.rgba.resize(static_cast<size_t>(c.w) * c.h * 4);
    for (int y = 0; y < c.h; y++)
        for (int x = 0; x < c.w; x++)
            for (int k = 0; k < 4; k++)
                out.rgba[(static_cast<size_t>(c.h - 1 - y) * c.w + x) * 4 + k] = static_cast<uint8_t>(
                    std::min(255.f, std::max(0.f, c.at(x, y)[k])) + 0.5f);
    return out;
}

// Each texture is drawn on a worker (the large ones take a moment); the
// game's shows until it is ready.
struct Build {
    Picture picture;
    uint64_t signature = 0;
    bool running = false, ready = false;
};
std::mutex g_mutex;
std::map<uint64_t, Build> g_builds;

uint64_t signature_now() {
    return controls::bindings_signature() ^ (g_settings.hd_textures ? 0x9E3779B97F4A7C15ull : 0u);
}

}  // namespace

bool art_claims(uint32_t format, uint32_t width, uint32_t height) {
    if (format != kCmpr)
        return false;
    for (const Texture& t : kTextures)
        if (t.width == width && t.height == height)
            return true;
    return false;
}

bool art_claims_hash(uint64_t hash) { return find(hash) != nullptr; }

int art_apply(const uint8_t* rgba, uint32_t width, uint32_t height, uint64_t hash,
              uint8_t** out_rgba, uint32_t* out_width, uint32_t* out_height) {
    const Texture* t = find(hash);
    if (t == nullptr || width != t->width || height != t->height)
        return 0;
    const uint64_t signature = signature_now();
    std::lock_guard<std::mutex> lock(g_mutex);
    Build& b = g_builds[hash];
    if (b.ready && b.signature == signature) {
        *out_rgba = static_cast<uint8_t*>(std::malloc(b.picture.rgba.size()));
        if (*out_rgba == nullptr)
            return 0;
        std::memcpy(*out_rgba, b.picture.rgba.data(), b.picture.rgba.size());
        *out_width = b.picture.width;
        *out_height = b.picture.height;
        return 1;
    }
    if (!b.running) {
        b.running = true;
        b.ready = false;
        std::thread(
            [t, signature](std::vector<uint8_t> game, Glyphs g) {
                Picture picture = compose(*t, game, g);
                std::lock_guard<std::mutex> lock(g_mutex);
                Build& done = g_builds[t->hash];
                done.picture = std::move(picture);
                done.signature = signature;
                done.running = false;
                done.ready = true;
            },
            std::vector<uint8_t>(rgba, rgba + static_cast<size_t>(width) * height * 4), glyphs_now())
            .detach();
    }
    return 2;
}

uint64_t art_generation() { return signature_now(); }

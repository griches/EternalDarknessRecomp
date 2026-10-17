// The rooms' baked light and shadow (their light maps: textures a draw
// samples through the second UV set), upscaled four times as they load. The
// originals are CMPR atlases of a few texels per wall, so at 4K a shadow's
// edge shows the compression's 4x4 blocks and the texels' stair-steps. Each
// map is evened out (a bilateral filter takes out CMPR's colour steps and
// keeps real edges). A big map (a floor) is then doubled twice by
// directional cubic convolution (DCCI, Zhou, Fang and Lu 2012: along an edge
// it interpolates with the edge, not across it), moved so its texel centres
// sit where the GPU samples them, and lightly sharpened; a small one (a
// wall's strip, a table) is upscaled with the smooth cubic B-spline, so its
// glows stay round instead of turning lumpy. The work runs on a worker; the
// original shows until its replacement is ready. An HD texture pack's own
// version of a map wins.
// Options > Game > Baked shadows: Sharp (this) or Original;
// ED_LIGHTMAP_SMOOTH=0 turns it off for a run.
#include "host/lightmaps.h"

#include "host/settings.h"

#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <xxhash.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint32_t kCmpr = 14;
constexpr float kCubic[4] = {-1.f / 16.f, 9.f / 16.f, 9.f / 16.f, -1.f / 16.f};

// RGBA as floats, w * h * 4.
struct Image {
    int w = 0, h = 0;
    std::vector<float> px;
    Image(int width, int height) : w(width), h(height), px(static_cast<size_t>(width) * height * 4) {}
    float* at(int x, int y) { return &px[(static_cast<size_t>(y) * w + x) * 4]; }
    const float* at(int x, int y) const { return &px[(static_cast<size_t>(y) * w + x) * 4]; }
    const float* clamped(int x, int y) const {
        return at(std::clamp(x, 0, w - 1), std::clamp(y, 0, h - 1));
    }
};

float lum(const float* p) {
    return (p[0] + p[1] + p[2] + p[3]) * 0.25f;
}

// CMPR keeps four colours a 4x4 block, so a glow comes out in flat steps that
// the upscale would sharpen into contours: a bilateral filter evens out
// differences under `contrast` levels and leaves a shadow's edge alone.
Image deblock_pass(const Image& in, float contrast) {
    constexpr int r = 2;
    constexpr float spatial = 1.2f;
    Image out(in.w, in.h);
    for (int y = 0; y < in.h; y++) {
        for (int x = 0; x < in.w; x++) {
            const float l = lum(in.at(x, y));
            float acc[4] = {0, 0, 0, 0}, weights = 0;
            for (int dy = -r; dy <= r; dy++) {
                for (int dx = -r; dx <= r; dx++) {
                    const float* n = in.clamped(x + dx, y + dy);
                    const float d = lum(n) - l;
                    const float wgt = std::exp(-(dx * dx + dy * dy) / (2 * spatial * spatial) -
                                               d * d / (2 * contrast * contrast));
                    for (int c = 0; c < 4; c++)
                        acc[c] += wgt * n[c];
                    weights += wgt;
                }
            }
            float* o = out.at(x, y);
            for (int c = 0; c < 4; c++)
                o[c] = acc[c] / weights;
        }
    }
    return out;
}

Image deblock(const Image& in, float contrast, int passes) {
    Image out = deblock_pass(in, contrast);
    for (int i = 1; i < passes; i++)
        out = deblock_pass(out, contrast);
    return out;
}

// p1 runs along direction 1 and p2 along direction 2, d being each direction's
// change: interpolate along the smoother one, blend where neither wins.
void blend(float d1, float d2, const float* p1, const float* p2, float* out) {
    d1 = d1 / 255.f * 8.f;
    d2 = d2 / 255.f * 8.f;
    if (1.f + d1 > 1.15f * (1.f + d2)) {
        std::memcpy(out, p2, sizeof(float) * 4);
        return;
    }
    if (1.f + d2 > 1.15f * (1.f + d1)) {
        std::memcpy(out, p1, sizeof(float) * 4);
        return;
    }
    const float w1 = 1.f / (1.f + std::pow(d1, 5.f)), w2 = 1.f / (1.f + std::pow(d2, 5.f));
    for (int c = 0; c < 4; c++)
        out[c] = (w1 * p1[c] + w2 * p2[c]) / (w1 + w2);
}

// One doubling. The image is padded by four texels (edges repeated) so every
// point of the result has its full neighbourhood, then the middle is kept.
Image dcci2x(const Image& in) {
    constexpr int pad = 4;
    const int pw = in.w + 2 * pad, ph = in.h + 2 * pad;
    const int W = 2 * pw, H = 2 * ph;
    Image o(W, H);
    std::vector<float> L(static_cast<size_t>(W) * H, 0.f);
    auto P = [&](int x, int y) { return in.clamped(x - pad, y - pad); };
    auto Lat = [&](int x, int y) -> float& { return L[static_cast<size_t>(y) * W + x]; };
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) {
            std::memcpy(o.at(2 * x, 2 * y), P(x, y), sizeof(float) * 4);
            Lat(2 * x, 2 * y) = lum(P(x, y));
        }
    // Diagonal points: the middle of each 2x2 of originals, from the 4x4
    // around it (down-right and down-left lines).
    for (int i = 1; i + 2 < ph; i++) {
        for (int j = 1; j + 2 < pw; j++) {
            float d1 = 0, d2 = 0;
            for (int y = -1; y <= 1; y++)
                for (int x = -1; x <= 1; x++) {
                    d1 += std::fabs(lum(P(j + x + 1, i + y + 1)) - lum(P(j + x, i + y)));
                    d2 += std::fabs(lum(P(j + x, i + y + 1)) - lum(P(j + x + 1, i + y)));
                }
            float p1[4] = {0, 0, 0, 0}, p2[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; k++) {
                const float* a = P(j + k - 1, i + k - 1);
                const float* b = P(j + 2 - k, i + k - 1);
                for (int c = 0; c < 4; c++) {
                    p1[c] += kCubic[k] * a[c];
                    p2[c] += kCubic[k] * b[c];
                }
            }
            float* d = o.at(2 * j + 1, 2 * i + 1);
            blend(d1, d2, p1, p2, d);
            Lat(2 * j + 1, 2 * i + 1) = lum(d);
        }
    }
    // Edge points: across and down from their four neighbours at distance 1
    // and 3 (the originals and the diagonal points, a rotated grid).
    auto fill = [&](int oy, int ox) {
        for (int gy = oy + 4; gy < H - 4; gy += 2) {
            for (int gx = ox + 4; gx < W - 4; gx += 2) {
                const float dh = std::fabs(Lat(gx - 1, gy) - Lat(gx + 1, gy)) +
                                 std::fabs(Lat(gx - 1, gy - 2) - Lat(gx + 1, gy - 2)) +
                                 std::fabs(Lat(gx - 1, gy + 2) - Lat(gx + 1, gy + 2)) +
                                 std::fabs(Lat(gx - 2, gy - 1) - Lat(gx, gy - 1)) +
                                 std::fabs(Lat(gx, gy - 1) - Lat(gx + 2, gy - 1)) +
                                 std::fabs(Lat(gx - 2, gy + 1) - Lat(gx, gy + 1)) +
                                 std::fabs(Lat(gx, gy + 1) - Lat(gx + 2, gy + 1));
                const float dv = std::fabs(Lat(gx, gy - 1) - Lat(gx, gy + 1)) +
                                 std::fabs(Lat(gx - 2, gy - 1) - Lat(gx - 2, gy + 1)) +
                                 std::fabs(Lat(gx + 2, gy - 1) - Lat(gx + 2, gy + 1)) +
                                 std::fabs(Lat(gx - 1, gy - 2) - Lat(gx - 1, gy)) +
                                 std::fabs(Lat(gx - 1, gy) - Lat(gx - 1, gy + 2)) +
                                 std::fabs(Lat(gx + 1, gy - 2) - Lat(gx + 1, gy)) +
                                 std::fabs(Lat(gx + 1, gy) - Lat(gx + 1, gy + 2));
                float ph4[4] = {0, 0, 0, 0}, pv4[4] = {0, 0, 0, 0};
                for (int k = 0; k < 4; k++) {
                    const float* a = o.at(gx - 3 + 2 * k, gy);
                    const float* b = o.at(gx, gy - 3 + 2 * k);
                    for (int c = 0; c < 4; c++) {
                        ph4[c] += kCubic[k] * a[c];
                        pv4[c] += kCubic[k] * b[c];
                    }
                }
                float* d = o.at(gx, gy);
                blend(dh, dv, ph4, pv4, d);
                Lat(gx, gy) = lum(d);
            }
        }
    };
    fill(0, 1);
    fill(1, 0);
    Image out(2 * in.w, 2 * in.h);
    for (int y = 0; y < out.h; y++)
        std::memcpy(out.at(0, y), o.at(2 * pad, 2 * pad + y), sizeof(float) * 4 * out.w);
    return out;
}

// The 4x grid holds texel i's value at 4i; the GPU samples texel centres at
// 4i + 1.5, so each pixel takes the value 1.5 pixels before it (a cubic at the
// half pixel), down the columns and then along the rows.
Image align(const Image& in) {
    Image a(in.w, in.h), b(in.w, in.h);
    for (int y = 0; y < in.h; y++)
        for (int x = 0; x < in.w; x++) {
            float* d = a.at(x, y);
            for (int c = 0; c < 4; c++) {
                float v = 0;
                for (int k = 0; k < 4; k++)
                    v += kCubic[k] * in.clamped(x, y - 3 + k)[c];
                d[c] = v;
            }
        }
    for (int y = 0; y < in.h; y++)
        for (int x = 0; x < in.w; x++) {
            float* d = b.at(x, y);
            for (int c = 0; c < 4; c++) {
                float v = 0;
                for (int k = 0; k < 4; k++)
                    v += kCubic[k] * a.clamped(x - 3 + k, y)[c];
                d[c] = v;
            }
        }
    return b;
}

// A plain 4x upscale with the cubic B-spline: smooth, never overshooting, with
// each output pixel sampled where the GPU would (no shift needed after).
Image bspline4x(const Image& in) {
    auto weights = [](float t, float w[4]) {
        const float t2 = t * t, t3 = t2 * t;
        w[0] = (1 - t) * (1 - t) * (1 - t) / 6.f;
        w[1] = (3 * t3 - 6 * t2 + 4) / 6.f;
        w[2] = (-3 * t3 + 3 * t2 + 3 * t + 1) / 6.f;
        w[3] = t3 / 6.f;
    };
    Image a(in.w * 4, in.h), out(in.w * 4, in.h * 4);
    for (int x = 0; x < a.w; x++) {
        const float u = (x + 0.5f) / 4.f - 0.5f;
        const int i = static_cast<int>(std::floor(u));
        float w[4];
        weights(u - i, w);
        for (int y = 0; y < in.h; y++)
            for (int c = 0; c < 4; c++) {
                float v = 0;
                for (int k = 0; k < 4; k++)
                    v += w[k] * in.clamped(i - 1 + k, y)[c];
                a.at(x, y)[c] = v;
            }
    }
    for (int y = 0; y < out.h; y++) {
        const float u = (y + 0.5f) / 4.f - 0.5f;
        const int i = static_cast<int>(std::floor(u));
        float w[4];
        weights(u - i, w);
        for (int x = 0; x < out.w; x++)
            for (int c = 0; c < 4; c++) {
                float v = 0;
                for (int k = 0; k < 4; k++)
                    v += w[k] * a.clamped(x, i - 1 + k)[c];
                out.at(x, y)[c] = v;
            }
    }
    return out;
}

// Separable Gaussian, edges repeated.
Image gaussian(const Image& img, float sigma) {
    const int r = std::max(1, static_cast<int>(std::ceil(3.f * sigma)));
    std::vector<float> k(2 * r + 1);
    float sum = 0;
    for (int i = -r; i <= r; i++)
        sum += k[i + r] = std::exp(-i * i / (2 * sigma * sigma));
    for (float& v : k)
        v /= sum;
    Image a(img.w, img.h), out(img.w, img.h);
    for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++)
            for (int c = 0; c < 4; c++) {
                float v = 0;
                for (int i = -r; i <= r; i++)
                    v += k[i + r] * img.clamped(x, y + i)[c];
                a.at(x, y)[c] = v;
            }
    for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++)
            for (int c = 0; c < 4; c++) {
                float v = 0;
                for (int i = -r; i <= r; i++)
                    v += k[i + r] * a.clamped(x + i, y)[c];
                out.at(x, y)[c] = v;
            }
    return out;
}

// Unsharp mask on the 4x image: a shadow's edge a little steeper.
void sharpen(Image& img, float amount) {
    const Image blur = gaussian(img, 1.5f);
    for (size_t i = 0; i < img.px.size(); i++)
        img.px[i] += amount * (img.px[i] - blur.px[i]);
}

// A map no more than kSmallMap texels on its short side (a wall's strip, a
// table, a chair) holds little shape, and edge-keeping doubling turns its
// CMPR steps into lumps: it is evened out harder and upscaled with the
// smooth B-spline, so a lamp's glow is a round pool. Bigger maps (the floors)
// keep the edges: doubled along them and lightly sharpened, so the light
// through a window keeps its lattice.
constexpr uint32_t kSmallMap = 40;

uint8_t* upscale4(const uint8_t* rgba, uint32_t width, uint32_t height) {
    Image img(static_cast<int>(width), static_cast<int>(height));
    for (size_t i = 0; i < img.px.size(); i++)
        img.px[i] = rgba[i];
    Image big(0, 0);
    if (std::min(width, height) <= kSmallMap) {
        big = bspline4x(deblock(img, 12.f, 2));
    } else {
        big = align(dcci2x(dcci2x(deblock(img, 6.f, 1))));
        sharpen(big, 1.2f);
    }
    auto* out = static_cast<uint8_t*>(std::malloc(big.px.size()));
    if (out == nullptr)
        return nullptr;
    for (size_t i = 0; i < big.px.size(); i++)
        out[i] = static_cast<uint8_t>(std::clamp(std::nearbyint(big.px[i]), 0.f, 255.f));
    return out;
}

// Asked again each frame while a map is being made, so a fast hash.
uint64_t key_of(const uint8_t* rgba, uint32_t width, uint32_t height) {
    return XXH3_64bits_withSeed(rgba, static_cast<size_t>(width) * height * 4,
                                static_cast<uint64_t>(width) << 32 | height);
}

struct Job {
    uint64_t key;
    uint32_t width, height;
    std::vector<uint8_t> rgba;
};
struct Done {
    uint8_t* rgba;  // malloc'd, 4x the map's size
    uint32_t width, height;
};
// Shared with the workers, which never stop: on the heap and never destroyed,
// so quitting neither waits on nor frees what they use.
struct Shared {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Job> queue;
    std::unordered_set<uint64_t> queued;  // queued or being made
    std::unordered_map<uint64_t, Done> done;
    std::deque<uint64_t> done_order;  // oldest first, to drop unclaimed ones
    uint64_t busy_ns = 0;             // the workers' time, for ED_LIGHTMAP_LOG
};
Shared& g = *new Shared;

void worker() {
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(g.mutex);
            g.cv.wait(lock, [] { return !g.queue.empty(); });
            job = std::move(g.queue.front());
            g.queue.pop_front();
        }
        const uint64_t start = SDL_GetTicksNS();
        uint8_t* out = upscale4(job.rgba.data(), job.width, job.height);
        std::lock_guard<std::mutex> lock(g.mutex);
        g.queued.erase(job.key);
        g.busy_ns += SDL_GetTicksNS() - start;
        static const bool log = std::getenv("ED_LIGHTMAP_LOG") != nullptr;
        if (log && g.queue.empty()) {
            std::fprintf(stderr, "[lightmaps] all made: %.0f ms of work so far\n", g.busy_ns / 1e6);
        }
        if (out == nullptr)
            continue;
        g.done[job.key] = Done{out, job.width * 4, job.height * 4};
        g.done_order.push_back(job.key);
        // A map whose texture went before its replacement was asked for
        while (g.done_order.size() > 256) {
            if (auto it = g.done.find(g.done_order.front()); it != g.done.end()) {
                std::free(it->second.rgba);
                g.done.erase(it);
            }
            g.done_order.pop_front();
        }
    }
}

bool enabled() {
    static const bool env_on = [] {
        const char* env = std::getenv("ED_LIGHTMAP_SMOOTH");
        return env == nullptr || env[0] != '0';
    }();
    return env_on && g_settings.sharp_shadows;
}

}  // namespace

bool lightmaps_claims_uv2(uint32_t format, uint32_t, uint32_t) {
    return format == kCmpr && enabled();
}

int lightmaps_apply(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t** out_rgba,
                    uint32_t* out_width, uint32_t* out_height) {
    static std::once_flag started;
    std::call_once(started, [] {
        const int workers = SDL_GetNumLogicalCPUCores() > 8 ? 2 : 1;
        for (int i = 0; i < workers; i++)
            std::thread(worker).detach();
    });
    const uint64_t key = key_of(rgba, width, height);
    std::lock_guard<std::mutex> lock(g.mutex);
    if (auto it = g.done.find(key); it != g.done.end()) {
        *out_rgba = it->second.rgba;
        *out_width = it->second.width;
        *out_height = it->second.height;
        g.done.erase(it);
        return 1;
    }
    if (g.queued.insert(key).second) {
        static const bool log = std::getenv("ED_LIGHTMAP_LOG") != nullptr;
        if (log)
            std::fprintf(stderr, "[lightmaps] %ux%u queued (%zu waiting)\n", width, height,
                         g.queue.size() + 1);
        g.queue.push_back(Job{key, width, height,
                              std::vector<uint8_t>(rgba, rgba + static_cast<size_t>(width) * height * 4)});
        g.cv.notify_one();
    }
    return 2;
}

uint64_t lightmaps_generation() {
    return 1u;
}

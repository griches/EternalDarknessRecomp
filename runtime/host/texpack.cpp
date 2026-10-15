// HD texture packs in Dolphin's format: a folder of tex1_<w>x<h>[_m]_<hash>_<fmt>.png
// files, the hash being XXH64 of the texture's base level (the renderer
// computes it for each texture it decodes and asks here). The folder is
// indexed by name at start-up; a texture's PNG is decoded on a loader thread
// the first time it is drawn (with its mip chain), the original showing until
// it is ready.
//
// The pack lives in textures/ beside res/ (any layout of subfolders), or
// ED_TEXTURE_PACK=<folder>. Options > Game > HD textures turns it off.
#include "host/texpack.h"

#include "gxruntime/aurora_backend.h"
#include "host/buttons.h"
#include "host/frontend.h"
#include "host/settings.h"

#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_thread.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct Key {
    uint32_t format, width, height;
    uint64_t hash;
    bool operator==(const Key& o) const {
        return format == o.format && width == o.width && height == o.height && hash == o.hash;
    }
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        return static_cast<size_t>(k.hash ^ (static_cast<uint64_t>(k.width) << 40) ^
                                   (static_cast<uint64_t>(k.height) << 20) ^ k.format);
    }
};

struct Loaded {
    std::vector<uint8_t> rgba;  // every mip level, largest first
    uint32_t width = 0, height = 0, mips = 1;
};

// Append the mip chain down to 1x1: each level the 2x2 average of the last,
// colour weighted by alpha so cut-out edges do not darken.
void build_mips(Loaded& img) {
    uint32_t w = img.width, h = img.height;
    size_t at = 0;
    while (w > 1 || h > 1) {
        const uint32_t nw = std::max(w / 2, 1u), nh = std::max(h / 2, 1u);
        const size_t next = img.rgba.size();
        img.rgba.resize(next + static_cast<size_t>(nw) * nh * 4);
        const uint8_t* src = img.rgba.data() + at;
        uint8_t* dst = img.rgba.data() + next;
        for (uint32_t y = 0; y < nh; y++)
            for (uint32_t x = 0; x < nw; x++) {
                uint32_t sum[3] = {0, 0, 0}, alpha = 0;
                for (uint32_t dy = 0; dy < 2; dy++)
                    for (uint32_t dx = 0; dx < 2; dx++) {
                        const uint32_t sx = std::min(x * 2 + dx, w - 1);
                        const uint32_t sy = std::min(y * 2 + dy, h - 1);
                        const uint8_t* p = src + (static_cast<size_t>(sy) * w + sx) * 4;
                        for (int c = 0; c < 3; c++)
                            sum[c] += p[c] * p[3];
                        alpha += p[3];
                    }
                uint8_t* o = dst + (static_cast<size_t>(y) * nw + x) * 4;
                for (int c = 0; c < 3; c++)
                    o[c] = static_cast<uint8_t>(alpha != 0 ? (sum[c] + alpha / 2) / alpha : 0);
                o[3] = static_cast<uint8_t>((alpha + 2) / 4);
            }
        at = next;
        w = nw;
        h = nh;
        img.mips++;
    }
}

// Shared with the loader threads, which never stop: on the heap and never
// destroyed, so exit neither waits on nor frees what they use (glibc's
// pthread_cond_destroy blocks while a thread waits on the condition, which
// kept the process alive after quitting on Linux).
struct Shared {
    std::unordered_map<Key, std::string, KeyHash> files;  // read-only after init
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Key> queue;
    std::unordered_set<Key, KeyHash> queued;  // queued or loading
    std::unordered_map<Key, Loaded, KeyHash> ready;
    std::unordered_set<Key, KeyHash> failed;
};
Shared& g = *new Shared;
std::string g_dir;
bool g_log = false;

// tex1_<w>x<h>[_m]_<hash16>_<fmt>.png (paletted and per-mip files are skipped).
bool parse_name(const std::string& name, Key* key) {
    unsigned w = 0, h = 0, fmt = 0;
    unsigned long long hash = 0;
    char m[3] = {};
    int used = 0;
    if (std::sscanf(name.c_str(), "tex1_%ux%u_%2[m_]%n", &w, &h, m, &used) == 3 &&
        std::strcmp(m, "m_") == 0) {
        if (std::sscanf(name.c_str() + used, "%16llx_%u.png", &hash, &fmt) != 2)
            return false;
    } else if (std::sscanf(name.c_str(), "tex1_%ux%u_%16llx_%u.png", &w, &h, &hash, &fmt) != 4) {
        return false;
    }
    if (name.find("_mip") != std::string::npos || name.size() < 6 ||
        name.compare(name.size() - 4, 4, ".png") != 0)
        return false;
    // "_<16 hex>_<fmt>.png" ends the name: a second hash means a palette.
    const size_t tail = name.rfind('_');
    const size_t before = name.rfind('_', tail - 1);
    if (tail == std::string::npos || before == std::string::npos || tail - before != 17)
        return false;
    *key = Key{fmt, w, h, hash};
    return true;
}

void loader() {
    // Below the game's own threads: decoding waits, a frame does not.
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    for (;;) {
        Key key;
        {
            std::unique_lock<std::mutex> lock(g.mutex);
            g.cv.wait(lock, [] { return !g.queue.empty(); });
            key = g.queue.front();
            g.queue.pop_front();
        }
        Loaded img;
        const bool ok = load_png_rgba(g.files.at(key), &img.rgba, &img.width, &img.height);
        static const bool mips = [] {
            const char* env = std::getenv("ED_TEXPACK_MIPS");
            return env == nullptr || std::strcmp(env, "0") != 0;
        }();
        if (ok && mips)
            build_mips(img);
        std::lock_guard<std::mutex> lock(g.mutex);
        g.queued.erase(key);
        if (ok)
            g.ready[key] = std::move(img);
        else
            g.failed.insert(key);
        if (g_log)
            std::fprintf(stderr, "[texpack] %s %s\n", ok ? "loaded" : "FAILED",
                         g.files.at(key).c_str());
    }
}

int lookup(uint32_t format, uint32_t width, uint32_t height, uint64_t hash, uint8_t** out_rgba,
           uint32_t* out_width, uint32_t* out_height, uint32_t* out_mips) {
    const Key key{format, width, height, hash};
    if (g.files.find(key) == g.files.end())
        return 0;
    std::lock_guard<std::mutex> lock(g.mutex);
    if (auto it = g.ready.find(key); it != g.ready.end()) {
        Loaded& img = it->second;
        *out_rgba = static_cast<uint8_t*>(std::malloc(img.rgba.size()));
        if (*out_rgba == nullptr)
            return 0;
        std::memcpy(*out_rgba, img.rgba.data(), img.rgba.size());
        *out_width = img.width;
        *out_height = img.height;
        *out_mips = img.mips;
        g.ready.erase(it);  // the renderer keeps it now
        return 1;
    }
    if (g.failed.count(key) != 0)
        return 0;
    if (g.queued.insert(key).second) {
        g.queue.push_back(key);
        g.cv.notify_one();
    }
    return 2;
}

uint64_t generation() { return g_settings.hd_textures && !g.files.empty() ? 1u : 0u; }

std::string pack_dir() {
    if (const char* env = std::getenv("ED_TEXTURE_PACK"); env != nullptr && env[0] != '\0')
        return env;
    std::error_code ec;
    const std::filesystem::path beside_res =
        std::filesystem::path(frontend_res_dir()).parent_path() / "textures";
    if (std::filesystem::is_directory(beside_res, ec))
        return beside_res.string();
    return "textures";
}

}  // namespace

extern "C" void texpack_init(void) {
    g_log = std::getenv("ED_TEXPACK_LOG") != nullptr;
    g_dir = pack_dir();
    std::error_code ec;
    if (!std::filesystem::is_directory(g_dir, ec))
        return;
    size_t skipped = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(
             g_dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        const std::string name = it->path().filename().string();
        if (name.rfind("tex1_", 0) != 0)
            continue;
        Key key;
        if (parse_name(name, &key))
            g.files.emplace(key, it->path().string());
        else
            skipped++;
    }
    std::fprintf(stderr, "[texpack] %zu textures in %s (%zu skipped)\n", g.files.size(),
                 g_dir.c_str(), skipped);
    if (g.files.empty())
        return;
    // PNG decoding is the slow part and must not hold up a frame: two loaders,
    // or one on a machine of 8 threads or fewer (the Steam Deck), where they
    // share cores with the game's.
    const int loaders = SDL_GetNumLogicalCPUCores() > 8 ? 2 : 1;
    for (int i = 0; i < loaders; i++)
        std::thread(loader).detach();
    static const DolAuroraTexturePack ops = {lookup, generation};
    dol_aurora_set_texture_pack(&ops);
}

size_t texpack_count() { return g.files.size(); }

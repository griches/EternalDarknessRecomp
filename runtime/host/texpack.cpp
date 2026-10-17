// HD texture packs in Dolphin's format: a folder of tex1_<w>x<h>[_m]_<hash>_<fmt>.png
// or .dds files, the hash being XXH64 of the texture's base level (the renderer
// computes it for each texture it decodes and asks here). The folder is
// indexed by name at start-up; a texture's file is read on a loader thread the
// first time it is drawn, the original showing until it is ready. A PNG is
// decoded and given a mip chain; a DDS file's BC1/BC2/BC3/BC7 image goes to the
// GPU compressed, with the mips the file holds (a quarter of the memory).
//
// The pack lives in textures/ beside res/ (any layout of subfolders, so packs
// can sit side by side), or ED_TEXTURE_PACK=<folder>. When two files are the
// same texture, one under a folder named with "supplement" or "override" wins
// (a fix-up pack beside the main one), and otherwise the path that sorts last.
// Options > Game > HD textures turns it off.
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

#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include "third_party/bcdec.h"

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
    std::vector<uint8_t> rgba;  // every mip level, largest first (BC blocks for a DDS)
    uint32_t width = 0, height = 0, mips = 1;
    uint32_t format = DOL_AURORA_PACK_RGBA8;
};

// A loaded image as the renderer takes it: malloc'd here on the loader thread,
// so handing it over costs the renderer's thread no copy.
struct Ready {
    uint8_t* rgba = nullptr;
    uint32_t width = 0, height = 0, mips = 1;
    uint32_t format = DOL_AURORA_PACK_RGBA8;
};

uint32_t read_le32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// A DDS file as Dolphin packs use them: BC1 (DXT1), BC2 (DXT3), BC3 (DXT5) or
// BC7 (DX10 header), or uncompressed 32-bit, with or without its mip chain.
// Uncompressed ones come back as RGBA8 without mips (build_mips adds them).
bool load_dds(const std::string& path, Loaded* img) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;
    std::vector<uint8_t> file;
    if (std::fseek(f, 0, SEEK_END) == 0) {
        const long size = std::ftell(f);
        if (size > 128) {
            file.resize(static_cast<size_t>(size));
            std::fseek(f, 0, SEEK_SET);
            if (std::fread(file.data(), 1, file.size(), f) != file.size())
                file.clear();
        }
    }
    std::fclose(f);
    if (file.size() <= 128 || std::memcmp(file.data(), "DDS ", 4) != 0 || read_le32(&file[4]) != 124)
        return false;
    const uint32_t flags = read_le32(&file[8]);
    const uint32_t height = read_le32(&file[12]), width = read_le32(&file[16]);
    uint32_t mips = (flags & 0x20000u) != 0 ? std::max(read_le32(&file[28]), 1u) : 1u;
    const uint32_t pf_flags = read_le32(&file[80]);
    const uint32_t fourcc = read_le32(&file[84]);
    const uint32_t bits = read_le32(&file[88]);
    size_t at = 128;
    uint32_t format = DOL_AURORA_PACK_RGBA8;
    auto cc = [](const char* s) { return read_le32(reinterpret_cast<const uint8_t*>(s)); };
    if ((pf_flags & 0x4u) != 0 && fourcc == cc("DX10")) {
        if (file.size() < 148)
            return false;
        const uint32_t dxgi = read_le32(&file[128]);
        at = 148;
        if (dxgi == 71 || dxgi == 72)
            format = DOL_AURORA_PACK_BC1;
        else if (dxgi == 74 || dxgi == 75)
            format = DOL_AURORA_PACK_BC2;
        else if (dxgi == 77 || dxgi == 78)
            format = DOL_AURORA_PACK_BC3;
        else if (dxgi == 98 || dxgi == 99)
            format = DOL_AURORA_PACK_BC7;
        else if (dxgi != 28 && dxgi != 29)  // R8G8B8A8_UNORM(_SRGB)
            return false;
    } else if ((pf_flags & 0x4u) != 0) {
        if (fourcc == cc("DXT1"))
            format = DOL_AURORA_PACK_BC1;
        else if (fourcc == cc("DXT3"))
            format = DOL_AURORA_PACK_BC2;
        else if (fourcc == cc("DXT5"))
            format = DOL_AURORA_PACK_BC3;
        else
            return false;
    } else if (bits != 32) {
        return false;
    }
    if (width == 0 || height == 0)
        return false;
    img->width = width;
    img->height = height;
    if (format == DOL_AURORA_PACK_RGBA8) {
        const size_t bytes = static_cast<size_t>(width) * height * 4;
        if (file.size() - at < bytes)
            return false;
        img->rgba.assign(file.begin() + at, file.begin() + at + bytes);
        // Masks tell the channel order: Dolphin writes B8G8R8A8 as often as R8G8B8A8.
        const bool bgra = at == 128 && read_le32(&file[92]) == 0x00FF0000u;
        if (bgra)
            for (size_t i = 0; i < bytes; i += 4)
                std::swap(img->rgba[i], img->rgba[i + 2]);
        img->mips = 1;
        img->format = format;
        return true;
    }
    // BC images go to the GPU as they are: the base level must be whole blocks,
    // and the file must hold every level it claims (else only those it holds).
    if (width % 4 != 0 || height % 4 != 0)
        return false;
    const uint32_t block = format == DOL_AURORA_PACK_BC1 ? 8u : 16u;
    size_t bytes = 0;
    uint32_t levels = 0;
    for (uint32_t m = 0; m < mips; m++) {
        const size_t level = static_cast<size_t>((std::max(width >> m, 1u) + 3) / 4) *
                             ((std::max(height >> m, 1u) + 3) / 4) * block;
        if (file.size() - at < bytes + level)
            break;
        bytes += level;
        levels++;
        if ((width >> m) <= 1 && (height >> m) <= 1)
            break;
    }
    if (levels == 0)
        return false;
    img->rgba.assign(file.begin() + at, file.begin() + at + bytes);
    img->mips = levels;
    img->format = format;
    return true;
}

// 1 for a file under a folder whose name says it fixes up a pack.
int overrides(const std::string& path, const std::string& root) {
    std::string lower = std::filesystem::path(path).parent_path().string();
    if (lower.size() > root.size())
        lower.erase(0, root.size());  // the pack folder's own path does not count
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("supplement") != std::string::npos ||
                   lower.find("override") != std::string::npos
               ? 1
               : 0;
}

bool is_dds(const std::string& path) {
    return path.size() > 4 && (path.compare(path.size() - 4, 4, ".dds") == 0 ||
                               path.compare(path.size() - 4, 4, ".DDS") == 0);
}

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
    std::unordered_map<Key, Ready, KeyHash> ready;
    std::unordered_set<Key, KeyHash> failed;
};
Shared& g = *new Shared;
std::string g_dir;
bool g_log = false;

// tex1_<w>x<h>[_m]_<hash16>_<fmt>.png or .dds (paletted and per-mip files are
// skipped).
bool parse_name(const std::string& name, Key* key) {
    if (name.size() < 6)
        return false;
    const std::string ext = name.substr(name.size() - 4);
    if (ext != ".png" && ext != ".dds" && ext != ".PNG" && ext != ".DDS")
        return false;
    unsigned w = 0, h = 0, fmt = 0;
    unsigned long long hash = 0;
    char m[3] = {};
    int used = 0;
    if (std::sscanf(name.c_str(), "tex1_%ux%u_%2[m_]%n", &w, &h, m, &used) == 3 &&
        std::strcmp(m, "m_") == 0) {
        if (std::sscanf(name.c_str() + used, "%16llx_%u.", &hash, &fmt) != 2)
            return false;
    } else if (std::sscanf(name.c_str(), "tex1_%ux%u_%16llx_%u.", &w, &h, &hash, &fmt) != 4) {
        return false;
    }
    if (name.find("_mip") != std::string::npos)
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
        const std::string& path = g.files.at(key);
        bool ok = is_dds(path) ? load_dds(path, &img)
                               : load_png_rgba(path, &img.rgba, &img.width, &img.height);
        // A GPU without BC textures (none this runs on, so far) keeps the original.
        if (ok && img.format != DOL_AURORA_PACK_RGBA8 && !dol_aurora_bc_textures_supported())
            ok = false;
        static const bool mips = [] {
            const char* env = std::getenv("ED_TEXPACK_MIPS");
            return env == nullptr || std::strcmp(env, "0") != 0;
        }();
        if (ok && mips && img.format == DOL_AURORA_PACK_RGBA8)
            build_mips(img);
        Ready ready;
        if (ok) {
            ready.rgba = static_cast<uint8_t*>(std::malloc(img.rgba.size()));
            if (ready.rgba != nullptr)
                std::memcpy(ready.rgba, img.rgba.data(), img.rgba.size());
            ready.width = img.width;
            ready.height = img.height;
            ready.mips = img.mips;
            ready.format = img.format;
        }
        std::lock_guard<std::mutex> lock(g.mutex);
        g.queued.erase(key);
        if (ready.rgba != nullptr)
            g.ready[key] = ready;
        else
            g.failed.insert(key);
        if (g_log)
            std::fprintf(stderr, "[texpack] %s %s\n", ok ? "loaded" : "FAILED",
                         g.files.at(key).c_str());
    }
}

int lookup(uint32_t format, uint32_t width, uint32_t height, uint64_t hash, uint8_t** out_rgba,
           uint32_t* out_width, uint32_t* out_height, uint32_t* out_mips, uint32_t* out_format) {
    const Key key{format, width, height, hash};
    if (g.files.find(key) == g.files.end())
        return 0;
    std::lock_guard<std::mutex> lock(g.mutex);
    if (auto it = g.ready.find(key); it != g.ready.end()) {
        const Ready& img = it->second;
        *out_rgba = img.rgba;  // the renderer frees it
        *out_width = img.width;
        *out_height = img.height;
        *out_mips = img.mips;
        *out_format = img.format;
        g.ready.erase(it);
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
        if (!parse_name(name, &key)) {
            skipped++;
            continue;
        }
        // The same texture twice (a supplement beside its pack): the one in a
        // supplement or override folder, else the path that sorts last,
        // whatever order the folder is listed in.
        std::string path = it->path().string();
        auto [found, added] = g.files.emplace(key, path);
        if (!added) {
            const int mine = overrides(path, g_dir), theirs = overrides(found->second, g_dir);
            if (mine > theirs || (mine == theirs && path > found->second))
                found->second = std::move(path);
        }
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

bool texpack_read_rgba(uint32_t format, uint32_t width, uint32_t height, uint64_t hash,
                       std::vector<uint8_t>* rgba, uint32_t* out_width, uint32_t* out_height) {
    if (generation() == 0)
        return false;
    const auto it = g.files.find(Key{format, width, height, hash});
    if (it == g.files.end())
        return false;
    Loaded img;
    const std::string& path = it->second;
    if (!(is_dds(path) ? load_dds(path, &img)
                       : load_png_rgba(path, &img.rgba, &img.width, &img.height)))
        return false;
    const size_t bytes = static_cast<size_t>(img.width) * img.height * 4;
    if (img.format == DOL_AURORA_PACK_RGBA8) {
        if (img.rgba.size() < bytes)
            return false;
        rgba->assign(img.rgba.begin(), img.rgba.begin() + bytes);
    } else {
        // Whole 4x4 blocks (load_dds checked), each decoded into place.
        rgba->assign(bytes, 0);
        const uint32_t block = img.format == DOL_AURORA_PACK_BC1 ? 8u : 16u;
        const uint8_t* src = img.rgba.data();
        const int pitch = static_cast<int>(img.width * 4);
        for (uint32_t by = 0; by < img.height; by += 4)
            for (uint32_t bx = 0; bx < img.width; bx += 4, src += block) {
                uint8_t* dst = rgba->data() + (static_cast<size_t>(by) * img.width + bx) * 4;
                switch (img.format) {
                case DOL_AURORA_PACK_BC1: bcdec_bc1(src, dst, pitch); break;
                case DOL_AURORA_PACK_BC2: bcdec_bc2(src, dst, pitch); break;
                case DOL_AURORA_PACK_BC3: bcdec_bc3(src, dst, pitch); break;
                default: bcdec_bc7(src, dst, pitch); break;
                }
            }
    }
    *out_width = img.width;
    *out_height = img.height;
    return true;
}

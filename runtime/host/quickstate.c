// Quick save / quick load. See quickstate.h.
//
// File layout (host byte order: a state belongs to this machine):
//   QsFileHeader, then sections, each a QsSectionHeader and its payload:
//     "CPU "                       CPUState's register prefix (the part
//                                  gxruntime/savestate.c also captures)
//     "MEM1" "ARAM" "LCKC" "VMEM"  guest memory, paged: u32 size, u32 page
//                                  size, then {u32 index, page bytes} for each
//                                  page that is not all zero, then u32 ~0
//     "INTR" "MMIO" "AUDI" ...     device blobs, packed by the owning module
//     "GXFI"                       windowed build: GX FIFO commands that
//                                  rebuild the GPU registers (variable size)
// A load reads and checks the whole file before it changes anything, so a
// damaged or mismatched file leaves the running game alone.
#include "host/quickstate.h"

#include "gxruntime/aram.h"
#ifdef ED_AURORA
#include "gxruntime/aurora_backend.h"
#endif
#include "gxruntime/savestate.h"
#include "host/audio.h"
#include "host/dtk.h"
#include "host/hle.h"
#include "host/interrupt.h"
#include "host/mmio.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define QS_MAGIC "EDQSTATE"
#define QS_VERSION 1u
#define QS_DEFAULT_PATH "build/runs/quick.state"
#define QS_PAGE_SIZE 4096u
#define QS_PAGE_END 0xFFFFFFFFu
#define QS_MAX_SECTIONS 32u
#define QS_MAX_REGIONS 4u
#define QS_PATH_MAX 1024u
// SDL scancodes are the USB HID keyboard usages.
#define QS_KEY_LOAD 30  // '1'
#define QS_KEY_SAVE 31  // '2'
// Blocks to wait for the GX FIFO to reach a command boundary before going
// ahead anyway; the longest draw is written in a small fraction of this.
#define QS_IDLE_PATIENCE 50000000u

typedef struct {
    char magic[8];
    u32 version;
    u32 section_count;
    char game_id[8];
} QsFileHeader;

typedef struct {
    char tag[4];
    u32 reserved;
    u64 size;
} QsSectionHeader;

typedef struct {
    char tag[4];
    const u8* data;
    u64 size;
} QsSection;

typedef struct {
    const char* tag;  // four characters, as in the file
    u8* data;
    u32 size;
} QsRegion;

// A module's state: a fixed-size blob it packs and unpacks itself, so its
// internals stay private. Optional blobs exist in one build only; load(NULL)
// tells the module the file has none.
typedef struct {
    const char* tag;  // four characters, as in the file
    size_t (*size)(void);
    void (*save)(void* out);
    void (*load)(const void* in);
    bool required;
} QsBlob;

static const QsBlob k_blobs[] = {
    {"INTR", interrupt_state_size, interrupt_state_save, interrupt_state_load, true},
    {"MMIO", mmio_state_size, mmio_state_save, mmio_state_load, true},
    {"AUDI", audio_state_size, audio_state_save, audio_state_load, true},
    {"DTK ", dtk_state_size, dtk_state_save, dtk_state_load, true},
    {"HLE ", hle_state_size, hle_state_save, hle_state_load, true},
    {"HLGX", hle_gx_state_size, hle_gx_state_save, hle_gx_state_load, false},
#ifdef ED_AURORA
    {"AURA", dol_aurora_state_size, dol_aurora_state_save, dol_aurora_state_load,
     false},
#endif
};
#define QS_BLOB_COUNT (sizeof k_blobs / sizeof k_blobs[0])

typedef enum { QS_SAVE, QS_LOAD } QsKind;
typedef struct {
    QsKind kind;
    char path[QS_PATH_MAX];
} QsRequest;

// Requests run in the order made: "save, then load" must not load first.
#define QS_QUEUE 4u
static QsRequest g_queue[QS_QUEUE];
static unsigned g_queue_count;
int g_quickstate_pending;

static char g_path[QS_PATH_MAX];
static char g_test_load_path[QS_PATH_MAX];
static bool g_save_at_armed;
static unsigned long long g_save_at;
static bool g_load_at_armed;
static unsigned long long g_load_at;
#ifdef ED_AURORA
static bool g_save_key_down;
// kill -USR1 <pid>: quick save without the keyboard (for handing the game
// over: synthetic key presses miss when the window is not the key window).
static volatile sig_atomic_t g_save_signal;
static void on_save_signal(int sig) {
    (void)sig;
    g_save_signal = 1;
}
static bool g_load_key_down;
#endif
static unsigned long long g_blocks;  // as of the last poll, for messages
static u32 g_idle_wait;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

static void request(QsKind kind, const char* path) {
    if (g_queue_count == QS_QUEUE)
        return;
    g_queue[g_queue_count].kind = kind;
    snprintf(g_queue[g_queue_count].path, QS_PATH_MAX, "%s", path);
    g_queue_count++;
    g_quickstate_pending = 1;
}

static void make_parent_dirs(const char* path);

void quickstate_request_save(const char* path) {
    make_parent_dirs(path);
    request(QS_SAVE, path);
}

void quickstate_request_load(const char* path) {
    request(QS_LOAD, path);
}

void quickstate_init(void) {
    // Leave SIGUSR1 to the --snapshot-out harness when it claimed it.
    void (*previous)(int) = signal(SIGUSR1, on_save_signal);
    if (previous != SIG_DFL && previous != SIG_ERR)
        signal(SIGUSR1, previous);
    const char* path = getenv("ED_QUICKSTATE");
    snprintf(g_path, sizeof g_path, "%s",
             path != NULL && path[0] != '\0' ? path : QS_DEFAULT_PATH);
    g_queue_count = 0;
    g_quickstate_pending = 0;
    const char* save_at = getenv("ED_QUICKSAVE_AT_BLOCK");
    g_save_at_armed = save_at != NULL && save_at[0] != '\0';
    g_save_at = g_save_at_armed ? strtoull(save_at, NULL, 0) : 0u;
    const char* load = getenv("ED_QUICKLOAD");
    const char* load_at = getenv("ED_QUICKLOAD_AT_BLOCK");
    g_load_at_armed = false;
    if (load != NULL && load[0] != '\0') {
        snprintf(g_test_load_path, sizeof g_test_load_path, "%s", load);
        if (load_at != NULL && load_at[0] != '\0') {
            g_load_at_armed = true;
            g_load_at = strtoull(load_at, NULL, 0);
        } else {
            request(QS_LOAD, g_test_load_path);  // at boot: block 0
        }
    }
}

void quickstate_poll(unsigned long long blocks) {
    g_blocks = blocks;
#ifdef ED_AURORA
    // Act once per press, not for as long as the key is held.
    const bool save_key = dol_aurora_key_down(QS_KEY_SAVE);
    const bool load_key = dol_aurora_key_down(QS_KEY_LOAD);
    if (save_key && !g_save_key_down)
        request(QS_SAVE, g_path);
    if (load_key && !g_load_key_down)
        request(QS_LOAD, g_path);
    g_save_key_down = save_key;
    g_load_key_down = load_key;
#endif
    if (g_save_signal) {
        g_save_signal = 0;
        request(QS_SAVE, g_path);
    }
    if (g_save_at_armed && blocks >= g_save_at) {
        g_save_at_armed = false;
        request(QS_SAVE, g_path);
    }
    if (g_load_at_armed && blocks >= g_load_at) {
        g_load_at_armed = false;
        request(QS_LOAD, g_test_load_path);
    }
}

// ---------------------------------------------------------------------------
// Memory regions
// ---------------------------------------------------------------------------
static unsigned collect_regions(CPUState* cpu, QsRegion out[QS_MAX_REGIONS]) {
    u32 size = 0;
    unsigned n = 0;
    out[n++] = (QsRegion){"MEM1", cpu->ram, cpu->ram != NULL ? cpu->ram_size : 0u};
    out[n++] = (QsRegion){"ARAM", aram_memory(),
                          aram_memory() != NULL ? ARAM_SIZE : 0u};
    u8* locked_cache = mmio_locked_cache(&size);
    out[n++] = (QsRegion){"LCKC", locked_cache, size};
    u8* vm = mmio_vm_memory(&size);
    out[n++] = (QsRegion){"VMEM", vm, size};
    return n;
}

static u32 page_count(u32 size) {
    return (size + QS_PAGE_SIZE - 1u) / QS_PAGE_SIZE;
}

static u32 page_bytes(u32 size, u32 index) {
    const u32 offset = index * QS_PAGE_SIZE;
    return size - offset < QS_PAGE_SIZE ? size - offset : QS_PAGE_SIZE;
}

static bool page_is_zero(const u8* data, u32 bytes) {
    static const u8 zero[QS_PAGE_SIZE];
    return memcmp(data, zero, bytes) == 0;
}

// Most of ARAM and the VM window is never touched; skipping zero pages keeps
// a state a fraction of the 72 MB of guest memory.
static u64 region_payload_size(const QsRegion* r) {
    u64 size = 8u + 4u;  // size, page size ... end marker
    for (u32 i = 0; i < page_count(r->size); i++) {
        const u32 bytes = page_bytes(r->size, i);
        if (!page_is_zero(r->data + (size_t)i * QS_PAGE_SIZE, bytes))
            size += 4u + bytes;
    }
    return size;
}

// Checks a region section against this process's buffer. Entries must be in
// ascending page order and fill the section exactly.
static bool check_region(const QsSection* s, const QsRegion* r,
                         const char** why) {
    if (s->size < 12u) {
        *why = "memory section truncated";
        return false;
    }
    u32 size = 0, page = 0;
    memcpy(&size, s->data, 4);
    memcpy(&page, s->data + 4, 4);
    if (size != r->size) {
        *why = "memory size differs from this build";
        return false;
    }
    if (page != QS_PAGE_SIZE) {
        *why = "unknown memory page size";
        return false;
    }
    u64 at = 8u;
    s64 last = -1;
    for (;;) {
        u32 index = 0;
        if (at + 4u > s->size) {
            *why = "memory section truncated";
            return false;
        }
        memcpy(&index, s->data + at, 4);
        at += 4u;
        if (index == QS_PAGE_END)
            break;
        if (index >= page_count(size) || (s64)index <= last) {
            *why = "memory section damaged";
            return false;
        }
        last = index;
        at += page_bytes(size, index);
    }
    if (at != s->size) {
        *why = "memory section damaged";
        return false;
    }
    return true;
}

static void apply_region(const QsSection* s, const QsRegion* r) {
    if (r->data == NULL)
        return;
    memset(r->data, 0, r->size);
    u64 at = 8u;
    for (;;) {
        u32 index = 0;
        memcpy(&index, s->data + at, 4);
        at += 4u;
        if (index == QS_PAGE_END)
            break;
        const u32 bytes = page_bytes(r->size, index);
        memcpy(r->data + (size_t)index * QS_PAGE_SIZE, s->data + at, bytes);
        at += bytes;
    }
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------
typedef struct {
    FILE* f;
    bool ok;
    u64 bytes;
} QsWriter;

static void put(QsWriter* w, const void* data, size_t n) {
    if (w->ok && n != 0u && fwrite(data, 1u, n, w->f) != n)
        w->ok = false;
    w->bytes += n;
}

static void put_u32(QsWriter* w, u32 value) {
    put(w, &value, sizeof value);
}

static void put_section(QsWriter* w, const char tag[4], u64 size) {
    QsSectionHeader h;
    memset(&h, 0, sizeof h);
    memcpy(h.tag, tag, 4);
    h.size = size;
    put(w, &h, sizeof h);
}

static void put_region(QsWriter* w, const QsRegion* r) {
    put_section(w, r->tag, region_payload_size(r));
    put_u32(w, r->size);
    put_u32(w, QS_PAGE_SIZE);
    for (u32 i = 0; i < page_count(r->size); i++) {
        const u8* page = r->data + (size_t)i * QS_PAGE_SIZE;
        const u32 bytes = page_bytes(r->size, i);
        if (page_is_zero(page, bytes))
            continue;
        put_u32(w, i);
        put(w, page, bytes);
    }
    put_u32(w, QS_PAGE_END);
}

// mkdir -p for the state's directory (build/runs may not exist yet).
static void make_parent_dirs(const char* path) {
    char dir[QS_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    for (char* p = dir + 1; *p != '\0'; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        mkdir(dir, 0755);
        *p = '/';
    }
}

static bool save_state(CPUState* cpu, const char* path) {
    const double start = now_ms();
    make_parent_dirs(path);
    // Write beside the target and rename, so a failed save keeps the old one.
    char tmp[QS_PATH_MAX + 8u];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (f == NULL) {
        fprintf(stderr, "[quick] save FAILED: cannot write %s\n", tmp);
        return false;
    }
    QsWriter w = {f, true, 0u};

    QsRegion regions[QS_MAX_REGIONS];
    const unsigned region_count = collect_regions(cpu, regions);
    u32 sections = 1u + region_count;
    for (size_t i = 0; i < QS_BLOB_COUNT; i++)
        if (k_blobs[i].size() != 0u)
            sections++;
    // The GPU's registers live in the GX decoder, not in guest memory: the
    // game sets much of them once, so a state without them draws wrongly.
    void* gx = NULL;
    size_t gx_size = 0;
#ifdef ED_AURORA
    gx_size = dol_aurora_gx_state_save(NULL, 0);
    if (gx_size != 0u && (gx = malloc(gx_size)) != NULL &&
        dol_aurora_gx_state_save(gx, gx_size) == gx_size)
        sections++;
    else
        gx_size = 0;
#endif

    QsFileHeader header;
    memset(&header, 0, sizeof header);
    memcpy(header.magic, QS_MAGIC, sizeof header.magic);
    header.version = QS_VERSION;
    header.section_count = sections;
    strncpy(header.game_id, ED_GAME_ID, sizeof header.game_id);
    put(&w, &header, sizeof header);

    const size_t pod = dol_savestate_cpu_pod_size();
    put_section(&w, "CPU ", pod);
    put(&w, cpu, pod);
    for (unsigned i = 0; i < region_count; i++)
        put_region(&w, &regions[i]);
    for (size_t i = 0; i < QS_BLOB_COUNT; i++) {
        const size_t size = k_blobs[i].size();
        if (size == 0u)
            continue;
        void* blob = calloc(1u, size);
        if (blob == NULL) {
            w.ok = false;
            break;
        }
        k_blobs[i].save(blob);
        put_section(&w, k_blobs[i].tag, size);
        put(&w, blob, size);
        free(blob);
    }
    if (gx_size != 0u) {
        put_section(&w, "GXFI", gx_size);
        put(&w, gx, gx_size);
    }
    free(gx);

    if (fclose(f) != 0)
        w.ok = false;
    if (!w.ok || rename(tmp, path) != 0) {
        remove(tmp);
        fprintf(stderr, "[quick] save FAILED: error writing %s\n", path);
        return false;
    }
    fprintf(stderr, "[quick] saved %s (block %llu, pc=0x%08X, %.1f MB, %.0f ms)\n",
            path, g_blocks, cpu->pc, (double)w.bytes / (1024.0 * 1024.0),
            now_ms() - start);
    return true;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------
static u8* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    u8* data = NULL;
    long length = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        length = ftell(f);
    if (length > 0 && fseek(f, 0, SEEK_SET) == 0) {
        data = (u8*)malloc((size_t)length);
        if (data != NULL && fread(data, 1u, (size_t)length, f) != (size_t)length) {
            free(data);
            data = NULL;
        }
    }
    fclose(f);
    *size = data != NULL ? (size_t)length : 0u;
    return data;
}

static const QsSection* find_section(const QsSection* sections, unsigned count,
                                     const char tag[4]) {
    for (unsigned i = 0; i < count; i++)
        if (memcmp(sections[i].tag, tag, 4) == 0)
            return &sections[i];
    return NULL;
}

static bool parse_file(const u8* file, size_t size, QsSection* sections,
                       unsigned* count, const char** why) {
    QsFileHeader header;
    if (size < sizeof header) {
        *why = "not a quick state";
        return false;
    }
    memcpy(&header, file, sizeof header);
    if (memcmp(header.magic, QS_MAGIC, sizeof header.magic) != 0) {
        *why = "not a quick state";
        return false;
    }
    if (header.version != QS_VERSION) {
        *why = "made by a different version of the quick state format";
        return false;
    }
    char game_id[sizeof header.game_id + 1u];
    memcpy(game_id, header.game_id, sizeof header.game_id);
    game_id[sizeof header.game_id] = '\0';
    if (strcmp(game_id, ED_GAME_ID) != 0) {
        static char message[96];
        snprintf(message, sizeof message, "made by %s, this build is %s",
                 game_id, ED_GAME_ID);
        *why = message;
        return false;
    }
    if (header.section_count > QS_MAX_SECTIONS) {
        *why = "file damaged";
        return false;
    }
    size_t at = sizeof header;
    *count = 0;
    for (u32 i = 0; i < header.section_count; i++) {
        QsSectionHeader h;
        if (size - at < sizeof h) {
            *why = "file truncated";
            return false;
        }
        memcpy(&h, file + at, sizeof h);
        at += sizeof h;
        if (h.size > size - at) {
            *why = "file truncated";
            return false;
        }
        if (find_section(sections, *count, h.tag) != NULL) {
            *why = "file damaged";
            return false;
        }
        memcpy(sections[*count].tag, h.tag, 4);
        sections[*count].data = file + at;
        sections[*count].size = h.size;
        (*count)++;
        at += (size_t)h.size;
    }
    if (at != size) {
        *why = "file damaged";
        return false;
    }
    return true;
}

static bool load_state(CPUState* cpu, const char* path) {
    const double start = now_ms();
    size_t size = 0;
    u8* file = read_file(path, &size);
    if (file == NULL) {
        fprintf(stderr, "[quick] load: no state at %s\n", path);
        return false;
    }
    QsSection sections[QS_MAX_SECTIONS];
    unsigned count = 0;
    const char* why = NULL;
    QsRegion regions[QS_MAX_REGIONS];
    const unsigned region_count = collect_regions(cpu, regions);
    const QsSection* cpu_section = NULL;
    bool ok = parse_file(file, size, sections, &count, &why);

    // Check everything before touching the machine.
    if (ok) {
        cpu_section = find_section(sections, count, "CPU ");
        if (cpu_section == NULL ||
            cpu_section->size != dol_savestate_cpu_pod_size()) {
            why = "CPU state missing or from a different build";
            ok = false;
        }
    }
    for (unsigned i = 0; ok && i < region_count; i++) {
        const QsSection* s = find_section(sections, count, regions[i].tag);
        if (s == NULL) {
            why = "memory section missing";
            ok = false;
        } else {
            ok = check_region(s, &regions[i], &why);
        }
    }
    for (size_t i = 0; ok && i < QS_BLOB_COUNT; i++) {
        const QsSection* s = find_section(sections, count, k_blobs[i].tag);
        const size_t expected = k_blobs[i].size();
        if (s == NULL ? k_blobs[i].required
                      : s->size != expected && expected != 0u) {
            static char message[96];
            snprintf(message, sizeof message,
                     "%.4s state %s (the host changed since it was saved)",
                     k_blobs[i].tag, s == NULL ? "missing" : "has another size");
            why = message;
            ok = false;
        }
    }
    if (!ok) {
        fprintf(stderr, "[quick] load REFUSED: %s (%s); game left as it was\n",
                path, why);
        free(file);
        return false;
    }

    memcpy(cpu, cpu_section->data, dol_savestate_cpu_pod_size());
    for (unsigned i = 0; i < region_count; i++)
        apply_region(find_section(sections, count, regions[i].tag), &regions[i]);
    for (size_t i = 0; i < QS_BLOB_COUNT; i++) {
        const QsSection* s = find_section(sections, count, k_blobs[i].tag);
        if (k_blobs[i].size() == 0u)
            continue;  // nothing of this kind in this build
        k_blobs[i].load(s != NULL ? s->data : NULL);
    }
#ifdef ED_AURORA
    // A state without GPU registers (saved headless) keeps the decoder as is:
    // stale, but better than a decoder that knows no vertex formats at all.
    const QsSection* gx = find_section(sections, count, "GXFI");
    if (gx != NULL)
        dol_aurora_gx_state_load(gx->data, (size_t)gx->size);
    dol_aurora_state_loaded();
#endif
    free(file);
    fprintf(stderr, "[quick] loaded %s (block %llu, pc=0x%08X, %.0f ms)\n",
            path, g_blocks, cpu->pc, now_ms() - start);
    return true;
}

// ---------------------------------------------------------------------------
// Dispatch-loop hook
// ---------------------------------------------------------------------------
static bool at_safe_point(void) {
#ifdef ED_AURORA
    return dol_aurora_gx_idle();
#else
    return true;  // nothing decodes the GX FIFO headless
#endif
}

bool quickstate_service(CPUState* cpu) {
    if (g_queue_count == 0u) {
        g_quickstate_pending = 0;
        return false;
    }
    if (!at_safe_point()) {
        if (++g_idle_wait < QS_IDLE_PATIENCE)
            return false;
        fprintf(stderr,
                "[quick] GX FIFO stayed mid-command; going ahead anyway\n");
    }
    g_idle_wait = 0;
    const QsRequest next = g_queue[0];
    memmove(&g_queue[0], &g_queue[1], (g_queue_count - 1u) * sizeof g_queue[0]);
    g_queue_count--;
    g_quickstate_pending = g_queue_count != 0u;
    // One request per call: after a load the caller restarts the iteration,
    // and a queued save then sees the loaded machine.
    if (next.kind == QS_LOAD)
        return load_state(cpu, next.path);
    save_state(cpu, next.path);
    return false;
}

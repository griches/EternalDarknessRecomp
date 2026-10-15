#include "mmio.h"

#include "gxruntime/aram.h"
#include "gxruntime/cp.h"
#include "gxruntime/di.h"
#ifdef ED_AURORA
#include "gxruntime/aurora_backend.h"
#endif
#include "gxruntime/efb_access.h"
#include "gxruntime/exi.h"
#include "gxruntime/guest_memory.h"
#include "gxruntime/interrupts.h"
#include "gxruntime/mi.h"
#include "gxruntime/mmio_bus.h"
#include "gxruntime/pe.h"
#include "gxruntime/platform.h"
#include "gxruntime/si.h"
#include "host/audio.h"
#include "host/interrupt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static bool g_log = false;
static DolGuestMemory g_guest_memory;
static DolMmioBus g_mmio_bus;
static DolExi g_exi;
static DolDi g_di;
static DolCp g_cp;
static DolPe g_pe;
static DolMi g_mi;
static DolEfbAccess g_efb;

#define DSP_BASE 0xCC005000u
#define DSP_SIZE 0x40u
#define AI_BASE  0xCC006C00u
#define AI_SIZE  0x20u
#define WGPIPE_BASE 0xCC008000u
#define WGPIPE_SIZE 0x20u

void mmio_set_logging(bool enabled) {
    g_log = enabled;
}

static const char* region_name(u32 ea) {
    if (ea >= 0xCC006800u && ea < 0xCC006C00u)
        return "EXI";
    if (ea >= 0xCC006400u && ea < 0xCC006800u)
        return "SI";
    if (ea >= 0xCC006000u && ea < 0xCC006400u)
        return "DI";
    u32 base = ea & 0xFFFFF000u;
    switch (base) {
    case 0xCC002000u: return "VI";   // video interface
    case 0xCC003000u: return "PI";   // processor interface
    case 0xCC004000u: return "MI";   // memory interface
    case 0xCC005000u: return "DSP/AI";
    case 0xCC006C00u: return "AI";   // audio interface
    case 0xCC000000u: return "CP";   // command processor (GX)
    case 0xCC008000u: return "PE/GX FIFO";
    default:
        if (ea >= DOL_EFB_ACCESS_BASE &&
            ea < DOL_EFB_ACCESS_BASE + DOL_EFB_ACCESS_BYTES)
            return "EFB";
        return "MMIO";
    }
}

static bool guest_memory_read_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                                 u64* value) {
    (void)cpu;
    return dol_guest_memory_read((const DolGuestMemory*)user, ea, size, value);
}

static bool guest_memory_write_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                                  u64 value) {
    (void)cpu;
    return dol_guest_memory_write((DolGuestMemory*)user, ea, size, value);
}

static bool aram_read_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                         u64* value) {
    (void)user;
    (void)cpu;
    if (!aram_contains(ea))
        return false;
    if (value != NULL)
        *value = aram_read(ea, size);
    return true;
}

static bool aram_write_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                          u64 value) {
    (void)user;
    (void)cpu;
    if (!aram_contains(ea))
        return false;
    aram_write(ea, value, size);
    return true;
}

// The CPU FIFO (Processor Interface FIFO_BASE/END/WPTR at 0xCC00300C-0x17).
// The write-gather pipe stores into RAM at the write pointer, wrapping at the
// end. The GPU reads the FIFO the CP is pointed at; when the CPU FIFO is that
// one (linked, the normal case) writes are GPU commands and go straight to the
// renderer. Otherwise the game is filling a buffer: a display list
// (GXBeginDisplayList) or the HVQ movie decoder, which streams pixels through
// the gather pipe as a fast copy. GXEndDisplayList reads the size back from the
// write pointer.
#define PI_FIFO_ADDRESS_MASK 0x03FFFFE0u
#define PI_FIFO_WRAP_BIT 0x04000000u
static u32 g_pi_fifo_base;
static u32 g_pi_fifo_end;
static u32 g_pi_fifo_wptr;  // byte-exact; reads see whole 32-byte lines
static bool g_pi_fifo_wrapped;

static bool g_pi_fifo_configured;

// GXRedirectWriteGatherPipe sets base 0 / end 64 MiB and the write pointer to
// the target, so base 0 is a redirect, not "unset".
// The write-gather pipe collects 32 bytes and bursts whole lines to the FIFO
// write pointer. A partial line stays in the pipe across a FIFO change: the SDK
// relies on that when it ends a gather-pipe redirect by pushing 31 zero bytes,
// which must not land past the end of the redirected buffer.
static u8 g_wg_line[32];
static u32 g_wg_fill;

// ED_FIFO_PROFILE=1 (debug): bytes written into buffer FIFO targets (display
// lists), by the storing instruction, printed when a large fill ends.
#define FIFO_PROFILE_SLOTS 512u
static struct { u32 pc, lr; u64 bytes; } g_fifo_profile[FIFO_PROFILE_SLOTS];
static int g_fifo_profile_on = -1;

static void fifo_profile_add(const CPUState* cpu, u32 bytes) {
    if (g_fifo_profile_on < 0)
        g_fifo_profile_on = getenv("ED_FIFO_PROFILE") != NULL;
    if (!g_fifo_profile_on)
        return;
    u32 slot = (cpu->pc * 2654435761u) % FIFO_PROFILE_SLOTS;
    for (u32 n = 0; n < FIFO_PROFILE_SLOTS; n++, slot = (slot + 1u) % FIFO_PROFILE_SLOTS) {
        if (g_fifo_profile[slot].bytes == 0u) {
            g_fifo_profile[slot].pc = cpu->pc;
            g_fifo_profile[slot].lr = cpu->lr;
        }
        if (g_fifo_profile[slot].pc == cpu->pc) {
            g_fifo_profile[slot].bytes += bytes;
            return;
        }
    }
}

static void fifo_profile_dump(u32 base, u32 size) {
    if (g_fifo_profile_on <= 0)
        return;
    fprintf(stderr, "[fifo-profile] buffer 0x%08X size 0x%X:\n", base, size);
    for (int k = 0; k < 16; k++) {
        u32 best = FIFO_PROFILE_SLOTS;
        for (u32 i = 0; i < FIFO_PROFILE_SLOTS; i++)
            if (g_fifo_profile[i].bytes != 0u &&
                (best == FIFO_PROFILE_SLOTS ||
                 g_fifo_profile[i].bytes > g_fifo_profile[best].bytes))
                best = i;
        if (best == FIFO_PROFILE_SLOTS)
            break;
        fprintf(stderr, "  pc=0x%08X lr=0x%08X bytes=%llu\n", g_fifo_profile[best].pc,
                g_fifo_profile[best].lr,
                (unsigned long long)g_fifo_profile[best].bytes);
        g_fifo_profile[best].bytes = 0;
    }
    memset(g_fifo_profile, 0, sizeof g_fifo_profile);
}

static void wg_burst_to_ram(CPUState* cpu) {
    for (u32 i = 0; i < 32u; i++)
        mem_write8(cpu, GC_RAM_BASE | (g_pi_fifo_wptr + i), g_wg_line[i]);
    g_pi_fifo_wptr += 32u;
    if (g_pi_fifo_end != 0u && g_pi_fifo_wptr >= g_pi_fifo_end) {
        static int wrap_log = -1;  // ED_PI_LOG=1: a buffer target wrapped
        if (wrap_log < 0)
            wrap_log = getenv("ED_PI_LOG") != NULL;
        if (wrap_log)
            fprintf(stderr, "[pi] buffer FIFO wrapped: base=0x%08X end=0x%08X "
                            "pc=0x%08X lr=0x%08X\n",
                    g_pi_fifo_base, g_pi_fifo_end, cpu->pc, cpu->lr);
        g_pi_fifo_wptr = g_pi_fifo_base;
        g_pi_fifo_wrapped = true;
    }
    g_wg_fill = 0;
}

static bool cpu_fifo_feeds_gpu(void) {
    return !g_pi_fifo_configured || g_cp.fifo_base == 0u ||
           g_pi_fifo_base == (g_cp.fifo_base & PI_FIFO_ADDRESS_MASK);
}

// PPCMtwpar (GXResetWriteGatherPipe, GXRedirectWriteGatherPipe) sets the
// gather pipe's address, which also empties it: pending bytes are dropped. The
// game reads the display-list write pointer as GXFlush then a reset, so the
// flush pads to a 32-byte line and the zeros left in the pipe are discarded;
// keeping them grew every room display list by 32 bytes per lookup until it
// overflowed its buffer after the prologue.
void mmio_write_gather_reset(void) {
    g_wg_fill = 0;
}

bool mmio_cpu_fifo_feeds_gpu(void) {
    return cpu_fifo_feeds_gpu();
}

bool mmio_pi_fifo_contains(u32 ea) {
    return ea >= 0xCC00300Cu && ea < 0xCC003018u;
}

u64 mmio_pi_fifo_read(u32 ea) {
    switch (ea & ~3u) {
    case 0xCC00300Cu: return g_pi_fifo_base;
    case 0xCC003010u: return g_pi_fifo_end;
    default:
        return (g_pi_fifo_wptr & PI_FIFO_ADDRESS_MASK) |
               (g_pi_fifo_wrapped ? PI_FIFO_WRAP_BIT : 0u);
    }
}

void mmio_pi_fifo_write(u32 ea, u64 value) {
    const u32 v = (u32)value;
    g_pi_fifo_configured = true;
    static int pi_log = -1;  // ED_PI_LOG=1: log gather-pipe redirects
    if (pi_log < 0)
        pi_log = getenv("ED_PI_LOG") != NULL;
    if (pi_log < 0)
        pi_log = 0;
    if (pi_log && (ea & ~3u) == 0xCC003014u && g_pi_fifo_base == 0u)
        fprintf(stderr, "[pi] gather pipe redirected to 0x%08X\n", v);
    {
        // ED_PI_LOG=2: every FIFO register write, with the old pointers.
        static int all = -1;
        if (all < 0) {
            const char* env = getenv("ED_PI_LOG");
            all = env != NULL && atoi(env) >= 2;
        }
        if (all)
            fprintf(stderr,
                    "[pi] %s=0x%08X (base=0x%08X end=0x%08X wptr=0x%08X)\n",
                    (ea & ~3u) == 0xCC00300Cu   ? "base"
                    : (ea & ~3u) == 0xCC003010u ? "end"
                                                : "wptr",
                    v, g_pi_fifo_base, g_pi_fifo_end, g_pi_fifo_wptr);
    }
    mmio_fifo_dump_note(ea == 0xCC00300Cu   ? "pi-fifo-base"
                        : ea == 0xCC003010u ? "pi-fifo-end"
                                            : "pi-fifo-wptr",
                        v, g_cp.fifo_base);
    if ((ea & ~3u) == 0xCC00300Cu && pi_log && !cpu_fifo_feeds_gpu() &&
        g_pi_fifo_base != 0u && g_pi_fifo_wptr - g_pi_fifo_base >= 0x10000u)
        fprintf(stderr, "[pi] large buffer fill: base=0x%08X size=0x%X wrapped=%d\n",
                g_pi_fifo_base, g_pi_fifo_wptr - g_pi_fifo_base,
                g_pi_fifo_wrapped ? 1 : 0);
    if ((ea & ~3u) == 0xCC00300Cu && !cpu_fifo_feeds_gpu() && g_pi_fifo_base != 0u) {
        if (g_pi_fifo_wptr - g_pi_fifo_base >= 0x10000u || g_pi_fifo_wrapped)
            fifo_profile_dump(g_pi_fifo_base, g_pi_fifo_wptr - g_pi_fifo_base);
        else if (g_fifo_profile_on > 0)
            memset(g_fifo_profile, 0, sizeof g_fifo_profile);
    }
    switch (ea & ~3u) {
    case 0xCC00300Cu: g_pi_fifo_base = v & PI_FIFO_ADDRESS_MASK; break;
    case 0xCC003010u: g_pi_fifo_end = v & PI_FIFO_ADDRESS_MASK; break;
    default:
        g_pi_fifo_wptr = v & PI_FIFO_ADDRESS_MASK;
        g_pi_fifo_wrapped = (v & PI_FIFO_WRAP_BIT) != 0u;
        break;
    }
}

// ED_FIFO_DUMP=<path>: log the first 256 KiB of the GX stream, one write per
// line (offset within the frame, size, value, writing pc), with frame and
// display-list markers, to find where the GPU stream goes wrong.
static FILE* g_fifo_dump;
static int g_fifo_dump_state;
static u64 g_fifo_dump_total;
static u64 g_fifo_dump_frame_offset;

static unsigned long long g_fifo_dump_from;  // ED_FIFO_DUMP_FROM=frame
static unsigned long long g_fifo_dump_present;

static bool fifo_dump_open(void) {
    if (g_fifo_dump_present < g_fifo_dump_from)
        return false;
    if (g_fifo_dump_state == 0) {
        const char* path = getenv("ED_FIFO_DUMP");
        g_fifo_dump = path ? fopen(path, "w") : NULL;
        g_fifo_dump_state = 1;
    }
    if (g_fifo_dump != NULL && g_fifo_dump_total > 262144u) {
        fclose(g_fifo_dump);
        g_fifo_dump = NULL;
    }
    return g_fifo_dump != NULL;
}

void mmio_fifo_dump_write(const CPUState* cpu, u8 size, u64 value) {
    if (!fifo_dump_open())
        return;
    fprintf(g_fifo_dump, "%6llu %u %0*llX pc=%08X lr=%08X%s\n",
            (unsigned long long)g_fifo_dump_frame_offset, size, size * 2,
            (unsigned long long)value, cpu->pc, cpu->lr,
            !cpu_fifo_feeds_gpu() ? " (into RAM)" : "");
    if (cpu_fifo_feeds_gpu())
        g_fifo_dump_frame_offset += size;
    g_fifo_dump_total += size;
}

void mmio_fifo_dump_note(const char* what, u32 a, u32 b) {
    if (g_fifo_dump_present == 0 && getenv("ED_FIFO_DUMP_FROM") != NULL)
        g_fifo_dump_from = strtoull(getenv("ED_FIFO_DUMP_FROM"), NULL, 0);
    if (strcmp(what, "present") == 0)
        g_fifo_dump_present++;
    if (!fifo_dump_open())
        return;
    fprintf(g_fifo_dump, "---- %s 0x%08X %u\n", what, a, b);
    if (strcmp(what, "present") == 0)
        g_fifo_dump_frame_offset = 0;
    else if (strcmp(what, "call-display-list") == 0)
        g_fifo_dump_frame_offset += b;
}



#ifdef ED_AURORA
static bool gx_fifo_write_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                             u64 value) {
    (void)user;
    (void)ea;
    mmio_fifo_dump_write(cpu, size, value);
    if (!cpu_fifo_feeds_gpu()) {
        fifo_profile_add(cpu, size);
        for (u8 i = 0; i < size; i++) {
            g_wg_line[g_wg_fill++] = (u8)(value >> ((size - 1u - i) * 8u));
            if (g_wg_fill == 32u)
                wg_burst_to_ram(cpu);
        }
        return true;
    }
    // Back on the GPU FIFO: bytes left in the pipe from a RAM target (the 31
    // zeros GXRestoreWriteGatherPipe pushes) complete their line here, as NOPs.
    if (g_wg_fill != 0u) {
        for (u32 i = 0; i < g_wg_fill; i++)
            dol_platform_gx_write(g_wg_line[i], 1u);
        g_wg_fill = 0;
    }
    // ED_EXP_NOZ=1 (experiment): clear the z-compare enable in BP 0x40 writes.
    {
        static int exp_noz = -1;
        static u8 last_opcode;
        if (exp_noz < 0)
            exp_noz = getenv("ED_EXP_NOZ") != NULL;
        if (exp_noz && size == 4u && last_opcode == 0x61u &&
            ((value >> 24) & 0xFFu) == 0x40u)
            value &= ~1ull;
        static int exp_texc = -1;
        if (exp_texc < 0)
            exp_texc = getenv("ED_EXP_TEXC") != NULL;
        if (exp_texc && size == 4u && last_opcode == 0x61u &&
            (u32)value == 0xC008F82Eu)
            value = 0xC0088FFFu;  // stage 0 colour: TEXC only
        last_opcode = size == 1u ? (u8)value : 0u;
    }
    // The write-gather pipe occupies one 32-byte cache line. Paired-single
    // stores write their second element at base+4, and every write in the line
    // is appended to the same FIFO stream.
    dol_platform_gx_write(value, size);
    return true;
}

static bool graphics_guest_address_resolver_cb(
    void* user, u32 address, u32 size, DolGuestAddressSpace space,
    DolGuestResourceKind resource, const void** data, u32* available) {
    CPUState* cpu = (CPUState*)user;
    if (cpu == NULL || data == NULL || available == NULL)
        return false;
    DolGuestAddressResolver resolver;
    DolGuestResolvedRange range;
    dol_guest_address_resolver_init(&resolver, &g_guest_memory, cpu);
    if (!dol_guest_address_resolver_resolve(&resolver, address, size, space,
                                            resource, &range)) {
        *data = NULL;
        *available = 0;
        return false;
    }
    *data = range.data;
    *available = range.available;
    return true;
}
#endif

static bool audio_read_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                          u64* value) {
    (void)user;
    (void)cpu;
    if (!audio_mmio_contains(ea))
        return false;
    if (value != NULL)
        *value = audio_mmio_read(ea, size);
    return true;
}

static bool audio_write_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                           u64 value) {
    (void)user;
    (void)cpu;
    if (!audio_mmio_contains(ea))
        return false;
    audio_mmio_write(ea, size, value);
    return true;
}

static bool interrupt_read_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                              u64* value) {
    (void)user;
    (void)cpu;
    if (!interrupt_mmio_contains(ea))
        return false;
    if (value != NULL)
        *value = interrupt_mmio_read(ea, size);
    return true;
}

static bool interrupt_write_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                               u64 value) {
    (void)user;
    (void)cpu;
    if (!interrupt_mmio_contains(ea))
        return false;
    interrupt_mmio_write(ea, size, value);
    return true;
}

static bool exi_read_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                        u64* value) {
    (void)cpu;
    DolExi* exi = (DolExi*)user;
    if (!dol_exi_mmio_contains(ea))
        return false;
    if (value != NULL)
        *value = dol_exi_mmio_read(exi, ea, size);
    interrupt_set_exi_pending(dol_exi_interrupt_pending(exi));
    return true;
}

static bool exi_write_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                         u64 value) {
    DolExi* exi = (DolExi*)user;
    if (!dol_exi_mmio_contains(ea))
        return false;
    dol_exi_mmio_write(exi, cpu, ea, size, value);
    interrupt_set_exi_pending(dol_exi_interrupt_pending(exi));
    return true;
}

static bool di_read_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                       u64* value) {
    (void)cpu;
    DolDi* di = (DolDi*)user;
    if (!dol_di_mmio_contains(ea))
        return false;
    if (value != NULL)
        *value = dol_di_mmio_read(di, ea, size);
    interrupt_set_di_pending(dol_di_interrupt_pending(di));
    return true;
}

static bool di_write_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                        u64 value) {
    DolDi* di = (DolDi*)user;
    if (!dol_di_mmio_contains(ea))
        return false;
    dol_di_mmio_write(di, cpu, ea, size, value);
    interrupt_set_di_pending(dol_di_interrupt_pending(di));
    return true;
}

static bool cp_read_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                       u64* value) {
    (void)cpu;
    DolCp* cp = (DolCp*)user;
    if (!dol_cp_mmio_contains(ea))
        return false;
    if (value != NULL)
        *value = dol_cp_mmio_read(cp, ea, size);
    return true;
}

static bool cp_write_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                        u64 value) {
    (void)cpu;
    DolCp* cp = (DolCp*)user;
    if (!dol_cp_mmio_contains(ea))
        return false;
    dol_cp_mmio_write(cp, ea, size, value);
    interrupt_gp_breakpoint_enable(
        (dol_cp_mmio_read(cp, DOL_CP_BASE + DOL_CP_CTRL_OFF, 2) &
         DOL_CP_CTRL_BP_ENABLE) != 0u);
    return true;
}

static bool pe_read_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                       u64* value) {
    (void)cpu;
    return dol_pe_mmio_read((DolPe*)user, ea, size, value);
}

static bool pe_write_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                        u64 value) {
    (void)cpu;
    return dol_pe_mmio_write((DolPe*)user, ea, size, value);
}

static bool mi_read_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                       u64* value) {
    (void)cpu;
    return dol_mi_mmio_read((DolMi*)user, ea, size, value);
}

static bool mi_write_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                        u64 value) {
    (void)cpu;
    return dol_mi_mmio_write((DolMi*)user, ea, size, value);
}

static bool efb_read_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                        u64* value) {
    (void)cpu;
    DolEfbAccess* efb = (DolEfbAccess*)user;
    if (!dol_efb_access_mmio_contains(ea))
        return false;
    if (value != NULL)
        *value = dol_efb_access_mmio_read(efb, ea, size);
    return true;
}

static bool efb_write_cb(void* user, CPUState* cpu, u32 ea, u8 size,
                         u64 value) {
    (void)cpu;
    DolEfbAccess* efb = (DolEfbAccess*)user;
    if (!dol_efb_access_mmio_contains(ea))
        return false;
    dol_efb_access_mmio_write(efb, ea, size, value);
    return true;
}

static u64 mmio_read(CPUState* cpu, u32 ea, u8 size) {
    u64 value = 0;
    if (dol_mmio_bus_read(&g_mmio_bus, cpu, ea, size, &value))
        return value;
    if (g_log)
        fprintf(stderr, "[mmio] read%u  %-10s 0x%08X -> 0\n",
                (unsigned)(size * 8u), region_name(ea), ea);
    return 0;
}

static void mmio_write(CPUState* cpu, u32 ea, u64 value, u8 size) {
    if (dol_mmio_bus_write(&g_mmio_bus, cpu, ea, size, value))
        return;
    if (g_log)
        fprintf(stderr, "[mmio] write%u %-10s 0x%08X <- 0x%llX\n",
                (unsigned)(size * 8u), region_name(ea), ea,
                (unsigned long long)value);
}

static u32 mmio_read32(CPUState* cpu, u32 ea, u8 rid) {
    (void)cpu; (void)rid;
    if (g_log)
        fprintf(stderr, "[mmio] eciwx %-10s 0x%08X -> 0\n", region_name(ea), ea);
    return 0;
}

static void mmio_write32(CPUState* cpu, u32 ea, u32 value, u8 rid) {
    (void)cpu; (void)rid;
    if (g_log)
        fprintf(stderr, "[mmio] ecowx %-10s 0x%08X <- 0x%08X\n",
                region_name(ea), ea, value);
}

static void* guest_region_pointer(u8* data, u32 base, u32 bytes,
                                  u32 ea, u32 size) {
    if (data == NULL || size == 0u || size > bytes || ea < base ||
        ea - base > bytes - size)
        return NULL;
    return data + (ea - base);
}

static void* mmio_pointer(CPUState* cpu, u32 ea, u32 size) {
    (void)cpu;
    void* ptr = guest_region_pointer(g_guest_memory.vm,
                                     g_guest_memory.config.vm_base,
                                     g_guest_memory.config.vm_size, ea, size);
    if (ptr != NULL)
        return ptr;
    return guest_region_pointer(g_guest_memory.locked_cache,
                                g_guest_memory.config.locked_cache_base,
                                g_guest_memory.config.locked_cache_size, ea,
                                size);
}

bool mmio_install(CPUState* cpu) {
    aram_init();
    audio_init();
    interrupt_init();
    dol_exi_init(&g_exi);
    dol_di_init(&g_di);
    dol_cp_init(&g_cp);
    dol_pe_init(&g_pe);
    dol_mi_init(&g_mi);
    dol_efb_access_init(&g_efb);
    dol_pe_link_bbox(&g_pe, &g_efb);
    if (!dol_guest_memory_init(&g_guest_memory, NULL)) {
        fprintf(stderr, "[mmio] failed to allocate guest memory regions\n");
        return false;
    }
    dol_mmio_bus_init(&g_mmio_bus);
    bool ok = true;
    ok = ok && dol_mmio_bus_register(&g_mmio_bus, DOL_CP_BASE,
                                     DOL_CP_REGISTER_BYTES, cp_read_cb,
                                     cp_write_cb, &g_cp);
    // PE config regs (0x00-0x08, token/bbox). PE_CTRL 0x0A is claimed by
    // interrupts (DOL_PE_INTERRUPT_STATUS); pe_* returns false there so the
    // interrupt region wins on the second pass.
    ok = ok && dol_mmio_bus_register(&g_mmio_bus, DOL_PE_BASE,
                                     DOL_PE_REGISTER_BYTES, pe_read_cb,
                                     pe_write_cb, &g_pe);
    ok = ok && dol_mmio_bus_register(&g_mmio_bus, DOL_MI_BASE,
                                     DOL_MI_REGISTER_BYTES, mi_read_cb,
                                     mi_write_cb, &g_mi);
    ok = ok && dol_mmio_bus_register(&g_mmio_bus, DOL_EFB_ACCESS_BASE,
                                     DOL_EFB_ACCESS_BYTES, efb_read_cb,
                                     efb_write_cb, &g_efb);
    ok = ok && dol_mmio_bus_register(
                   &g_mmio_bus, g_guest_memory.config.vm_base,
                   g_guest_memory.config.vm_size, guest_memory_read_cb,
                   guest_memory_write_cb, &g_guest_memory);
    ok = ok && dol_mmio_bus_register(
                   &g_mmio_bus, g_guest_memory.config.locked_cache_base,
                   g_guest_memory.config.locked_cache_size,
                   guest_memory_read_cb, guest_memory_write_cb,
                   &g_guest_memory);
    ok = ok && dol_mmio_bus_register(&g_mmio_bus, ARAM_BASE, ARAM_SIZE,
                                     aram_read_cb, aram_write_cb, NULL);
#ifdef ED_AURORA
    ok = ok && dol_mmio_bus_register(&g_mmio_bus, WGPIPE_BASE, WGPIPE_SIZE,
                                     NULL, gx_fifo_write_cb, NULL);
#endif
    ok = ok && dol_mmio_bus_register(&g_mmio_bus, DSP_BASE, DSP_SIZE,
                                     audio_read_cb, audio_write_cb, NULL);
    ok = ok && dol_mmio_bus_register(&g_mmio_bus, AI_BASE, AI_SIZE,
                                     audio_read_cb, audio_write_cb, NULL);
    ok = ok && dol_mmio_bus_register(&g_mmio_bus, DOL_PE_INTERRUPT_STATUS, 2u,
                                     interrupt_read_cb, interrupt_write_cb,
                                     NULL);
    ok = ok && dol_mmio_bus_register(&g_mmio_bus, DOL_VI_BASE,
                                     DOL_VI_REGISTER_BYTES,
                                     interrupt_read_cb, interrupt_write_cb,
                                     NULL);
    ok = ok && dol_mmio_bus_register(&g_mmio_bus, DOL_PI_BASE, 0x40u,
                                     interrupt_read_cb, interrupt_write_cb,
                                     NULL);
    ok = ok && dol_mmio_bus_register(&g_mmio_bus, DOL_SI_BASE,
                                     DOL_SI_REGISTER_BYTES,
                                     interrupt_read_cb, interrupt_write_cb,
                                     NULL);
    ok = ok && dol_mmio_bus_register(&g_mmio_bus, DOL_EXI_BASE,
                                     DOL_EXI_REGISTER_BYTES,
                                     exi_read_cb, exi_write_cb, &g_exi);
    ok = ok && dol_mmio_bus_register(&g_mmio_bus, DOL_DI_BASE,
                                     DOL_DI_REGISTER_BYTES,
                                     di_read_cb, di_write_cb, &g_di);
    if (!ok) {
        fprintf(stderr, "[mmio] failed to register runtime MMIO regions\n");
        dol_guest_memory_shutdown(&g_guest_memory);
        return false;
    }
    cpu->external_read   = mmio_read;
    cpu->external_write  = mmio_write;
    cpu->external_read32 = mmio_read32;
    cpu->external_write32 = mmio_write32;
#ifdef ED_AURORA
    dol_platform_set_guest_address_resolver(
        graphics_guest_address_resolver_cb, cpu);
#endif
    return true;
}

void mmio_set_pe_token(u16 token) {
    g_pe.words[DOL_PE_TOKEN_OFF / 2u] = token;
}

void mmio_set_disc_present(bool present) {
    dol_di_set_disc_present(&g_di, present);
    interrupt_set_di_pending(dol_di_interrupt_pending(&g_di));
}

void mmio_shutdown(void) {
#ifdef ED_AURORA
    dol_platform_set_guest_address_resolver(NULL, NULL);
#endif
    dol_guest_memory_shutdown(&g_guest_memory);
    aram_free();
}

void* mmio_guest_pointer(CPUState* cpu, u32 address, u32* available) {
    return dol_guest_memory_pointer(&g_guest_memory, cpu, address, available);
}

void mmio_guest_copy(CPUState* cpu, u32 dest, u32 src, u32 bytes) {
    dol_guest_memory_copy(&g_guest_memory, cpu, dest, src, bytes);
}

DolEfbAccess* mmio_efb(void) {
    return &g_efb;
}

#ifdef ED_AURORA
static void efb_frame_fill_cb(const uint8_t* rgba, uint32_t width,
                              uint32_t height, void* user) {
    (void)user;
    if (rgba == NULL || width == 0u || height == 0u)
        return;
    // ED_EFB_DUMP=<dir>: every 30th frame's EFB colour as a PPM (debugging).
    static const char* dump_dir;
    static int dump_checked;
    static unsigned frame;
    if (!dump_checked) {
        dump_dir = getenv("ED_EFB_DUMP");
        dump_checked = 1;
    }
    if (dump_dir != NULL && (++frame % (getenv("ED_EFB_DUMP_EVERY") ? (unsigned)atoi(getenv("ED_EFB_DUMP_EVERY")) : 30u)) == 0u) {
        char path[512];
        snprintf(path, sizeof path, "%s/efb_%05u.ppm", dump_dir, frame);
        FILE* f = fopen(path, "wb");
        if (f != NULL) {
            fprintf(f, "P6\n%u %u\n255\n", width, height);
            for (uint32_t i = 0; i < width * height; i++)
                fwrite(rgba + i * 4u, 1, 3, f);
            fclose(f);
        }
    }
    u16 w = width > 0xFFFFu ? 0xFFFFu : (u16)width;
    u16 h = height > 0xFFFFu ? 0xFFFFu : (u16)height;
    (void)dol_efb_access_fill_color_rgba8(&g_efb, rgba, w, h);
}

void mmio_attach_efb_readback(void) {
    dol_aurora_set_efb_frame_fill(efb_frame_fill_cb, NULL);
}
#else
void mmio_attach_efb_readback(void) {}
#endif

// ---------------------------------------------------------------------------
// Quick state (host/quickstate.c)
// ---------------------------------------------------------------------------
// Device registers only. Host pointers in the device structs (transfer and
// command callbacks, the PE's bbox link, the EFB software backing) belong to
// this process: they are left out of the blob and kept on load.
typedef struct {
    DolExiChannel exi[DOL_EXI_CHANNELS];
    DolDi di;
    DolCp cp;
    u16 pe_words[DOL_PE_REGISTER_BYTES / 2u];
    DolMi mi;
    DolEfbAccess efb;
    u32 pi_fifo_base;
    u32 pi_fifo_end;
    u32 pi_fifo_wptr;
    bool pi_fifo_wrapped;
    bool pi_fifo_configured;
    u8 wg_line[32];
    u32 wg_fill;
} MmioQuickState;

size_t mmio_state_size(void) {
    return sizeof(MmioQuickState);
}

void mmio_state_save(void* out) {
    MmioQuickState s;
    memset(&s, 0, sizeof s);
    memcpy(s.exi, g_exi.channels, sizeof s.exi);
    memcpy(&s.di, &g_di, sizeof s.di);
    s.di.execute = NULL;
    s.di.execute_user = NULL;
    memcpy(&s.cp, &g_cp, sizeof s.cp);
    memcpy(s.pe_words, g_pe.words, sizeof s.pe_words);
    memcpy(&s.mi, &g_mi, sizeof s.mi);
    memcpy(&s.efb, &g_efb, sizeof s.efb);
    s.efb.color = NULL;
    s.efb.depth = NULL;
    s.efb.width = 0;
    s.efb.height = 0;
    s.efb.backing_owned = false;
    s.pi_fifo_base = g_pi_fifo_base;
    s.pi_fifo_end = g_pi_fifo_end;
    s.pi_fifo_wptr = g_pi_fifo_wptr;
    s.pi_fifo_wrapped = g_pi_fifo_wrapped;
    s.pi_fifo_configured = g_pi_fifo_configured;
    memcpy(s.wg_line, g_wg_line, sizeof s.wg_line);
    s.wg_fill = g_wg_fill;
    memcpy(out, &s, sizeof s);
}

void mmio_state_load(const void* in) {
    MmioQuickState s;
    memcpy(&s, in, sizeof s);
    memcpy(g_exi.channels, s.exi, sizeof s.exi);
    const DolDi live_di = g_di;
    g_di = s.di;
    g_di.execute = live_di.execute;
    g_di.execute_user = live_di.execute_user;
    g_cp = s.cp;
    memcpy(g_pe.words, s.pe_words, sizeof s.pe_words);
    g_mi = s.mi;
    const DolEfbAccess live_efb = g_efb;
    g_efb = s.efb;
    g_efb.color = live_efb.color;
    g_efb.depth = live_efb.depth;
    g_efb.width = live_efb.width;
    g_efb.height = live_efb.height;
    g_efb.backing_owned = live_efb.backing_owned;
    g_pi_fifo_base = s.pi_fifo_base;
    g_pi_fifo_end = s.pi_fifo_end;
    g_pi_fifo_wptr = s.pi_fifo_wptr;
    g_pi_fifo_wrapped = s.pi_fifo_wrapped;
    g_pi_fifo_configured = s.pi_fifo_configured;
    memcpy(g_wg_line, s.wg_line, sizeof g_wg_line);
    g_wg_fill = s.wg_fill < sizeof g_wg_line ? s.wg_fill : 0u;
}

u8* mmio_locked_cache(u32* size) {
    *size = g_guest_memory.locked_cache != NULL
                ? g_guest_memory.config.locked_cache_size
                : 0u;
    return g_guest_memory.locked_cache;
}

u8* mmio_vm_memory(u32* size) {
    *size = g_guest_memory.vm != NULL ? g_guest_memory.config.vm_size : 0u;
    return g_guest_memory.vm;
}

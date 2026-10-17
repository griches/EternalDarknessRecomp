// Recomp host entry point.
//
// Loads the recompiled GameCube DOL into guest RAM, sets up the GameCube boot
// environment, and drives the generated block dispatcher from the entry point.
//
// The generated header provides the CPU ABI (via core/cpu.h), every
// func_XXXXXXXX prototype, and the static-inline dispatcher helpers
// dolrecomp_call() / DOLRECOMP_ENTRY_POINT.
#include "generated.h"

#include "gxruntime/aurora_backend.h"
#include "host/buttons.h"
#include "host/texpack.h"
#include "host/controls.h"
#include "host/frontend.h"
#include "host/guestprof.h"
#include "host/settings.h"
#include "gxruntime/boot.h"
#include "gxruntime/gap_report.h"
#include "gxruntime/dvd.h"
#include "gxruntime/loader.h"
#include "gxruntime/platform.h"
#include "gxruntime/savestate.h"
#include "host/audio.h"
#include "host/dtk.h"
#include "game_data.h"
#include "host/mmio.h"
#include "host/hle.h"
#include "host/interrupt.h"
#include "host/quickstate.h"
#ifdef ED_AURORA
#include "host/report.h"
#endif
#include "host/compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_DOL_PATH   "generated/main.dol"
#define DEFAULT_CARD_PATH  "ed-" ED_GAME_ID "-slot-a.dolcard"
#define DEFAULT_MAX_BLOCKS 50000000ull  // watchdog: HW-wait spins would loop forever
// --call mode: lr points here so the run loop detects the function's return
// (blr -> pc == sentinel). Aligned, well outside any real code range.
#define CALL_RETURN_SENTINEL 0x0BADF00Cu

// Deterministic save-state harness (see GXRuntime savestate.h). SIGUSR1 requests
// a snapshot at the next safe dispatch boundary, so an agent can `kill -USR1` the
// process at any interactive gameplay moment without a controller hotkey.
#include <signal.h>
static volatile sig_atomic_t g_snapshot_request = 0;
static void request_snapshot(int sig) {
    (void)sig;
    g_snapshot_request = 1;
}
// SIGUSR2 dumps the current guest pc + recent pc history -- used to pinpoint
// what a restored/hung run is spin-waiting on (map the pcs to symbols.txt).
static volatile sig_atomic_t g_pcdump_request = 0;
static void request_pcdump(int sig) {
    (void)sig;
    g_pcdump_request = 1;
}
// Capture MEM1 + registers. (ARAM/locked-cache can be added; MEM1 holds the
// gameplay simulation state this harness compares.)
static bool write_state_snapshot(const char* path, const CPUState* cpu) {
    DolSaveRegion regions[1] = {{"MEM1", cpu->ram, cpu->ram_size}};
    bool ok = dol_savestate_write(path, cpu, regions, 1u);
    fprintf(stderr, "[state] snapshot %s -> %s (pc=0x%08X)\n",
            ok ? "written" : "FAILED", path, cpu->pc);
    return ok;
}
static bool dump_mem1(const char* path, const CPUState* cpu) {
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "[state] dump-mem FAILED to open %s\n", path);
        return false;
    }
    bool ok = fwrite(cpu->ram, 1u, cpu->ram_size, f) == cpu->ram_size;
    if (fclose(f) != 0)
        ok = false;
    fprintf(stderr, "[state] MEM1 dump %s -> %s (%u bytes)\n",
            ok ? "written" : "FAILED", path, cpu->ram_size);
    return ok;
}

static void instruction_fallback(CPUState* ctx, u32 raw, u32 cia) {
    // DolRecomp defers environment instructions (cache maintenance,
    // unmodeled SPRs) to this hook. This host has no dcache/icache or DMA
    // model, so keep the pre-deferral semantics: cache ops and unmodeled
    // mtspr are no-ops, unmodeled mfspr reads zero. The hook owns pc.
    if ((raw >> 26) == 31u) {
        const u32 xo = (raw >> 1) & 0x3FFu;
        if (xo == 982u || xo == 86u || xo == 54u || xo == 470u ||  // icbi/dcbf/dcbst/dcbi
            xo == 467u) {                                          // mtspr (unmodeled)
            ctx->pc = cia + 4u;
            return;
        }
        if (xo == 339u) {  // mfspr (unmodeled): read zero
            ctx->gpr[(raw >> 21) & 31u] = 0;
            ctx->pc = cia + 4u;
            return;
        }
    }
    fprintf(stderr, "[fallback] unhandled instruction 0x%08X at 0x%08X\n", raw, cia);
    ctx->exception |= PPC_EXC_PROGRAM;
}

// Walk the PPC EABI backchain to reconstruct the guest call stack:
// [sp] -> caller frame, [caller+4] -> caller's saved return address.
static void dump_backchain_to(FILE* out, CPUState* cpu) {
    u32 sp = cpu->gpr[1];
    fprintf(out, "[run] guest backchain:\n");
    for (int f = 0; f < 16 && sp >= GC_RAM_BASE && sp < GC_RAM_BASE + cpu->ram_size; f++) {
        u32 caller = mem_read32(cpu, sp);
        if (caller <= sp || caller < GC_RAM_BASE) break;
        u32 ret = mem_read32(cpu, caller + 4u);
        fprintf(out, "    #%-2d sp=0x%08X ret=0x%08X\n", f, sp, ret);
        sp = caller;
    }
}
static void dump_backchain(CPUState* cpu) { dump_backchain_to(stderr, cpu); }

static const char* exception_name(u32 exc) {
    if (exc & PPC_EXC_PROGRAM)       return "PROGRAM";
    if (exc & PPC_EXC_DSI)           return "DSI";
    if (exc & PPC_EXC_ALIGNMENT)     return "ALIGNMENT";
    if (exc & PPC_EXC_SYSTEM_CALL)   return "SYSTEM_CALL";
    if (exc & PPC_EXC_MACHINE_CHECK) return "MACHINE_CHECK";
    return "none";
}

// Every OS thread on __OSActiveThreadQueue: where it stopped and what it waits
// on (OSThread layout from dolphin/os/OSThread.h).
static void dump_threads_to(FILE* out, CPUState* cpu) {
    u32 thread = mem_read32(cpu, 0x800000DCu);
    const u32 current = mem_read32(cpu, 0x800000E4u);
    for (int i = 0; i < 64 && thread >= GC_RAM_BASE &&
                    thread < GC_RAM_BASE + cpu->ram_size; i++) {
        fprintf(out,
                "[thread] 0x%08X%s state=%u suspend=%d priority=%u queue=0x%08X "
                "srr0=0x%08X lr=0x%08X sp=0x%08X\n",
                thread, thread == current ? " (current)" : "",
                mem_read16(cpu, thread + 0x2C8u),
                (s32)mem_read32(cpu, thread + 0x2CCu),
                mem_read32(cpu, thread + 0x2D0u), mem_read32(cpu, thread + 0x2DCu),
                mem_read32(cpu, thread + 0x198u), mem_read32(cpu, thread + 0x84u),
                mem_read32(cpu, thread + 0x04u));
        thread = mem_read32(cpu, thread + 0x2FCu);
    }
}
static void dump_threads(CPUState* cpu) { dump_threads_to(stderr, cpu); }

// ED_HEAP_CHECK=N: every N blocks walk the OS heaps' free and allocated cell
// lists (OSAlloc HeapDesc {size, free, allocated}, Cell {prev, next, size}) and
// report the first broken link, to find what corrupts the heap.
static u32 g_heap_array_ptr;  // address of the HeapArray pointer (r13 small data)
static bool heap_ok(CPUState* cpu, unsigned long long blocks) {
    const u32 heaps = mem_read32(cpu, g_heap_array_ptr);
    const s32 count = (s32)mem_read32(cpu, g_heap_array_ptr + 4u);
    if (heaps < GC_RAM_BASE || count <= 0 || count > 64)
        return true;
    for (s32 h = 0; h < count; h++) {
        const u32 hd = heaps + (u32)h * 12u;
        if ((s32)mem_read32(cpu, hd) < 0)
            continue;  // unused descriptor
        for (u32 list = 4u; list <= 8u; list += 4u) {
            u32 cell = mem_read32(cpu, hd + list);
            for (int n = 0; cell != 0u && n < 200000; n++) {
                const u32 next = mem_read32(cpu, cell + 4u);
                if (next != 0u && (next < GC_RAM_BASE ||
                                   next >= GC_RAM_BASE + cpu->ram_size ||
                                   mem_read32(cpu, next) != cell)) {
                    fprintf(stderr,
                            "[heap] broken at block %llu pc=0x%08X lr=0x%08X: heap %d "
                            "%s list, cell 0x%08X next 0x%08X next->prev 0x%08X; "
                            "cell words %08X %08X %08X %08X\n",
                            blocks, cpu->pc, cpu->lr, (int)h,
                            list == 4u ? "free" : "allocated", cell, next,
                            next >= GC_RAM_BASE && next < GC_RAM_BASE + cpu->ram_size
                                ? mem_read32(cpu, next) : 0u,
                            mem_read32(cpu, cell), mem_read32(cpu, cell + 4u),
                            mem_read32(cpu, cell + 8u), mem_read32(cpu, cell + 12u));
                    return false;
                }
                cell = next;
            }
        }
    }
    return true;
}

// The dispatch loop's settings, and what it leaves for the report after it.
enum { PC_HIST = 24 };
typedef struct RunLoop {
    CPUState* cpu;
    unsigned long long max_blocks;
    unsigned long long call_addr;
    unsigned long long trace_every;
    unsigned long long heap_check_every;
    unsigned long long snapshot_at_block;
    const char* snapshot_out;
    unsigned long long blocks;
    const char* stop_reason;
    u32 pc_hist[PC_HIST];
    unsigned pc_hist_i;
} RunLoop;

// The run loop's recent block entries, also read by problem reports while
// it runs.
static u32 g_pc_hist[PC_HIST];
static volatile unsigned g_pc_hist_i;
static volatile unsigned long long g_blocks_now;

// Run guest code until it stops (window closed, watchdog, a fault). With the
// GX thread this is the game thread (dol_aurora_run_game).
static int run_loop(void* user) {
    RunLoop* r = (RunLoop*)user;
    CPUState* cpu = r->cpu;
    const unsigned long long max_blocks = r->max_blocks;
    const unsigned long long call_addr = r->call_addr;
    const unsigned long long trace_every = r->trace_every;
    unsigned long long heap_check_every = r->heap_check_every;
    const unsigned long long snapshot_at_block = r->snapshot_at_block;
    const char* snapshot_out = r->snapshot_out;

    unsigned long long blocks = 0;
    const char* stop_reason = "max-blocks watchdog";
    u32 last_pc = cpu->pc;
    u32* const pc_hist = g_pc_hist;
    unsigned pc_hist_i = 0;
    while (blocks < max_blocks) {
        // --call mode: the function returned (blr jumped to the lr sentinel).
        if (call_addr != 0 && cpu->pc == CALL_RETURN_SENTINEL) {
            stop_reason = "called function returned";
            break;
        }
#ifdef ED_AURORA
        if (call_addr == 0 && dol_platform_should_quit()) {
            stop_reason = "window closed";
            break;
        }
#endif
        // In --call mode run the function in isolation: no interrupts/device
        // polling, so its output depends only on the loaded MEM1 + arg registers.
        if (call_addr != 0) {
            u32 cpc = cpu->pc;
            if (!dolrecomp_call(cpu, cpc)) {
                stop_reason = "called function left recompiled code";
                fprintf(stderr, "[call] pc=0x%08X has no handler (lr=0x%08X)\n",
                        cpu->pc, cpu->lr);
                break;
            }
            if (cpu->exception != 0) {
                stop_reason = "cpu exception in called function";
                break;
            }
            blocks++;
            continue;
        }
        // Snapshot on request (SIGUSR1, interactive) or at a deterministic block
        // count (--snapshot-at-block, automated/agent). Taken at the top of a
        // dispatch iteration -- a block boundary -- where cpu->pc is a valid
        // resume point.
        if (snapshot_out != NULL &&
            (g_snapshot_request ||
             (snapshot_at_block != 0 && blocks == snapshot_at_block))) {
            g_snapshot_request = 0;
            write_state_snapshot(snapshot_out, cpu);
        }
        if (g_pcdump_request) {
            g_pcdump_request = 0;
            fprintf(stderr, "[pcdump] pc=0x%08X lr=0x%08X r1=0x%08X msr=0x%08X "
                            "blocks=%llu\n  recent:",
                    cpu->pc, cpu->lr, cpu->gpr[1], cpu->msr, blocks);
            for (unsigned k = 0; k < PC_HIST; ++k)
                fprintf(stderr, " 0x%08X",
                        pc_hist[(pc_hist_i + k) % PC_HIST]);
            fprintf(stderr, "\n");
            dump_threads(cpu);
            // ED_PCDUMP_MEM=path: also write raw MEM1, for tools/backchain.py.
            if (getenv("ED_PCDUMP_MEM") != NULL)
                dump_mem1(getenv("ED_PCDUMP_MEM"), cpu);
        }

        // Quick save / load (keys 2 / 1): at a block boundary like the
        // snapshot above, but only once the GX FIFO is between commands.
        if ((blocks & 0xFFFu) == 0u)
            quickstate_poll(blocks);
        if (g_quickstate_pending && quickstate_service(cpu))
            continue;  // loaded: pc and the devices changed under us

        // Deliver a due VI-retrace interrupt before dispatching the next block,
        // so the redirected pc (the OS interrupt dispatcher) runs here.
        interrupt_poll(cpu);
        hle_poll_callback(cpu);

        u32 pc = cpu->pc;
        pc_hist[pc_hist_i++ % PC_HIST] = pc;
        g_pc_hist_i = pc_hist_i;
        if (!dolrecomp_call(cpu, pc)) {
            stop_reason = "pc left recompiled code (no host handler)";
            {
                char key[16];
                snprintf(key, sizeof key, "0x%08X", pc);
                gap_report_note("hle", "dispatch-miss", key, last_pc,
                                "guest pc has no recompiled block and no HLE handler");
            }
            fprintf(stderr, "[run] left recompiled code: pc=0x%08X reached from "
                            "block 0x%08X (lr=0x%08X)\n",
                    cpu->pc, last_pc, cpu->lr);
            fprintf(stderr, "[run] recent block entry PCs (oldest first):\n   ");
            for (unsigned k = 0; k < PC_HIST; k++)
                fprintf(stderr, " 0x%08X", pc_hist[(pc_hist_i + k) % PC_HIST]);
            fprintf(stderr, "\n");
            dump_backchain(cpu);
            break;
        }
        last_pc = pc;
        if (cpu->exception) {
            // GameCube uses the `sc` instruction purely as a post-cache-op sync
            // barrier: DCFlushRange/DCStoreRange/etc. end with `dcbf...; sc; blr`,
            // and the SDK's default system-call vector simply syncs and returns.
            // There is no real cache to wait on here, so emulate that vector by
            // returning (rfi) to the instruction after `sc`.
            if (cpu->exception == PPC_EXC_SYSTEM_CALL) {
                cpu->exception = 0;
                ppc_rfi(cpu, cpu->pc);
                continue;
            }
            // Lazy FPU: a thread that is not the FPU owner used an FP
            // instruction. Switch the FPU to it and retry the instruction.
            if (cpu->exception == PPC_EXC_FP_UNAVAILABLE) {
                interrupt_switch_fpu_context(cpu);
                continue;
            }
            stop_reason = "cpu exception";
            break;
        }
        blocks++;
        g_blocks_now = blocks;
        if (heap_check_every && (blocks % heap_check_every) == 0 &&
            !heap_ok(cpu, blocks))
            heap_check_every = 0;  // report the first break only
        if (trace_every && (blocks % trace_every) == 0)
            fprintf(stderr, "[trace] block %llu pc=0x%08X lr=0x%08X r1=0x%08X\n",
                    blocks, cpu->pc, cpu->lr, cpu->gpr[1]);
    }

    r->blocks = blocks;
    r->stop_reason = stop_reason;
    memcpy(r->pc_hist, pc_hist, sizeof r->pc_hist);
    r->pc_hist_i = pc_hist_i;
    return 0;
}

// The disc this build was translated from, for the launcher (frontend.cpp,
// which is built once for every disc).
const char* ed_game_id(void) { return ED_GAME_ID; }

// The guest CPU: one per process. Chunks built with DOLRECOMP_STATIC_CPU
// (CMakeLists.txt) address it by name, a link-time constant, instead of
// through their CPUState argument.
CPUState dolrecomp_cpu;

#ifdef ED_AURORA
// For problem reports (report.c), from another thread while the game runs:
// a snapshot that may be a block out of date, which is fine for a report.
void main_describe_guest(FILE* out) {
    CPUState* cpu = &dolrecomp_cpu;
    fprintf(out, "pc=0x%08X lr=0x%08X r1=0x%08X msr=0x%08X blocks=%llu\n", cpu->pc,
            cpu->lr, cpu->gpr[1], cpu->msr, g_blocks_now);
    fprintf(out, "recent blocks (oldest first):");
    const unsigned at = g_pc_hist_i;
    for (unsigned k = 0; k < PC_HIST; k++)
        fprintf(out, " 0x%08X", g_pc_hist[(at + k) % PC_HIST]);
    fprintf(out, "\n");
    if (cpu->ram == NULL)
        return;
    dump_backchain_to(out, cpu);
    dump_threads_to(out, cpu);
}

void main_request_pcdump(void) { g_pcdump_request = 1; }
#endif

int main(int argc, char** argv) {
    // Keep stdout/stderr unbuffered so diagnostics are not lost when a worker
    // thread calls abort() (e.g. an Aurora FATAL) before buffered output is
    // flushed.
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef ED_AURORA
    report_init();
#endif
    gap_report_reset();
    const char* dol_path = DEFAULT_DOL_PATH;
    const char* iso_path = NULL;
    const char* card_path = NULL;
    const char* card_b_path = NULL;
    bool card_enabled = true;
    unsigned long long max_blocks = DEFAULT_MAX_BLOCKS;
    unsigned long long trace_every = 0;
    const char* snapshot_out = NULL;   // SIGUSR1 / --snapshot-at-block target
    bool no_launcher = false;          // --no-launcher: boot straight in
    const char* restore_path = NULL;   // load a snapshot at boot and resume
    const char* dump_mem_path = NULL;  // raw MEM1 at exit (Dolphin differential)
    const char* gap_report_path = NULL;  // C2 structured tripwire report (JSON)
    unsigned long long snapshot_at_block = 0;  // 0 = signal-only (interactive)
    // Single-function differential: load a raw MEM1 (Dolphin MRAM), set arg
    // registers, call one guest function, run to return, dump MEM1. Rigorous and
    // resume-free -- compares one function's output against Dolphin's.
    const char* restore_mem_path = NULL;  // raw 24 MB MEM1 to load
    unsigned long long call_addr = 0;     // function entry to call (0 = normal)
    u32 set_reg_idx[32];
    u32 set_reg_val[32];
    unsigned set_reg_count = 0;
    u32 set_fpr_idx[32];
    u64 set_fpr_bits[32];
    unsigned set_fpr_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--max-blocks") == 0 && i + 1 < argc) {
            max_blocks = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--trace-every") == 0 && i + 1 < argc) {
            trace_every = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--iso") == 0 && i + 1 < argc) {
            iso_path = argv[++i];
        } else if (strcmp(argv[i], "--card") == 0 && i + 1 < argc) {
            card_path = argv[++i];
        } else if (strcmp(argv[i], "--card-b") == 0 && i + 1 < argc) {
            card_b_path = argv[++i];
        } else if (strcmp(argv[i], "--no-launcher") == 0) {
            no_launcher = true;
        } else if (strcmp(argv[i], "--no-card") == 0) {
            card_enabled = false;
        } else if (strcmp(argv[i], "--mmio-log") == 0) {
            mmio_set_logging(true);
        } else if (strcmp(argv[i], "--snapshot-out") == 0 && i + 1 < argc) {
            snapshot_out = argv[++i];
        } else if (strcmp(argv[i], "--snapshot-at-block") == 0 && i + 1 < argc) {
            snapshot_at_block = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--restore") == 0 && i + 1 < argc) {
            restore_path = argv[++i];
        } else if (strcmp(argv[i], "--dump-mem") == 0 && i + 1 < argc) {
            dump_mem_path = argv[++i];
        } else if (strcmp(argv[i], "--gap-report") == 0 && i + 1 < argc) {
            gap_report_path = argv[++i];
        } else if (strcmp(argv[i], "--restore-mem") == 0 && i + 1 < argc) {
            restore_mem_path = argv[++i];
        } else if (strcmp(argv[i], "--call") == 0 && i + 1 < argc) {
            call_addr = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--set-reg") == 0 && i + 1 < argc &&
                   set_reg_count < 32u) {
            // --set-reg N=0xVAL
            char* eq = strchr(argv[++i], '=');
            if (eq != NULL) {
                set_reg_idx[set_reg_count] = (u32)strtoul(argv[i], NULL, 0);
                set_reg_val[set_reg_count] = (u32)strtoul(eq + 1, NULL, 0);
                set_reg_count++;
            }
        } else if (strcmp(argv[i], "--set-fpr") == 0 && i + 1 < argc &&
                   set_fpr_count < 32u) {
            // --set-fpr N=0xRAWBITS (full f64 bits) or N=<float literal>
            char* eq = strchr(argv[++i], '=');
            if (eq != NULL) {
                set_fpr_idx[set_fpr_count] = (u32)strtoul(argv[i], NULL, 0);
                if (eq[1] == '0' && (eq[2] == 'x' || eq[2] == 'X')) {
                    set_fpr_bits[set_fpr_count] = strtoull(eq + 1, NULL, 16);
                } else {
                    double d = strtod(eq + 1, NULL);
                    memcpy(&set_fpr_bits[set_fpr_count], &d, sizeof(double));
                }
                set_fpr_count++;
            }
        } else if (argv[i][0] != '-') {
            dol_path = argv[i];
        } else {
            fprintf(stderr,
                    "usage: %s [dol-path] [--iso path] [--max-blocks N] "
                    "[--card path | --no-card] [--trace-every N] [--mmio-log]\n"
                    "  --no-launcher           boot straight into the game\n"
                    "  deterministic save-state harness:\n"
                    "    --snapshot-out PATH   write a snapshot on SIGUSR1\n"
                    "    --restore PATH        load a snapshot at boot and resume\n"
                    "    --dump-mem PATH       write raw MEM1 at exit (for diff)\n",
                    argv[0]);
            return 2;
        }
    }
#ifdef SIGUSR1
    if (snapshot_out != NULL)
        signal(SIGUSR1, request_snapshot);
    signal(SIGUSR2, request_pcdump);  // always available for hang diagnosis
#endif

#ifdef ED_AURORA
    const AuroraBackendConfig backend_config = {
        .app_name = "Eternal Darkness",
        .window_width = 1280,
        .window_height = 960,
        .vsync = getenv("ED_NO_VSYNC") == NULL,
        .allow_texture_dumps = getenv("ED_TEXTURE_DUMP") != NULL,
        .info_logging = getenv("ED_AURORA_LOG") != NULL,
        .graphics_logging = getenv("ED_GFX_LOG") != NULL,
        .force_untextured =
            getenv("ED_GFX_FORCE_UNTEXTURED") != NULL,
        .imgui_init = frontend_imgui_init,
    };
    // --call mode runs one pure-compute function with no rendering: skip Aurora
    // init so it runs headless (no window), agent-drivable and side-effect-free.
    const bool aurora_enabled = (call_addr == 0);
    if (aurora_enabled && !dol_aurora_initialize(argc, argv, &backend_config))
        return 1;
    // Settings and the launcher (disc + options). Test runs (--no-launcher,
    // a boot quick load or ED_NO_LAUNCHER) go straight in.
    static char launcher_iso[1024];
    if (aurora_enabled) {
        settings_load();
        settings_apply();
        controls_init();
        buttons_init();
        frontend_init(argc, argv);
        texpack_init();
        if (!no_launcher && getenv("ED_QUICKLOAD") == NULL &&
            getenv("ED_NO_LAUNCHER") == NULL) {
            const char* default_iso = iso_path != NULL ? iso_path : getenv("ED_ISO");
            if (default_iso == NULL || default_iso[0] == '\0')
                default_iso = "dump/" ED_GAME_ID ".iso";
            if (!frontend_run_launcher(default_iso, launcher_iso, sizeof launcher_iso)) {
                dol_aurora_shutdown();
                return 0;
            }
            iso_path = launcher_iso;
        }
    }
#endif

    if (!cpu_init(&dolrecomp_cpu)) {
#ifdef ED_AURORA
        dol_aurora_shutdown();
#endif
        return 1;
    }

    DolLayout layout;
    if (!dol_load_into_ram(&dolrecomp_cpu, dol_path, &layout)) {
        cpu_free(&dolrecomp_cpu);
#ifdef ED_AURORA
        dol_aurora_shutdown();
#endif
        return 1;
    }

    if (layout.entry_point != DOLRECOMP_ENTRY_POINT) {
        fprintf(stderr,
                "warning: DOL entry 0x%08X != recompiled entry 0x%08X "
                "(DOL does not match the recompiled build)\n",
                layout.entry_point, (u32)DOLRECOMP_ENTRY_POINT);
    }

    boot_setup_os_globals(&dolrecomp_cpu, &layout);
    if (!mmio_install(&dolrecomp_cpu)) {
        cpu_free(&dolrecomp_cpu);
#ifdef ED_AURORA
        dol_aurora_shutdown();
#endif
        return 1;
    }
#ifdef ED_AURORA
    if (aurora_enabled)
        mmio_attach_efb_readback();
#endif
    if (card_enabled) {
        if (card_path == NULL)
            card_path = getenv("ED_CARD");
        if (card_path == NULL || card_path[0] == '\0')
            card_path = DEFAULT_CARD_PATH;
        if (!hle_card_open(card_path))
            fprintf(stderr,
                    "[card] slot A unavailable; continuing with no card\n");
        // Slot B: --card-b or ED_CARD_B (made if it is not there yet), else
        // slot A's file with -slot-b in its name when that file exists.
        if (card_b_path == NULL)
            card_b_path = getenv("ED_CARD_B");
        char beside_a[1024];
        if (card_b_path == NULL || card_b_path[0] == '\0') {
            card_b_path = NULL;
            const char* tag = strstr(card_path, "-slot-a.dolcard");
            if (tag != NULL && strlen(card_path) < sizeof beside_a) {
                strcpy(beside_a, card_path);
                memcpy(beside_a + (tag - card_path), "-slot-b", 7);
                FILE* f = fopen(beside_a, "rb");
                if (f != NULL) {
                    fclose(f);
                    card_b_path = beside_a;
                }
            }
        }
        if (card_b_path != NULL && !hle_card_open_b(card_b_path))
            fprintf(stderr, "[card] slot B unavailable: %s\n", card_b_path);
        // The console's settings beside slot A's card: -slot-a.dolcard becomes
        // -sram.bin (any other card name gets .sram on the end).
        char sram_path[1024];
        const char* sram_tag = strstr(card_path, "-slot-a.dolcard");
        if (sram_tag != NULL)
            snprintf(sram_path, sizeof sram_path, "%.*s-sram.bin",
                     (int)(sram_tag - card_path), card_path);
        else
            snprintf(sram_path, sizeof sram_path, "%s.sram", card_path);
        mmio_set_sram_path(sram_path);
    }
    hle_install(&dolrecomp_cpu);
    if (iso_path == NULL)
        iso_path = getenv("ED_ISO");
    if (iso_path == NULL || iso_path[0] == '\0')
        iso_path = "dump/" ED_GAME_ID ".iso";
    dvd_open_image(iso_path);  // disc image backing the file system
    dtk_init(iso_path);        // disc music streams, read from the same image
    mmio_set_disc_present(dvd_image_ready());
    // The IPL leaves the disc header's first 0x20 bytes (game and maker code,
    // disc number, version, streaming) at 0x80000000, where DVDGetCurrentDiskID
    // points. Starting a game checks the save's game and maker code against it.
    if (dvd_image_ready())
        dvd_read_to_guest(&dolrecomp_cpu, 0x80000000u, 0, 0x20u);
    quickstate_init();  // after every device it saves is set up
    dolrecomp_cpu.instruction_fallback = instruction_fallback;
    // FP instructions check MSR[FP]; the run loop below answers the resulting
    // FP-unavailable exceptions with the OS's lazy FPU switch (interrupt.c).

    dolrecomp_cpu.pc = layout.entry_point;

    // --call ambient SPR state. A bare single-function call skips the boot/OSInit
    // path that sets HID2, so HID2[PSE|LSQE] is clear and the FIRST paired-single
    // load/store (psq_l/psq_st -- pervasive in SDK vector/matrix/physics code)
    // raises a spurious PROGRAM(ILLEGAL) before block 0 (psq_check_enabled,
    // GXRuntime/src/core/cpu.c). These enables are architectural: any booted
    // Gekko running PS code has them set, so default them here. The GQR *contents*
    // (SDK fastcast quantization, e.g. gqr[5]/gqr[6]) are game policy, not
    // architectural -- pass --restore <gameplay savestate> below to recover the
    // real GQR/MSR context for a fully faithful differential. A --restore loads
    // its captured HID2 over this default (later, so it wins), which is correct.
    if (call_addr != 0) {
        dolrecomp_cpu.hid2 |= PPC_HID2_PSE | PPC_HID2_LSQE | PPC_HID2_LCE;
        fprintf(stderr,
                "[call] ambient SPR default: hid2=0x%08X (PSE|LSQE|LCE); "
                "pass --restore <savestate> for real GQR/MSR fidelity\n",
                dolrecomp_cpu.hid2);
    }

    // Deterministic restore: load a previously captured gameplay snapshot over
    // the freshly-booted state. The snapshot's registers (including pc) win, so
    // execution resumes exactly where it was captured -- no menu navigation, no
    // controller, identical state every run. Host function pointers (rebound by
    // cpu_init/hle_install above) are preserved by dol_savestate_read.
    if (restore_path != NULL) {
        DolSaveRegion regions[1] = {{"MEM1", dolrecomp_cpu.ram, dolrecomp_cpu.ram_size}};
        bool mismatch = false;
        if (!dol_savestate_read(restore_path, &dolrecomp_cpu, regions, 1u, &mismatch)) {
            fprintf(stderr, "[state] restore FAILED from %s\n", restore_path);
            cpu_free(&dolrecomp_cpu);
#ifdef ED_AURORA
            dol_aurora_shutdown();
#endif
            return 1;
        }
        fprintf(stderr,
                "[state] restored from %s (pc=0x%08X)%s; resuming\n",
                restore_path, dolrecomp_cpu.pc, mismatch ? " [size mismatch]" : "");
    }

    // Single-function differential setup. Load a raw MEM1 (Dolphin MRAM), apply
    // arg registers, and aim pc at the function with lr = a sentinel so the run
    // loop stops cleanly when the function returns (blr -> pc == sentinel).
    if (restore_mem_path != NULL) {
        FILE* mf = fopen(restore_mem_path, "rb");
        if (mf == NULL) {
            fprintf(stderr, "[call] FAILED to open MEM1 %s\n", restore_mem_path);
            cpu_free(&dolrecomp_cpu);
#ifdef ED_AURORA
            dol_aurora_shutdown();
#endif
            return 1;
        }
        size_t got = fread(dolrecomp_cpu.ram, 1u, dolrecomp_cpu.ram_size, mf);
        fclose(mf);
        fprintf(stderr, "[call] loaded MEM1 %s (%zu bytes)\n", restore_mem_path,
                got);
    }
    for (unsigned r = 0; r < set_reg_count; ++r) {
        if (set_reg_idx[r] < 32u) {
            dolrecomp_cpu.gpr[set_reg_idx[r]] = set_reg_val[r];
            fprintf(stderr, "[call] r%u = 0x%08X\n", set_reg_idx[r],
                    set_reg_val[r]);
        }
    }
    for (unsigned r = 0; r < set_fpr_count; ++r) {
        if (set_fpr_idx[r] < 32u) {
            double d;
            memcpy(&d, &set_fpr_bits[r], sizeof(double));
            dolrecomp_cpu.fpr[set_fpr_idx[r]] = d;
            dolrecomp_cpu.ps1[set_fpr_idx[r]] = d;
            fprintf(stderr, "[call] f%u = %g (bits 0x%016llX)\n", set_fpr_idx[r],
                    d, (unsigned long long)set_fpr_bits[r]);
        }
    }
    if (call_addr != 0) {
        dolrecomp_cpu.pc = (u32)call_addr;
        dolrecomp_cpu.lr = CALL_RETURN_SENTINEL;
        fprintf(stderr, "[call] calling 0x%08X (lr sentinel 0x%08X)\n", dolrecomp_cpu.pc,
                CALL_RETURN_SENTINEL);
    }

    unsigned long long heap_check_every =
        getenv("ED_HEAP_CHECK") ? strtoull(getenv("ED_HEAP_CHECK"), NULL, 0) : 0ull;
    g_heap_array_ptr = ED_GUEST_R13 - 21344u;  // OSAlloc's HeapArray (sbss)
    printf("[run] starting execution at 0x%08X (max %llu blocks)\n",
           dolrecomp_cpu.pc, max_blocks);

    RunLoop run = {
        .cpu = &dolrecomp_cpu,
        .max_blocks = max_blocks,
        .call_addr = call_addr,
        .trace_every = trace_every,
        .heap_check_every = heap_check_every,
        .snapshot_at_block = snapshot_at_block,
        .snapshot_out = snapshot_out,
    };
#ifdef ED_AURORA
    if (getenv("ED_GUEST_PROF") != NULL)
        guest_prof_start(&dolrecomp_cpu);
    if (aurora_enabled) {
        dol_aurora_set_frame_hook(hle_frame_boundary);
        dol_aurora_set_idle_hook(report_idle);
        report_start_watchdog();
        dol_aurora_run_game(run_loop, &run);
    } else
#endif
        run_loop(&run);
    const unsigned long long blocks = run.blocks;
    const char* stop_reason = run.stop_reason;
#ifdef ED_AURORA
    // The player sees a message, not a window that just vanishes.
    if (aurora_enabled && strcmp(stop_reason, "window closed") != 0)
        report_game_stopped(stop_reason);
#endif
    const u32* pc_hist = run.pc_hist;
    const unsigned pc_hist_i = run.pc_hist_i;

    if (getenv("ED_PC_HIST")) {
        fprintf(stderr, "[run] recent block entry PCs (oldest first):\n   ");
        for (unsigned k = 0; k < PC_HIST; k++)
            fprintf(stderr, " 0x%08X", pc_hist[(pc_hist_i + k) % PC_HIST]);
        fprintf(stderr, "\n");
    }
    if (getenv("ED_BACKCHAIN"))
        dump_backchain(&dolrecomp_cpu);
    if (getenv("ED_DUMP_CTX")) {
        u32 a = (u32)strtoul(getenv("ED_DUMP_CTX"), NULL, 0);
        // OSContext: gpr1 @ +0x04, lr @ +0x84, srr0 @ +0x198 (where it resumes).
        fprintf(stderr, "[ctx] 0x%08X srr0=0x%08X lr=0x%08X r1=0x%08X r3=0x%08X\n",
                a, mem_read32(&dolrecomp_cpu, a + 0x198u), mem_read32(&dolrecomp_cpu, a + 0x84u),
                mem_read32(&dolrecomp_cpu, a + 0x04u), mem_read32(&dolrecomp_cpu, a + 0x0Cu));
        // Treat the address as an OSThread: state @+0x2C8, suspend @+0x2CC,
        // priority @+0x2D0, queue (the OSThreadQueue* it waits on) @+0x2DC.
        // state: 1=READY 2=RUNNING 4=WAITING 8=MORIBUND.
        fprintf(stderr,
                "[thread] 0x%08X state=%u suspend=%d priority=%u waiting-on-queue="
                "0x%08X\n",
                a, mem_read16(&dolrecomp_cpu, a + 0x2C8u), (s32)mem_read32(&dolrecomp_cpu, a + 0x2CCu),
                mem_read16(&dolrecomp_cpu, a + 0x2D0u), mem_read32(&dolrecomp_cpu, a + 0x2DCu));
    }

    if (getenv("ED_DUMP_THREADS"))
        dump_threads(&dolrecomp_cpu);
    printf("[run] stopped: %s\n", stop_reason);
    printf("[run] blocks executed : %llu\n", blocks);
    printf("[run] final pc         : 0x%08X\n", dolrecomp_cpu.pc);
    printf("[run] link register    : 0x%08X\n", dolrecomp_cpu.lr);
    printf("[run] stack (r1)       : 0x%08X\n", dolrecomp_cpu.gpr[1]);
    printf("[run] exception        : %s (0x%08X)\n",
           exception_name(dolrecomp_cpu.exception), dolrecomp_cpu.exception);

    // Raw MEM1 at exit for the recomp-vs-Dolphin differential. Guest big-endian
    // bytes, directly diffable against a Dolphin RAM dump.
    if (dump_mem_path != NULL)
        dump_mem1(dump_mem_path, &dolrecomp_cpu);

    // C2: fold gxcore GapCounters into the report before serialising so live
    // --gap-report includes gxcore/* rows (backend-only until this flush).
#ifdef ED_AURORA
    dol_aurora_flush_gap_report();
#endif

    // C2: emit the structured tripwire report. Full JSON tags every row with
    // class (gap|policy|diagnostic). Empty-report acceptance uses only class
    // "gap" (policy/* + documented diagnostics filtered).
    printf("[gap-report] %zu distinct / %llu total  "
           "(acceptance: %zu / %llu after policy+diagnostic filter)\n",
           gap_report_size(), (unsigned long long)gap_report_total(),
           gap_report_acceptance_size(),
           (unsigned long long)gap_report_acceptance_total());
    if (gap_report_path != NULL)
        gap_report_write_json(gap_report_path);

    hle_audio_api_report();
    dvd_close_image();
    hle_card_close();
    audio_shutdown();
    mmio_shutdown();
#ifdef ED_AURORA
    dol_aurora_shutdown();
#endif
    cpu_free(&dolrecomp_cpu);
    return 0;
}

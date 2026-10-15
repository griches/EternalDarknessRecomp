// Faithful VI-retrace external interrupt. See interrupt.h.
#include "host/interrupt.h"
#include "host/sdk_map.h"
#include "host/audio.h"
#include "host/mmio.h"
#include "gxruntime/interrupts.h"
#include "gxruntime/si.h"
#include "gxruntime/vi_clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MSR_EE 0x00008000u  // MSR[EE], external interrupts enabled (bit 16)
#define MSR_FP 0x00002000u  // MSR[FP], floating point available

// Guest globals / entry points.
#define OS_CURRENT_CONTEXT 0x800000D4u  // OSContext* __OSCurrentContext
#define OS_FPU_CONTEXT 0x800000D8u      // OSContext* __OSFPUContext, the FPU owner
#define DISPATCH_INTERRUPT sdk_symbol_address("__OSDispatchInterrupt")  // (exc, context)
#define EXC_EXTERNAL 4u                 // __OS_EXCEPTION_EXTERNAL_INTERRUPT

// OSContext field offsets (dolphin/os/OSContext.h).
#define CTX_CR 0x080u
#define CTX_LR 0x084u
#define CTX_CTR 0x088u
#define CTX_XER 0x08Cu
#define CTX_FPR 0x090u
#define CTX_FPSCR 0x194u
#define CTX_SRR0 0x198u
#define CTX_SRR1 0x19Cu
#define CTX_STATE 0x1A2u
#define CTX_GQR 0x1A4u
#define CTX_PSF 0x1C8u
#define OS_CONTEXT_STATE_FPSAVED 0x0001u
#define OS_CONTEXT_STATE_EXC 0x0002u

// Dispatch iterations between simulated retraces. Aurora's present path is
// vsynced, so this cadence only needs to be large enough that normal frame work
// reaches GXCopyDisp before the next VI tick. The old 2,000,000-block cadence
// made the front end present at roughly 11 FPS on macOS.
// Eternal Darkness's HVQ movie decoder runs several times that much code per
// frame; at 350,000 the guest clock raced ahead of the audio it produced and the
// movies crawled at a few frames per second. ED_FRAME_BLOCKS overrides.
#define DEFAULT_FRAME_BLOCKS 2000000ull

// The Broadway/Gekko time base runs at one quarter of the 162 MHz bus clock.
// DolViClock advances it in lockstep with the simulated 60 Hz VI cadence so
// OSGetTick, task delta times, animations, and gameplay timers progress
// deterministically instead of depending on host wall-clock speed.
#define GC_TIMEBASE_HZ 40500000ull
#define VI_REFRESH_HZ 60ull

static DolInterrupts interrupts;
static DolSiDevice si;
static DolViClock vi_clock;
static u64 frame_work_units;

// The windowed build runs retraces and audio chunks off the wall clock, so the
// game keeps real time whatever the host's block rate: the movie decoder needs
// the 2M-block cadence, which ran the title screen at 67% speed on an M4 Pro
// and the audio with it. The headless build, or ED_FRAME_BLOCKS=N, counts N
// blocks per retrace instead (deterministic, for debugging).
#define WALL_UNITS_PER_FRAME 1000000ull  // VI clock units, 60,000,000 a second
#define WALL_MAX_STEP_NS 100000000ull     // after a stall, drop time, don't race
static bool wall_clock;
static u64 wall_last_ns;
static u64 wall_vi_rem;     // ns * 3 not yet turned into units (units = ns*3/50)
static u64 wall_audio_rem;  // ns * chunks/s not yet turned into chunks

static u64 wall_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

// ED_SPEED_LOG=1: wall-clock seconds per 60 retraces (1.00 is full speed).
static bool speed_log;
static u64 speed_retraces;
static double speed_mark;

static double wall_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void speed_note_retrace(void) {
    if (!speed_log || ++speed_retraces % VI_REFRESH_HZ != 0u)
        return;
    const double now = wall_seconds();
    if (speed_mark != 0.0)
        fprintf(stderr, "[speed] 60 retraces in %.3f s (%.0f%% speed)\n",
                now - speed_mark, 100.0 / (now - speed_mark));
    speed_mark = now;
}

// Blocks between batched retrace/interrupt-delivery evaluations (see
// interrupt_poll). Accumulator of guest blocks since the last batched pass.
#define INTERRUPT_POLL_INTERVAL 128u
static u32 interrupt_poll_accum;

// Lazy FPU switching, as the OS's own FP-unavailable handler does it
// (OSSwitchFPUContext). The OS runs every thread but the FPU owner
// (__OSFPUContext) with MSR[FP] clear; the first FP instruction such a thread
// executes traps, the owner's registers are saved into its OSContext, the
// current context's are loaded, and the current context becomes the owner. The
// vector stub that would reach that handler lives in low memory, copied there at
// run time, so the recompiler has no block for it: the host does the switch.
static void save_fpu(CPUState* cpu, u32 ctx) {
    for (u32 i = 0; i < 32u; ++i) {
        u64 bits;
        memcpy(&bits, &cpu->fpr[i], sizeof bits);
        mem_write64(cpu, ctx + CTX_FPR + i * 8u, bits);
        memcpy(&bits, &cpu->ps1[i], sizeof bits);
        mem_write64(cpu, ctx + CTX_PSF + i * 8u, bits);
    }
    mem_write32(cpu, ctx + CTX_FPSCR, cpu->fpscr);
    mem_write16(cpu, ctx + CTX_STATE,
                mem_read16(cpu, ctx + CTX_STATE) | OS_CONTEXT_STATE_FPSAVED);
}

static void load_fpu(CPUState* cpu, u32 ctx) {
    if ((mem_read16(cpu, ctx + CTX_STATE) & OS_CONTEXT_STATE_FPSAVED) == 0u)
        return;
    for (u32 i = 0; i < 32u; ++i) {
        u64 bits = mem_read64(cpu, ctx + CTX_FPR + i * 8u);
        memcpy(&cpu->fpr[i], &bits, sizeof bits);
        bits = mem_read64(cpu, ctx + CTX_PSF + i * 8u);
        memcpy(&cpu->ps1[i], &bits, sizeof bits);
    }
    cpu->fpscr = mem_read32(cpu, ctx + CTX_FPSCR);
}

static bool guest_context(const CPUState* cpu, u32 ctx) {
    return ctx >= GC_RAM_BASE && ctx - GC_RAM_BASE < cpu->ram_size - 0x2C8u;
}

void interrupt_switch_fpu_context(CPUState* cpu) {
    const u32 current = mem_read32(cpu, OS_CURRENT_CONTEXT);
    const u32 owner = mem_read32(cpu, OS_FPU_CONTEXT);
    if (owner != current) {
        if (guest_context(cpu, owner))
            save_fpu(cpu, owner);
        if (guest_context(cpu, current))
            load_fpu(cpu, current);
        mem_write32(cpu, OS_FPU_CONTEXT, current);
    }
    if (guest_context(cpu, current))
        mem_write32(cpu, current + CTX_SRR1,
                    mem_read32(cpu, current + CTX_SRR1) | MSR_FP);
    cpu->exception &= ~PPC_EXC_FP_UNAVAILABLE;
    cpu->srr1 |= MSR_FP;
    ppc_rfi(cpu, cpu->srr0);
}

static void sync_audio_interrupt(void) {
    dol_interrupts_set_source(&interrupts, DOL_PI_CAUSE_DSP,
                              audio_interrupt_pending());
    dol_interrupts_set_source(&interrupts, DOL_PI_CAUSE_AI,
                              audio_ai_interrupt_pending());
}

static void sync_si_interrupt(void) {
    dol_interrupts_set_source(&interrupts, DOL_PI_CAUSE_SI,
                              dol_si_interrupt_pending(&si));
}

void interrupt_init(void) {
    dol_interrupts_init(&interrupts);
    dol_si_init(&si);
    dol_vi_clock_init(&vi_clock);
    interrupt_poll_accum = 0;
    frame_work_units = DEFAULT_FRAME_BLOCKS;
#ifdef ED_AURORA
    wall_clock = true;
#endif
    const char* env = getenv("ED_FRAME_BLOCKS");
    if (env != NULL && env[0] != '\0') {
        char* end = NULL;
        unsigned long long value = strtoull(env, &end, 0);
        if (end != env && value != 0) {
            frame_work_units = value;
            wall_clock = false;
        }
    }
    if (wall_clock) {
        dol_vi_clock_configure(&vi_clock, WALL_UNITS_PER_FRAME,
                               (u32)VI_REFRESH_HZ, GC_TIMEBASE_HZ);
        audio_use_wall_clock();
        wall_last_ns = wall_ns();
        wall_vi_rem = 0;
        wall_audio_rem = 0;
    } else {
        dol_vi_clock_configure(&vi_clock, frame_work_units, (u32)VI_REFRESH_HZ,
                               GC_TIMEBASE_HZ);
        audio_set_vi_clock(frame_work_units, (u32)VI_REFRESH_HZ);
    }
    speed_log = getenv("ED_SPEED_LOG") != NULL;
}

// GPU time for a draw-done that nobody waits on: the PE finish interrupt is
// raised this many work units after GXSetDrawDone, not at once. This game's
// draw-done callback does a blocking OSReceiveMessage for the message the main
// loop sends right after GXSetDrawDone; on hardware the GPU is always later than
// that send, and finishing first would block the callback inside the interrupt.
#define PE_FINISH_LATENCY 4096u
static u32 pe_finish_countdown;

void interrupt_defer_pe_finish(void) {
    if (pe_finish_countdown == 0)
        pe_finish_countdown = PE_FINISH_LATENCY;
}

void interrupt_commit_pe_finish(void) {
    dol_interrupts_commit_pe_finish(&interrupts);
    if (getenv("ED_IRQ_LOG"))
        fprintf(stderr, "[irq] PE finish raised: cause=0x%08X mask=0x%08X\n",
                dol_interrupts_pi_cause(&interrupts),
                dol_interrupts_pi_mask(&interrupts));
}

// Pixel Engine interrupt control (PE_CTRL, 0xCC00100A) and the draw-sync token.
// Bits 0/1 enable the token/finish interrupts and read back as written; bits
// 2/3 are write-one-to-clear for the pending token/finish interrupts and read
// back as zero (Dolphin PixelEngine). The SDK's handlers ack with a
// read-modify-write, so reporting a pending bit on read would make the token
// handler ack a pending finish too, and the draw-done callback would never run.
// GXSetDrawSync puts a token in the FIFO; once the GPU reaches it the PE
// latches it in PE_TOKEN (0xCC00100E, GXRuntime's PE model, read by
// GXReadDrawSync) and raises PI cause PE_TOKEN.
#define PE_TOKEN_ACK_BIT 0x0004u
#define PE_FINISH_ACK_BIT 0x0008u
static u16 pe_ctrl_enables;

void interrupt_commit_pe_token(u16 token) {
    mmio_set_pe_token(token);
    dol_interrupts_set_source(&interrupts, DOL_PI_CAUSE_PE_TOKEN, true);
}

bool interrupt_mmio_contains(u32 ea) {
    return dol_interrupts_mmio_contains(ea) || dol_si_mmio_contains(ea);
}

u64 interrupt_mmio_read(u32 ea, u8 size) {
    if (mmio_pi_fifo_contains(ea))
        return mmio_pi_fifo_read(ea);
    sync_audio_interrupt();
    if (ea == DOL_PE_INTERRUPT_STATUS)
        return pe_ctrl_enables;
    u64 value = dol_si_mmio_contains(ea)
                    ? dol_si_mmio_read(&si, ea, size)
                    : dol_interrupts_mmio_read(&interrupts, ea, size);
    sync_si_interrupt();
    return value;
}

void interrupt_mmio_write(u32 ea, u8 size, u64 value) {
    if (mmio_pi_fifo_contains(ea)) {
        mmio_pi_fifo_write(ea, value);
        return;
    }
    if (ea == DOL_PE_INTERRUPT_STATUS) {
        pe_ctrl_enables = (u16)(value & 0x3u);
        if (value & PE_TOKEN_ACK_BIT)
            dol_interrupts_set_source(&interrupts, DOL_PI_CAUSE_PE_TOKEN, false);
        if (value & PE_FINISH_ACK_BIT)
            dol_interrupts_set_source(&interrupts, DOL_PI_CAUSE_PE_FINISH, false);
        return;
    }
    if (dol_si_mmio_contains(ea))
        dol_si_mmio_write(&si, ea, size, value);
    else
        dol_interrupts_mmio_write(&interrupts, ea, size, value);
    sync_audio_interrupt();
    sync_si_interrupt();
}

void interrupt_set_exi_pending(bool pending) {
    dol_interrupts_set_source(&interrupts, DOL_PI_CAUSE_EXI, pending);
}

void interrupt_set_di_pending(bool pending) {
    dol_interrupts_set_source(&interrupts, DOL_PI_CAUSE_DI, pending);
}

// Save the interrupted CPU state into the current OSContext and vector to the
// recompiled __OSDispatchInterrupt. It dispatches to the VI handler (which acks
// the VI DI register and wakes the retrace waiter), reschedules, and ends in
// OSLoadContext -> rfi, transferring control to the resumed thread.
u32 interrupt_enter_exception(CPUState* cpu) {
    u32 ctx = mem_read32(cpu, OS_CURRENT_CONTEXT);
    if (ctx < GC_RAM_BASE || ctx >= GC_RAM_BASE + cpu->ram_size)
        return 0;  // no current context established yet

    for (int i = 0; i < 32; i++)
        mem_write32(cpu, ctx + (u32)i * 4u, cpu->gpr[i]);
    mem_write32(cpu, ctx + CTX_CR, cpu->cr);
    mem_write32(cpu, ctx + CTX_LR, cpu->lr);
    mem_write32(cpu, ctx + CTX_CTR, cpu->ctr);
    mem_write32(cpu, ctx + CTX_XER, cpu->xer);
    mem_write32(cpu, ctx + CTX_SRR0, cpu->pc);
    mem_write32(cpu, ctx + CTX_SRR1, cpu->msr);
    mem_write16(cpu, ctx + CTX_STATE,
                mem_read16(cpu, ctx + CTX_STATE) | OS_CONTEXT_STATE_EXC);
    for (int i = 0; i < 8; i++)
        mem_write32(cpu, ctx + CTX_GQR + (u32)i * 4u, cpu->gqr[i]);

    cpu->srr0 = cpu->pc;
    cpu->srr1 = cpu->msr;
    // The handler runs as the exception leaves it: external interrupts masked
    // and MSR[FP] clear, so FP use in it goes through the lazy switch.
    cpu->msr &= ~(MSR_EE | MSR_FP);
    return ctx;
}

static void deliver_external(CPUState* cpu) {
    const u32 ctx = interrupt_enter_exception(cpu);
    if (ctx == 0)
        return;
    cpu->gpr[3] = EXC_EXTERNAL;
    cpu->gpr[4] = ctx;
    cpu->pc = DISPATCH_INTERRUPT;
}

// A VI retrace is one work unit in ~350,000, and external-interrupt delivery is
// level-triggered (a pending, unmasked source stays pending until the guest
// acks it). Neither needs to be evaluated on every single guest block. Batching
// the retrace clock + interrupt-delivery work every INTERRUPT_POLL_INTERVAL
// blocks keeps the per-block hot path down to just audio_poll (a counter
// increment that early-returns on all but every ~5250th call). The retrace
// timing error is bounded by INTERVAL/frame_work_units (~0.037% at 128/350000),
// far below one guest frame. Audio stays per-block because its chunk cadence is
// driven by audio_poll's own work counter, not by this batched clock.
// (INTERRUPT_POLL_INTERVAL + interrupt_poll_accum declared once above.)

void interrupt_poll(CPUState* cpu) {
    if (!wall_clock)
        audio_poll(cpu);

    if (++interrupt_poll_accum < INTERRUPT_POLL_INTERVAL)
        return;
    const u64 units = interrupt_poll_accum;
    interrupt_poll_accum = 0;
    u64 vi_units = units;
    if (wall_clock) {
        const u64 now = wall_ns();
        u64 elapsed = now - wall_last_ns;
        wall_last_ns = now;
        if (elapsed > WALL_MAX_STEP_NS)
            elapsed = WALL_MAX_STEP_NS;
        wall_vi_rem += elapsed * 3u;
        vi_units = wall_vi_rem / 50u;
        wall_vi_rem %= 50u;
        wall_audio_rem += elapsed * audio_chunks_per_second();
        while (wall_audio_rem >= 1000000000ull) {
            wall_audio_rem -= 1000000000ull;
            audio_poll(cpu);
        }
    }
    if (pe_finish_countdown != 0) {
        pe_finish_countdown = pe_finish_countdown > units
                                  ? pe_finish_countdown - (u32)units
                                  : 0;
        if (pe_finish_countdown == 0)
            interrupt_commit_pe_finish();
    }

    sync_audio_interrupt();
    // Transitional adapter: DolRecomp does not yet report real guest work, so
    // one generated-block dispatch advances one work unit. DolViClock owns the
    // periodic retrace schedule; replacing the feed with instruction/cycle
    // counts will not change this interrupt/device path.
    dol_vi_clock_advance(&vi_clock, vi_units);
    u64 ticks = 0;
    while (dol_vi_clock_pop_retrace(&vi_clock, &ticks)) {
        cpu->timebase += ticks;
        dol_interrupts_assert_vi_retrace(&interrupts);
        speed_note_retrace();
        dol_si_latch_poll(&si, 0xFu);
        sync_si_interrupt();
    }

    // Deliver only when the guest can take an external interrupt and at least one
    // pending source is unmasked in the PI. One delivery drains every pending,
    // unmasked source: __OSDispatchInterrupt loops over the PI cause bits.
    if (!(cpu->msr & MSR_EE))
        return;  // interrupts disabled; stay pending until the guest enables them
    if (!dol_interrupts_external_pending(&interrupts))
        return;  // no pending source is unmasked yet; wait

    if (getenv("ED_IRQ_LOG"))
        fprintf(stderr, "[irq] deliver cause=0x%08X mask=0x%08X pc=0x%08X\n",
                dol_interrupts_pi_cause(&interrupts),
                dol_interrupts_pi_mask(&interrupts), cpu->pc);
    deliver_external(cpu);
}

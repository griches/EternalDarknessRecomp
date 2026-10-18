#ifndef ED_HOST_INTERRUPT_H
#define ED_HOST_INTERRUPT_H

#include "core/cpu.h"

// Faithful external-interrupt delivery for the VI vertical retrace.
//
// The GameCube drives its frame loop from the VI retrace interrupt: the main
// thread blocks in VIWaitForRetrace, and once per frame the VI raises an
// interrupt whose handler wakes it. We model the Processor Interface (PI) and
// the VI display-interrupt registers, and deliver the interrupt by saving the
// interrupted state into the current OSContext and jumping to the recompiled
// __OSDispatchInterrupt -- bypassing the asm vector stub the SDK relocates to
// low memory (which the recompiler has no block for).

void interrupt_init(void);
// True when retraces follow the wall clock (the windowed build), false when
// they are counted in blocks (headless, ED_FRAME_BLOCKS).
bool interrupt_wall_clock(void);

// Fast-forward: run guest time (retraces and audio) at up to speed times the
// wall clock, as fast as the game keeps up, with the sound muted; 1 is normal.
void interrupt_set_speed(u32 speed);

// MMIO delegation for the runtime-owned PI/VI/PE interrupt and SI device models.
bool interrupt_mmio_contains(u32 ea);
u64  interrupt_mmio_read(u32 ea, u8 size);
void interrupt_mmio_write(u32 ea, u8 size, u64 value);
void interrupt_set_exi_pending(bool pending);
void interrupt_set_di_pending(bool pending);

// Handle an FP-unavailable exception the way the OS's OSSwitchFPUContext does:
// save the FPU owner's registers into its context, load the current context's,
// make it the owner, and return to the faulting instruction with MSR[FP] set.
void interrupt_switch_fpu_context(CPUState* cpu);

// Take an external-interrupt-style exception: save the running state into the
// current OSContext (marked as an exception context, so the scheduler resumes
// it intact), mask external interrupts and clear MSR[FP]. Returns the context,
// or 0 before the OS has one.
u32 interrupt_enter_exception(CPUState* cpu);

// Called once per current generated-dispatch work unit. Advances the runtime
// VI clock; when a retrace is due and external interrupts are enabled and
// unmasked, delivers the VI interrupt to the guest.
void interrupt_poll(CPUState* cpu);

// Commit a Pixel Engine "finish" interrupt (PI cause bit 10). Called when a
// thread blocks in GXWaitDrawDone (sleeps on FinishQueue), which only happens
// while a draw-done token is in flight (DrawDone == 0). Committing on the wait
// itself is the faithful trigger and -- unlike arming on the standalone
// GXSetDrawDone -- also covers the inlined GXDrawDone() path (it never calls the
// standalone GXSetDrawDone, so the loading screen used to hang here forever).
void interrupt_commit_pe_finish(void);

// Raise the PE finish interrupt after a short simulated GPU latency, for a
// GXSetDrawDone that the guest does not wait on (see interrupt.c).
void interrupt_defer_pe_finish(void);
// The message queue the game's draw-done callback receives from (blocking,
// inside the interrupt): the deferred finish waits until it holds a message.
void interrupt_set_finish_queue(u32 queue);

// Raise the Pixel Engine draw-sync token interrupt for `token`, as the GPU does
// when it reaches a GXSetDrawSync token. Called from the GXSetDrawSync hook: the
// host GPU has no queue to drain, so the token is reached at once.
void interrupt_commit_pe_token(u16 token);
// GP breakpoint: draw-sync tokens queued while it is armed are held until
// GXEnableBreakPt moves it on (moved) or CP_CTRL switches it off (enable).
void interrupt_gp_breakpoint_moved(void);
void interrupt_gp_breakpoint_enable(bool enabled);

// Quick state: the PI/VI/SI/PE interrupt models and the retrace clock, as a
// fixed-size blob. Loading keeps this process's pacing and restarts the wall
// clock from now.
size_t interrupt_state_size(void);
void interrupt_state_save(void* out);
void interrupt_state_load(const void* in);

#endif // ED_HOST_INTERRUPT_H

#ifndef ED_HOST_AUDIO_H
#define ED_HOST_AUDIO_H

#include "core/cpu.h"

// Strikers game-client audio shell over GXRuntime:
//   - DolAudioDma: AI/DSP-AID device registers + AID chunk drain
//   - DolAxHost: MusyX/AX DSP command-list mixer (B2.2 transitional host path)
// Guest MusyX CPU-side sequencing/voice allocation/streaming/3D emitters stay
// in recompiled code + HLE notify counters (ED_AUDIO_LOG).

void audio_init(void);
void audio_shutdown(void);
void audio_set_vi_clock(u64 work_units_per_frame, u32 refresh_hz);
// Wall-clock pacing: the caller runs audio_poll once per AID chunk due, at
// audio_chunks_per_second, instead of once per block.
void audio_use_wall_clock(void);
u32 audio_chunks_per_second(void);

bool audio_mmio_contains(u32 address);
u64  audio_mmio_read(u32 address, u8 size);
void audio_mmio_write(u32 address, u8 size, u64 value);

// Called once per current generated-dispatch work unit. Consumes 32-byte AI DMA
// chunks at the guest-selected AID cadence and delivers AID through the normal
// PI/external interrupt path.
void audio_poll(CPUState* cpu);
bool audio_interrupt_pending(void);

// AI stream-counter interrupt (PI cause AI): AISCNT passed AIIT and the guest
// unmasked it. The DTK library uses it to follow disc music playback.
bool audio_ai_interrupt_pending(void);

// Observe a CPU-to-DSP mailbox send. MusyX sends a BABE-prefixed command-list
// size followed by the command-list address; the second mail runs the host AX
// mixer synchronously and marks the DSP work complete.
void audio_dsp_mail(CPUState* cpu, u32 mail);

// salInitDsp is replaced at the HLE boundary because no physical DSP task is
// booted. This initializes the guest-visible completion state expected by the
// rest of MusyX.
void audio_skip_dsp_init(CPUState* cpu);

// Quick state: the AI/DSP device and AI stream registers as a fixed-size blob.
size_t audio_state_size(void);
void audio_state_save(void* out);
void audio_state_load(const void* in);

#endif // ED_HOST_AUDIO_H

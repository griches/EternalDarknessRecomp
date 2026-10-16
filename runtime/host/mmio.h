#ifndef MELEERECOMP_MMIO_H
#define MELEERECOMP_MMIO_H

#include "core/cpu.h"

// Hardware register access hooks. The GameCube maps its hardware (VI, GX/CP/PE,
// AI, DSP, DI/DVD, SI, EXI, PI) into the 0xCC00_xxxx range. Compiled game code
// reaches those addresses through CPUState::external_read / external_write
// because they fall outside the 24 MB of guest RAM.
//
// This is the scaffold: reads return 0 and writes are recorded but otherwise
// ignored. Real behavior is the job of the future Aurora HLE layer. Install the
// hooks with mmio_install().
bool mmio_install(CPUState* cpu);
void mmio_shutdown(void);

// Enable/disable per-access logging (off by default to keep boot output quiet).
void mmio_set_logging(bool enabled);
// Latch a draw-sync token in the PE token register, as the GPU does on
// reaching a GXSetDrawSync token.
void mmio_set_pe_token(u16 token);
// False while the CPU FIFO points somewhere other than the GPU's FIFO: the game
// is recording a display list or using the gather pipe as a fast copy.
bool mmio_cpu_fifo_feeds_gpu(void);
// The write-gather pipe was re-pointed (PPCMtwpar): drop its pending bytes.
void mmio_write_gather_reset(void);
// A store to the write-gather pipe from the generated code (generated.h).
void dolrecomp_wgp_write(CPUState* cpu, u32 ea, u64 value, u8 size);

// The Processor Interface CPU-FIFO registers (0xCC00300C-0xCC003017), owned
// here because the gather pipe writes through them (see mmio.c).
bool mmio_pi_fifo_contains(u32 ea);
u64 mmio_pi_fifo_read(u32 ea);
void mmio_pi_fifo_write(u32 ea, u64 value);

// ED_FIFO_DUMP diagnostics: log a gather-pipe write, or a marker
// ("present", "call-display-list address size").
void mmio_fifo_dump_write(const CPUState* cpu, u8 size, u64 value);
void mmio_fifo_dump_note(const char* what, u32 a, u32 b);

void mmio_set_disc_present(bool present);

// Translate a guest address backed by the runtime into a stable host pointer.
// `available` receives the contiguous byte count remaining in that region.
void* mmio_guest_pointer(CPUState* cpu, u32 address, u32* available);

// Copy raw guest bytes between host-backed regions, falling back to byte loads
// and stores when either side is not a single contiguous host pointer.
void mmio_guest_copy(CPUState* cpu, u32 dest, u32 src, u32 bytes);

// EFB software backing (GXPeek/Poke MMIO window). Host may fill from GPU
// readback via mmio_efb() + dol_efb_access_fill_color_rgba8.
struct DolEfbAccess;
struct DolEfbAccess* mmio_efb(void);

// Wire continuous present-source readback → software EFB (Aurora builds only).
void mmio_attach_efb_readback(void);

// Quick state: the device registers modelled here (EXI, DI, CP, PE, MI, EFB
// access, PI CPU FIFO, gather pipe) as a fixed-size blob, and the guest memory
// regions this module owns besides MEM1 (saved verbatim by quickstate.c).
size_t mmio_state_size(void);
void mmio_state_save(void* out);
void mmio_state_load(const void* in);
u8* mmio_locked_cache(u32* size);
u8* mmio_vm_memory(u32* size);

#endif /* MELEERECOMP_MMIO_H */

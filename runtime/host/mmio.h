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

#endif /* MELEERECOMP_MMIO_H */

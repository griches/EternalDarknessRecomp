#ifndef ED_HOST_QUICKSTATE_H
#define ED_HOST_QUICKSTATE_H

#include "core/cpu.h"

// Quick save / quick load: one file holding the whole emulated machine (CPU
// registers, MEM1, ARAM, the locked cache and every host device model), so a
// tester can skip the slow prologue. In the windowed build keyboard 2 saves and
// 1 loads; ED_QUICKSTATE=path picks the file (default build/runs/quick.state).
// A state loads in a later run as well as the one that saved it.
//
// Test triggers (headless runs):
//   ED_QUICKSAVE_AT_BLOCK=N   save to the ED_QUICKSTATE file once the run
//                             reaches block N (0: right after a boot load)
//   ED_QUICKLOAD=path         load path at boot, or at ED_QUICKLOAD_AT_BLOCK=N
//
// Requests are carried out between blocks, and in the windowed build only once
// the GX FIFO decoder is between commands (see dol_aurora_gx_idle).

void quickstate_init(void);

// Turn key presses and the test triggers into requests. Cheap; call it every
// few thousand blocks.
void quickstate_poll(unsigned long long blocks);

// Queue a save to / load from a given file (the in-game menu's slots).
void quickstate_request_save(const char* path);
void quickstate_request_load(const char* path);

// The machine as this run booted it (saved before any boot load), for a New
// game without restarting; NULL until it is saved.
const char* quickstate_boot_path(void);

// The game restarting itself (OSResetSystem, hle.c): the machine as it booted,
// with guest memory [start, end) kept as it is now (the game's
// OSSetSaveRegion; start 0 keeps nothing), and the reboot flag OSGetResetCode
// reads set when `rebooted` (a restart; a hot reset leaves it clear). False
// when there is no boot state.
bool quickstate_request_reboot(CPUState* cpu, u32 start, u32 end, bool rebooted);

// Nonzero while a save or load waits for a safe point.
extern int g_quickstate_pending;

// Carry out pending requests if the machine is at a safe point. Returns true
// after a load, which replaced cpu->pc: restart the dispatch iteration.
bool quickstate_service(CPUState* cpu);

#endif // ED_HOST_QUICKSTATE_H

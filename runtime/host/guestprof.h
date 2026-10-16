#pragma once
// ED_GUEST_PROF=1 (debug): a statistical profile of the guest code
// (guestprof.c). In the host, not main.c: the game half is built on the
// player's machine (the Mac app) without SDL's headers.
#include "core/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif
// Start the sampler thread on this CPU.
void guest_prof_start(const CPUState* cpu);
#ifdef __cplusplus
}
#endif

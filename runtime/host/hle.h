#ifndef ED_HLE_H
#define ED_HLE_H

#include "core/cpu.h"

// High-level-emulation intercept point.
//
// The dispatcher calls ppc_host_call(cpu, pc) before running compiled code for
// an address (see dolrecomp_call in the generated header). If a hook returns
// true, the compiled function for that address is skipped entirely and the host
// is expected to have produced the same observable effect (set return registers
// and advanced pc, typically to the link register).
//
void hle_install(CPUState* cpu);

// B2.1: dump MusyX API call counters (no-op unless ED_AUDIO_LOG is set).
void hle_audio_api_report(void);

// Configure the persistent slot-A card before hle_install. Failure leaves the
// old no-card behavior available so the game can still continue without saves.
bool hle_card_open(const char* path);
void hle_card_close(void);

// Inject one queued asynchronous SDK completion callback into guest execution.
// The callback runs with a saved architectural context and returns through a
// host trampoline, matching an interrupt-driven SDK callback rather than
// recursively running it inside the intercepted CARD call.
bool hle_poll_callback(CPUState* cpu);

#endif /* ED_HLE_H */

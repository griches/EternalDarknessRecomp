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
// Skip the intro movies (or not) from the next boot state on: the launcher's
// New game without restarting the process.
void hle_set_skip_intro(bool on);
// Once a game frame, at VIFlush (Aurora builds): FPS in the title and the
// in-game menu. Needs the window, so with the GX thread the backend calls it on
// the main thread instead (dol_aurora_set_frame_hook).
void hle_frame_boundary(void);

// B2.1: dump MusyX API call counters (no-op unless ED_AUDIO_LOG is set).
void hle_audio_api_report(void);

// Configure the persistent slot-A card before hle_install. Failure leaves the
// old no-card behavior available so the game can still continue without saves.
bool hle_card_open(const char* path);
// A second card in slot B (after hle_card_open; without one the slot is empty).
bool hle_card_open_b(const char* path);
// Both slots.
void hle_card_close(void);

// Inject one queued asynchronous SDK completion callback into guest execution.
// The callback runs with a saved architectural context and returns through a
// host trampoline, matching an interrupt-driven SDK callback rather than
// recursively running it inside the intercepted CARD call.
bool hle_poll_callback(CPUState* cpu);

// Quick state: callback queue and in-flight callback, DVD current directory,
// skip-intro progress and card mount, as a fixed-size blob. The hle_gx_* pair
// covers the windowed build's GX shortcut stages (size 0 headless);
// hle_gx_state_load(NULL) clears them for a state that has none.
size_t hle_state_size(void);
void hle_state_save(void* out);
void hle_state_load(const void* in);
// After a quick load, with guest memory in place: repairs a loaded game that
// an older build left stuck (disc streams waiting on a read that never comes).
void hle_state_loaded(CPUState* cpu);
size_t hle_gx_state_size(void);
void hle_gx_state_save(void* out);
void hle_gx_state_load(const void* in);

#endif /* ED_HLE_H */

#pragma once
// Problem reports for testers, in saves/reports/: what the game was doing
// when it stopped responding (a hang watchdog), stopped by itself, crashed,
// or first failed to draw a frame. A hang or a stop also saves a quick state
// beside the report when it can, and the window says where both are.
#include "core/types.h"

#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Early in main: keep the log's tail in memory (stderr through a pipe) and
// write a report if the process crashes.
void report_init(void);

// Once the game runs (main.c): start the watchdog. No new game frame for
// ED_HANG_SECONDS (default 6) while no menu holds the game is a hang.
void report_start_watchdog(void);

// Each game frame (hle_frame_boundary): the game is alive.
void report_frame(void);

// A menu holds the game (frontend.cpp): no frames, but no hang either.
void report_pause(bool paused);

// Write a report of the given kind ("hang", "stopped", "crash", "gx") with a
// one-line description; with_state also saves a quick state beside it (the
// run loop must still be running guest code). Returns the report's path, or
// NULL if it could not be written.
const char* report_write(const char* kind, const char* what, bool with_state);

// The main thread while it waits for the game (dol_aurora_set_idle_hook):
// shows the hang message once the watchdog has written its report.
void report_idle(void);

// The run loop stopped for a reason other than the window closing: report it
// and show it (main.c, main thread, after the game thread ended).
void report_game_stopped(const char* reason);

// main.c: the guest's registers, recent blocks and OS threads, for a report.
void main_describe_guest(FILE* out);
// main.c: ask the run loop for a [pcdump] in the log at its next block.
void main_request_pcdump(void);

#ifdef __cplusplus
}
#endif

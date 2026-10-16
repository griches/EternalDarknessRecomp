// ED_GUEST_PROF=1 (debug): a statistical profile of the guest code. A thread
// samples the guest pc (the generated code stores it at each block start) and
// the running guest thread every 0.5 ms, and rewrites logs/guestprof.txt every
// 2 s: samples, block pc, guest thread, most first (name the pcs with
// tools/addr2sym.py).
#include "host/guestprof.h"

#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUEST_PROF_SLOTS 65536u
typedef struct { u32 pc, thread, count; } GuestProfSlot;
static GuestProfSlot g_guest_prof[GUEST_PROF_SLOTS];
static GuestProfSlot g_guest_prof_sorted[GUEST_PROF_SLOTS];

static int guest_prof_by_count(const void* a, const void* b) {
    const u32 x = ((const GuestProfSlot*)a)->count, y = ((const GuestProfSlot*)b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}

static int SDLCALL guest_prof_main(void* user) {
    const CPUState* cpu = (const CPUState*)user;
    for (u64 n = 1;; n++) {
        SDL_DelayNS(500000u);
        const u32 pc = *(volatile const u32*)&cpu->pc;
        u32 thread;
        memcpy(&thread, cpu->ram + 0xE4u, 4u);  // OS current thread
        thread = __builtin_bswap32(thread);
        u32 h = ((pc ^ (thread * 0x9E3779B1u)) * 2654435761u) >> 16;
        for (u32 i = 0; i < GUEST_PROF_SLOTS; i++, h = (h + 1u) & (GUEST_PROF_SLOTS - 1u)) {
            GuestProfSlot* slot = &g_guest_prof[h];
            if (slot->count == 0u) {
                slot->pc = pc;
                slot->thread = thread;
            }
            if (slot->pc == pc && slot->thread == thread) {
                slot->count++;
                break;
            }
        }
        if (n % 4000u != 0u)
            continue;
        memcpy(g_guest_prof_sorted, g_guest_prof, sizeof g_guest_prof);
        qsort(g_guest_prof_sorted, GUEST_PROF_SLOTS, sizeof g_guest_prof_sorted[0],
              guest_prof_by_count);
        FILE* f = fopen("logs/guestprof.txt", "w");
        if (f == NULL)
            continue;
        fprintf(f, "# %llu samples (0.5 ms)\n", (unsigned long long)n);
        for (u32 i = 0; i < GUEST_PROF_SLOTS && g_guest_prof_sorted[i].count != 0u; i++)
            fprintf(f, "%u %08X %08X\n", g_guest_prof_sorted[i].count,
                    g_guest_prof_sorted[i].pc, g_guest_prof_sorted[i].thread);
        fclose(f);
    }
    return 0;
}

void guest_prof_start(const CPUState* cpu) {
    SDL_DetachThread(SDL_CreateThread(guest_prof_main, "guest-prof", (void*)cpu));
}

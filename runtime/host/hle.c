// High-level emulation (HLE) dispatch.
//
// The recompiled game runs natively, but GameCube SDK functions that touch
// hardware (GX/VI/PAD/SI/AI/DSP/DI/EXI) or the OS kernel cannot. Those are
// intercepted at their guest addresses. `hle_install` resolves the generated
// SDK names once and builds an address-indexed table; `host_call` then performs
// one constant-time lookup per dispatched block.
//
// Functions in the table without a handler fall through to their recompiled
// implementation (true for pure-computation SDK routines that run fine
// natively); set ED_HLE_LOG=1 to trace which SDK functions the game
// calls and whether each is handled.
#include "hle.h"

#include "gxruntime/aram.h"
#include "gxruntime/memory_card.h"
#include "host/audio.h"
#include "gxruntime/dvd.h"
#include "gxruntime/platform.h"
#include "host/hle_abi.h"
#include "host/interrupt.h"
#include "host/mmio.h"
#include "host/sdk_map.h"
#include "host/compat.h"
#include "host/settings.h"
#ifdef ED_AURORA
#include "gxruntime/aurora_backend.h"
#include "host/frontend.h"
#endif
#include "game_data.h"
#include "host/dtk.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void (*HleHandler)(CPUState* cpu);

typedef struct {
    const char* name;
    HleHandler  fn;
} HleEntry;

typedef struct {
    HleHandler intercept;
    HleHandler notify;
} HleDispatchEntry;

// Trace every intercepted SDK call (and DVD activity). Set by ED_HLE_LOG.
static bool g_hle_log = false;
static bool g_dvd_log = false;  // ED_DVD_LOG: file opens only
static u32 g_trace_fn[16];
static u32 g_trace_fn_count;
// ED_CALL_PROFILE=name: count calls of one function by caller (lr) and print
// the counts once per VIFlush, so per-frame differences show without the
// slowdown of logging every call.
#ifdef ED_AURORA
void VISetWindowTitle(const char* title);  // Aurora (dolphin/vi.h)
#endif
static u32 g_call_prof_fn;
static u32 g_call_prof_vi;
static struct { u32 lr, n; } g_call_prof[64];
static bool g_audio_log = false;
static bool g_graphics_log = false;
static bool g_movie_log = false;
static bool g_movie_cadence_log = false;
static bool g_card_log = false;
static bool g_auto_input = false;
static bool g_mash_to_gameplay = false;
static bool g_mash_side_assigned = false;
static bool g_mash_prematch = false;
static bool g_mash_route_complete = false;
static bool g_auto_skip_card_prompt = false;
static bool g_state_log = false;
static u64 g_auto_input_last_pulse = ~(u64)0;
static u64 g_auto_input_once_frame = 0;
static u64 g_auto_input_once_sent = ~(u64)0;
static u64 g_auto_skip_card_last_pulse = ~(u64)0;
typedef struct {
    u64 start_frame;
    u64 end_frame; // exclusive
    u16 buttons;
    s8 stick_x;
    s8 stick_y;
    u8 analog_a;
} InputScriptEvent;
#define INPUT_SCRIPT_MAX_EVENTS 256u
#define INPUT_SCRIPT_MAX_TEXT   4096u
static InputScriptEvent g_input_script[INPUT_SCRIPT_MAX_EVENTS];
static unsigned g_input_script_count = 0;
static u64 g_input_script_last_log_frame = ~(u64)0;
static u16 g_input_script_last_buttons = 0;
static s8 g_input_script_last_stick_x = 0;
static s8 g_input_script_last_stick_y = 0;
static u8 g_input_script_last_analog_a = 0;
static unsigned g_movie_texobj_log_count = 0;
// Address handlers normally return to the intercepted function's caller.
// Synchronous host operations with a guest completion callback can instead
// tail-call that callback and let its normal `blr` return to the same caller.
static bool g_hle_tail_call = false;
// Set by an intercept that declines the call: the recompiled function runs.
static bool g_hle_passthrough = false;

static char* input_script_trim(char* text) {
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r')
        ++text;
    char* end = text + strlen(text);
    while (end > text) {
        char c = end[-1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        *--end = '\0';
    }
    return text;
}

static void parse_input_script(const char* text) {
    g_input_script_count = 0;
    g_input_script_last_log_frame = ~(u64)0;
    g_input_script_last_buttons = 0;
    g_input_script_last_stick_x = 0;
    g_input_script_last_stick_y = 0;
    g_input_script_last_analog_a = 0;
    if (text == NULL || text[0] == '\0')
        return;
    char buffer[INPUT_SCRIPT_MAX_TEXT];
    strncpy(buffer, text, sizeof buffer - 1u);
    buffer[sizeof buffer - 1u] = '\0';
    unsigned skipped_events = 0;
    for (char* event = buffer; event != NULL && *event != '\0';) {
        char* next_event = strchr(event, ';');
        if (next_event != NULL) {
            *next_event = '\0';
            ++next_event;
        }
        event = input_script_trim(event);
        if (*event == '\0') {
            event = next_event;
            continue;
        }
        char* fields[6] = {0};
        unsigned field_count = 0;
        bool too_many_fields = false;
        for (char* field = event; field != NULL;) {
            char* next_field = strchr(field, ',');
            if (next_field != NULL) {
                *next_field = '\0';
                ++next_field;
            }
            if (field_count < 6u) {
                fields[field_count++] = input_script_trim(field);
            } else {
                too_many_fields = true;
            }
            field = next_field;
        }
        if (field_count < 3u || too_many_fields ||
            g_input_script_count >= INPUT_SCRIPT_MAX_EVENTS) {
            ++skipped_events;
            event = next_event;
            continue;
        }
        InputScriptEvent parsed;
        memset(&parsed, 0, sizeof parsed);
        parsed.start_frame = strtoull(fields[0], NULL, 0);
        parsed.end_frame = strtoull(fields[1], NULL, 0);
        parsed.buttons = (u16)strtoul(fields[2], NULL, 0);
        parsed.stick_x = field_count >= 4u ? (s8)strtol(fields[3], NULL, 0) : 0;
        parsed.stick_y = field_count >= 5u ? (s8)strtol(fields[4], NULL, 0) : 0;
        parsed.analog_a =
            field_count >= 6u ? (u8)strtoul(fields[5], NULL, 0)
                              : ((parsed.buttons & 0x0100u) ? 0xFFu : 0u);
        if (parsed.end_frame <= parsed.start_frame)
            parsed.end_frame = parsed.start_frame + 1u;
        if (parsed.buttons == 0u && parsed.stick_x == 0 && parsed.stick_y == 0 &&
            parsed.analog_a == 0u) {
            ++skipped_events;
            event = next_event;
            continue;
        }
        g_input_script[g_input_script_count++] = parsed;
        event = next_event;
    }
    fprintf(stderr, "[input] script events=%u skipped=%u\n",
            g_input_script_count, skipped_events);
}

// ED_SKIP_INTRO=1 (testing): tap A every 16 frames to skip the intro movies,
// and stop once they are over, leaving the pad to the player for the language
// and memory-card prompts and the title. Two ways the intro ends: fmv0001 plays
// and a non-movie file opens after it, or A on the Poe quote skips the movies
// and the title loads /UBookInv.tpl after the second /UMemcardText.bin open
// (boot opens both once before that).
static bool g_skip_intro;
static bool g_skip_intro_done;
static bool g_skip_intro_armed;
static unsigned g_skip_intro_memcard_text;
static u64 g_skip_intro_start_frame;  // first frame it ran (0: not yet)

// The launcher's New game in a running process (frontend.cpp): skip the
// intro movies or not, as at boot (the boot state it loads resets the rest).
void hle_set_skip_intro(bool on) {
    g_skip_intro = on;
    g_skip_intro_done = false;
    g_skip_intro_armed = false;
    g_skip_intro_memcard_text = 0;
    g_skip_intro_start_frame = 0;
}

static void skip_intro_note_open(const CPUState* cpu, const char* path) {
    if (!g_skip_intro || g_skip_intro_done)
        return;
    if (strstr(path, "/UMemcardText.bin"))
        g_skip_intro_memcard_text++;
    if (strstr(path, "/fmv/fmv0001."))
        g_skip_intro_armed = true;
    else if ((g_skip_intro_armed && !strstr(path, "/fmv/")) ||
             (g_skip_intro_memcard_text >= 2u &&
              strstr(path, "/UBookInv.tpl"))) {
        g_skip_intro_done = true;
        fprintf(stderr,
                "[input] opening movie over (%s) at frame %llu; skip-intro off\n",
                path, (unsigned long long)(cpu->timebase / 675000ull));
    }
}

// The intro's end is spotted from the files it opens, and a New game from the
// launcher does not always open them in that order: A was then tapped on
// through play (the gun fired over and over, Steam Deck 2026-10-16). So the
// player taking over (a button, or the main stick pushed) also ends it, and
// so does a minute and a half of tapping.
static bool skip_intro_apply(CPUState* cpu, u16* buttons, s8 stick_x, s8 stick_y,
                             u8* analog_a) {
    if (!g_skip_intro || g_skip_intro_done)
        return false;
    const u64 frame = cpu->timebase / 675000ull;
    if (g_skip_intro_start_frame == 0u || frame < g_skip_intro_start_frame)
        g_skip_intro_start_frame = frame != 0u ? frame : 1u;
    const u64 elapsed = frame - g_skip_intro_start_frame;
    const char* why = NULL;
    // Ignore the first frames: the launcher's A may still be held.
    if (elapsed > 20u && (*buttons != 0u || abs(stick_x) > 48 || abs(stick_y) > 48))
        why = "the player took over";
    else if (elapsed > 90u * 60u)
        why = "90 s";
    if (why != NULL) {
        g_skip_intro_done = true;
        fprintf(stderr, "[input] skip-intro off at frame %llu (%s)\n",
                (unsigned long long)frame, why);
        return false;
    }
    if (frame < 30u || (frame % 16u) >= 6u)
        return false;
    *buttons |= 0x0100u;
    *analog_a = 0xFFu;
    return true;
}

static bool input_script_apply(CPUState* cpu,
                               u16* buttons,
                               s8* stick_x,
                               s8* stick_y,
                               u8* analog_a) {
    if (g_input_script_count == 0u)
        return false;
    const u64 frame = cpu->timebase / 675000ull;
    bool active = false;
    for (unsigned i = 0; i < g_input_script_count; ++i) {
        const InputScriptEvent* event = &g_input_script[i];
        if (frame < event->start_frame || frame >= event->end_frame)
            continue;
        *buttons |= event->buttons;
        if (event->stick_x != 0)
            *stick_x = event->stick_x;
        if (event->stick_y != 0)
            *stick_y = event->stick_y;
        if (event->analog_a != 0u)
            *analog_a = event->analog_a;
        active = true;
    }
    if (active) {
        const bool continuation =
            g_input_script_last_log_frame + 1u == frame &&
            g_input_script_last_buttons == *buttons &&
            g_input_script_last_stick_x == *stick_x &&
            g_input_script_last_stick_y == *stick_y &&
            g_input_script_last_analog_a == *analog_a;
        g_input_script_last_log_frame = frame;
        g_input_script_last_buttons = *buttons;
        g_input_script_last_stick_x = *stick_x;
        g_input_script_last_stick_y = *stick_y;
        g_input_script_last_analog_a = *analog_a;
        if (continuation)
            return true;
        fprintf(stderr,
                "[input] script frame=%llu buttons=0x%04X stick=%d,%d analog_a=%u\n",
                (unsigned long long)frame, *buttons, *stick_x, *stick_y,
                (unsigned)*analog_a);
    }
    return active;
}

// Repeated edges are intentional: each front-end screen may begin polling at
// a different time, and most of them consume JustPressed rather than Held.
static bool mash_to_gameplay_apply(CPUState* cpu,
                                   u16* buttons,
                                   s8* stick_x,
                                   u8* analog_a) {
    if (!g_mash_to_gameplay || g_mash_route_complete)
        return false;
    // TEMP DIAG (G013): once prematch is reached, go neutral instead of mashing
    // A so the in-engine prematch camera pan cutscene actually PLAYS (mashing A
    // skips it). Lets a live run observe the cutscene's gxcore gap counters.
    if (g_mash_prematch) {
        static int s_cutscene_diag = -1;
        if (s_cutscene_diag < 0)
            s_cutscene_diag = getenv("ED_CUTSCENE_DIAG") != NULL ? 1 : 0;
        if (s_cutscene_diag)
            return false;
    }
    const u64 frame = cpu->timebase / 675000ull;
    if ((frame % 16u) >= 8u)
        return false;
    *buttons |= 0x0100u; // PAD_BUTTON_A
    if (!g_mash_prematch)
        *buttons |= 0x1000u; // PAD_BUTTON_START
    if (!g_mash_side_assigned) {
        *buttons |= 0x0001u; // PAD_BUTTON_LEFT
        *stick_x = -127;
    }
    *analog_a = 0xFFu;
    return true;
}

#ifdef ED_AURORA
#define HLE_DISPLAY_LIST_RETURN 0x7FFF0010u
#define HLE_GX_BEGIN_RETURN     0x7FFF0020u
static u32 g_gx_dirty_state_helper;  // __GXSetDirtyState
static u32 g_gx_flush_prim_helper;   // __GXSendFlushPrim
#define GX_DATA_SDA_OFFSET      ED_GX_DATA_SDA_OFFSET
#define GX_DIRTY_STATE_OFFSET   ED_GX_DIRTY_STATE_OFFSET

typedef struct {
    bool active;
    u8 stage;
    u32 caller;
    u32 address;
    u32 size;
} HleDisplayListCall;

static HleDisplayListCall g_display_list_call;

typedef struct {
    bool active;
    u8 stage;
    u8 primitive;
    u8 vtxfmt;
    u16 vertex_count;
    u32 caller;
} HleGxBegin;

static HleGxBegin g_gx_begin;
#endif

// CARD API completions are asynchronous on GameCube. Performing host storage
// synchronously is fine, but invoking a guest callback recursively from the
// intercepted API is not: Strikers checks the API's immediate return before
// the callback is allowed to advance its state machine. Queue completions and
// run them on the next dispatch with an interrupt-style saved register context.
#define HLE_CALLBACK_RETURN 0x7FFF0000u
#define HLE_CALLBACK_QUEUE_CAPACITY 32u

typedef struct {
    u32 address;
    s32 channel;
    s32 result;
} HlePendingCallback;

typedef struct {
    u32 gpr[32];
    f64 fpr[32];
    f64 ps1[32];
    u32 pc;
    u32 lr;
    u32 ctr;
    u32 cr;
    u32 xer;
    u32 fpscr;
    u32 msr;
    u32 srr0;
    u32 srr1;
    u32 dar;
    u32 dsisr;
    u32 ear;
    u32 hid2;
    u32 sr[16];
    u32 gqr[8];
    u32 exception;
    u32 program_exception;
    u32 reserve_addr;
    bool reserve_valid;
} HleSavedContext;

static HlePendingCallback g_callback_queue[HLE_CALLBACK_QUEUE_CAPACITY];
static u32 g_callback_read;
static u32 g_callback_count;
static bool g_callback_active;
static HleSavedContext g_callback_context;
static DolMemoryCard* g_memory_card;

// A host DVD read is done the moment it is asked for, but its completion
// callback waits until the thread that asked has moved past the call, as it
// always has on a drive. Eternal Darkness's disc streams (voices, effects) call
// DVDReadAsyncPrio and only then mark the read in flight; the callback clears
// the mark. Run on the call's return, before that store, it left the mark set
// for good: the stream never read again (its buffer replayed, a door's sound
// repeating) and never closed (picking up a book waited for it forever).
// Due once that thread has run DVD_CALLBACK_VISITS dispatch-loop visits of its
// own, or sleeps (it got to its wait). A read asked for with interrupts off is
// due at once: nothing lands before they are back on. (A fixed 2 ms instead
// held the chained 4 KB reads of the game's loader to 8 a frame.) A thread
// switched out in the window (a retrace waking main, which outranks the stream
// feeder) is ready but not past its store, and on the Steam Deck heavy scenes
// keep it off the CPU for over a second; so the give-up only breaks a deadlock
// (a higher-priority thread spinning on that callback) and is long. Not in the
// quick state: after a load every queued callback waits for the thread running
// then (hle_state_loaded). Callbacks stay in order, as a drive completes them.
#define DVD_CALLBACK_VISITS 64u
#define DVD_CALLBACK_GIVE_UP_NS 1000000000ull
#define DVD_CALLBACK_GIVE_UP_VISITS (1ull << 27)  // block-paced: ~1 s of 2M-block frames
#define OS_CURRENT_THREAD 0x800000E4u
#define OS_THREAD_STATE 0x2C8u                     // u16: 1 ready, 2 running
typedef struct {
    u32 thread;  // OSThread* that asked, 0 = due at once
    u32 visits;  // its own dispatch-loop visits since
    u64 queued_ns;
    u64 queued_visit;
} HleCallbackWait;
static HleCallbackWait g_callback_wait[HLE_CALLBACK_QUEUE_CAPACITY];
static u64 g_callback_visits;

static u64 host_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (u64)now.tv_sec * 1000000000ull + (u64)now.tv_nsec;
}

static void delay_callback(CPUState* cpu, u32 index) {
    HleCallbackWait* w = &g_callback_wait[index];
    w->thread = (cpu->msr & 0x8000u) ? mem_read32(cpu, OS_CURRENT_THREAD) : 0u;
    w->visits = 0;
    w->queued_ns = host_ns();
    w->queued_visit = g_callback_visits;
}

static bool callback_due(CPUState* cpu, const HleCallbackWait* w) {
    if (w->thread == 0u || w->visits >= DVD_CALLBACK_VISITS)
        return true;
    const u16 state = mem_read16(cpu, w->thread + OS_THREAD_STATE);
    if (state != 1u && state != 2u)
        return true;  // waiting (or gone): past the call
    return interrupt_wall_clock()
               ? host_ns() - w->queued_ns >= DVD_CALLBACK_GIVE_UP_NS
               : g_callback_visits - w->queued_visit >= DVD_CALLBACK_GIVE_UP_VISITS;
}

static void save_callback_context(HleSavedContext* saved,
                                  const CPUState* cpu) {
    memcpy(saved->gpr, cpu->gpr, sizeof saved->gpr);
    memcpy(saved->fpr, cpu->fpr, sizeof saved->fpr);
    memcpy(saved->ps1, cpu->ps1, sizeof saved->ps1);
    saved->pc = cpu->pc;
    saved->lr = cpu->lr;
    saved->ctr = cpu->ctr;
    saved->cr = cpu->cr;
    saved->xer = cpu->xer;
    saved->fpscr = cpu->fpscr;
    saved->msr = cpu->msr;
    saved->srr0 = cpu->srr0;
    saved->srr1 = cpu->srr1;
    saved->dar = cpu->dar;
    saved->dsisr = cpu->dsisr;
    saved->ear = cpu->ear;
    saved->hid2 = cpu->hid2;
    memcpy(saved->sr, cpu->sr, sizeof saved->sr);
    memcpy(saved->gqr, cpu->gqr, sizeof saved->gqr);
    saved->exception = cpu->exception;
    saved->program_exception = cpu->program_exception;
    saved->reserve_addr = cpu->reserve_addr;
    saved->reserve_valid = cpu->reserve_valid;
}

static void restore_callback_context(CPUState* cpu,
                                     const HleSavedContext* saved) {
    memcpy(cpu->gpr, saved->gpr, sizeof saved->gpr);
    memcpy(cpu->fpr, saved->fpr, sizeof saved->fpr);
    memcpy(cpu->ps1, saved->ps1, sizeof saved->ps1);
    cpu->pc = saved->pc;
    cpu->lr = saved->lr;
    cpu->ctr = saved->ctr;
    cpu->cr = saved->cr;
    cpu->xer = saved->xer;
    cpu->fpscr = saved->fpscr;
    cpu->msr = saved->msr;
    cpu->srr0 = saved->srr0;
    cpu->srr1 = saved->srr1;
    cpu->dar = saved->dar;
    cpu->dsisr = saved->dsisr;
    cpu->ear = saved->ear;
    cpu->hid2 = saved->hid2;
    memcpy(cpu->sr, saved->sr, sizeof saved->sr);
    memcpy(cpu->gqr, saved->gqr, sizeof saved->gqr);
    cpu->exception = saved->exception;
    cpu->program_exception = saved->program_exception;
    cpu->reserve_addr = saved->reserve_addr;
    cpu->reserve_valid = saved->reserve_valid;
}

static bool queue_guest_callback_after(CPUState* cpu, u32 address, s32 channel,
                                       s32 result) {
    if (address == 0)
        return true;
    if (g_callback_count == HLE_CALLBACK_QUEUE_CAPACITY) {
        fprintf(stderr, "[card] completion callback queue overflow\n");
        return false;
    }
    u32 index =
        (g_callback_read + g_callback_count) % HLE_CALLBACK_QUEUE_CAPACITY;
    g_callback_queue[index].address = address;
    g_callback_queue[index].channel = channel;
    g_callback_queue[index].result = result;
    g_callback_wait[index].thread = 0u;
    if (cpu != NULL)
        delay_callback(cpu, index);
    g_callback_count++;
    if (g_card_log)
        fprintf(stderr,
                "[card] queued callback=0x%08X channel=%d result=%d depth=%u\n",
                address, channel, result, g_callback_count);
    return true;
}

static bool queue_guest_callback(u32 address, s32 channel, s32 result) {
    return queue_guest_callback_after(NULL, address, channel, result);
}

// A completion callback runs the way the SDK's interrupt dispatcher runs device
// handlers (__OSDispatchInterrupt): from an exception context with the scheduler
// held off, then OSEnableScheduler, __OSReschedule and OSLoadContext. A callback
// that wakes a thread (OSSendMessage from a DVD completion, say) therefore
// switches threads only once it has finished, never from inside the call.
// When __OSReschedule does switch, the frame is gone for good, as on the
// hardware: SelectThread does not save an exception context (its thread later
// resumes where the callback interrupted it), and from the idle loop there is
// no thread to save. So the callback is over once __OSReschedule starts; the
// stage alone recognises a return from it. Left waiting for that return, every
// later callback queued behind it (a DVD read finishing while the game idled
// woke the main thread, and the book's movie never got its next read).
static struct {
    u32 stage;
    u32 context;
    HlePendingCallback pending;
} g_irq_callback;

static void continue_irq_callback(CPUState* cpu) {
    switch (g_irq_callback.stage++) {
    case 1:
        cpu->gpr[3] = (u32)g_irq_callback.pending.channel;
        cpu->gpr[4] = (u32)g_irq_callback.pending.result;
        cpu->pc = g_irq_callback.pending.address;
        cpu->lr = HLE_CALLBACK_RETURN;
        if (g_card_log)
            fprintf(stderr, "[card] dispatch callback=0x%08X channel=%d result=%d\n",
                    g_irq_callback.pending.address, g_irq_callback.pending.channel,
                    g_irq_callback.pending.result);
        break;
    case 2:
        cpu->pc = sdk_symbol_address("OSEnableScheduler");
        cpu->lr = HLE_CALLBACK_RETURN;
        break;
    case 3:
        cpu->pc = sdk_symbol_address("__OSReschedule");
        cpu->lr = HLE_CALLBACK_RETURN;
        g_callback_active = false;  // EE stays off until it returns or switches
        break;
    default:
        cpu->gpr[3] = g_irq_callback.context;
        cpu->pc = sdk_symbol_address("OSLoadContext");
        g_irq_callback.stage = 0;
        g_callback_active = false;
        break;
    }
}

bool hle_poll_callback(CPUState* cpu) {
    g_callback_visits++;
    if (cpu == NULL || g_callback_count == 0 ||
        !(cpu->msr & 0x8000u))  // MSR[EE]: only where an interrupt could land
        return false;
    const u32 current = mem_read32(cpu, OS_CURRENT_THREAD);
    for (u32 i = 0; i < g_callback_count; i++) {
        HleCallbackWait* w = &g_callback_wait[(g_callback_read + i) % HLE_CALLBACK_QUEUE_CAPACITY];
        if (w->thread == current && w->visits < DVD_CALLBACK_VISITS)
            w->visits++;
    }
    if (g_callback_active || !callback_due(cpu, &g_callback_wait[g_callback_read]))
        return false;  // a DVD read its thread has not moved past (DVD_CALLBACK_VISITS)
    const u32 context = interrupt_enter_exception(cpu);
    if (context == 0)
        return false;

    g_irq_callback.pending = g_callback_queue[g_callback_read];
    g_callback_read = (g_callback_read + 1u) % HLE_CALLBACK_QUEUE_CAPACITY;
    g_callback_count--;
    g_irq_callback.context = context;
    g_irq_callback.stage = 1;
    g_callback_active = true;
    cpu->pc = sdk_symbol_address("OSDisableScheduler");
    cpu->lr = HLE_CALLBACK_RETURN;
    return true;
}

// ---------------------------------------------------------------------------
// Guest memory helpers
// ---------------------------------------------------------------------------
char* hle_read_cstr(CPUState* c, u32 gaddr, char* buf, size_t cap) {
    size_t i = 0;
    if (gaddr != 0) {
        for (; i + 1 < cap; i++) {
            u8 ch = mem_read8(c, gaddr + (u32)i);
            if (ch == 0)
                break;
            buf[i] = (char)ch;
        }
    }
    buf[i] = '\0';
    return buf;
}

static bool copy_guest_to_host(CPUState* cpu, u32 guest_address, void* output,
                               u32 length) {
    if (length == 0)
        return true;
    u32 available = 0;
    const void* direct =
        mmio_guest_pointer(cpu, guest_address, &available);
    if (direct != NULL && available >= length) {
        memcpy(output, direct, length);
        return true;
    }
    u8* bytes = (u8*)output;
    for (u32 i = 0; i < length; i++)
        bytes[i] = mem_read8(cpu, guest_address + i);
    return true;
}

static bool copy_host_to_guest(CPUState* cpu, u32 guest_address,
                               const void* input, u32 length) {
    if (length == 0)
        return true;
    u32 available = 0;
    void* direct = mmio_guest_pointer(cpu, guest_address, &available);
    if (direct != NULL && available >= length) {
        memcpy(direct, input, length);
        return true;
    }
    const u8* bytes = (const u8*)input;
    for (u32 i = 0; i < length; i++)
        mem_write8(cpu, guest_address + i, bytes[i]);
    return true;
}

// ---------------------------------------------------------------------------
// OS handlers
// ---------------------------------------------------------------------------

// OSReport(const char* fmt, ...) — the game's own diagnostic logging. Aurora
// does not provide it, so reformat against the EABI varargs here. This is the
// most valuable early intercept: it surfaces what the boot code is doing.
static void hle_OSReport(CPUState* cpu) {
    char fmt[1024];
    hle_read_cstr(cpu, cpu->gpr[3], fmt, sizeof fmt);

    char out[2048];
    size_t oi = 0;
    unsigned iarg = 1;  // next integer vararg -> r4
    unsigned farg = 1;  // next float vararg   -> f2 (f1 unused for varargs)

    for (size_t i = 0; fmt[i] && oi + 1 < sizeof out;) {
        if (fmt[i] != '%') {
            out[oi++] = fmt[i++];
            continue;
        }
        // Rebuild a single conversion spec, dropping length modifiers so the
        // host snprintf matches the value type we pass (always 32-bit / double).
        char spec[32];
        size_t si = 0;
        spec[si++] = fmt[i++];
        while (fmt[i] && si + 2 < sizeof spec && strchr("-+ #0123456789.", fmt[i]))
            spec[si++] = fmt[i++];
        while (fmt[i] && strchr("lhLzjt", fmt[i]))
            i++;  // skip length modifier
        char conv = fmt[i] ? fmt[i++] : '\0';
        spec[si++] = conv;
        spec[si] = '\0';

        char tmp[576];
        tmp[0] = '\0';
        switch (conv) {
        case '%':
            tmp[0] = '%';
            tmp[1] = '\0';
            break;
        case 'd': case 'i':
            snprintf(tmp, sizeof tmp, spec, (int)hle_arg_u32(cpu, iarg++));
            break;
        case 'u': case 'x': case 'X': case 'o': case 'c':
            snprintf(tmp, sizeof tmp, spec, (unsigned)hle_arg_u32(cpu, iarg++));
            break;
        case 'p':
            snprintf(tmp, sizeof tmp, "0x%08X", hle_arg_u32(cpu, iarg++));
            break;
        case 's': {
            char s[512];
            hle_read_cstr(cpu, hle_arg_u32(cpu, iarg++), s, sizeof s);
            snprintf(tmp, sizeof tmp, spec, s);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            snprintf(tmp, sizeof tmp, spec, hle_arg_f64(cpu, farg++));
            break;
        default:
            snprintf(tmp, sizeof tmp, "%s", spec);
            break;
        }
        for (size_t k = 0; tmp[k] && oi + 1 < sizeof out;)
            out[oi++] = tmp[k++];
    }
    out[oi] = '\0';
    fputs(out, stdout);
    fflush(stdout);
}

// A handler that does nothing and returns to the caller. Used to skip recompiled
// SDK routines whose only job is to drive hardware the host owns instead.
static void hle_noop(CPUState* cpu) { (void)cpu; }

// Skip a recompiled routine and report success (BOOL/int return of 1). For
// init functions whose hardware bring-up the host replaces but whose callers
// branch on a success return.
static void hle_return_true(CPUState* cpu) { hle_set_u32(cpu, 1); }
static void hle_return_zero(CPUState* cpu) { hle_set_u32(cpu, 0); }

static u32 lc_transaction_count(u32 bytes) {
    const u32 blocks = (bytes + 31u) / 32u;
    return (blocks + 127u) / 128u;
}

static void hle_LCStoreBlocks(CPUState* cpu) {
    const u32 dest = hle_arg_u32(cpu, 0);
    const u32 src = hle_arg_u32(cpu, 1);
    const u32 blocks = hle_arg_u32(cpu, 2) ? hle_arg_u32(cpu, 2) : 128u;
    mmio_guest_copy(cpu, dest, src, blocks * 32u);
}

// LCLoadBlocks(lcDest, ramSrc, blocks): RAM -> locked cache. The recompiled
// routine only writes DMA_U/DMA_L, which start no transfer here, so without
// this the cache kept stale bytes and LCStoreBlocks copied them out. The game
// moves its per-frame character display lists between buffers this way; the
// stale copies were zero or an old list, so characters vanished on some frames.
static void hle_LCLoadBlocks(CPUState* cpu) {
    const u32 dest = hle_arg_u32(cpu, 0);
    const u32 src = hle_arg_u32(cpu, 1);
    const u32 blocks = hle_arg_u32(cpu, 2) ? hle_arg_u32(cpu, 2) : 128u;
    mmio_guest_copy(cpu, dest, src, blocks * 32u);
}

static void hle_LCStoreData(CPUState* cpu) {
    const u32 dest = hle_arg_u32(cpu, 0);
    const u32 src = hle_arg_u32(cpu, 1);
    const u32 bytes = hle_arg_u32(cpu, 2);
    mmio_guest_copy(cpu, dest, src, bytes);
    hle_set_u32(cpu, lc_transaction_count(bytes));
}

// Desktop hosts have no GameCube chassis reset switch. Controller-based
// Start+L+R soft reset remains implemented by the game's own input path.
static void hle_OSGetResetButtonState(CPUState* cpu) {
    hle_set_u32(cpu, 0u);
}

static void hle_salInitDsp(CPUState* cpu) {
    audio_skip_dsp_init(cpu);
    hle_set_u32(cpu, 1);
}

// ---------------------------------------------------------------------------
// ARAM (AR) handlers
//
// The real ARAM setup (__OSInitAudioSystem -> ARInit) does a DSP/ARAM hardware
// handshake we skip, so the AR* SDK functions are served here against the host
// ARAM buffer (aram.c). Without these, ARGetBaseAddress returns uninitialized
// garbage and the game's ARAM heap walks an invalid free list forever.
// ---------------------------------------------------------------------------
// ARAM addresses are offsets into the 16 MB of sound RAM. ARInit and
// ARGetBaseAddress return the start of the user area, 0x4000 on hardware (the
// first 16 KB are reserved for the OS). This game's MusyX lays its sample and
// stream buffers out from that base and checks them against ARGetSize, so a
// synthetic CPU-window base made every stream allocation fail. GXRuntime's ARAM
// accepts plain offsets.
#define ARAM_USER_BASE 0x4000u
// ED_WATCH=addr[:len] (debug): report every host transfer into guest RAM that
// overlaps the range (disc reads, ARAM to RAM), with the caller.
static void watch_host_write(CPUState* cpu, const char* what, u32 dest,
                             u32 length) {
    static int armed = -1;
    static u32 watch, watch_len;
    if (armed < 0) {
        const char* env = getenv("ED_WATCH");
        armed = env != NULL && env[0] != '\0';
        char* end = NULL;
        watch = armed ? (u32)strtoul(env, &end, 0) | 0x80000000u : 0u;
        watch_len = armed && end != NULL && *end == ':'
                        ? (u32)strtoul(end + 1, NULL, 0)
                        : 1u;
    }
    if (!armed)
        return;
    const u32 d = dest | 0x80000000u;
    if (d < watch + watch_len && watch < d + length)
        fprintf(stderr, "[watch] %s writes 0x%08X..0x%08X (len 0x%X) lr=0x%08X "
                        "pc=0x%08X frame=%llu\n",
                what, dest, dest + length, length, cpu->lr, cpu->pc,
                (unsigned long long)(cpu->timebase / 675000ull));
}

// ED_ARAM_LOG=1: every host ARAM transfer, to find who owns an ARAM range.
static void aram_log(CPUState* cpu, const char* path, u32 type, u32 mainmem,
                     u32 aram, u32 length) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ED_ARAM_LOG") != NULL;
    if (enabled)
        fprintf(stderr, "[aram] %s %s mram=0x%08X aram=0x%08X..0x%08X len=0x%X "
                        "lr=0x%08X\n",
                path, type == 0 ? "to-aram" : "to-mram", mainmem, aram,
                aram + length, length, cpu->lr);
}

static void hle_ARInit(CPUState* cpu) { hle_set_u32(cpu, ARAM_USER_BASE); }
static void hle_ARGetBaseAddress(CPUState* cpu) { hle_set_u32(cpu, ARAM_USER_BASE); }
static void hle_ARGetSize(CPUState* cpu) { hle_set_u32(cpu, ARAM_SIZE); }
static void hle_ARGetDMAStatus(CPUState* cpu) { hle_set_u32(cpu, 0); }  // idle

// ARStartDMA(u32 type, u32 mainmem, u32 aram, u32 length). type 0 = MRAM->ARAM,
// 1 = ARAM->MRAM. Performed synchronously against the host ARAM buffer.
static void hle_ARStartDMA(CPUState* cpu) {
    u32 type = hle_arg_u32(cpu, 0);
    u32 mainmem = hle_arg_u32(cpu, 1);
    u32 aram = hle_arg_u32(cpu, 2);
    u32 length = hle_arg_u32(cpu, 3);
    aram_log(cpu, "ARStartDMA", type, mainmem, aram, length);
    if (type == 0)
        aram_dma_to_aram(cpu->ram, mainmem, aram, length);
    else
        watch_host_write(cpu, "ARStartDMA", mainmem, length);
        aram_dma_to_ram(cpu->ram, mainmem, aram, length);
}

// MusyX's own aramUploadData runs (no HLE): it hands each request to
// aramUploadCallback when the game set one, else to ARQPostRequest below. This
// game sets it to stream its sample banks from the disc straight into ARAM; a
// host shortcut that copied from main RAM uploaded memory the game never filled
// and made every sound effect and menu tune crackle.

// Some MusyX builds inline aramUploadData into aramInit, leaving ARQPostRequest
// as the actual hardware boundary. Complete the transfer immediately and
// tail-call ARQ's completion routine with the request pointer. The caller
// increments its queue count after ARQPostRequest returns; MusyX's callback
// decrements it first, so the original bookkeeping still settles at zero.
// ARQPostRequest(ARQRequest*, owner, type, priority, source, dest, length,
// callback). The SDK fills the request in before queueing it, and callbacks
// read it back: the room streamer's completion callback finds its stream
// through request->owner, so leaving the struct unwritten hung Start Game.
#define ARQ_REQ_NEXT 0x00u
#define ARQ_REQ_OWNER 0x04u
#define ARQ_REQ_TYPE 0x08u
#define ARQ_REQ_PRIORITY 0x0Cu
#define ARQ_REQ_SOURCE 0x10u
#define ARQ_REQ_DEST 0x14u
#define ARQ_REQ_LENGTH 0x18u
#define ARQ_REQ_CALLBACK 0x1Cu
static void hle_ARQPostRequest(CPUState* cpu) {
    u32 request = hle_arg_u32(cpu, 0);
    u32 type = hle_arg_u32(cpu, 2);
    u32 source = hle_arg_u32(cpu, 4);
    u32 destination = hle_arg_u32(cpu, 5);
    u32 length = hle_arg_u32(cpu, 6);
    u32 callback = hle_arg_u32(cpu, 7);
    if (request != 0) {
        mem_write32(cpu, request + ARQ_REQ_NEXT, 0);
        mem_write32(cpu, request + ARQ_REQ_OWNER, hle_arg_u32(cpu, 1));
        mem_write32(cpu, request + ARQ_REQ_TYPE, type);
        mem_write32(cpu, request + ARQ_REQ_PRIORITY, hle_arg_u32(cpu, 3));
        mem_write32(cpu, request + ARQ_REQ_SOURCE, source);
        mem_write32(cpu, request + ARQ_REQ_DEST, destination);
        mem_write32(cpu, request + ARQ_REQ_LENGTH, length);
        mem_write32(cpu, request + ARQ_REQ_CALLBACK, callback);
    }
    if (type == 0) {
        aram_log(cpu, "ARQPostRequest", 0, source, destination, length);
        aram_dma_to_aram(cpu->ram, source, destination, length);
    } else {
        aram_log(cpu, "ARQPostRequest", 1, destination, source, length);
        watch_host_write(cpu, "ARQPostRequest", destination, length);
        aram_dma_to_ram(cpu->ram, destination, source, length);
    }
    if (callback) {
        cpu->gpr[3] = request;
        cpu->pc = callback;
        g_hle_tail_call = true;
    }
}

// ---------------------------------------------------------------------------
// DVD (disc) handlers
//
// The game's file system reads assets off the disc through the DVD SDK API. We
// never ran the apploader, so there is no in-memory FST and no DI hardware to
// stream sectors; the host owns the "drive" and serves these from the original
// ISO (dvd.c). Intercepting the high-level entry points keeps the SDK's async
// read state machine intact -- it polls the command block, which our reads
// complete synchronously by setting cb.state = DVD_STATE_END.
// ---------------------------------------------------------------------------
// DVDFileInfo / DVDCommandBlock field offsets (dolphin/dvd.h).
#define DVD_CB_STATE      0x0Cu
#define DVD_CB_CURRXFER   0x1Cu
#define DVD_CB_XFERRED    0x20u
#define DVD_FI_STARTADDR  0x30u
#define DVD_FI_LENGTH     0x34u
#define DVD_FI_CALLBACK   0x38u
#define DVD_STATE_END     0u

// DVDInit / DVDClose: the host owns the drive, so init is a no-op and close has
// nothing to cancel. Both report success.
static void hle_DVDInit(CPUState* cpu) { (void)cpu; }
static void hle_DVDClose(CPUState* cpu) { hle_set_u32(cpu, 1); }  // TRUE

// The original status helpers consult the SDK DVD driver's `executing`
// command pointer and error flags. Host reads bypass that driver entirely, so
// those globals are stale and can leave nlServiceFileSystem spinning forever
// in DVD_STATE_BUSY after a synchronously completed read.
static void hle_DVDGetCommandBlockStatus(CPUState* cpu) {
    u32 block = hle_arg_u32(cpu, 0);
    s32 state = block ? (s32)mem_read32(cpu, block + DVD_CB_STATE) : DVD_STATE_END;
    hle_set_u32(cpu, state == 3 ? 1u : (u32)state);
}

static void hle_DVDGetDriveStatus(CPUState* cpu) {
    hle_set_u32(cpu, DVD_STATE_END);
}

// DVDConvertPathToEntrynum(const char* path) -> s32 entry (or -1).
// The SDK's current directory (DVDChangeDir). Relative paths resolve against
// it; the host keeps it as an absolute path into the disc image's FST.
static char g_dvd_cwd[256] = "/";

static void dvd_guest_path(CPUState* cpu, u32 address, char* out, size_t cap) {
    char path[256];
    hle_read_cstr(cpu, address, path, sizeof path);
    if (path[0] == '/')
        snprintf(out, cap, "%s", path);
    else
        snprintf(out, cap, "%s%s%s", g_dvd_cwd,
                 g_dvd_cwd[strlen(g_dvd_cwd) - 1] == '/' ? "" : "/", path);
}

static void hle_DVDConvertPathToEntrynum(CPUState* cpu) {
    char path[256];
    dvd_guest_path(cpu, hle_arg_u32(cpu, 0), path, sizeof path);
    s32 entry = dvd_path_to_entrynum(path);
    if (g_dvd_log)
        fprintf(stderr, "[dvd] entry '%s' -> %d\n", path, entry);
    hle_set_u32(cpu, (u32)entry);
}

// DVDChangeDir(dirName) -> BOOL
static void hle_DVDChangeDir(CPUState* cpu) {
    char path[256];
    dvd_guest_path(cpu, hle_arg_u32(cpu, 0), path, sizeof path);
    const bool ok = dvd_path_to_entrynum(path) >= 0;
    if (ok)
        snprintf(g_dvd_cwd, sizeof g_dvd_cwd, "%s", path);
    if (g_hle_log)
        fprintf(stderr, "[dvd] chdir '%s' -> %s\n", path, ok ? "ok" : "not found");
    hle_set_u32(cpu, ok ? 1u : 0u);
}

// DVDGetCurrentDir(path, maxlen) -> BOOL
static void hle_DVDGetCurrentDir(CPUState* cpu) {
    const u32 out = hle_arg_u32(cpu, 0);
    const u32 cap = hle_arg_u32(cpu, 1);
    u32 i = 0;
    for (; cap && i + 1u < cap && g_dvd_cwd[i] != '\0'; i++)
        mem_write8(cpu, out + i, (u8)g_dvd_cwd[i]);
    if (cap)
        mem_write8(cpu, out + i, 0);
    hle_set_u32(cpu, 1);
}

// DVDFastOpen(s32 entry, DVDFileInfo* fi) -> BOOL. Fills the file's disc offset
// and length, exactly as the SDK does from the FST.
static void hle_DVDFastOpen(CPUState* cpu) {
    s32 entry = (s32)hle_arg_u32(cpu, 0);
    u32 fi = hle_arg_u32(cpu, 1);
    u32 start = 0, length = 0;
    if (fi && dvd_entry_info(entry, &start, &length)) {
        mem_write32(cpu, fi + DVD_FI_STARTADDR, start);
        mem_write32(cpu, fi + DVD_FI_LENGTH, length);
        mem_write32(cpu, fi + DVD_FI_CALLBACK, 0);
        mem_write32(cpu, fi + DVD_CB_STATE, DVD_STATE_END);
        hle_set_u32(cpu, 1);  // TRUE
    } else {
        hle_set_u32(cpu, 0);  // FALSE
    }
}

// DVDOpen(path, fileInfo). The SDK resolves the entry through the FST in guest
// memory, which the host never loads (there is no apploader); resolve it from
// the disc image instead.
static void hle_DVDOpen(CPUState* cpu) {
    char path[256];
    dvd_guest_path(cpu, hle_arg_u32(cpu, 0), path, sizeof path);
    cpu->gpr[3] = (u32)dvd_path_to_entrynum(path);
    if ((s32)cpu->gpr[3] < 0) {
        if (g_hle_log)
            fprintf(stderr, "[dvd] open '%s' -> not found\n", path);
        hle_set_u32(cpu, 0);
        return;
    }
    if (g_dvd_log)
        fprintf(stderr, "[dvd] open '%s' (frame %llu)\n", path,
                (unsigned long long)(cpu->timebase / 675000ull));
    skip_intro_note_open(cpu, path);
    hle_DVDFastOpen(cpu);
}

// DVDReadAsyncPrio(DVDFileInfo* fi, void* addr, s32 len, s32 offset, cb, prio).
// Copies the file bytes immediately and marks the command block complete; the
// caller's poll loop (DVDGetCommandBlockStatus) then sees DVD_STATE_END.
// Every host DVD operation finishes at once: mark the block done and run the
// completion callback, callback(s32 result, DVDCommandBlock* block), the way the
// driver's interrupt path would, once the caller is past the call
// (DVD_CALLBACK_VISITS).
static void dvd_complete(CPUState* cpu, u32 block, u32 callback, s32 result,
                         u32 transferred) {
    if (block) {
        mem_write32(cpu, block + DVD_CB_STATE, DVD_STATE_END);
        mem_write32(cpu, block + DVD_CB_CURRXFER, transferred);
        mem_write32(cpu, block + DVD_CB_XFERRED, transferred);
    }
    queue_guest_callback_after(cpu, callback, result, (s32)block);
}

static void hle_DVDReadAsyncPrio(CPUState* cpu) {
    u32 fi = hle_arg_u32(cpu, 0);
    u32 addr = hle_arg_u32(cpu, 1);
    u32 length = hle_arg_u32(cpu, 2);
    u32 offset = hle_arg_u32(cpu, 3);
    u32 callback = hle_arg_u32(cpu, 4);

    u32 start = mem_read32(cpu, fi + DVD_FI_STARTADDR);
    if (g_dvd_log)
        fprintf(stderr, "[dvd] read fi=0x%08X disc=0x%08X+0x%X len=0x%X (file 0x%X) addr=0x%08X "
                        "cb=0x%08X lr=0x%08X frame=%llu\n",
                fi, start, offset, length, mem_read32(cpu, fi + DVD_FI_STARTADDR + 4u), addr,
                callback, cpu->lr, (unsigned long long)(cpu->timebase / 675000ull));
    watch_host_write(cpu, "DVDReadAsyncPrio", addr, length);
    dvd_read_to_guest(cpu, addr, start + offset, length);
    mem_write32(cpu, fi + DVD_FI_CALLBACK, callback);
    dvd_complete(cpu, fi, callback, (s32)length, length);
    hle_set_u32(cpu, 1);  // TRUE (queued)
}

// DVDReadPrio(fileInfo, addr, length, offset, prio): synchronous, returns bytes read.
static void hle_DVDReadPrio(CPUState* cpu) {
    u32 fi = hle_arg_u32(cpu, 0);
    u32 addr = hle_arg_u32(cpu, 1);
    u32 length = hle_arg_u32(cpu, 2);
    u32 offset = hle_arg_u32(cpu, 3);
    if (g_hle_log)
        fprintf(stderr, "[dvd] readprio addr=0x%08X len=%u disc=0x%08X lr=0x%08X\n",
                addr, length, mem_read32(cpu, fi + DVD_FI_STARTADDR) + offset,
                cpu->lr);
    watch_host_write(cpu, "DVDReadPrio", addr, length);
    dvd_read_to_guest(cpu, addr, mem_read32(cpu, fi + DVD_FI_STARTADDR) + offset, length);
    dvd_complete(cpu, fi, 0, 0, length);
    hle_set_u32(cpu, length);
}

// DVDReadAbsAsyncPrio(block, addr, length, discOffset, callback, prio)
static void hle_DVDReadAbsAsyncPrio(CPUState* cpu) {
    u32 block = hle_arg_u32(cpu, 0);
    u32 addr = hle_arg_u32(cpu, 1);
    u32 length = hle_arg_u32(cpu, 2);
    if (g_hle_log)
        fprintf(stderr, "[dvd] readabs addr=0x%08X len=%u disc=0x%08X cb=0x%08X\n",
                addr, length, hle_arg_u32(cpu, 3), hle_arg_u32(cpu, 4));
    watch_host_write(cpu, "DVDReadAbsAsyncPrio", addr, length);
    dvd_read_to_guest(cpu, addr, hle_arg_u32(cpu, 3), length);
    dvd_complete(cpu, block, hle_arg_u32(cpu, 4), (s32)length, length);
    hle_set_u32(cpu, 1);
}

// DVDReadDiskID(block, diskID, callback): the 0x20-byte disc header.
static void hle_DVDReadDiskID(CPUState* cpu) {
    u32 block = hle_arg_u32(cpu, 0);
    dvd_read_to_guest(cpu, hle_arg_u32(cpu, 1), 0, 0x20u);
    dvd_complete(cpu, block, hle_arg_u32(cpu, 2), 0x20, 0x20u);
    hle_set_u32(cpu, 1);
}

// DVDInquiryAsync(block, DVDDriveInfo* info, callback): a retail drive's reply.
static void hle_DVDInquiryAsync(CPUState* cpu) {
    u32 block = hle_arg_u32(cpu, 0);
    u32 info = hle_arg_u32(cpu, 1);
    for (u32 i = 0; i < 0x20u; i += 4u)
        mem_write32(cpu, info + i, 0);
    mem_write16(cpu, info + 0u, 0x0002u);       // revisionLevel
    mem_write16(cpu, info + 2u, 0x0006u);       // deviceCode
    mem_write32(cpu, info + 4u, 0x20020402u);   // releaseDate
    dvd_complete(cpu, block, hle_arg_u32(cpu, 2), 0x20, 0x20u);
    hle_set_u32(cpu, 1);
}

// Commands with nothing for the host drive to do: cancel, seek and the audio
// streaming requests (DTK). They succeed at once; the streamed music itself is
// played by the host from the disc image (see the DTK notes in audio.c).
// DVD*Async(block, ..., callback) with the callback as the last argument.
static void dvd_async_ok(CPUState* cpu, u32 callback_arg) {
    dvd_complete(cpu, hle_arg_u32(cpu, 0), hle_arg_u32(cpu, callback_arg), 0, 0);
    hle_set_u32(cpu, 1);
}
static void hle_DVDCancelAsync(CPUState* cpu) { dvd_async_ok(cpu, 1); }
static void hle_DVDCancel(CPUState* cpu) {
    dvd_complete(cpu, hle_arg_u32(cpu, 0), 0, 0, 0);
    hle_set_u32(cpu, 0);
}
static void hle_DVDCancelAll(CPUState* cpu) { hle_set_u32(cpu, 0); }
static void hle_DVDCheckDisk(CPUState* cpu) { hle_set_u32(cpu, 1); }
static void hle_DVDGetCurrentDiskID(CPUState* cpu) { hle_set_u32(cpu, 0x80000000u); }
static void hle_DVDReset(CPUState* cpu) { (void)cpu; }

// Disc audio streaming (DTK): the drive plays a prepared byte range of an .adp
// file into the AI stream; dtk.c decodes it from the image (see audio.c for the
// AI side). Replies follow Dolphin's DVDInterface: the play address is in
// 4-byte units, the status is 1 while a stream plays.
// DVDPrepareStreamAsync(fileInfo, length, offset, callback); length 0 = to end.
static void hle_DVDPrepareStreamAsync(CPUState* cpu) {
    const u32 fi = hle_arg_u32(cpu, 0);
    const u32 length = hle_arg_u32(cpu, 1);
    const u32 offset = hle_arg_u32(cpu, 2);
    const u32 start = mem_read32(cpu, fi + DVD_FI_STARTADDR);
    const u32 file_length = mem_read32(cpu, fi + DVD_FI_LENGTH);
    dtk_prepare(start + offset,
                length != 0u ? length : file_length - offset);
    dvd_complete(cpu, fi, hle_arg_u32(cpu, 3), 0, 0);
    hle_set_u32(cpu, 1);
}
// DVDPrepareStreamAbsAsync(block, length, discOffset, callback)
static void hle_DVDPrepareStreamAbsAsync(CPUState* cpu) {
    dtk_prepare(hle_arg_u32(cpu, 2), hle_arg_u32(cpu, 1));
    dvd_complete(cpu, hle_arg_u32(cpu, 0), hle_arg_u32(cpu, 3), 0, 0);
    hle_set_u32(cpu, 1);
}
static void hle_DVDCancelStreamAsync(CPUState* cpu) {
    dtk_cancel();
    dvd_async_ok(cpu, 1);
}
static void hle_DVDStopStreamAtEndAsync(CPUState* cpu) {
    dtk_stop_at_end();
    dvd_async_ok(cpu, 1);
}
static void hle_DVDGetStreamErrorStatusAsync(CPUState* cpu) {
    dvd_complete(cpu, hle_arg_u32(cpu, 0), hle_arg_u32(cpu, 1),
                 dtk_active() ? 1 : 0, 0);
    hle_set_u32(cpu, 1);
}
static void hle_DVDGetStreamPlayAddrAsync(CPUState* cpu) {
    dvd_complete(cpu, hle_arg_u32(cpu, 0), hle_arg_u32(cpu, 1),
                 (s32)(dtk_play_address() >> 2), 0);
    hle_set_u32(cpu, 1);
}

// ---------------------------------------------------------------------------
// CARD handlers
//
// GXRuntime owns a persistent, game-independent virtual card. These handlers
// translate the G4QE01 SDK ABI and guest structs onto that service. Slot A is
// configured by the executable; slot B remains empty.
// ---------------------------------------------------------------------------
#define CARD_FILE_INFO_CHAN 0x00u
#define CARD_FILE_INFO_FILENO 0x04u
#define CARD_FILE_INFO_OFFSET 0x08u
#define CARD_FILE_INFO_LENGTH 0x0Cu
#define CARD_FILE_INFO_IBLOCK 0x10u

#define CARD_STAT_FILENAME 0x00u
#define CARD_STAT_LENGTH 0x20u
#define CARD_STAT_TIME 0x24u
#define CARD_STAT_GAME 0x28u
#define CARD_STAT_COMPANY 0x2Cu
#define CARD_STAT_BANNER_FORMAT 0x2Eu
#define CARD_STAT_ICON_ADDRESS 0x30u
#define CARD_STAT_ICON_FORMAT 0x34u
#define CARD_STAT_ICON_SPEED 0x36u
#define CARD_STAT_COMMENT_ADDRESS 0x38u
#define CARD_STAT_OFFSET_BANNER 0x3Cu
#define CARD_STAT_OFFSET_BANNER_TLUT 0x40u
#define CARD_STAT_OFFSET_ICON 0x44u
#define CARD_STAT_OFFSET_ICON_TLUT 0x64u
#define CARD_STAT_OFFSET_DATA 0x68u

static DolMemoryCard* card_for_channel(s32 channel) {
    return channel == 0 ? g_memory_card : NULL;
}

static s32 card_invalid_channel_result(s32 channel) {
    return (channel < 0 || channel >= 2) ? DOL_CARD_RESULT_FATAL
                                         : DOL_CARD_RESULT_NO_CARD;
}

static void card_set_async_result(CPUState* cpu, s32 channel, s32 result,
                                  u32 callback) {
    if (result >= 0 && callback != 0 &&
        !queue_guest_callback(callback, channel, result))
        result = DOL_CARD_RESULT_IO_ERROR;
    hle_set_u32(cpu, (u32)result);
}

static void card_write_file_info(CPUState* cpu, u32 address, s32 channel,
                                 s32 file_no) {
    mem_write32(cpu, address + CARD_FILE_INFO_CHAN, (u32)channel);
    mem_write32(cpu, address + CARD_FILE_INFO_FILENO, (u32)file_no);
    mem_write32(cpu, address + CARD_FILE_INFO_OFFSET, 0);
    mem_write32(cpu, address + CARD_FILE_INFO_LENGTH, 0);
    mem_write16(cpu, address + CARD_FILE_INFO_IBLOCK,
                (u16)(DOL_CARD_SYSTEM_BLOCKS + (u32)file_no));
}

static void card_update_icon_offsets(CPUState* cpu, u32 address,
                                     const DolMemoryCardStat* stat) {
    u32 offset = stat->icon_address;
    bool icon_tlut = false;
    u8 banner_format = stat->banner_format & 3u;
    if (stat->icon_address == 0xFFFFFFFFu) {
        banner_format = 0;
        offset = 0;
    }

    if (banner_format == 1u) {
        mem_write32(cpu, address + CARD_STAT_OFFSET_BANNER, offset);
        offset += 96u * 32u;
        mem_write32(cpu, address + CARD_STAT_OFFSET_BANNER_TLUT, offset);
        offset += 2u * 256u;
    } else if (banner_format == 2u) {
        mem_write32(cpu, address + CARD_STAT_OFFSET_BANNER, offset);
        offset += 2u * 96u * 32u;
        mem_write32(cpu, address + CARD_STAT_OFFSET_BANNER_TLUT, 0xFFFFFFFFu);
    } else {
        mem_write32(cpu, address + CARD_STAT_OFFSET_BANNER, 0xFFFFFFFFu);
        mem_write32(cpu, address + CARD_STAT_OFFSET_BANNER_TLUT, 0xFFFFFFFFu);
    }

    for (u32 i = 0; i < 8u; i++) {
        u32 format = (stat->icon_format >> (2u * i)) & 3u;
        if (format == 1u) {
            mem_write32(cpu, address + CARD_STAT_OFFSET_ICON + i * 4u,
                        offset);
            offset += 32u * 32u;
            icon_tlut = true;
        } else if (format == 2u) {
            mem_write32(cpu, address + CARD_STAT_OFFSET_ICON + i * 4u,
                        offset);
            offset += 2u * 32u * 32u;
        } else {
            mem_write32(cpu, address + CARD_STAT_OFFSET_ICON + i * 4u,
                        0xFFFFFFFFu);
        }
    }
    if (icon_tlut) {
        mem_write32(cpu, address + CARD_STAT_OFFSET_ICON_TLUT, offset);
        offset += 2u * 256u;
    } else {
        mem_write32(cpu, address + CARD_STAT_OFFSET_ICON_TLUT, 0xFFFFFFFFu);
    }
    mem_write32(cpu, address + CARD_STAT_OFFSET_DATA, offset);
}

static void card_write_status(CPUState* cpu, u32 address,
                              const DolMemoryCardStat* stat) {
    const bool has_icon_data = stat->icon_address != 0xFFFFFFFFu;
    for (u32 i = 0; i < DOL_CARD_FILENAME_MAX; i++)
        mem_write8(cpu, address + CARD_STAT_FILENAME + i,
                   (u8)stat->file_name[i]);
    mem_write32(cpu, address + CARD_STAT_LENGTH, stat->length);
    mem_write32(cpu, address + CARD_STAT_TIME, stat->time);
    for (u32 i = 0; i < 4u; i++)
        mem_write8(cpu, address + CARD_STAT_GAME + i, stat->game_code[i]);
    for (u32 i = 0; i < 2u; i++)
        mem_write8(cpu, address + CARD_STAT_COMPANY + i, stat->company[i]);
    mem_write8(cpu, address + CARD_STAT_BANNER_FORMAT,
               has_icon_data ? stat->banner_format : 0);
    mem_write32(cpu, address + CARD_STAT_ICON_ADDRESS, stat->icon_address);
    mem_write16(cpu, address + CARD_STAT_ICON_FORMAT,
                has_icon_data ? stat->icon_format : 0);
    mem_write16(cpu, address + CARD_STAT_ICON_SPEED,
                has_icon_data ? stat->icon_speed : 0);
    mem_write32(cpu, address + CARD_STAT_COMMENT_ADDRESS,
                stat->comment_address);
    card_update_icon_offsets(cpu, address, stat);
}

static void card_read_status(CPUState* cpu, u32 address,
                             DolMemoryCardStat* stat) {
    memset(stat, 0, sizeof(*stat));
    stat->banner_format =
        mem_read8(cpu, address + CARD_STAT_BANNER_FORMAT);
    stat->icon_address =
        mem_read32(cpu, address + CARD_STAT_ICON_ADDRESS);
    stat->icon_format =
        mem_read16(cpu, address + CARD_STAT_ICON_FORMAT);
    stat->icon_speed =
        mem_read16(cpu, address + CARD_STAT_ICON_SPEED);
    stat->comment_address =
        mem_read32(cpu, address + CARD_STAT_COMMENT_ADDRESS);
}

static void hle_CARDInit(CPUState* cpu) {
    (void)cpu;
}

static void hle_CARDProbe(CPUState* cpu) {
    s32 channel = (s32)hle_arg_u32(cpu, 0);
    u16 size_mbits = 0;
    u32 sector_size = 0;
    hle_set_u32(cpu,
                dol_card_probe(card_for_channel(channel), &size_mbits,
                               &sector_size) == DOL_CARD_RESULT_READY ? 1u : 0u);
}

static void hle_CARDProbeEx(CPUState* cpu) {
    static unsigned probe_log_count;
    s32 channel = (s32)hle_arg_u32(cpu, 0);
    u32 memory_size_address = hle_arg_u32(cpu, 1);
    u32 sector_size_address = hle_arg_u32(cpu, 2);
    DolMemoryCard* card = card_for_channel(channel);
    u16 size_mbits = 0;
    u32 sector_size = 0;
    s32 result = card != NULL
                     ? dol_card_probe(card, &size_mbits, &sector_size)
                     : card_invalid_channel_result(channel);
    if (result == DOL_CARD_RESULT_READY) {
        if (memory_size_address != 0)
            mem_write32(cpu, memory_size_address, size_mbits);
        if (sector_size_address != 0)
            mem_write32(cpu, sector_size_address, sector_size);
    }
    if (g_card_log &&
        (probe_log_count < 32u || (probe_log_count % 600u) == 0u))
        fprintf(stderr,
                "[card] CARDProbeEx channel=%d result=%d size=%u sector=%u\n",
                channel, result, size_mbits, sector_size);
    probe_log_count++;
    hle_set_u32(cpu, (u32)result);
}

static void hle_CARDGetResultCode(CPUState* cpu) {
    s32 channel = (s32)hle_arg_u32(cpu, 0);
    DolMemoryCard* card = card_for_channel(channel);
    hle_set_u32(cpu,
                (u32)(card != NULL ? dol_card_last_result(card)
                                   : card_invalid_channel_result(channel)));
}

static void hle_CARDGetFastMode(CPUState* cpu) {
    hle_set_u32(cpu, 0u);
}

static void hle_CARDGetXferredBytes(CPUState* cpu) {
    s32 channel = (s32)hle_arg_u32(cpu, 0);
    hle_set_u32(cpu, dol_card_transferred_bytes(card_for_channel(channel)));
}

static void hle_CARDUnmount(CPUState* cpu) {
    s32 channel = (s32)hle_arg_u32(cpu, 0);
    DolMemoryCard* card = card_for_channel(channel);
    s32 result = card != NULL ? dol_card_unmount(card)
                              : card_invalid_channel_result(channel);
    if (g_card_log)
        fprintf(stderr, "[card] CARDUnmount channel=%d result=%d\n",
                channel, result);
    hle_set_u32(cpu, (u32)result);
}

static void hle_CARDClose(CPUState* cpu) {
    u32 file_info = hle_arg_u32(cpu, 0);
    s32 channel =
        file_info != 0 ? (s32)mem_read32(cpu, file_info + CARD_FILE_INFO_CHAN)
                       : -1;
    s32 file_no =
        file_info != 0
            ? (s32)mem_read32(cpu, file_info + CARD_FILE_INFO_FILENO)
            : -1;
    DolMemoryCard* card = card_for_channel(channel);
    s32 result = card != NULL ? dol_card_close_file(card, file_no)
                              : card_invalid_channel_result(channel);
    if (result == DOL_CARD_RESULT_READY)
        mem_write32(cpu, file_info + CARD_FILE_INFO_CHAN, 0xFFFFFFFFu);
    hle_set_u32(cpu, (u32)result);
}

static void hle_CARDMountAsync(CPUState* cpu) {
    s32 channel = (s32)hle_arg_u32(cpu, 0);
    u32 callback = hle_arg_u32(cpu, 3);
    DolMemoryCard* card = card_for_channel(channel);
    s32 result = card != NULL ? dol_card_mount(card)
                              : card_invalid_channel_result(channel);
    if (g_card_log)
        fprintf(stderr,
                "[card] CARDMountAsync channel=%d result=%d callback=0x%08X\n",
                channel, result, callback);
    card_set_async_result(cpu, channel, result, callback);
}

static void hle_CARDCheckAsync(CPUState* cpu) {
    s32 channel = (s32)hle_arg_u32(cpu, 0);
    u32 callback = hle_arg_u32(cpu, 1);
    DolMemoryCard* card = card_for_channel(channel);
    s32 result = card != NULL ? dol_card_check(card)
                              : card_invalid_channel_result(channel);
    if (g_card_log)
        fprintf(stderr,
                "[card] CARDCheckAsync channel=%d result=%d callback=0x%08X\n",
                channel, result, callback);
    card_set_async_result(cpu, channel, result, callback);
}

static void hle_CARDCheckExAsync(CPUState* cpu) {
    s32 channel = (s32)hle_arg_u32(cpu, 0);
    u32 transferred_address = hle_arg_u32(cpu, 1);
    u32 callback = hle_arg_u32(cpu, 2);
    DolMemoryCard* card = card_for_channel(channel);
    s32 result = card != NULL ? dol_card_check(card)
                              : card_invalid_channel_result(channel);
    if (transferred_address != 0)
        mem_write32(cpu, transferred_address, 0);
    card_set_async_result(cpu, channel, result, callback);
}

static void hle_CARDFreeBlocks(CPUState* cpu) {
    s32 channel = (s32)hle_arg_u32(cpu, 0);
    u32 bytes_address = hle_arg_u32(cpu, 1);
    u32 files_address = hle_arg_u32(cpu, 2);
    DolMemoryCard* card = card_for_channel(channel);
    u32 bytes_free = 0;
    u32 files_free = 0;
    s32 result =
        card != NULL
            ? dol_card_free_space(card, &bytes_free, &files_free)
            : card_invalid_channel_result(channel);
    if (result == DOL_CARD_RESULT_READY) {
        if (bytes_address != 0)
            mem_write32(cpu, bytes_address, bytes_free);
        if (files_address != 0)
            mem_write32(cpu, files_address, files_free);
    }
    if (g_card_log)
        fprintf(stderr,
                "[card] CARDFreeBlocks channel=%d result=%d bytes=%u files=%u\n",
                channel, result, bytes_free, files_free);
    hle_set_u32(cpu, (u32)result);
}

static void hle_CARDOpen(CPUState* cpu) {
    s32 channel = (s32)hle_arg_u32(cpu, 0);
    u32 name_address = hle_arg_u32(cpu, 1);
    u32 file_info = hle_arg_u32(cpu, 2);
    char name[DOL_CARD_FILENAME_MAX + 2u];
    hle_read_cstr(cpu, name_address, name, sizeof name);
    DolMemoryCard* card = card_for_channel(channel);
    s32 file_no = -1;
    u32 length = 0;
    s32 result =
        card != NULL ? dol_card_open_file(card, name, &file_no, &length)
                     : card_invalid_channel_result(channel);
    (void)length;
    if (result == DOL_CARD_RESULT_READY && file_info != 0)
        card_write_file_info(cpu, file_info, channel, file_no);
    else if (file_info != 0)
        mem_write32(cpu, file_info + CARD_FILE_INFO_CHAN, 0xFFFFFFFFu);
    if (g_card_log)
        fprintf(stderr,
                "[card] CARDOpen channel=%d name='%s' result=%d file=%d "
                "length=%u\n",
                channel, name, result, file_no, length);
    hle_set_u32(cpu, (u32)result);
}

static void hle_CARDCreateAsync(CPUState* cpu) {
    s32 channel = (s32)hle_arg_u32(cpu, 0);
    u32 name_address = hle_arg_u32(cpu, 1);
    u32 length = hle_arg_u32(cpu, 2);
    u32 file_info = hle_arg_u32(cpu, 3);
    u32 callback = hle_arg_u32(cpu, 4);
    char name[DOL_CARD_FILENAME_MAX + 2u];
    hle_read_cstr(cpu, name_address, name, sizeof name);
    DolMemoryCard* card = card_for_channel(channel);
    s32 file_no = -1;
    s32 result =
        card != NULL
            ? dol_card_create_file(card, name, length, &file_no)
            : card_invalid_channel_result(channel);
    if (result == DOL_CARD_RESULT_READY && file_info != 0)
        card_write_file_info(cpu, file_info, channel, file_no);
    if (g_card_log)
        fprintf(stderr,
                "[card] CARDCreateAsync channel=%d name='%s' length=%u "
                "result=%d file=%d callback=0x%08X\n",
                channel, name, length, result, file_no, callback);
    card_set_async_result(cpu, channel, result, callback);
}

static void hle_CARDDeleteAsync(CPUState* cpu) {
    s32 channel = (s32)hle_arg_u32(cpu, 0);
    u32 name_address = hle_arg_u32(cpu, 1);
    u32 callback = hle_arg_u32(cpu, 2);
    char name[DOL_CARD_FILENAME_MAX + 2u];
    hle_read_cstr(cpu, name_address, name, sizeof name);
    DolMemoryCard* card = card_for_channel(channel);
    s32 result =
        card != NULL ? dol_card_delete_file(card, name)
                     : card_invalid_channel_result(channel);
    if (g_card_log)
        fprintf(stderr,
                "[card] CARDDeleteAsync channel=%d name='%s' result=%d "
                "callback=0x%08X\n",
                channel, name, result, callback);
    card_set_async_result(cpu, channel, result, callback);
}

static void hle_CARDReadAsync(CPUState* cpu) {
    u32 file_info = hle_arg_u32(cpu, 0);
    u32 buffer = hle_arg_u32(cpu, 1);
    u32 length = hle_arg_u32(cpu, 2);
    u32 offset = hle_arg_u32(cpu, 3);
    u32 callback = hle_arg_u32(cpu, 4);
    s32 channel =
        file_info != 0 ? (s32)mem_read32(cpu, file_info + CARD_FILE_INFO_CHAN)
                       : -1;
    s32 file_no =
        file_info != 0
            ? (s32)mem_read32(cpu, file_info + CARD_FILE_INFO_FILENO)
            : -1;
    DolMemoryCard* card = card_for_channel(channel);
    u8* bytes = length != 0 ? (u8*)malloc(length) : NULL;
    s32 result = length != 0 && bytes == NULL
                     ? DOL_CARD_RESULT_IO_ERROR
                     : (card != NULL
                            ? dol_card_read_file(card, file_no, offset, bytes,
                                                 length)
                            : card_invalid_channel_result(channel));
    if (result == DOL_CARD_RESULT_READY)
        copy_host_to_guest(cpu, buffer, bytes, length);
    free(bytes);
    if (g_card_log)
        fprintf(stderr,
                "[card] CARDReadAsync channel=%d file=%d offset=%u length=%u "
                "result=%d callback=0x%08X\n",
                channel, file_no, offset, length, result, callback);
    card_set_async_result(cpu, channel, result, callback);
}

static void hle_CARDWriteAsync(CPUState* cpu) {
    u32 file_info = hle_arg_u32(cpu, 0);
    u32 buffer = hle_arg_u32(cpu, 1);
    u32 length = hle_arg_u32(cpu, 2);
    u32 offset = hle_arg_u32(cpu, 3);
    u32 callback = hle_arg_u32(cpu, 4);
    s32 channel =
        file_info != 0 ? (s32)mem_read32(cpu, file_info + CARD_FILE_INFO_CHAN)
                       : -1;
    s32 file_no =
        file_info != 0
            ? (s32)mem_read32(cpu, file_info + CARD_FILE_INFO_FILENO)
            : -1;
    DolMemoryCard* card = card_for_channel(channel);
    u8* bytes = length != 0 ? (u8*)malloc(length) : NULL;
    s32 result = length != 0 && bytes == NULL
                     ? DOL_CARD_RESULT_IO_ERROR
                     : DOL_CARD_RESULT_READY;
    if (result == DOL_CARD_RESULT_READY)
        copy_guest_to_host(cpu, buffer, bytes, length);
    if (result == DOL_CARD_RESULT_READY)
        result = card != NULL
                     ? dol_card_write_file(card, file_no, offset, bytes, length)
                     : card_invalid_channel_result(channel);
    free(bytes);
    if (g_card_log)
        fprintf(stderr,
                "[card] CARDWriteAsync channel=%d file=%d offset=%u "
                "length=%u result=%d callback=0x%08X\n",
                channel, file_no, offset, length, result, callback);
    card_set_async_result(cpu, channel, result, callback);
}

static void hle_CARDGetStatus(CPUState* cpu) {
    s32 channel = (s32)hle_arg_u32(cpu, 0);
    s32 file_no = (s32)hle_arg_u32(cpu, 1);
    u32 status_address = hle_arg_u32(cpu, 2);
    DolMemoryCard* card = card_for_channel(channel);
    DolMemoryCardStat status;
    s32 result =
        card != NULL ? dol_card_get_status(card, file_no, &status)
                     : card_invalid_channel_result(channel);
    if (result == DOL_CARD_RESULT_READY && status_address != 0)
        card_write_status(cpu, status_address, &status);
    if (g_card_log)
        fprintf(stderr,
                "[card] CARDGetStatus channel=%d file=%d result=%d\n",
                channel, file_no, result);
    hle_set_u32(cpu, (u32)result);
}

static void hle_CARDSetStatusAsync(CPUState* cpu) {
    s32 channel = (s32)hle_arg_u32(cpu, 0);
    s32 file_no = (s32)hle_arg_u32(cpu, 1);
    u32 status_address = hle_arg_u32(cpu, 2);
    u32 callback = hle_arg_u32(cpu, 3);
    DolMemoryCard* card = card_for_channel(channel);
    DolMemoryCardStat status;
    card_read_status(cpu, status_address, &status);
    s32 result;
    if ((status.icon_address != 0xFFFFFFFFu &&
         status.icon_address >= 512u) ||
        (status.comment_address != 0xFFFFFFFFu &&
         status.comment_address % DOL_CARD_SECTOR_SIZE >
             DOL_CARD_SECTOR_SIZE - 64u)) {
        result = DOL_CARD_RESULT_FATAL;
    } else {
        result = card != NULL ? dol_card_set_status(card, file_no, &status)
                              : card_invalid_channel_result(channel);
    }
    if (g_card_log)
        fprintf(stderr,
                "[card] CARDSetStatusAsync channel=%d file=%d result=%d "
                "callback=0x%08X\n",
                channel, file_no, result, callback);
    card_set_async_result(cpu, channel, result, callback);
}

static void hle_CARDGetSerialNo(CPUState* cpu) {
    s32 channel = (s32)hle_arg_u32(cpu, 0);
    u32 serial_address = hle_arg_u32(cpu, 1);
    DolMemoryCard* card = card_for_channel(channel);
    s32 result = card != NULL ? dol_card_check(card)
                              : card_invalid_channel_result(channel);
    if (result == DOL_CARD_RESULT_READY && serial_address != 0)
        mem_write64(cpu, serial_address, dol_card_serial(card));
    hle_set_u32(cpu, (u32)result);
}

static void hle_CARDFormatAsync(CPUState* cpu) {
    s32 channel = (s32)hle_arg_u32(cpu, 0);
    u32 callback = hle_arg_u32(cpu, 1);
    DolMemoryCard* card = card_for_channel(channel);
    s32 result = card != NULL ? dol_card_format(card)
                              : card_invalid_channel_result(channel);
    if (g_card_log)
        fprintf(stderr,
                "[card] CARDFormatAsync channel=%d result=%d callback=0x%08X\n",
                channel, result, callback);
    card_set_async_result(cpu, channel, result, callback);
}

static u32 guest_hash_bytes(const void* data, u32 bytes) {
    const u8* p = (const u8*)data;
    u32 hash = 2166136261u;
    for (u32 i = 0; i < bytes; i++)
        hash = (hash ^ p[i]) * 16777619u;
    return hash ? hash : 1u;
}

static u32 guest_hash(CPUState* cpu, u32 address, u32 bytes,
                      u32* hashed_bytes) {
    u32 available = 0;
    const void* data = mmio_guest_pointer(cpu, address, &available);
    if (data == NULL || bytes == 0 || available == 0) {
        if (hashed_bytes != NULL)
            *hashed_bytes = 0;
        return 0;
    }
    if (bytes > available)
        bytes = available;
    if (hashed_bytes != NULL)
        *hashed_bytes = bytes;
    return guest_hash_bytes(data, bytes);
}

#define THP_SIMPLE_CONTROL 0x8032F000u

static f32 guest_read_f32(CPUState* cpu, u32 address) {
    u32 bits = mem_read32(cpu, address);
    f32 value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

#ifdef ED_AURORA
// ---------------------------------------------------------------------------
// Aurora graphics metadata
//
// Raw writes to the GameCube write-gather pipe are forwarded by mmio.c into
// Aurora's GX FIFO. Most GX state therefore needs no per-function marshalling.
// Pointer-bearing commands are the exception: the guest encodes 32-bit
// GameCube physical addresses, while Aurora needs native host pointers. Track
// the guest objects selected by GX and emit Aurora metadata immediately before
// each draw, after the guest's raw texture/state commands are already queued.
// ---------------------------------------------------------------------------
#define GX_MAX_TEXTURES 8u
#define GX_MAX_TLUTS    20u

typedef struct {
    bool valid;
    u32 object;
    u32 guest_data;
    u32 width;
    u32 height;
    u32 format;
    u32 tlut;
    u32 version;
    u32 emitted_version;
    u32 data_bytes;
    u32 data_hash;
    u8 flags;
} GxTextureBinding;

typedef struct {
    bool valid;
    u32 object;
    u32 guest_data;
    u32 format;
    u32 version;
    u32 emitted_version;
    u32 data_bytes;
    u32 data_hash;
    u16 entries;
} GxTlutBinding;

static GxTextureBinding g_textures[GX_MAX_TEXTURES];
static GxTlutBinding g_tluts[GX_MAX_TLUTS];
static u32 g_array_address[32];
static u8 g_array_stride[32];
static unsigned g_gx_begin_count;
static unsigned g_gx_array_count;
// Per-frame accounting of indexed-array uploads. Aurora uploads each distinct
// indexed array's *whole advertised size* into a fixed, non-growable 8 MB
// per-frame storage buffer (aurora/lib/gfx StorageBufferSize); if the sum of
// all arrays bound in one frame exceeds that, the non-owned ByteBuffer aborts
// (ByteBuffer::resize, m_owned==false). We watch the running total so a future
// heavy scene warns loudly instead of crashing silently.
static u64 g_frame_array_bytes;
static unsigned g_frame_array_calls;
static u64 g_frame_array_bytes_peak;
static bool g_array_budget_warned;
// Aurora's per-frame storage buffer is 8 MB; warn once past 75% of it.
enum { AURORA_STORAGE_BUDGET = 8u * 1024u * 1024u };
enum { ARRAY_UPLOAD_WARN_BYTES = (AURORA_STORAGE_BUDGET * 3u) / 4u };
// GXSetArray carries no vertex count (smstrikers-decomp NL/glx/glxSend.cpp:1708
// passes only base+stride). With the local Aurora span patch live, push_gx_draw
// uploads only each draw's REAL indexed span min(array.size,(maxidx+1)*stride),
// so we advertise the FULL resolvable range and let the span patch bound the
// actual upload. No vertex cap is needed, and a cap can ONLY truncate geometry:
// the old flat 2048 cap truncated gameplay player/pitch meshes (which index
// >2048 verts per base) and collapsed the whole 3D scene to the clear color.
// g_max_array_verts is therefore an OPTIONAL DEBUG CEILING ONLY; 0 (the default)
// means uncapped. Set ED_MAX_ARRAY_VERTS only to deliberately truncate.
static u32 g_max_array_verts = 0u;
static unsigned g_vi_wait_count;
static unsigned g_movie_texture_load_count;
static u64 g_movie_cadence_present_count;
static u64 g_movie_cadence_texframe_changes;
static double g_movie_cadence_start_time;
static s32 g_movie_cadence_last_texframe;
static bool g_movie_cadence_started;

static void* graphics_guest_pointer(CPUState* cpu, u32 address, u32* available) {
    return mmio_guest_pointer(cpu, address, available);
}

static u32 gx_texture_base_bytes(u32 width, u32 height, u32 format) {
    u32 shift_x = 2;
    u32 shift_y = 2;
    switch (format) {
    case 0x00u: // GX_TF_I4
    case 0x08u: // GX_TF_C4
    case 0x0Eu: // GX_TF_CMPR
    case 0x20u: // GX_CTF_R4
    case 0x30u: // GX_CTF_Z4
        shift_x = 3;
        shift_y = 3;
        break;
    case 0x01u: // GX_TF_I8
    case 0x02u: // GX_TF_IA4
    case 0x09u: // GX_TF_C8
    case 0x11u: // GX_TF_Z8
    case 0x22u: // GX_CTF_RA4
    case 0x27u: // GX_CTF_A8
    case 0x28u: // GX_CTF_R8
    case 0x29u: // GX_CTF_G8
    case 0x2Au: // GX_CTF_B8
    case 0x39u: // GX_CTF_Z8M
    case 0x3Au: // GX_CTF_Z8L
        shift_x = 3;
        shift_y = 2;
        break;
    default:
        shift_x = 2;
        shift_y = 2;
        break;
    }

    const u32 tile_x = (width + (1u << shift_x) - 1u) >> shift_x;
    const u32 tile_y = (height + (1u << shift_y) - 1u) >> shift_y;
    const u32 block_bits = (format == 0x06u || format == 0x16u) ? 64u : 32u;
    return block_bits * tile_x * tile_y;
}

static u32 gx_hash_token_mix(u32 hash, u32 value) {
    for (u32 i = 0; i < 4u; i++) {
        hash = (hash ^ (u8)(value >> (i * 8u))) * 16777619u;
    }
    return hash ? hash : 1u;
}

static u32 gx_texture_resource_version(u32 width, u32 height, u32 format,
                                       u32 tlut, u8 flags,
                                       u32 data_bytes, u32 data_hash) {
    u32 hash = 2166136261u;
    hash = gx_hash_token_mix(hash, 0x54455831u);
    hash = gx_hash_token_mix(hash, width);
    hash = gx_hash_token_mix(hash, height);
    hash = gx_hash_token_mix(hash, format);
    hash = gx_hash_token_mix(hash, tlut);
    hash = gx_hash_token_mix(hash, flags);
    hash = gx_hash_token_mix(hash, data_bytes);
    return gx_hash_token_mix(hash, data_hash);
}

static u32 gx_tlut_resource_version(u32 format, u16 entries, u32 data_bytes,
                                    u32 data_hash) {
    u32 hash = 2166136261u;
    hash = gx_hash_token_mix(hash, 0x544C5431u);
    hash = gx_hash_token_mix(hash, format);
    hash = gx_hash_token_mix(hash, entries);
    hash = gx_hash_token_mix(hash, data_bytes);
    return gx_hash_token_mix(hash, data_hash);
}

static bool thp_movie_plane_generation(CPUState* cpu, u32 guest_data,
                                       u32 width, u32 height, u32 format,
                                       u32* data_bytes, u32* data_hash,
                                       u32* cache_object) {
    if (format != 1u)
        return false;

    const u32 base = THP_SIMPLE_CONTROL;
    const u32 y = mem_read32(cpu, base + 0x15Cu);
    const u32 u = mem_read32(cpu, base + 0x160u);
    const u32 v = mem_read32(cpu, base + 0x164u);
    bool matches_plane = false;
    if (guest_data == y && width == 512u && height == 416u)
        matches_plane = true;
    else if ((guest_data == u || guest_data == v) &&
             width == 256u && height == 208u)
        matches_plane = true;
    if (!matches_plane)
        return false;

    const s32 tex_frame = (s32)mem_read32(cpu, base + 0x168u);
    if (tex_frame < 0)
        return false;

    const u32 bytes = gx_texture_base_bytes(width, height, format);
    u32 hash = 2166136261u;
    hash = gx_hash_token_mix(hash, 0x54485031u);
    hash = gx_hash_token_mix(hash, guest_data);
    hash = gx_hash_token_mix(hash, (u32)tex_frame);
    hash = gx_hash_token_mix(hash, width);
    hash = gx_hash_token_mix(hash, height);
    hash = gx_hash_token_mix(hash, format);
    if (data_bytes != NULL)
        *data_bytes = bytes;
    if (data_hash != NULL)
        *data_hash = hash;
    if (cache_object != NULL)
        *cache_object = guest_data;
    return true;
}

static double host_time_seconds(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static u32 thp_audio_valid_samples(CPUState* cpu, u32* output_valid) {
    const u32 base = THP_SIMPLE_CONTROL;
    u32 total = 0;
    for (u32 i = 0; i < 6u; i++)
        total += mem_read32(cpu, base + 0x16Cu + i * 12u + 8u);

    if (output_valid != NULL) {
        const u32 output_index = mem_read32(cpu, base + 0x1B8u);
        *output_valid = output_index < 6u
            ? mem_read32(cpu, base + 0x16Cu + output_index * 12u + 8u)
            : 0u;
    }
    return total;
}

static void movie_cadence_present(CPUState* cpu) {
    if (!g_movie_cadence_log)
        return;

    const u32 base = THP_SIMPLE_CONTROL;
    if (mem_read8(cpu, base + 0x6Cu) == 0u) {
        g_movie_cadence_started = false;
        return;
    }

    const s32 tex_frame = (s32)mem_read32(cpu, base + 0x168u);
    const double now = host_time_seconds();
    if (!g_movie_cadence_started) {
        g_movie_cadence_started = true;
        g_movie_cadence_start_time = now;
        g_movie_cadence_present_count = 0;
        g_movie_cadence_texframe_changes = 0;
        g_movie_cadence_last_texframe = tex_frame;
    }

    g_movie_cadence_present_count++;
    if (tex_frame != g_movie_cadence_last_texframe) {
        g_movie_cadence_texframe_changes++;
        g_movie_cadence_last_texframe = tex_frame;
    }

    const double elapsed = now - g_movie_cadence_start_time;
    if (elapsed >= 1.0) {
        u32 audio_output_valid = 0;
        const u32 audio_valid = thp_audio_valid_samples(cpu, &audio_output_valid);
        fprintf(stderr,
                "[movie-cadence] seconds=%.3f presents=%llu "
                "texframe_changes=%llu last_texframe=%d audio_dec=%u "
                "audio_out=%u audio_valid=%u audio_out_valid=%u\n",
                elapsed,
                (unsigned long long)g_movie_cadence_present_count,
                (unsigned long long)g_movie_cadence_texframe_changes,
                g_movie_cadence_last_texframe,
                mem_read32(cpu, base + 0x1B4u),
                mem_read32(cpu, base + 0x1B8u),
                audio_valid,
                audio_output_valid);
        g_movie_cadence_start_time = now;
        g_movie_cadence_present_count = 0;
        g_movie_cadence_texframe_changes = 0;
    }
}

static void guest_write_f32(CPUState* cpu, u32 address, f32 value) {
    u32 bits;
    memcpy(&bits, &value, sizeof bits);
    mem_write32(cpu, address, bits);
}

// PSMTXConcat is pure SDK math, but its paired-single implementation is used
// on virtually every rendered packet. Keep a portable scalar implementation at
// the HLE boundary so host floating-point behavior cannot corrupt transforms.
static void hle_PSMTXConcat(CPUState* cpu) {
    const u32 a_address = hle_arg_u32(cpu, 0);
    const u32 b_address = hle_arg_u32(cpu, 1);
    const u32 out_address = hle_arg_u32(cpu, 2);
    f32 a[12], b[12], out[12];

    for (u32 i = 0; i < 12; i++) {
        a[i] = guest_read_f32(cpu, a_address + i * 4u);
        b[i] = guest_read_f32(cpu, b_address + i * 4u);
    }
    for (u32 row = 0; row < 3; row++) {
        for (u32 col = 0; col < 3; col++) {
            out[row * 4u + col] =
                (f32)((f32)(a[row * 4u] * b[col]) +
                      (f32)(a[row * 4u + 1u] * b[4u + col]) +
                      (f32)(a[row * 4u + 2u] * b[8u + col]));
        }
        out[row * 4u + 3u] =
            (f32)(a[row * 4u + 3u] +
                  (f32)(a[row * 4u] * b[3u]) +
                  (f32)(a[row * 4u + 1u] * b[7u]) +
                  (f32)(a[row * 4u + 2u] * b[11u]));
    }
    for (u32 i = 0; i < 12; i++)
        guest_write_f32(cpu, out_address + i * 4u, out[i]);
}

// GXSetArray(GXAttr attr, void* base, u8 stride). Replace the hardware CP-base
// write with Aurora's extended command carrying a real host pointer.
// While the game records a display list (the CPU FIFO points at its buffer,
// not the GPU), the ED_GX_HLE shortcuts below step aside so the real SDK
// functions write their real commands into the list: the shortcuts feed the
// renderer directly and would leave the recorded list short of its commands.
static bool gx_recording_display_list(void) {
    if (mmio_cpu_fifo_feeds_gpu())
        return false;
    g_hle_passthrough = true;
    return true;
}

static void hle_GXSetArray(CPUState* cpu) {
    if (gx_recording_display_list())
        return;
    u32 attr = hle_arg_u32(cpu, 0);
    u32 address = hle_arg_u32(cpu, 1);
    u8 stride = (u8)hle_arg_u32(cpu, 2);
    u32 available = 0;
    void* data = graphics_guest_pointer(cpu, address, &available);
    // GXSetArray carries no length on real hardware, but Aurora's FIFO path
    // uploads the whole advertised array into a fixed per-frame storage buffer
    // (8 MB) every time the array is used, once per distinct base. The game
    // re-bases the position/color/texcoord arrays per object and indexes locally
    // from each base, so a single frame can bind dozens of arrays; advertising
    // the 16-bit index ceiling, or even stride*8192, summed past 8 MB once enough
    // character models were on screen (e.g. both team captains on the VS screen)
    // and aborted Aurora's non-owned storage ByteBuffer. Bound the advertised
    // range to a realistic per-mesh vertex count: small enough to keep the total
    // far under 8 MB, large enough to cover every index a real draw references.
    // Advertise the full resolvable range; Aurora's span patch uploads only the
    // real per-draw indexed span, so this never inflates storage and never
    // truncates geometry. An explicit ED_MAX_ARRAY_VERTS (g_max_array_verts
    // != 0) applies a debug ceiling only.
    u32 size = available;
    if (g_max_array_verts != 0u) {
        u64 indexed_span = (u64)(stride ? stride : 1u) * g_max_array_verts;
        if (indexed_span < (u64)size)
            size = (u32)indexed_span;
    }
    if (attr < 32u) {
        g_array_address[attr] = address;
        g_array_stride[attr] = stride;
    }
    if (g_graphics_log && (++g_gx_array_count <= 24 || size > 500000u))
        fprintf(stderr, "[gfx] array attr=%u guest=0x%08X host=%p size=%u stride=%u available=%u\n",
                attr, address, data, size, stride, available);
    g_frame_array_bytes += size;
    g_frame_array_calls++;
    // The advertised-byte budget warn modelled the OLD whole-array upload; with
    // the span patch the real upload is per-draw span-bounded, so this only makes
    // sense (and only risks false positives) when an explicit cap is set.
    if (g_max_array_verts != 0u &&
        g_frame_array_bytes > ARRAY_UPLOAD_WARN_BYTES && !g_array_budget_warned) {
        g_array_budget_warned = true;
        fprintf(stderr,
                "[gfx] WARNING: indexed-array uploads this frame reached %llu bytes, "
                "nearing Aurora's %u-byte per-frame storage limit. Lower "
                "ED_MAX_ARRAY_VERTS (currently %u) if this scene aborts.\n",
                (unsigned long long)g_frame_array_bytes,
                (unsigned)AURORA_STORAGE_BUDGET, g_max_array_verts);
    }
    dol_platform_set_array_guest(attr, address, data, size, stride);
}

static void emit_gx_resource_metadata(CPUState* cpu);

static void finish_GXCallDisplayList(CPUState* cpu) {
    u32 available = 0;
    void* data = graphics_guest_pointer(
        cpu, g_display_list_call.address, &available);
    if (data == NULL || g_display_list_call.size > available) {
        fprintf(stderr,
                "[gx] GXCallDisplayList unresolved address=0x%08X size=%u "
                "available=%u\n",
                g_display_list_call.address, g_display_list_call.size,
                available);
    } else {
        emit_gx_resource_metadata(cpu);
        mmio_fifo_dump_note("call-display-list", g_display_list_call.address,
                            g_display_list_call.size);
        dol_platform_call_display_list(data, g_display_list_call.size);
    }
    cpu->lr = g_display_list_call.caller;
    cpu->pc = g_display_list_call.caller;
    g_display_list_call.active = false;
}

static void continue_GXCallDisplayList(CPUState* cpu) {
    const u32 gx_data =
        mem_read32(cpu, cpu->gpr[ED_GX_DATA_BASE_REG] + (u32)(s32)GX_DATA_SDA_OFFSET);
    if (g_display_list_call.stage == 0u &&
        mem_read32(cpu, gx_data + GX_DIRTY_STATE_OFFSET) != 0u) {
        g_display_list_call.stage = 1u;
        cpu->lr = HLE_DISPLAY_LIST_RETURN;
        cpu->pc = g_gx_dirty_state_helper;
        return;
    }
    if (g_display_list_call.stage <= 1u &&
        ((mem_read32(cpu, gx_data) == 0u) == (ED_GX_FLUSH_PRIM_WHEN_ZERO != 0u))) {
        g_display_list_call.stage = 2u;
        cpu->lr = HLE_DISPLAY_LIST_RETURN;
        cpu->pc = g_gx_flush_prim_helper;
        return;
    }
    finish_GXCallDisplayList(cpu);
}

static void hle_GXCallDisplayList(CPUState* cpu) {
    if (gx_recording_display_list())
        return;
    if (g_display_list_call.active) {
        fprintf(stderr, "[gx] nested GXCallDisplayList host continuation\n");
        return;
    }
    g_display_list_call.active = true;
    g_display_list_call.stage = 0u;
    g_display_list_call.caller = cpu->lr;
    g_display_list_call.address = hle_arg_u32(cpu, 0);
    g_display_list_call.size = hle_arg_u32(cpu, 1);
    g_hle_tail_call = true;
    continue_GXCallDisplayList(cpu);
}

static void notify_GXBegin(CPUState* cpu);

static void finish_GXBegin(CPUState* cpu) {
    emit_gx_resource_metadata(cpu);
    dol_platform_mark_gx_begin();
    mem_write8(cpu, 0xCC008000u,
               (u8)(g_gx_begin.primitive | g_gx_begin.vtxfmt));
    mem_write16(cpu, 0xCC008000u, g_gx_begin.vertex_count);
    cpu->lr = g_gx_begin.caller;
    cpu->pc = g_gx_begin.caller;
    g_gx_begin.active = false;
}

static void continue_GXBegin(CPUState* cpu) {
    const u32 gx_data =
        mem_read32(cpu, cpu->gpr[ED_GX_DATA_BASE_REG] + (u32)(s32)GX_DATA_SDA_OFFSET);
    if (g_gx_begin.stage == 0u &&
        mem_read32(cpu, gx_data + GX_DIRTY_STATE_OFFSET) != 0u) {
        g_gx_begin.stage = 1u;
        cpu->lr = HLE_GX_BEGIN_RETURN;
        cpu->pc = g_gx_dirty_state_helper;
        return;
    }
    if (g_gx_begin.stage <= 1u && ((mem_read32(cpu, gx_data) == 0u) == (ED_GX_FLUSH_PRIM_WHEN_ZERO != 0u))) {
        g_gx_begin.stage = 2u;
        cpu->lr = HLE_GX_BEGIN_RETURN;
        cpu->pc = g_gx_flush_prim_helper;
        return;
    }
    finish_GXBegin(cpu);
}

static void hle_GXBegin(CPUState* cpu) {
    if (gx_recording_display_list())
        return;
    if (g_gx_begin.active) {
        fprintf(stderr, "[gx] nested GXBegin host continuation\n");
        cpu->pc = cpu->lr;
        return;
    }
    notify_GXBegin(cpu);
    g_gx_begin.active = true;
    g_gx_begin.stage = 0u;
    g_gx_begin.primitive = (u8)hle_arg_u32(cpu, 0);
    g_gx_begin.vtxfmt = (u8)hle_arg_u32(cpu, 1);
    g_gx_begin.vertex_count = (u16)hle_arg_u32(cpu, 2);
    g_gx_begin.caller = cpu->lr;
    g_hle_tail_call = true;
    continue_GXBegin(cpu);
}

static void record_GXLoadTexObj(CPUState* cpu, u32 obj, u32 slot) {
    static unsigned texture_trace_count = 0;
    static bool texture_watch_initialized = false;
    static u32 texture_watch_address = 0;
    static unsigned texture_watch_begin = UINT_MAX;
    if (slot >= GX_MAX_TEXTURES)
        return;
    u32 image0 = mem_read32(cpu, obj + 8u);
    u32 image3 = mem_read32(cpu, obj + 12u);
    u32 guest_data = GC_RAM_BASE | ((image3 & 0x001FFFFFu) << 5);
    u32 width = (image0 & 0x3FFu) + 1u;
    u32 height = ((image0 >> 10) & 0x3FFu) + 1u;
    u32 format = mem_read32(cpu, obj + 20u);
    u32 tlut = mem_read32(cpu, obj + 24u);
    u8 flags = mem_read8(cpu, obj + 31u);
    u32 data_bytes = gx_texture_base_bytes(width, height, format);
    u32 data_hash = 0;
    u32 cache_object = obj;
    const bool movie_generation =
        thp_movie_plane_generation(cpu, guest_data, width, height, format,
                                   &data_bytes, &data_hash, &cache_object);
    if (!movie_generation)
        data_hash = guest_hash(cpu, guest_data, data_bytes, &data_bytes);
    const u32 version =
        gx_texture_resource_version(width, height, format, tlut, flags,
                                    data_bytes, data_hash);
    if (!movie_generation)
        cache_object = version;
    if (!texture_watch_initialized) {
        const char* watch = getenv("ED_TEXTURE_WATCH");
        texture_watch_address =
            watch != NULL ? (u32)strtoul(watch, NULL, 0) : 0u;
        texture_watch_initialized = true;
    }
    if (guest_data == texture_watch_address &&
        texture_watch_begin != g_gx_begin_count) {
        texture_watch_begin = g_gx_begin_count;
        fprintf(stderr,
                "[texture-watch] begin=%u slot=%u obj=%08X data=%08X "
                "%ux%u fmt=%u flags=%02X hash=%08X id=%08X version=%08X\n",
                g_gx_begin_count, slot, obj, guest_data, width, height, format,
                flags, data_hash, cache_object, version);
    }
    if (g_mash_route_complete && getenv("ED_TEXTURE_CACHE_LOG") != NULL &&
        texture_trace_count < 256u) {
        ++texture_trace_count;
        fprintf(stderr,
                "[texture-cache] load=%u begin=%u slot=%u obj=%08X data=%08X "
                "%ux%u fmt=%u tlut=%u flags=%02X bytes=%u hash=%08X "
                "id=%08X version=%08X movie=%u\n",
                texture_trace_count, g_gx_begin_count, slot, obj, guest_data,
                width, height, format, tlut, flags, data_bytes, data_hash,
                cache_object, version, movie_generation ? 1u : 0u);
    }
    GxTextureBinding* binding = &g_textures[slot];
    bool changed = !binding->valid ||
                   binding->object != cache_object ||
                   binding->version != version;
    binding->valid = true;
    binding->object = cache_object;
    binding->guest_data = guest_data;
    binding->width = width;
    binding->height = height;
    binding->format = format;
    binding->tlut = tlut;
    binding->flags = flags;
    binding->data_bytes = data_bytes;
    binding->data_hash = data_hash;
    binding->version = version;
    // The retail BP texture-load sequence clears Aurora's recomp metadata.
    // Re-emit the stable identity after every guest bind, even when the bytes
    // are unchanged, so the renderer can hit its converted-texture cache.
    binding->emitted_version = 0;
    if (g_movie_log && format == 1u) {
        unsigned seq = ++g_movie_texobj_log_count;
        bool should_log = changed || seq <= 96u || (seq % 60u) == 0u;
        if (should_log) {
        fprintf(stderr,
                "[movie] GXLoadTexObj slot=%u obj=0x%08X data=0x%08X "
                "%ux%u fmt=%u bytes=%u hash=%08X changed=%u ver=%u seq=%u "
                "gen=%u cache=0x%08X lr=0x%08X\n",
                slot, obj, guest_data, width, height, format, data_bytes,
                data_hash, changed ? 1u : 0u, binding->version, seq,
                movie_generation ? 1u : 0u, cache_object, cpu->lr);
        }
    }
}

#endif

static bool ball_state_log_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ED_BALL_STATE_LOG") != NULL ? 1 : 0;
    return enabled != 0;
}

typedef struct {
    u64 quick_step;
    unsigned position_sets;
    unsigned velocity_sets;
    bool quick_entry_finite;
    bool quick_entry_valid;
    bool first_failure_reported;
} BallPhysicsProbe;

static BallPhysicsProbe g_ball_physics_probe;

typedef struct {
    bool active;
    bool interesting;
    u64 quick_step;
    u32 m;
    u32 nb;
    u32 j;
    u32 jb;
    u32 bodies;
    u32 inv_i;
    u32 lambda;
    u32 cforce;
    u32 rhs;
    u32 lo;
    u32 hi;
    u32 cfm;
    u32 findex;
    s32 ball_body_index;
} BallSorProbe;

static BallSorProbe g_ball_sor_probe;

typedef struct {
    u32 body;
    bool finite_seen;
    bool failure_reported;
} PhysicsBodyProbeEntry;

#define PHYSICS_BODY_PROBE_CAPACITY 256u
static PhysicsBodyProbeEntry
    g_physics_body_probes[PHYSICS_BODY_PROBE_CAPACITY];
static unsigned g_physics_body_probe_count;
static bool g_physics_sor_failure_reported;
static bool g_physics_set_failure_reported;

static void report_ball_physics_failure(CPUState* cpu, const char* boundary);

static bool guest_ram_range_valid(const CPUState* cpu, u32 address, u32 size) {
    if (address < GC_RAM_BASE || size > cpu->ram_size)
        return false;
    return address - GC_RAM_BASE <= cpu->ram_size - size;
}

static bool guest_vec3_finite(CPUState* cpu, u32 address) {
    return isfinite(guest_read_f32(cpu, address)) &&
           isfinite(guest_read_f32(cpu, address + 4u)) &&
           isfinite(guest_read_f32(cpu, address + 8u));
}

static bool guest_vec4_finite(CPUState* cpu, u32 address) {
    return guest_vec3_finite(cpu, address) &&
           isfinite(guest_read_f32(cpu, address + 12u));
}

static bool physics_body_finite(CPUState* cpu, u32 body) {
    return guest_vec3_finite(cpu, body + 0x98u) &&
           guest_vec4_finite(cpu, body + 0xA8u) &&
           guest_vec3_finite(cpu, body + 0xB8u) &&
           guest_vec3_finite(cpu, body + 0xC8u) &&
           guest_vec3_finite(cpu, body + 0xD8u) &&
           guest_vec3_finite(cpu, body + 0xE8u) &&
           guest_vec3_finite(cpu, body + 0xF8u) &&
           guest_vec3_finite(cpu, body + 0x108u) &&
           guest_vec3_finite(cpu, body + 0x118u);
}

static void log_physics_body(CPUState* cpu, const char* phase, u32 body) {
    const u32 object = mem_read32(cpu, body + 0x0Cu);
    const u32 vtable = guest_ram_range_valid(cpu, object, 0x94u)
                           ? mem_read32(cpu, object)
                           : 0u;
    const u32 parent = guest_ram_range_valid(cpu, object, 0x94u)
                           ? mem_read32(cpu, object + 0x0Cu)
                           : 0u;
    const u32 ai_character =
        guest_ram_range_valid(cpu, object, 0x94u)
            ? mem_read32(cpu, object + 0x8Cu)
            : 0u;
    fprintf(stderr,
            "[physics-body-first-failure] phase=%s step=%llu "
            "guest-frame=%llu body=0x%08X object=0x%08X "
            "vtable=0x%08X parent=0x%08X ai=0x%08X flags=0x%08X "
            "joint=0x%08X pc=0x%08X lr=0x%08X\n"
            "  pos=% .7g,% .7g,% .7g q=% .7g,% .7g,% .7g,% .7g\n"
            "  vel=% .7g,% .7g,% .7g avel=% .7g,% .7g,% .7g "
            "force=% .7g,% .7g,% .7g torque=% .7g,% .7g,% .7g\n",
            phase, (unsigned long long)g_ball_physics_probe.quick_step,
            (unsigned long long)(cpu->timebase / 675000ull), body, object,
            vtable, parent, ai_character, mem_read32(cpu, body + 0x18u),
            mem_read32(cpu, body + 0x14u), cpu->pc, cpu->lr,
            (double)guest_read_f32(cpu, body + 0x98u),
            (double)guest_read_f32(cpu, body + 0x9Cu),
            (double)guest_read_f32(cpu, body + 0xA0u),
            (double)guest_read_f32(cpu, body + 0xA8u),
            (double)guest_read_f32(cpu, body + 0xACu),
            (double)guest_read_f32(cpu, body + 0xB0u),
            (double)guest_read_f32(cpu, body + 0xB4u),
            (double)guest_read_f32(cpu, body + 0xE8u),
            (double)guest_read_f32(cpu, body + 0xECu),
            (double)guest_read_f32(cpu, body + 0xF0u),
            (double)guest_read_f32(cpu, body + 0xF8u),
            (double)guest_read_f32(cpu, body + 0xFCu),
            (double)guest_read_f32(cpu, body + 0x100u),
            (double)guest_read_f32(cpu, body + 0x108u),
            (double)guest_read_f32(cpu, body + 0x10Cu),
            (double)guest_read_f32(cpu, body + 0x110u),
            (double)guest_read_f32(cpu, body + 0x118u),
            (double)guest_read_f32(cpu, body + 0x11Cu),
            (double)guest_read_f32(cpu, body + 0x120u));
}

static void observe_physics_body(CPUState* cpu, const char* phase, u32 body) {
    if (!ball_state_log_enabled() ||
        !guest_ram_range_valid(cpu, body, 0x128u))
        return;
    PhysicsBodyProbeEntry* entry = NULL;
    for (unsigned i = 0; i < g_physics_body_probe_count; ++i) {
        if (g_physics_body_probes[i].body == body) {
            entry = &g_physics_body_probes[i];
            break;
        }
    }
    if (entry == NULL &&
        g_physics_body_probe_count < PHYSICS_BODY_PROBE_CAPACITY) {
        entry = &g_physics_body_probes[g_physics_body_probe_count++];
        entry->body = body;
        entry->finite_seen = false;
        entry->failure_reported = false;
    }
    if (entry == NULL)
        return;
    const bool finite = physics_body_finite(cpu, body);
    if (finite) {
        entry->finite_seen = true;
        return;
    }
    if (!entry->failure_reported) {
        entry->failure_reported = true;
        log_physics_body(cpu, phase, body);
    }
}

static void observe_physics_world(CPUState* cpu, const char* phase, u32 world) {
    if (!ball_state_log_enabled() ||
        !guest_ram_range_valid(cpu, world, 4u))
        return;
    u32 body = mem_read32(cpu, world);
    for (unsigned count = 0; body != 0u && count < 1024u; ++count) {
        if (!guest_ram_range_valid(cpu, body, 0x128u))
            break;
        observe_physics_body(cpu, phase, body);
        body = mem_read32(cpu, body + 4u);
    }
}

static void log_nonfinite_body_set(CPUState* cpu, const char* kind, u32 body,
                                   f64 x, f64 y, f64 z) {
    if ((isfinite(x) && isfinite(y) && isfinite(z)) ||
        g_physics_set_failure_reported)
        return;
    g_physics_set_failure_reported = true;
    fprintf(stderr,
            "[physics-body-set-nonfinite] kind=%s step=%llu body=0x%08X "
            "object=0x%08X value=% .9g,% .9g,% .9g pc=0x%08X lr=0x%08X\n",
            kind, (unsigned long long)g_ball_physics_probe.quick_step, body,
            guest_ram_range_valid(cpu, body, 20u)
                ? mem_read32(cpu, body + 0x0Cu)
                : 0u,
            x, y, z, cpu->pc, cpu->lr);
}

static void log_character_force_write(CPUState* cpu, const char* kind) {
    if (!ball_state_log_enabled() ||
        g_ball_physics_probe.quick_step < 252u ||
        g_ball_physics_probe.quick_step > 256u ||
        !guest_ram_range_valid(cpu, cpu->gpr[3], 0x128u))
        return;
    const u32 body = cpu->gpr[3];
    const u32 object = mem_read32(cpu, body + 0x0Cu);
    if (!guest_ram_range_valid(cpu, object, 0x94u) ||
        mem_read32(cpu, object) != 0x802AFA18u)
        return;
    fprintf(stderr,
            "[physics-force-write] kind=%s step=%llu body=0x%08X "
            "object=0x%08X ai=0x%08X value=% .9g,% .9g,% .9g "
            "before=% .9g,% .9g,% .9g lr=0x%08X\n",
            kind, (unsigned long long)g_ball_physics_probe.quick_step, body,
            object, mem_read32(cpu, object + 0x8Cu), cpu->fpr[1],
            cpu->fpr[2], cpu->fpr[3],
            (double)guest_read_f32(cpu, body + 0x108u),
            (double)guest_read_f32(cpu, body + 0x10Cu),
            (double)guest_read_f32(cpu, body + 0x110u), cpu->lr);
}

static bool ball_physics_addresses(CPUState* cpu, u32* ball_out,
                                   u32* physics_out, u32* body_out) {
    const u32 ball = mem_read32(cpu, 0x80373664u); // g_pBall
    if (!guest_ram_range_valid(cpu, ball, 0xACu))
        return false;
    const u32 physics = mem_read32(cpu, ball + 0x38u);
    if (!guest_ram_range_valid(cpu, physics, 0x5Cu))
        return false;
    const u32 body = mem_read32(cpu, physics + 4u);
    if (!guest_ram_range_valid(cpu, body, 0x128u))
        return false;
    if (ball_out != NULL)
        *ball_out = ball;
    if (physics_out != NULL)
        *physics_out = physics;
    if (body_out != NULL)
        *body_out = body;
    return true;
}

static bool ball_physics_finite(CPUState* cpu, u32 ball, u32 physics,
                                u32 body) {
    return guest_vec3_finite(cpu, ball + 0x40u) &&
           guest_vec3_finite(cpu, physics + 0x14u) &&
           guest_vec3_finite(cpu, physics + 0x20u) &&
           guest_vec3_finite(cpu, body + 0x98u) &&
           guest_vec3_finite(cpu, body + 0xE8u) &&
           guest_vec3_finite(cpu, body + 0xF8u) &&
           guest_vec3_finite(cpu, body + 0x108u) &&
           guest_vec3_finite(cpu, body + 0x118u);
}

static bool log_ball_physics_probe(CPUState* cpu, const char* phase,
                                   bool periodic) {
    u32 ball;
    u32 physics;
    u32 body;
    if (!ball_state_log_enabled() ||
        !ball_physics_addresses(cpu, &ball, &physics, &body))
        return true;

    const bool finite = ball_physics_finite(cpu, ball, physics, body);
    if (!periodic && finite)
        return true;
    if (!periodic && g_ball_physics_probe.first_failure_reported)
        return false;

    fprintf(stderr,
            "[ball-physics] phase=%s step=%llu guest-frame=%llu pc=0x%08X "
            "lr=0x%08X finite=%u ball=0x%08X physics=0x%08X "
            "body=0x%08X owner=0x%08X damage=%u target=0x%08X "
            "body-flags=0x%08X joint=0x%08X\n"
            "  sim=% .7g,% .7g,% .7g cache-pos=% .7g,% .7g,% .7g "
            "cache-vel=% .7g,% .7g,% .7g\n"
            "  ode-pos=% .7g,% .7g,% .7g ode-vel=% .7g,% .7g,% .7g "
            "ode-avel=% .7g,% .7g,% .7g\n"
            "  force=% .7g,% .7g,% .7g torque=% .7g,% .7g,% .7g\n",
            phase, (unsigned long long)g_ball_physics_probe.quick_step,
            (unsigned long long)(cpu->timebase / 675000ull), cpu->pc, cpu->lr,
            finite ? 1u : 0u, ball, physics, body,
            mem_read32(cpu, ball + 0x24u),
            (unsigned)mem_read8(cpu, ball + 0xA6u),
            mem_read32(cpu, ball + 0xA8u), mem_read32(cpu, body + 0x18u),
            mem_read32(cpu, body + 0x14u),
            (double)guest_read_f32(cpu, ball + 0x40u),
            (double)guest_read_f32(cpu, ball + 0x44u),
            (double)guest_read_f32(cpu, ball + 0x48u),
            (double)guest_read_f32(cpu, physics + 0x14u),
            (double)guest_read_f32(cpu, physics + 0x18u),
            (double)guest_read_f32(cpu, physics + 0x1Cu),
            (double)guest_read_f32(cpu, physics + 0x20u),
            (double)guest_read_f32(cpu, physics + 0x24u),
            (double)guest_read_f32(cpu, physics + 0x28u),
            (double)guest_read_f32(cpu, body + 0x98u),
            (double)guest_read_f32(cpu, body + 0x9Cu),
            (double)guest_read_f32(cpu, body + 0xA0u),
            (double)guest_read_f32(cpu, body + 0xE8u),
            (double)guest_read_f32(cpu, body + 0xECu),
            (double)guest_read_f32(cpu, body + 0xF0u),
            (double)guest_read_f32(cpu, body + 0xF8u),
            (double)guest_read_f32(cpu, body + 0xFCu),
            (double)guest_read_f32(cpu, body + 0x100u),
            (double)guest_read_f32(cpu, body + 0x108u),
            (double)guest_read_f32(cpu, body + 0x10Cu),
            (double)guest_read_f32(cpu, body + 0x110u),
            (double)guest_read_f32(cpu, body + 0x118u),
            (double)guest_read_f32(cpu, body + 0x11Cu),
            (double)guest_read_f32(cpu, body + 0x120u));
    return finite;
}

static void log_ball_contact_joints(CPUState* cpu, const char* phase,
                                    u32 body) {
    u32 node = mem_read32(cpu, body + 0x14u);
    for (unsigned index = 0; index < 12u; ++index) {
        if (!guest_ram_range_valid(cpu, node, 12u))
            break;
        const u32 joint = mem_read32(cpu, node);
        const u32 other_body = mem_read32(cpu, node + 4u);
        const u32 next = mem_read32(cpu, node + 8u);
        if (!guest_ram_range_valid(cpu, joint, 0xBCu))
            break;
        const u32 vtable = mem_read32(cpu, joint + 0x14u);
        const u32 type = guest_ram_range_valid(cpu, vtable, 20u)
                             ? mem_read32(cpu, vtable + 0x10u)
                             : ~0u;
        const u32 other_object =
            guest_ram_range_valid(cpu, other_body, 20u)
                ? mem_read32(cpu, other_body + 0x0Cu)
                : 0u;
        fprintf(stderr,
                "[ball-contact] phase=%s step=%llu index=%u node=0x%08X "
                "joint=0x%08X type=%u flags=0x%08X other-body=0x%08X "
                "other-object=0x%08X next=0x%08X "
                "lambda=% .7g,% .7g,% .7g,% .7g,% .7g,% .7g\n",
                phase, (unsigned long long)g_ball_physics_probe.quick_step,
                index, node, joint, type, mem_read32(cpu, joint + 0x18u),
                other_body, other_object, next,
                (double)guest_read_f32(cpu, joint + 0x38u),
                (double)guest_read_f32(cpu, joint + 0x3Cu),
                (double)guest_read_f32(cpu, joint + 0x40u),
                (double)guest_read_f32(cpu, joint + 0x44u),
                (double)guest_read_f32(cpu, joint + 0x48u),
                (double)guest_read_f32(cpu, joint + 0x4Cu));
        if (type == 4u) {
            const u32 contact = joint + 0x54u;
            const u32 geom1 = mem_read32(cpu, contact + 0x50u);
            const u32 geom2 = mem_read32(cpu, contact + 0x54u);
            fprintf(stderr,
                    "  contact rows=%d mode=0x%08X mu=% .7g mu2=% .7g "
                    "bounce=% .7g bounce-vel=% .7g erp=% .7g cfm=% .7g "
                    "motion=% .7g,% .7g slip=% .7g,% .7g\n"
                    "  point=% .7g,% .7g,% .7g normal=% .7g,% .7g,% .7g "
                    "depth=% .7g geom=0x%08X(type=%u,data=0x%08X),"
                    "0x%08X(type=%u,data=0x%08X)\n",
                    (s32)mem_read32(cpu, joint + 0x50u),
                    mem_read32(cpu, contact),
                    (double)guest_read_f32(cpu, contact + 4u),
                    (double)guest_read_f32(cpu, contact + 8u),
                    (double)guest_read_f32(cpu, contact + 0x0Cu),
                    (double)guest_read_f32(cpu, contact + 0x10u),
                    (double)guest_read_f32(cpu, contact + 0x14u),
                    (double)guest_read_f32(cpu, contact + 0x18u),
                    (double)guest_read_f32(cpu, contact + 0x1Cu),
                    (double)guest_read_f32(cpu, contact + 0x20u),
                    (double)guest_read_f32(cpu, contact + 0x24u),
                    (double)guest_read_f32(cpu, contact + 0x28u),
                    (double)guest_read_f32(cpu, contact + 0x2Cu),
                    (double)guest_read_f32(cpu, contact + 0x30u),
                    (double)guest_read_f32(cpu, contact + 0x34u),
                    (double)guest_read_f32(cpu, contact + 0x3Cu),
                    (double)guest_read_f32(cpu, contact + 0x40u),
                    (double)guest_read_f32(cpu, contact + 0x44u),
                    (double)guest_read_f32(cpu, contact + 0x4Cu), geom1,
                    guest_ram_range_valid(cpu, geom1, 16u)
                        ? mem_read32(cpu, geom1)
                        : ~0u,
                    guest_ram_range_valid(cpu, geom1, 16u)
                        ? mem_read32(cpu, geom1 + 8u)
                        : 0u,
                    geom2,
                    guest_ram_range_valid(cpu, geom2, 16u)
                        ? mem_read32(cpu, geom2)
                        : ~0u,
                    guest_ram_range_valid(cpu, geom2, 16u)
                        ? mem_read32(cpu, geom2 + 8u)
                        : 0u);
        }
        node = next;
    }
}

static void log_ball_sor_rows(CPUState* cpu, const char* phase) {
    const BallSorProbe* probe = &g_ball_sor_probe;
    if (!probe->active || probe->ball_body_index < 0)
        return;
    fprintf(stderr,
            "[ball-sor] phase=%s step=%llu m=%u nb=%u ball-index=%d "
            "J=0x%08X jb=0x%08X lambda=0x%08X cforce=0x%08X\n",
            phase, (unsigned long long)probe->quick_step, probe->m, probe->nb,
            probe->ball_body_index, probe->j, probe->jb, probe->lambda,
            probe->cforce);
    for (u32 row = 0; row < probe->m; ++row) {
        const s32 b1 = (s32)mem_read32(cpu, probe->jb + row * 8u);
        const s32 b2 = (s32)mem_read32(cpu, probe->jb + row * 8u + 4u);
        if (b1 != probe->ball_body_index && b2 != probe->ball_body_index)
            continue;
        const u32 j = probe->j + row * 48u;
        fprintf(stderr,
                "  row=%u bodies=%d,%d rhs=% .7g lambda=% .7g "
                "lo=% .7g hi=% .7g cfm=% .7g findex=%d\n"
                "    J1=% .7g,% .7g,% .7g | % .7g,% .7g,% .7g\n"
                "    J2=% .7g,% .7g,% .7g | % .7g,% .7g,% .7g\n",
                row, b1, b2, (double)guest_read_f32(cpu, probe->rhs + row * 4u),
                (double)guest_read_f32(cpu, probe->lambda + row * 4u),
                (double)guest_read_f32(cpu, probe->lo + row * 4u),
                (double)guest_read_f32(cpu, probe->hi + row * 4u),
                (double)guest_read_f32(cpu, probe->cfm + row * 4u),
                (s32)mem_read32(cpu, probe->findex + row * 4u),
                (double)guest_read_f32(cpu, j),
                (double)guest_read_f32(cpu, j + 4u),
                (double)guest_read_f32(cpu, j + 8u),
                (double)guest_read_f32(cpu, j + 12u),
                (double)guest_read_f32(cpu, j + 16u),
                (double)guest_read_f32(cpu, j + 20u),
                (double)guest_read_f32(cpu, j + 24u),
                (double)guest_read_f32(cpu, j + 28u),
                (double)guest_read_f32(cpu, j + 32u),
                (double)guest_read_f32(cpu, j + 36u),
                (double)guest_read_f32(cpu, j + 40u),
                (double)guest_read_f32(cpu, j + 44u));
    }
    const u32 force =
        probe->cforce + (u32)probe->ball_body_index * 24u;
    fprintf(stderr,
            "  cforce=% .7g,% .7g,% .7g | % .7g,% .7g,% .7g\n",
            (double)guest_read_f32(cpu, force),
            (double)guest_read_f32(cpu, force + 4u),
            (double)guest_read_f32(cpu, force + 8u),
            (double)guest_read_f32(cpu, force + 12u),
            (double)guest_read_f32(cpu, force + 16u),
            (double)guest_read_f32(cpu, force + 20u));
}

static void notify_SorLcp(CPUState* cpu) {
    memset(&g_ball_sor_probe, 0, sizeof g_ball_sor_probe);
    if (!ball_state_log_enabled())
        return;
    u32 ball;
    u32 body;
    if (!ball_physics_addresses(cpu, &ball, NULL, &body))
        return;

    BallSorProbe* probe = &g_ball_sor_probe;
    probe->m = cpu->gpr[3];
    probe->nb = cpu->gpr[4];
    if (probe->m == 0u || probe->m > 4096u || probe->nb == 0u ||
        probe->nb > 1024u)
        return;
    probe->j = cpu->gpr[5];
    probe->jb = cpu->gpr[6];
    probe->bodies = cpu->gpr[7];
    probe->inv_i = cpu->gpr[8];
    probe->lambda = cpu->gpr[9];
    probe->cforce = cpu->gpr[10];
    probe->rhs = mem_read32(cpu, cpu->gpr[1] + 8u);
    probe->lo = mem_read32(cpu, cpu->gpr[1] + 12u);
    probe->hi = mem_read32(cpu, cpu->gpr[1] + 16u);
    probe->cfm = mem_read32(cpu, cpu->gpr[1] + 20u);
    probe->findex = mem_read32(cpu, cpu->gpr[1] + 24u);
    probe->ball_body_index = -1;
    for (u32 index = 0; index < probe->nb; ++index) {
        if (mem_read32(cpu, probe->bodies + index * 4u) == body) {
            probe->ball_body_index = (s32)index;
            break;
        }
    }
    probe->quick_step = g_ball_physics_probe.quick_step;
    probe->interesting =
        !g_ball_physics_probe.first_failure_reported &&
        probe->quick_step <= 260u &&
        isfinite(guest_read_f32(cpu, ball + 0x44u)) &&
        fabsf(guest_read_f32(cpu, ball + 0x44u)) >= 5.0f;
    probe->active = true;

    if (!g_physics_sor_failure_reported) {
        for (u32 row = 0; row < probe->m; ++row) {
            const u32 j = probe->j + row * 48u;
            bool finite = guest_vec4_finite(cpu, j) &&
                          guest_vec4_finite(cpu, j + 16u) &&
                          guest_vec4_finite(cpu, j + 32u) &&
                          isfinite(guest_read_f32(cpu,
                                                  probe->rhs + row * 4u)) &&
                          isfinite(guest_read_f32(cpu,
                                                  probe->cfm + row * 4u));
            if (finite)
                continue;
            g_physics_sor_failure_reported = true;
            const s32 b1 = (s32)mem_read32(cpu, probe->jb + row * 8u);
            const s32 b2 =
                (s32)mem_read32(cpu, probe->jb + row * 8u + 4u);
            fprintf(stderr,
                    "[physics-sor-invalid-input] step=%llu row=%u "
                    "bodies=%d,%d rhs=% .7g cfm=% .7g\n",
                    (unsigned long long)probe->quick_step, row, b1, b2,
                    (double)guest_read_f32(cpu, probe->rhs + row * 4u),
                    (double)guest_read_f32(cpu, probe->cfm + row * 4u));
            if (b1 >= 0 && (u32)b1 < probe->nb)
                log_physics_body(
                    cpu, "SOR-invalid-input-body-1",
                    mem_read32(cpu, probe->bodies + (u32)b1 * 4u));
            if (b2 >= 0 && (u32)b2 < probe->nb)
                log_physics_body(
                    cpu, "SOR-invalid-input-body-2",
                    mem_read32(cpu, probe->bodies + (u32)b2 * 4u));
            break;
        }
    }
    if (probe->active && probe->interesting)
        log_ball_sor_rows(cpu, "entry");
}

static void notify_SorLcpReturn(CPUState* cpu) {
    if (!g_ball_sor_probe.active)
        return;
    bool ball_force_finite = true;
    if (g_ball_sor_probe.ball_body_index >= 0) {
        const u32 force =
            g_ball_sor_probe.cforce +
            (u32)g_ball_sor_probe.ball_body_index * 24u;
        ball_force_finite = guest_vec3_finite(cpu, force) &&
                            guest_vec3_finite(cpu, force + 12u);
    }
    if (g_ball_sor_probe.interesting || !ball_force_finite)
        log_ball_sor_rows(cpu, "return");
    if (!ball_force_finite)
        report_ball_physics_failure(cpu, "SOR-LCP");
    if (!g_physics_sor_failure_reported) {
        for (u32 index = 0; index < g_ball_sor_probe.nb; ++index) {
            const u32 force = g_ball_sor_probe.cforce + index * 24u;
            if (guest_vec3_finite(cpu, force) &&
                guest_vec3_finite(cpu, force + 12u))
                continue;
            g_physics_sor_failure_reported = true;
            fprintf(stderr,
                    "[physics-sor-invalid-output] step=%llu body-index=%u\n",
                    (unsigned long long)g_ball_sor_probe.quick_step, index);
            log_physics_body(
                cpu, "SOR-invalid-output",
                mem_read32(cpu, g_ball_sor_probe.bodies + index * 4u));
            break;
        }
    }
    g_ball_sor_probe.active = false;
}

static void report_ball_physics_failure(CPUState* cpu, const char* boundary) {
    if (g_ball_physics_probe.first_failure_reported)
        return;
    g_ball_physics_probe.first_failure_reported = true;
    fprintf(stderr,
            "[ball-physics-first-failure] boundary=%s step=%llu "
            "guest-frame=%llu pc=0x%08X lr=0x%08X\n",
            boundary, (unsigned long long)g_ball_physics_probe.quick_step,
            (unsigned long long)(cpu->timebase / 675000ull), cpu->pc, cpu->lr);
}

static void notify_dWorldQuickStep(CPUState* cpu) {
    if (!ball_state_log_enabled())
        return;
    ++g_ball_physics_probe.quick_step;
    observe_physics_world(cpu, "quick-step-entry", cpu->gpr[3]);
    const bool periodic = g_ball_physics_probe.quick_step <= 8u ||
                          (g_ball_physics_probe.quick_step % 60u) == 0u;
    g_ball_physics_probe.quick_entry_finite =
        log_ball_physics_probe(cpu, "quick-step-entry", periodic);
    g_ball_physics_probe.quick_entry_valid = true;
    u32 ball;
    u32 body;
    if (ball_physics_addresses(cpu, &ball, NULL, &body) &&
        isfinite(guest_read_f32(cpu, ball + 0x44u)) &&
        fabsf(guest_read_f32(cpu, ball + 0x44u)) >= 5.0f)
        log_ball_contact_joints(cpu, "quick-step-entry", body);
    if (!g_ball_physics_probe.quick_entry_finite)
        report_ball_physics_failure(cpu, "before-quick-step");
}

static void notify_dxStepBody(CPUState* cpu) {
    observe_physics_body(cpu, "integrator-entry", cpu->gpr[3]);
    u32 body;
    if (!ball_state_log_enabled() ||
        !ball_physics_addresses(cpu, NULL, NULL, &body) ||
        cpu->gpr[3] != body)
        return;
    const bool finite = log_ball_physics_probe(cpu, "integrator-entry", false);
    if (!finite && g_ball_physics_probe.quick_entry_valid &&
        g_ball_physics_probe.quick_entry_finite) {
        log_ball_contact_joints(cpu, "integrator-entry", body);
        report_ball_physics_failure(cpu, "quick-step-solver");
    }
}

static void notify_PhysicsAIBallPostUpdate(CPUState* cpu) {
    if (!ball_state_log_enabled())
        return;
    const bool finite = log_ball_physics_probe(cpu, "ode-post-update", false);
    if (!finite && g_ball_physics_probe.quick_entry_valid &&
        g_ball_physics_probe.quick_entry_finite)
        report_ball_physics_failure(cpu, "quick-step-integration");
}

static void notify_cBallPostPhysicsUpdate(CPUState* cpu) {
    if (!ball_state_log_enabled())
        return;
    const bool finite =
        log_ball_physics_probe(cpu, "game-post-physics-entry", false);
    if (!finite)
        report_ball_physics_failure(cpu, "before-game-copy");
}

static void notify_cBallUpdateOrientation(CPUState* cpu) {
    if (!ball_state_log_enabled())
        return;
    const bool finite =
        log_ball_physics_probe(cpu, "game-update-orientation", false);
    if (!finite)
        report_ball_physics_failure(cpu, "inside-game-copy");
}

static void notify_dBodySetPosition(CPUState* cpu) {
    if (ball_state_log_enabled())
        log_nonfinite_body_set(cpu, "position", cpu->gpr[3], cpu->fpr[1],
                               cpu->fpr[2], cpu->fpr[3]);
    u32 body;
    if (!ball_state_log_enabled() ||
        !ball_physics_addresses(cpu, NULL, NULL, &body) ||
        cpu->gpr[3] != body)
        return;
    ++g_ball_physics_probe.position_sets;
    const bool finite = isfinite(cpu->fpr[1]) && isfinite(cpu->fpr[2]) &&
                        isfinite(cpu->fpr[3]);
    if (finite && g_ball_physics_probe.position_sets > 20u &&
        (g_ball_physics_probe.position_sets % 60u) != 0u)
        return;
    fprintf(stderr,
            "[ball-physics-set] kind=position count=%u step=%llu "
            "value=% .9g,% .9g,% .9g finite=%u lr=0x%08X\n",
            g_ball_physics_probe.position_sets,
            (unsigned long long)g_ball_physics_probe.quick_step,
            cpu->fpr[1], cpu->fpr[2], cpu->fpr[3], finite ? 1u : 0u, cpu->lr);
    if (!finite)
        report_ball_physics_failure(cpu, "dBodySetPosition");
}

static void notify_dBodySetLinearVel(CPUState* cpu) {
    if (ball_state_log_enabled())
        log_nonfinite_body_set(cpu, "linear-velocity", cpu->gpr[3],
                               cpu->fpr[1], cpu->fpr[2], cpu->fpr[3]);
    u32 body;
    if (!ball_state_log_enabled() ||
        !ball_physics_addresses(cpu, NULL, NULL, &body) ||
        cpu->gpr[3] != body)
        return;
    ++g_ball_physics_probe.velocity_sets;
    const bool finite = isfinite(cpu->fpr[1]) && isfinite(cpu->fpr[2]) &&
                        isfinite(cpu->fpr[3]);
    if (finite && g_ball_physics_probe.velocity_sets > 20u &&
        (g_ball_physics_probe.velocity_sets % 60u) != 0u)
        return;
    fprintf(stderr,
            "[ball-physics-set] kind=velocity count=%u step=%llu "
            "value=% .9g,% .9g,% .9g finite=%u lr=0x%08X\n",
            g_ball_physics_probe.velocity_sets,
            (unsigned long long)g_ball_physics_probe.quick_step,
            cpu->fpr[1], cpu->fpr[2], cpu->fpr[3], finite ? 1u : 0u, cpu->lr);
    if (!finite)
        report_ball_physics_failure(cpu, "dBodySetLinearVel");
}

static void notify_dBodySetAngularVel(CPUState* cpu) {
    if (ball_state_log_enabled())
        log_nonfinite_body_set(cpu, "angular-velocity", cpu->gpr[3],
                               cpu->fpr[1], cpu->fpr[2], cpu->fpr[3]);
}

static void notify_dBodySetRotation(CPUState* cpu) {
    if (!ball_state_log_enabled() ||
        !guest_ram_range_valid(cpu, cpu->gpr[4], 44u))
        return;
    const u32 matrix = cpu->gpr[4];
    bool finite = true;
    for (u32 row = 0; row < 3u; ++row)
        finite = finite && guest_vec3_finite(cpu, matrix + row * 16u);
    if (!finite)
        fprintf(stderr,
                "[physics-body-set-nonfinite] kind=rotation step=%llu "
                "body=0x%08X object=0x%08X matrix=0x%08X "
                "pc=0x%08X lr=0x%08X\n",
                (unsigned long long)g_ball_physics_probe.quick_step,
                cpu->gpr[3],
                guest_ram_range_valid(cpu, cpu->gpr[3], 20u)
                    ? mem_read32(cpu, cpu->gpr[3] + 0x0Cu)
                    : 0u,
                matrix, cpu->pc, cpu->lr);
}

static void notify_dBodySetForce(CPUState* cpu) {
    if (!ball_state_log_enabled())
        return;
    log_character_force_write(cpu, "set");
    log_nonfinite_body_set(cpu, "force", cpu->gpr[3], cpu->fpr[1],
                           cpu->fpr[2], cpu->fpr[3]);
}

static void notify_dBodyAddForce(CPUState* cpu) {
    if (!ball_state_log_enabled())
        return;
    log_character_force_write(cpu, "add");
    log_nonfinite_body_set(cpu, "add-force", cpu->gpr[3], cpu->fpr[1],
                           cpu->fpr[2], cpu->fpr[3]);
}

static void notify_PhysicsUpdate(CPUState* cpu) {
    if (guest_ram_range_valid(cpu, cpu->gpr[3], 4u))
        observe_physics_world(cpu, "physics-update-entry",
                              mem_read32(cpu, cpu->gpr[3]));
}

static void notify_PhysicsWorldPreUpdate(CPUState* cpu) {
    if (guest_ram_range_valid(cpu, cpu->gpr[3], 4u))
        observe_physics_world(cpu, "world-pre-update-entry",
                              mem_read32(cpu, cpu->gpr[3]));
}

static void notify_PhysicsWorldUpdate(CPUState* cpu) {
    if (guest_ram_range_valid(cpu, cpu->gpr[3], 4u))
        observe_physics_world(cpu, "world-update-entry",
                              mem_read32(cpu, cpu->gpr[3]));
}

#ifdef ED_AURORA

static bool guest_backchain_contains(CPUState* cpu, u32 start, u32 end) {
    if (cpu->lr >= start && cpu->lr < end)
        return true;
    u32 sp = cpu->gpr[1];
    const u32 ram_end = GC_RAM_BASE + cpu->ram_size;
    for (unsigned frame = 0; frame < 24u; ++frame) {
        if (sp < GC_RAM_BASE || sp + 8u > ram_end)
            break;
        const u32 caller = mem_read32(cpu, sp);
        if (caller <= sp || caller < GC_RAM_BASE || caller + 8u > ram_end)
            break;
        const u32 ret = mem_read32(cpu, caller + 4u);
        if (ret >= start && ret < end)
            return true;
        sp = caller;
    }
    return false;
}

static void notify_DrawableBallRender(CPUState* cpu) {
    if (!ball_state_log_enabled())
        return;
    static unsigned render_count = 0;
    ++render_count;
    if (render_count > 20u && (render_count % 60u) != 0u)
        return;

    const u32 snapshot_ball = cpu->gpr[3];
    const u32 ball = mem_read32(cpu, 0x80373664u); // g_pBall
    const u32 drawable = ball ? mem_read32(cpu, ball + 0x20u) : 0u;
    fprintf(stderr,
            "[ball-state] render=%u guest-frame=%llu snapshot=0x%08X "
            "visible=%u owner-index=%d pos=% .7g,% .7g,% .7g "
            "ball=0x%08X owner=0x%08X sim=% .7g,% .7g,% .7g "
            "drawable=0x%08X world=% .7g,% .7g,% .7g "
            "translation=% .7g,% .7g,% .7g\n",
            render_count,
            (unsigned long long)(cpu->timebase / 675000ull),
            snapshot_ball, (unsigned)mem_read8(cpu, snapshot_ball + 4u),
            (int)(s8)mem_read8(cpu, snapshot_ball + 6u),
            (double)guest_read_f32(cpu, snapshot_ball + 0x18u),
            (double)guest_read_f32(cpu, snapshot_ball + 0x1Cu),
            (double)guest_read_f32(cpu, snapshot_ball + 0x20u),
            ball, ball ? mem_read32(cpu, ball + 0x24u) : 0u,
            ball ? (double)guest_read_f32(cpu, ball + 0x40u) : 0.0,
            ball ? (double)guest_read_f32(cpu, ball + 0x44u) : 0.0,
            ball ? (double)guest_read_f32(cpu, ball + 0x48u) : 0.0,
            drawable,
            drawable ? (double)guest_read_f32(cpu, drawable + 0x34u) : 0.0,
            drawable ? (double)guest_read_f32(cpu, drawable + 0x38u) : 0.0,
            drawable ? (double)guest_read_f32(cpu, drawable + 0x3Cu) : 0.0,
            drawable ? (double)guest_read_f32(cpu, drawable + 0x58u) : 0.0,
            drawable ? (double)guest_read_f32(cpu, drawable + 0x5Cu) : 0.0,
            drawable ? (double)guest_read_f32(cpu, drawable + 0x60u) : 0.0);
}

// GXLoadPosMtxImm(MtxPtr mtx, u32 pnIdx): every 3D actor/camera modelview is
// loaded here. The frame-end gx_modelview snapshot only catches the LAST matrix
// (a 2D/overlay layer), so to tell whether the recomp ever applies the real 3D
// camera tilt we sample the matrices loaded mid-frame. To avoid flooding we log
// only matrices with a meaningful rotation (|row1.z| or |row2.y| > 0.1, the
// camera-tilt elements) plus a per-frame count of how many loads carried tilt.
static unsigned g_posmtx_loads_this_frame = 0;
static unsigned g_posmtx_tilted_this_frame = 0;
static unsigned g_posmtx_logged = 0;
static void notify_GXLoadPosMtxImm(CPUState* cpu) {
    static int matrix_log_enabled = -1;
    if (matrix_log_enabled < 0)
        matrix_log_enabled = getenv("ED_MATRIX_LOG") != NULL ? 1 : 0;
    const bool ball_load =
        ball_state_log_enabled() &&
        guest_backchain_contains(cpu, 0x8011DF10u, 0x8011E38Cu);
    if (!matrix_log_enabled && !ball_load)
        return;
    const u32 mtx_addr = hle_arg_u32(cpu, 0);
    const u32 pn_idx = hle_arg_u32(cpu, 1);
    u8 raw[48];
    copy_guest_to_host(cpu, mtx_addr, raw, sizeof raw);
    float m[12];
    for (int i = 0; i < 12; i++) {
        u32 b = ((u32)raw[i * 4] << 24) | ((u32)raw[i * 4 + 1] << 16) |
                ((u32)raw[i * 4 + 2] << 8) | (u32)raw[i * 4 + 3];
        memcpy(&m[i], &b, sizeof(float));
    }
    if (ball_load) {
        static unsigned ball_matrix_count = 0;
        ++ball_matrix_count;
        if (ball_matrix_count <= 40u || (ball_matrix_count % 60u) == 0u) {
            fprintf(stderr,
                    "[ball-posmtx] load=%u guest-frame=%llu begin=%u "
                    "addr=0x%08X pn=%u lr=0x%08X\n"
                    "  % .7g % .7g % .7g % .7g\n"
                    "  % .7g % .7g % .7g % .7g\n"
                    "  % .7g % .7g % .7g % .7g\n",
                    ball_matrix_count,
                    (unsigned long long)(cpu->timebase / 675000ull),
                    g_gx_begin_count, mtx_addr, pn_idx, cpu->lr,
                    (double)m[0], (double)m[1], (double)m[2], (double)m[3],
                    (double)m[4], (double)m[5], (double)m[6], (double)m[7],
                    (double)m[8], (double)m[9], (double)m[10], (double)m[11]);
        }
    }
    if (!matrix_log_enabled)
        return;
    ++g_posmtx_loads_this_frame;
    // Row1.z = m[6], Row2.y = m[9] carry the camera pitch/tilt rotation.
    const float tilt = (m[6] < 0 ? -m[6] : m[6]) + (m[9] < 0 ? -m[9] : m[9]);
    if (tilt > 0.1f) {
        ++g_posmtx_tilted_this_frame;
        if (g_posmtx_logged < 60u) {
            ++g_posmtx_logged;
            fprintf(stderr,
                    "[posmtx] id=%u TILTED:\n  % .5g % .5g % .5g % .5g\n"
                    "  % .5g % .5g % .5g % .5g\n  % .5g % .5g % .5g % .5g\n",
                    pn_idx, (double)m[0], (double)m[1], (double)m[2], (double)m[3],
                    (double)m[4], (double)m[5], (double)m[6], (double)m[7],
                    (double)m[8], (double)m[9], (double)m[10], (double)m[11]);
        }
    }
}

static void notify_GXLoadTexObj(CPUState* cpu) {
    record_GXLoadTexObj(cpu, hle_arg_u32(cpu, 0), hle_arg_u32(cpu, 1));
}

static void notify_GXLoadTlut(CPUState* cpu) {
    u32 slot = hle_arg_u32(cpu, 1);
    if (slot >= GX_MAX_TLUTS)
        return;
    u32 obj = hle_arg_u32(cpu, 0);
    u32 load_tlut0 = mem_read32(cpu, obj + 4u);
    u32 guest_data = GC_RAM_BASE | ((load_tlut0 & 0x001FFFFFu) << 5);
    u32 format = (mem_read32(cpu, obj) >> 10) & 3u;
    u16 entries = mem_read16(cpu, obj + 8u);
    u32 data_bytes = (u32)entries * 2u;
    u32 data_hash = guest_hash(cpu, guest_data, data_bytes, &data_bytes);
    const u32 version =
        gx_tlut_resource_version(format, entries, data_bytes, data_hash);
    GxTlutBinding* binding = &g_tluts[slot];
    binding->valid = true;
    binding->object = version;
    binding->guest_data = guest_data;
    binding->format = format;
    binding->entries = entries;
    binding->data_bytes = data_bytes;
    binding->data_hash = data_hash;
    binding->version = version;
    binding->emitted_version = 0;
}

static void emit_gx_resource_metadata(CPUState* cpu) {
    for (u32 slot = 0; slot < GX_MAX_TLUTS; slot++) {
        GxTlutBinding* binding = &g_tluts[slot];
        if (!binding->valid)
            continue;
        if (binding->emitted_version == binding->version)
            continue;
        void* data = graphics_guest_pointer(cpu, binding->guest_data, NULL);
        dol_platform_load_tlut_guest((u8)slot, binding->guest_data, data,
                                     binding->format, binding->entries,
                                     binding->object, binding->version);
        binding->emitted_version = binding->version;
    }

    for (u32 slot = 0; slot < GX_MAX_TEXTURES; slot++) {
        GxTextureBinding* binding = &g_textures[slot];
        if (!binding->valid)
            continue;
        if (binding->emitted_version == binding->version)
            continue;
        void* data = graphics_guest_pointer(cpu, binding->guest_data, NULL);
        if (g_graphics_log && g_gx_begin_count == 1 && data != NULL) {
            u64 pixels = (u64)binding->width * binding->height;
            u32 bytes = (binding->format == 0u || binding->format == 8u ||
                         binding->format == 14u)
                            ? (u32)((pixels + 1u) / 2u)
                            : (binding->format == 1u || binding->format == 2u ||
                               binding->format == 9u)
                                  ? (u32)pixels
                                  : (binding->format == 6u) ? (u32)(pixels * 4u)
                                                            : (u32)(pixels * 2u);
            const u8* source = (const u8*)data;
            u32 nonzero = 0;
            u32 hash = 2166136261u;
            for (u32 i = 0; i < bytes; i++) {
                nonzero += source[i] != 0;
                hash = (hash ^ source[i]) * 16777619u;
            }
            fprintf(stderr, "[gfx] tex-data slot=%u bytes=%u nonzero=%u hash=%08X\n",
                    slot, bytes, nonzero, hash);
        }
        if (g_graphics_log && g_gx_begin_count < 12)
            fprintf(stderr,
                    "[gfx] tex slot=%u obj=0x%08X data=0x%08X host=%p "
                    "%ux%u fmt=%u tlut=%u flags=0x%02X ver=%u\n",
                    slot, binding->object, binding->guest_data, data,
                    binding->width, binding->height, binding->format,
                    binding->tlut, binding->flags, binding->version);
        if (g_movie_log && binding->format == 1u) {
            const unsigned movie_tex_seq = ++g_movie_texture_load_count;
            if (movie_tex_seq < 96u || (movie_tex_seq % 60u) == 0u) {
                fprintf(stderr,
                        "[movie] draw-tex slot=%u data=0x%08X %ux%u fmt=I8 "
                        "bytes=%u hash=%08X ver=%u begin=%u seq=%u\n",
                        slot, binding->guest_data, binding->width, binding->height,
                        binding->data_bytes, binding->data_hash, binding->version,
                        g_gx_begin_count, movie_tex_seq);
            }
        }
        dol_platform_load_texture_guest(
            (u8)slot, binding->guest_data, data, binding->width,
            binding->height, binding->format, binding->tlut,
            (binding->flags & 1u) != 0, binding->object, binding->version);
        binding->emitted_version = binding->version;
    }
}

static void notify_GXBegin(CPUState* cpu) {
    ++g_gx_begin_count;
    if (ball_state_log_enabled() &&
        guest_backchain_contains(cpu, 0x8011DF10u, 0x8011E38Cu)) {
        fprintf(stderr,
                "[ball-draw] guest-frame=%llu begin=%u prim=%u fmt=%u "
                "count=%u lr=0x%08X\n",
                (unsigned long long)(cpu->timebase / 675000ull),
                g_gx_begin_count, hle_arg_u32(cpu, 0), hle_arg_u32(cpu, 1),
                hle_arg_u32(cpu, 2), cpu->lr);
    }
    if (g_graphics_log && g_gx_begin_count <= 12)
        fprintf(stderr,
                "[gfx] begin #%u prim=%u fmt=%u count=%u lr=0x%08X\n",
                g_gx_begin_count, hle_arg_u32(cpu, 0), hle_arg_u32(cpu, 1),
                hle_arg_u32(cpu, 2), cpu->lr);
    if (g_graphics_log && g_gx_begin_count == 1) {
        u32 packet = cpu->gpr[30];
        fprintf(stderr,
                "[gfx] packet=0x%08X index=0x%08X vertices=%u prim=%u "
                "streams=%u stream-list=0x%08X\n",
                packet, mem_read32(cpu, packet + 4u), mem_read16(cpu, packet + 8u),
                mem_read8(cpu, packet + 0x0Au), mem_read8(cpu, packet + 0x0Bu),
                mem_read32(cpu, packet + 0x0Cu));
        fprintf(stderr,
                "[gfx] packet-state texture=%08X%08X material=0x%08X "
                "program=0x%08X raster=0x%08X matrix=0x%08X "
                "textures=%08X,%08X,%08X,%08X,%08X,%08X "
                "texconfig=%u user-key=0x%08X material-set=0x%08X\n",
                mem_read32(cpu, packet + 0x10u),
                mem_read32(cpu, packet + 0x14u),
                mem_read32(cpu, packet + 0x18u),
                mem_read32(cpu, packet + 0x1Cu),
                mem_read32(cpu, packet + 0x20u),
                mem_read32(cpu, packet + 0x24u),
                mem_read32(cpu, packet + 0x28u),
                mem_read32(cpu, packet + 0x2Cu),
                mem_read32(cpu, packet + 0x30u),
                mem_read32(cpu, packet + 0x34u),
                mem_read32(cpu, packet + 0x38u),
                mem_read32(cpu, packet + 0x3Cu),
                mem_read8(cpu, packet + 0x40u),
                mem_read32(cpu, packet + 0x42u),
                mem_read32(cpu, packet + 0x46u));
        fprintf(stderr,
                "[gfx] state view=%u program=0x%08X texconfig=0x%08X "
                "viewport=%08X,%08X,%08X,%08X\n",
                mem_read32(cpu, 0x80374010u), mem_read32(cpu, 0x80372FA4u),
                mem_read32(cpu, 0x80374024u), mem_read32(cpu, 0x8032C238u),
                mem_read32(cpu, 0x8032C23Cu), mem_read32(cpu, 0x8032C240u),
                mem_read32(cpu, 0x8032C244u));
    }
}

static void notify_GXCopyTex(CPUState* cpu) {
    u32 address = hle_arg_u32(cpu, 0);
    void* data = graphics_guest_pointer(cpu, address, NULL);
    dol_platform_set_copy_destination_guest(address, data);
}

// GXCopyDisp(void* dest, GXBool clear) copies the EFB to the external frame
// buffer -- this is the game's real frame boundary (one per rendered frame).
// Drive Aurora's present here so each game frame is drained, rendered, and
// reset on its own. Presenting on VIWaitForRetrace instead let the front-end's
// load/transition loop (which renders many frames between retraces) pile every
// frame's geometry into a single Aurora frame packet, overflowing its fixed
// per-frame vertex/index staging buffers (ByteBuffer::resize abort). Aurora
// presents the EFB directly, so the stubbed display copy is harmless; we only
// need the boundary signal.
// Dump the game's GL/glx transform globals (G4QE01 addresses) as big-endian
// floats so the recomp's per-frame matrices can be diffed against Dolphin's
// View->Memory read of the same addresses. Gated by ED_MATRIX_LOG; rate
// limited so a long run does not flood. The conclusion fork: matching guest
// matrices + missing geometry -> bug is downstream (XF emission/Aurora); a
// degenerate/diverging matrix -> upstream recomp matrix math.
static void matrix_log_floats(CPUState* cpu, const char* name, u32 addr,
                              u32 count) {
    u8 raw[64];
    if (count * 4u > sizeof(raw))
        count = (u32)(sizeof(raw) / 4u);
    copy_guest_to_host(cpu, addr, raw, count * 4u);
    fprintf(stderr, "[matrix] %s @0x%08X:", name, addr);
    for (u32 i = 0; i < count; i++) {
        u32 bits = ((u32)raw[i * 4 + 0] << 24) | ((u32)raw[i * 4 + 1] << 16) |
                   ((u32)raw[i * 4 + 2] << 8) | (u32)raw[i * 4 + 3];
        float f;
        memcpy(&f, &bits, sizeof(f));
        if ((i & 3u) == 0u)
            fprintf(stderr, "\n  ");
        fprintf(stderr, " % .6g", (double)f);
    }
    fprintf(stderr, "\n");
}

static void matrix_log_frame(CPUState* cpu) {
    static int s_enabled = -1;
    static unsigned s_frame = 0;
    if (s_enabled < 0)
        s_enabled = getenv("ED_MATRIX_LOG") != NULL ? 1 : 0;
    if (!s_enabled)
        return;
    ++s_frame;
    if (s_frame > 20u && (s_frame % 60u) != 0u)
        return;
    fprintf(stderr, "[matrix] ---- frame %u ----\n", s_frame);
    matrix_log_floats(cpu, "gx_proj     ", 0x8032C090u, 16u); // projection 4x4
    matrix_log_floats(cpu, "gx_modelview", 0x8032C0D0u, 12u); // camera x object
    matrix_log_floats(cpu, "gx_mview    ", 0x8032C060u, 12u); // view (camera)
    // How many of this frame's GXLoadPosMtxImm loads carried a real 3D rotation.
    // Resolves whether the recomp ever applies the camera tilt (H2) or never
    // does (H1) — independent of the last-set frame-end gx_modelview.
    fprintf(stderr, "[matrix] posmtx loads=%u tilted=%u\n",
            g_posmtx_loads_this_frame, g_posmtx_tilted_this_frame);
    g_posmtx_loads_this_frame = 0;
    g_posmtx_tilted_this_frame = 0;
}

static void notify_GXCopyDisp(CPUState* cpu) {
    // TEMP DIAG (G013 cutscene): observe the guest transition-state flags each
    // frame so a live goal celebration can discriminate "guest chose not to
    // render the world" from "host skipped the draws". Gated by env.
    {
        static int s_cutscene_diag = -1;
        if (s_cutscene_diag < 0)
            s_cutscene_diag = getenv("ED_CUTSCENE_DIAG") != NULL ? 1 : 0;
        if (s_cutscene_diag) {
            static u64 s_diag_frame = 0;
            u8 render_world = mem_read8(cpu, 0x8037273Cu); // g_bRenderWorld
            // Log only transitions / every 60th frame to cut spam; always log
            // when world is off (H3-adjacent guest skip).
            static u8 s_prev_rw = 0xFFu;
            u64 fr = ++s_diag_frame;
            if (render_world != s_prev_rw || render_world == 0u ||
                (fr % 60u) == 0u)
                fprintf(stderr, "[cutscene] frame=%llu g_bRenderWorld=%u\n",
                        (unsigned long long)fr, render_world);
            s_prev_rw = render_world;
        }
    }
    matrix_log_frame(cpu);
    if (g_frame_array_bytes > g_frame_array_bytes_peak)
        g_frame_array_bytes_peak = g_frame_array_bytes;
    if (g_graphics_log && g_frame_array_calls)
        fprintf(stderr,
                "[gfx] frame array uploads: calls=%u bytes=%llu (%.2f MB) peak=%.2f MB cap=8MB\n",
                g_frame_array_calls,
                (unsigned long long)g_frame_array_bytes,
                (double)g_frame_array_bytes / (1024.0 * 1024.0),
                (double)g_frame_array_bytes_peak / (1024.0 * 1024.0));
    g_frame_array_bytes = 0;
    g_frame_array_calls = 0;
    movie_cadence_present(cpu);
}

static void notify_VIConfigure(CPUState* cpu) {
    u32 mode = hle_arg_u32(cpu, 0);
    if (!mode)
        return;
    if (g_graphics_log)
        fprintf(stderr,
                "[gfx] VIConfigure mode=0x%08X tv=%u fb=%ux%u xfb=%u vi=%ux%u\n",
                mode, mem_read32(cpu, mode), mem_read16(cpu, mode + 4u),
                mem_read16(cpu, mode + 6u), mem_read16(cpu, mode + 8u),
                mem_read16(cpu, mode + 0x0Eu), mem_read16(cpu, mode + 0x10u));
    dol_platform_configure_vi(
        mem_read32(cpu, mode),
        mem_read16(cpu, mode + 4u),
        mem_read16(cpu, mode + 6u),
        mem_read16(cpu, mode + 8u),
        mem_read16(cpu, mode + 0x0Eu),
        mem_read16(cpu, mode + 0x10u));
}

// E7 Virtual XFB: latch guest XFB address for present-from-XFB.
static void notify_VISetNextFrameBuffer(CPUState* cpu) {
    u32 fb = hle_arg_u32(cpu, 0);
    dol_platform_set_next_xfb(fb & 0x1FFFFFFFu);
}

// The frame is presented at VIFlush, not at GXCopyDisp as the Strikers host
// did: this game copies the frame (GXCopyDisp), then points the VI at it
// (VISetNextFrameBuffer) and flushes. Presenting on entry to GXCopyDisp showed
// the VI's buffer before its display copy existed, which was a black window.
static void notify_VIFlush(CPUState* cpu) {
    (void)cpu;
    dol_platform_flush_xfb();
    mmio_fifo_dump_note("present", 0, 0);
    dol_platform_present();
}

// TEMP DIAG (G013 H3): guest glxSwapSetBlack / VISetBlack. Gated by
// ED_CUTSCENE_DIAG. Discriminates "guest left VI black latched" from
// host skip paths (H1/H2). Does not alter black state — observe only.
static void notify_glxSwapSetBlack(CPUState* cpu) {
    static int s_cutscene_diag = -1;
    if (s_cutscene_diag < 0)
        s_cutscene_diag = getenv("ED_CUTSCENE_DIAG") != NULL ? 1 : 0;
    if (!s_cutscene_diag)
        return;
    fprintf(stderr, "[cutscene] glxSwapSetBlack black=%u lr=0x%08X\n",
            cpu->gpr[3] & 0xFFu, cpu->lr);
}

static void notify_VISetBlack(CPUState* cpu) {
    // E7: always latch black into the platform (present-from-XFB respects it).
    dol_platform_set_xfb_black((cpu->gpr[3] & 0xFFu) != 0u);
    {
        static int s_cutscene_diag = -1;
        if (s_cutscene_diag < 0)
            s_cutscene_diag = getenv("ED_CUTSCENE_DIAG") != NULL ? 1 : 0;
        if (s_cutscene_diag)
            fprintf(stderr, "[cutscene] VISetBlack black=%u lr=0x%08X\n",
                    cpu->gpr[3] & 0xFFu, cpu->lr);
    }
}

static void notify_VIWaitForRetrace(CPUState* cpu) {
    ++g_vi_wait_count;
    if (g_graphics_log && (g_vi_wait_count <= 12u || g_vi_wait_count % 60u == 0u)) {
        u32 manager = mem_read32(cpu, 0x803742A4u);
        u32 active = manager ? mem_read32(cpu, manager + 0xA04u) : 0u;
        fprintf(stderr, "[gfx] retrace=%u async-manager=0x%08X active=0x%08X",
                g_vi_wait_count, manager, active);
        if (active) {
            u32 entry = mem_read32(cpu, active);
            fprintf(stderr,
                    " entry=0x%08X file=0x%08X callback=0x%08X buffer=0x%08X "
                    "size=%u position=%u phase=%u remaining=%u",
                    entry, mem_read32(cpu, entry + 8u), mem_read32(cpu, entry + 0x0Cu),
                    mem_read32(cpu, entry + 0x10u), mem_read32(cpu, entry + 0x14u),
                    mem_read32(cpu, entry + 0x18u), mem_read32(cpu, entry + 0x20u),
                    mem_read32(cpu, entry + 0x24u));
        }
        fputc('\n', stderr);
    }
    // The frame is presented from GXCopyDisp (the real frame boundary); the VI
    // retrace only paces the game (handled by the retrace interrupt).
}

static void hle_PADInit(CPUState* cpu) {
    hle_set_u32(cpu, dol_platform_pad_init() ? 1u : 0u);
}

static void hle_PADRead(CPUState* cpu) {
    u32 out = hle_arg_u32(cpu, 0);
    DolPadState state[4];
    memset(state, 0, sizeof state);
    u32 motor_mask = dol_platform_pad_read(state);
    if (g_auto_input) {
        const u64 frame = cpu->timebase / 675000ull;
        if (frame >= 135u && ((frame - 135u) % 90u) < 12u) {
            state[0].button |= 0x0100u;
            state[0].analog_a = 0xFFu;
            if (g_auto_input_last_pulse != frame) {
                fprintf(stderr, "[input] scripted A at frame %llu\n",
                        (unsigned long long)frame);
                g_auto_input_last_pulse = frame;
            }
        }
        state[0].error = 0;
    }
    if (g_auto_input_once_frame != 0) {
        const u64 frame = cpu->timebase / 675000ull;
        if (frame >= g_auto_input_once_frame &&
            frame < g_auto_input_once_frame + 8u &&
            g_auto_input_once_sent != g_auto_input_once_frame) {
            state[0].button |= 0x0100u;
            state[0].analog_a = 0xFFu;
            state[0].error = 0;
            fprintf(stderr, "[input] one-shot A at frame %llu\n",
                    (unsigned long long)frame);
            g_auto_input_once_sent = g_auto_input_once_frame;
        }
    }
    if (g_auto_skip_card_prompt) {
        const u64 frame = cpu->timebase / 675000ull;
        u16 scripted = 0;
        const char* action = NULL;
        if (frame >= 360u && frame < 372u) {
            scripted = 0x0004u; // PAD_BUTTON_DOWN
            action = "Down";
        } else if (frame >= 420u && frame < 432u) {
            scripted = 0x0100u; // PAD_BUTTON_A
            action = "A";
        }
        if (scripted != 0) {
            state[0].button |= scripted;
            if (scripted == 0x0004u)
                state[0].stick_y = -80;
            if (scripted == 0x0100u)
                state[0].analog_a = 0xFFu;
            state[0].error = 0;
            if (g_auto_skip_card_last_pulse != frame) {
                fprintf(stderr, "[input] card-prompt %s at frame %llu\n",
                        action, (unsigned long long)frame);
                g_auto_skip_card_last_pulse = frame;
            }
        }
    }
    if (input_script_apply(cpu, &state[0].button, &state[0].stick_x,
                           &state[0].stick_y, &state[0].analog_a))
        state[0].error = 0;
    if (skip_intro_apply(cpu, &state[0].button, state[0].stick_x, state[0].stick_y,
                         &state[0].analog_a))
        state[0].error = 0;
    mash_to_gameplay_apply(cpu, &state[0].button, &state[0].stick_x,
                           &state[0].analog_a);
    if (g_mash_to_gameplay)
        state[0].error = 0;
    for (u32 i = 0; i < 4; i++) {
        u32 p = out + i * 12u;
        mem_write16(cpu, p, state[i].button);
        mem_write8(cpu, p + 2u, (u8)state[i].stick_x);
        mem_write8(cpu, p + 3u, (u8)state[i].stick_y);
        mem_write8(cpu, p + 4u, (u8)state[i].substick_x);
        mem_write8(cpu, p + 5u, (u8)state[i].substick_y);
        mem_write8(cpu, p + 6u, state[i].trigger_left);
        mem_write8(cpu, p + 7u, state[i].trigger_right);
        mem_write8(cpu, p + 8u, state[i].analog_a);
        mem_write8(cpu, p + 9u, state[i].analog_b);
        mem_write8(cpu, p + 10u, (u8)state[i].error);
        mem_write8(cpu, p + 11u, 0);
    }
    hle_set_u32(cpu, motor_mask);
}

static void hle_PADReset(CPUState* cpu) {
    bool reset = dol_platform_pad_reset(hle_arg_u32(cpu, 0));
    hle_set_u32(cpu, (reset || g_mash_to_gameplay) ? 1u : 0u);
}

static void hle_PADRecalibrate(CPUState* cpu) {
    bool recalibrated = dol_platform_pad_recalibrate(hle_arg_u32(cpu, 0));
    hle_set_u32(cpu, (recalibrated || g_mash_to_gameplay) ? 1u : 0u);
}

static void hle_PADControlMotor(CPUState* cpu) {
    dol_platform_pad_control_motor(hle_arg_u32(cpu, 0), hle_arg_u32(cpu, 1));
}

static void hle_PADSetSpec(CPUState* cpu) {
    dol_platform_pad_set_spec(hle_arg_u32(cpu, 0));
}
#else
// Deterministic controller pulses for headless integration tests. Port 1 always
// reports a connected controller (with nothing pressed unless scripted): this
// game parks on its "controller unplugged" screen otherwise.
static bool headless_scripted_pad(CPUState* cpu,
                                  u16* buttons,
                                  s8* stick_x,
                                  s8* stick_y,
                                  u8* analog_a) {
    const bool connected = true;
    const u64 frame = cpu->timebase / 675000ull;
    if (g_auto_input) {
        const bool pressed = frame >= 135u && ((frame - 135u) % 90u) < 12u;
        if (pressed) {
            *buttons |= 0x0100u;
            *analog_a = 0xFFu;
            if (g_auto_input_last_pulse != frame) {
                fprintf(stderr, "[input] scripted A at frame %llu\n",
                        (unsigned long long)frame);
                g_auto_input_last_pulse = frame;
            }
        }
    }
    (void)input_script_apply(cpu, buttons, stick_x, stick_y, analog_a);
    (void)skip_intro_apply(cpu, buttons, *stick_x, *stick_y, analog_a);
    mash_to_gameplay_apply(cpu, buttons, stick_x, analog_a);
    return connected;
}

static void hle_PADInit(CPUState* cpu) {
    hle_set_u32(cpu, 1u);
}

static void hle_PADRead(CPUState* cpu) {
    const u32 out = hle_arg_u32(cpu, 0);
    u16 buttons = 0;
    s8 stick_x = 0;
    s8 stick_y = 0;
    u8 analog_a = 0;
    const bool connected =
        headless_scripted_pad(cpu, &buttons, &stick_x, &stick_y, &analog_a);
    for (u32 i = 0; i < 4u; i++) {
        const u32 p = out + i * 12u;
        mem_write16(cpu, p, i == 0u ? buttons : 0u);
        mem_write8(cpu, p + 2u, (u8)(i == 0u ? stick_x : 0));
        mem_write8(cpu, p + 3u, (u8)(i == 0u ? stick_y : 0));
        for (u32 byte = 4u; byte < 8u; byte++)
            mem_write8(cpu, p + byte, 0u);
        mem_write8(cpu, p + 8u, i == 0u ? analog_a : 0u);
        mem_write8(cpu, p + 9u, 0u);
        mem_write8(cpu, p + 10u, (u8)((i == 0u && connected) ? 0 : -1));
        mem_write8(cpu, p + 11u, 0u);
    }
    hle_set_u32(cpu, 0u);
}

static void hle_PADReset(CPUState* cpu) {
    hle_set_u32(cpu, 1u);
}

static void hle_PADRecalibrate(CPUState* cpu) {
    hle_set_u32(cpu, 1u);
}

static void hle_PADControlMotor(CPUState* cpu) {
    (void)cpu;
}

static void hle_PADSetSpec(CPUState* cpu) {
    (void)cpu;
}
#endif

// ---------------------------------------------------------------------------
// Handler registry
// ---------------------------------------------------------------------------
// Aurora-implemented SDK functions, matched by name against the generated table.
static const HleEntry kHandlers[] = {
    { "OSReport",          hle_OSReport },
    { "ARInit",            hle_ARInit },
    { "ARGetBaseAddress",  hle_ARGetBaseAddress },
    { "ARGetSize",         hle_ARGetSize },
    { "ARGetDMAStatus",    hle_ARGetDMAStatus },
    { "ARStartDMA",        hle_ARStartDMA },
    { "CARDInit",          hle_CARDInit },
    { "CARDProbe",         hle_CARDProbe },
    { "CARDProbeEx",       hle_CARDProbeEx },
    { "CARDGetResultCode", hle_CARDGetResultCode },
    { "CARDGetFastMode",   hle_CARDGetFastMode },
    { "CARDGetXferredBytes", hle_CARDGetXferredBytes },
    { "CARDMountAsync",    hle_CARDMountAsync },
    { "CARDCheckExAsync",  hle_CARDCheckExAsync },
    { "CARDCheckAsync",    hle_CARDCheckAsync },
    { "CARDFreeBlocks",    hle_CARDFreeBlocks },
    { "CARDOpen",          hle_CARDOpen },
    { "CARDCreateAsync",   hle_CARDCreateAsync },
    { "CARDDeleteAsync",   hle_CARDDeleteAsync },
    { "CARDGetStatus",     hle_CARDGetStatus },
    { "CARDSetStatusAsync", hle_CARDSetStatusAsync },
    { "CARDGetSerialNo",   hle_CARDGetSerialNo },
    { "CARDUnmount",       hle_CARDUnmount },
    { "CARDClose",         hle_CARDClose },
    { "CARDReadAsync",     hle_CARDReadAsync },
    { "CARDWriteAsync",    hle_CARDWriteAsync },
};

// Functions intercepted by address rather than by Aurora name. These are host-
// owned hardware/OS boundaries below (or beside) the public SDK API: either a
// helper that busy-waits on unmodeled hardware (skipped) or a device the host
// implements itself (DVD, served from the ISO). Addresses are from the decomp
// symbol map (G4QE01).
typedef struct {
    u32         address;
    HleHandler  fn;
    const char* name;
} HleAddrEntry;

static const HleAddrEntry kAddrHandlers[] = {
    // DSP boot handshake: polls a DSP mailbox bit that never changes without a
    // modeled DSP. Audio is routed through Aurora, so skip the hardware init.
    { 0u, hle_noop,                     "__OSInitAudioSystem" },
    // AI sample-rate-converter calibration: times transitions of the AI sample
    // counter (__AIRegs[2]), which never ticks without modeled AI hardware.
    // Aurora owns audio, so skip the calibration (its result feeds only AI regs).
    { 0u, hle_noop,                     "__AI_SRC_INIT" },
    // MusyX's CPU-side sequencer and voice engine run unchanged. Only skip the
    // physical DSP task boot; audio.c executes the resulting AX command lists.
    { 0u, hle_salInitDsp,                "salInitDsp" },
    { 0u, hle_ARQPostRequest,            "ARQPostRequest" },
    { 0u, hle_return_zero,                "DSPCheckMailToDSP" },
    // Cache maintenance: this host has no dcache/icache model, so the SDK range
    // helpers would only loop dcbf/dcbi/dcbst/icbi (each a no-op through the
    // instruction fallback and a return to run_loop) over every 32-byte line:
    // ~0.6 MB a frame, 4.5 M round trips in 7 s at the "rats" cutscene. Skip them.
    { 0u, hle_noop,                     "DCInvalidateRange" },
    { 0u, hle_noop,                     "DCFlushRange" },
    { 0u, hle_noop,                     "DCStoreRange" },
    { 0u, hle_noop,                     "DCFlushRangeNoSync" },
    { 0u, hle_noop,                     "DCStoreRangeNoSync" },
    { 0u, hle_noop,                     "ICInvalidateRange" },
    // DVD: host serves the disc file system from the ISO (see DVD handlers).
    { 0u, hle_DVDInit,                  "DVDInit" },
    { 0u, hle_DVDConvertPathToEntrynum, "DVDConvertPathToEntrynum" },
    { 0u, hle_DVDFastOpen,              "DVDFastOpen" },
    { 0u, hle_DVDOpen,                  "DVDOpen" },
    { 0u, hle_DVDChangeDir,             "DVDChangeDir" },
    { 0u, hle_DVDGetCurrentDir,         "DVDGetCurrentDir" },
    { 0u, hle_DVDClose,                 "DVDClose" },
    { 0u, hle_DVDReadAsyncPrio,         "DVDReadAsyncPrio" },
    { 0u, hle_DVDGetCommandBlockStatus, "DVDGetCommandBlockStatus" },
    { 0u, hle_DVDGetDriveStatus,        "DVDGetDriveStatus" },
    { 0u, hle_DVDReadPrio,              "DVDReadPrio" },
    { 0u, hle_DVDReadAbsAsyncPrio,      "DVDReadAbsAsyncPrio" },
    { 0u, hle_DVDReadDiskID,            "DVDReadDiskID" },
    { 0u, hle_DVDInquiryAsync,          "DVDInquiryAsync" },
    { 0u, hle_DVDCancelAsync,           "DVDCancelAsync" },
    { 0u, hle_DVDCancel,                "DVDCancel" },
    { 0u, hle_DVDCancelAll,             "DVDCancelAll" },
    { 0u, hle_DVDCheckDisk,             "DVDCheckDisk" },
    { 0u, hle_DVDGetCurrentDiskID,      "DVDGetCurrentDiskID" },
    { 0u, hle_DVDReset,                 "DVDReset" },
    { 0u, hle_DVDPrepareStreamAsync,    "DVDPrepareStreamAsync" },
    { 0u, hle_DVDPrepareStreamAbsAsync, "DVDPrepareStreamAbsAsync" },
    { 0u, hle_DVDCancelStreamAsync,     "DVDCancelStreamAsync" },
    { 0u, hle_DVDStopStreamAtEndAsync,  "DVDStopStreamAtEndAsync" },
    { 0u, hle_DVDGetStreamErrorStatusAsync, "DVDGetStreamErrorStatusAsync" },
    { 0u, hle_DVDGetStreamPlayAddrAsync, "DVDGetStreamPlayAddrAsync" },
    // CARDFormatAsync is present in G4QE01 but absent from Aurora's SDK header
    // intersection, so it is registered by its exact game address.
    { 0u, hle_CARDFormatAsync,           "CARDFormatAsync" },
    { 0u, hle_OSGetResetButtonState,    "OSGetResetButtonState" },
    // Locked-cache DMA helpers. THP video decode writes into the 0xE0000000
    // scratch area and uses these to move rows into MEM1 texture planes.
    { 0u, hle_LCStoreBlocks,             "LCStoreBlocks" },
    { 0u, hle_LCLoadBlocks,              "LCLoadBlocks" },
    { 0u, hle_LCStoreData,               "LCStoreData" },
    { 0u, hle_noop,                      "LCQueueWait" },
#ifdef ED_AURORA
    { 0u, hle_PSMTXConcat,                "PSMTXConcat" },
    { 0u, hle_GXSetArray,                "GXSetArray" },
    { 0u, hle_GXBegin,                   "GXBegin" },
    { 0u, hle_GXCallDisplayList,          "GXCallDisplayList" },
#endif
    { 0u, hle_PADReset,                  "PADReset" },
    { 0u, hle_PADRecalibrate,            "PADRecalibrate" },
    { 0u, hle_PADInit,                   "PADInit" },
    { 0u, hle_PADRead,                   "PADRead" },
    { 0u, hle_PADControlMotor,           "PADControlMotor" },
    { 0u, hle_PADSetSpec,                "PADSetSpec" },
};

// Functions to trace (log name + first args) without intercepting: the
// recompiled body still runs. A debugging aid for bringing up the OS scheduler,
// VI, and interrupt paths. Enabled with ED_HLE_LOG=1.
typedef struct {
    u32         address;
    const char* name;
} HleTraceEntry;

static HleTraceEntry kTrace[] = {
    { 0u, "GXGetGPStatus" },
    { 0u, "GXGetFifoStatus" },
    { 0u, "GXEnableBreakPt" },
    { 0u, "GXDisableBreakPt" },
    { 0u, "OSSendMessage" },
    { 0u, "OSReceiveMessage" },
    { 0u, "VISetPostRetraceCallback" },
    { 0u, "VISetPreRetraceCallback" },
    { 0u, "GXSetBreakPtCallback" },
    { 0u, "GXSetDrawDoneCallback" },
    { 0u, "GXSetDrawDone" },
    { 0u, "GXFinishInterruptHandler" },
    { 0u, "OSJamMessage" },
    { 0u, "OSDisableScheduler" },
    { 0u, "VISetBlack" },
    { 0u, "GXCopyDisp" },
    { 0u, "VISetNextFrameBuffer" },
    { 0u, "OSEnableScheduler" },
    { 0u, "ARInit" },
    { 0u, "ARGetBaseAddress" },
    { 0u, "ARGetSize" },
    { 0u, "VIInit" },
    { 0u, "VIWaitForRetrace" },
    { 0u, "VIConfigure" },
    { 0u, "VISetNextFrameBuffer" },
    { 0u, "OSLoadContext" },
    { 0u, "__OSReschedule" },
    { 0u, "OSResumeThread" },
    { 0u, "__OSInterruptInit" },
    { 0u, "OSSleepThread" },
    { 0u, "OSSuspendThread" },
    { 0u, "GXSetDrawDone" },
    { 0u, "GXFinishInterruptHandler" },
};

static const char* trace_name(u32 address) {
    for (size_t i = 0; i < sizeof kTrace / sizeof kTrace[0]; i++)
        if (kTrace[i].address == address)
            return kTrace[i].name;
    return NULL;
}

// Notify hooks run *alongside* the recompiled function (it still executes), to
// observe a call and drive host state -- unlike kAddrHandlers, which replace it.

// FinishQueue in GXMisc.c for G4QE01. A thread sleeps here (in GXWaitDrawDone)
// only while a draw-done token is in flight (DrawDone == 0), so the wait itself
// is the faithful trigger for the Pixel Engine FINISH interrupt. By the time
// execution reaches this OSSleepThread call external interrupts are disabled, so
// committing here enqueues the waiter before interrupt_poll can deliver FINISH,
// matching a GPU completing submitted work. This covers both the standalone
// GXSetDrawDone+GXWaitDrawDone pair and the inlined GXDrawDone() (which never
// calls the standalone GXSetDrawDone -- the loading-screen GXDrawDone() used to
// hang here forever because nothing armed the finish).
#define GX_FINISH_QUEUE ED_GX_FINISH_QUEUE

// This game sets a draw-done callback and calls GXSetDrawDone once a frame
// without ever waiting in GXWaitDrawDone; the callback, run from the PE finish
// interrupt, receives the message the main loop sends just after. Raise the
// interrupt once the GPU would have finished, after that send.
static void notify_GXSetDrawDone(CPUState* cpu) {
    (void)cpu;
    interrupt_defer_pe_finish();
}

static void notify_GXSetDrawSync(CPUState* cpu) {
    interrupt_commit_pe_token((u16)hle_arg_u32(cpu, 0));
}

static void notify_PPCMtwpar(CPUState* cpu) {
    (void)cpu;
    mmio_write_gather_reset();
}

// The breakpoint moves to the current write pointer: the GPU can now run up to
// there, through the tokens held behind the old breakpoint (interrupt.c).
static void notify_GXEnableBreakPt(CPUState* cpu) {
    (void)cpu;
    interrupt_gp_breakpoint_moved();
}

static void notify_OSSleepThread(CPUState* cpu) {
    if (hle_arg_u32(cpu, 0) == GX_FINISH_QUEUE)
        interrupt_commit_pe_finish();
}

static void notify_DSPSendMailToDSP(CPUState* cpu) {
    audio_dsp_mail(cpu, hle_arg_u32(cpu, 0));
}

// B2.1 measure: per-entry call counts for certificate-demanded MusyX API surface.
// Names are G4QE01 symbols; addresses from symbols.txt / certificate_G4QE01.json.
typedef struct {
    u32 address;
    const char* name;
    u64 calls;
} AudioApiCounter;

static AudioApiCounter g_audio_api[] = {
    { 0u, "sndInit", 0 },
    { 0u, "sndQuit", 0 },
    { 0u, "sndIsInstalled", 0 },
    { 0u, "sndSetHooks", 0 },
    { 0u, "sndFXStartParaInfo", 0 },
    { 0u, "sndFXKeyOff", 0 },
    { 0u, "sndFXCheck", 0 },
    { 0u, "sndFXCtrl", 0 },
    { 0u, "sndFXCtrl14", 0 },
    { 0u, "sndFXAssignVolGroup2FXId", 0 },
    { 0u, "sndVolume", 0 },
    { 0u, "sndSilence", 0 },
    { 0u, "sndOutputMode", 0 },
    { 0u, "sndSetAuxProcessingCallbacks", 0 },
    { 0u, "sndStreamActivate", 0 },
    { 0u, "sndStreamDeactivate", 0 },
    { 0u, "sndStreamAllocEx", 0 },
    { 0u, "sndStreamFree", 0 },
    { 0u, "sndStreamMixParameterEx", 0 },
    { 0u, "sndStreamARAMUpdate", 0 },
    { 0u, "sndStreamLPFParameter", 0 },
    { 0u, "sndStreamADPCMParameter", 0 },
    { 0u, "sndStreamFrq", 0 },
    { 0u, "sndPushGroup", 0 },
    { 0u, "sndPopGroup", 0 },
    { 0u, "sndSeqPlayEx", 0 },
    { 0u, "sndStackSetCurrent", 0 },
    { 0u, "sndStackGetSize", 0 },
    { 0u, "sndStackAdd", 0 },
    { 0u, "sndStackGetARAMAddressRange", 0 },
    { 0u, "sndStackGetAvailableSampleMemory", 0 },
    { 0u, "sndSetSampleDataUploadCallback", 0 },
    { 0u, "sndAddEmitter2StudioPara", 0 },
    { 0u, "sndUpdateEmitter", 0 },
    { 0u, "sndRemoveEmitter", 0 },
    { 0u, "sndCheckEmitter", 0 },
    { 0u, "sndEmitterVoiceID", 0 },
    { 0u, "sndAddListenerEx", 0 },
    { 0u, "sndUpdateListener", 0 },
    { 0u, "sndRemoveListener", 0 },
    { 0u, "sndPitchUpOne", 0 },
    { 0u, "sndGetPitch", 0 },
    { 0u, "sndSin", 0 },
    { 0u, "sndAuxCallbackPrepareReverbSTD", 0 },
    { 0u, "sndAuxCallbackUpdateSettingsReverbSTD", 0 },
    { 0u, "sndAuxCallbackShutdownReverbSTD", 0 },
    { 0u, "sndAuxCallbackPrepareReverbHI", 0 },
    { 0u, "sndAuxCallbackUpdateSettingsReverbHI", 0 },
    { 0u, "sndAuxCallbackShutdownReverbHI", 0 },
    { 0u, "sndAuxCallbackPrepareDelay", 0 },
    { 0u, "sndAuxCallbackUpdateSettingsDelay", 0 },
    { 0u, "sndAuxCallbackShutdownDelay", 0 },
    { 0u, "sndAuxCallbackPrepareChorus", 0 },
    { 0u, "sndAuxCallbackUpdateSettingsChorus", 0 },
    { 0u, "sndAuxCallbackShutdownChorus", 0 },
};

static void notify_audio_api(CPUState* cpu) {
    for (size_t i = 0; i < sizeof g_audio_api / sizeof g_audio_api[0]; i++) {
        if (g_audio_api[i].address == cpu->pc) {
            g_audio_api[i].calls++;
            if (g_audio_log && (g_audio_api[i].calls <= 8u ||
                                (g_audio_api[i].calls % 64u) == 0u))
                fprintf(stderr,
                        "[audio] API %s #%llu r3=0x%08X r4=0x%08X r5=0x%08X\n",
                        g_audio_api[i].name,
                        (unsigned long long)g_audio_api[i].calls, cpu->gpr[3],
                        cpu->gpr[4], cpu->gpr[5]);
            return;
        }
    }
    if (g_audio_log)
        fprintf(stderr, "[audio] API 0x%08X r3=0x%08X r4=0x%08X r5=0x%08X\n",
                cpu->pc, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}

void hle_audio_api_report(void) {
    if (!g_audio_log)
        return;
    u64 total = 0;
    u32 fired = 0;
    for (size_t i = 0; i < sizeof g_audio_api / sizeof g_audio_api[0]; i++) {
        if (g_audio_api[i].calls) {
            fired++;
            total += g_audio_api[i].calls;
        }
    }
    fprintf(stderr,
            "[audio] API report: entries=%zu fired=%u total_calls=%llu\n",
            sizeof g_audio_api / sizeof g_audio_api[0], fired,
            (unsigned long long)total);
    for (size_t i = 0; i < sizeof g_audio_api / sizeof g_audio_api[0]; i++) {
        if (g_audio_api[i].calls)
            fprintf(stderr, "[audio]   %-40s %llu\n", g_audio_api[i].name,
                    (unsigned long long)g_audio_api[i].calls);
    }
    for (size_t i = 0; i < sizeof g_audio_api / sizeof g_audio_api[0]; i++) {
        if (!g_audio_api[i].calls)
            fprintf(stderr, "[audio]   %-40s 0 (demanded, never called)\n",
                    g_audio_api[i].name);
    }
}

static void notify_OSResetSystem(CPUState* cpu) {
    fprintf(stderr,
            "[reset] OSResetSystem mode=%u code=0x%08X force-menu=%u lr=0x%08X\n",
            cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->lr);
}

static void notify_SetNextState(CPUState* cpu) {
    if (g_mash_to_gameplay && g_mash_side_assigned &&
        !g_mash_route_complete) {
        if (g_mash_prematch && cpu->gpr[3] == 0x00000002u) {
            g_mash_route_complete = true;
            fprintf(stderr,
                    "[input] gameplay reached; controller is now neutral\n");
        } else if (!g_mash_prematch && cpu->gpr[3] == 0x00000100u) {
            g_mash_prematch = true;
            fprintf(stderr,
                    "[input] prematch reached; mashing A only\n");
        }
    }
    if (!g_state_log)
        return;
    const u32 manager = mem_read32(cpu, 0x803742B8u);
    fprintf(stderr,
            "[state] SetNextState next=0x%08X manager=0x%08X "
            "current=0x%08X pending=0x%08X lr=0x%08X\n",
            cpu->gpr[3], manager,
            manager ? mem_read32(cpu, manager + 8u) : 0u,
            manager ? mem_read32(cpu, manager + 0x0Cu) : 0u, cpu->lr);
}

static void notify_TaskManagerStartup(CPUState* cpu) {
    if (g_state_log)
        fprintf(stderr,
                "[state] TaskManager::Startup initial=0x%08X old=0x%08X "
                "lr=0x%08X timebase=%llu\n",
                cpu->gpr[3], mem_read32(cpu, 0x803742B8u), cpu->lr,
                (unsigned long long)cpu->timebase);
}

static void fe_route_pad_state(CPUState* cpu,
                               u16* buttons,
                               s8* stick_x,
                               s8* stick_y,
                               s8* err,
                               u16* just_pressed) {
    *buttons = 0;
    *stick_x = 0;
    *stick_y = 0;
    *err = -1;
    *just_pressed = 0;
    const u32 current = mem_read32(cpu, 0x80372FF0u); // PadStatus::s_Current
    if (current >= GC_RAM_BASE && current + 12u <= GC_RAM_BASE + cpu->ram_size) {
        *buttons = mem_read16(cpu, current);
        *stick_x = (s8)mem_read8(cpu, current + 2u);
        *stick_y = (s8)mem_read8(cpu, current + 3u);
        *err = (s8)mem_read8(cpu, current + 10u);
    }
    const u32 categories = mem_read32(cpu, 0x80372FF8u); // platpad.cpp padStatus
    if (categories >= GC_RAM_BASE &&
        categories + 0x382u <= GC_RAM_BASE + cpu->ram_size) {
        *just_pressed = mem_read16(cpu, categories + 0x380u);
    }
}

static bool fe_route_should_log(CPUState* cpu,
                                u16 buttons,
                                u16 just_pressed,
                                u64* last_frame) {
    const u64 frame = cpu->timebase / 675000ull;
    if (*last_frame == frame)
        return false;
    if (buttons != 0u || just_pressed != 0u || (frame % 120u) == 0u) {
        *last_frame = frame;
        return true;
    }
    return false;
}

static void notify_SHMainMenuUpdate(CPUState* cpu) {
    if (!g_state_log)
        return;
    static u64 s_last_frame = ~(u64)0;
    static s32 s_last_index = -999;
    const u32 self = cpu->gpr[3];
    const s32 index = (s32)mem_read32(cpu, self + 0x228u);
    const s32 count = (s32)mem_read32(cpu, self + 0x22Cu);
    const u32 flags = mem_read32(cpu, self + 0x234u);
    u16 buttons = 0;
    u16 just_pressed = 0;
    s8 stick_x = 0;
    s8 stick_y = 0;
    s8 err = -1;
    fe_route_pad_state(cpu, &buttons, &stick_x, &stick_y, &err, &just_pressed);
    const bool changed = index != s_last_index;
    if (changed || fe_route_should_log(cpu, buttons, just_pressed, &s_last_frame)) {
        fprintf(stderr,
                "[fe-route] main-menu frame=%llu self=0x%08X index=%d/%d "
                "flags=0x%08X pad=%04X jp=%04X stick=%d,%d err=%d%s\n",
                (unsigned long long)(cpu->timebase / 675000ull), self, index,
                count, flags, buttons, just_pressed, stick_x, stick_y, err,
                changed ? " index-change" : "");
        s_last_index = index;
    }
}

static void notify_IChooseSideCheckControllers(CPUState* cpu) {
    const u32 self = cpu->gpr[3];
    if (g_mash_to_gameplay && (s32)mem_read32(cpu, self) >= 0)
        g_mash_side_assigned = true;
    if (!g_state_log)
        return;
    static u64 s_last_frame = ~(u64)0;
    static s32 s_last_sides[4] = { -999, -999, -999, -999 };
    static u8 s_last_ready = 0xFFu;
    s32 sides[4];
    u8 ready = 0;
    for (u32 i = 0; i < 4u; ++i) {
        sides[i] = (s32)mem_read32(cpu, self + i * 4u);
        ready |= (mem_read8(cpu, self + 0x10u + i) != 0u) ? (u8)(1u << i) : 0u;
    }
    const s32 disabled_side = (s32)cpu->gpr[4];
    const s32 context = (s32)mem_read32(cpu, self + 0x9Cu);
    u16 buttons = 0;
    u16 just_pressed = 0;
    s8 stick_x = 0;
    s8 stick_y = 0;
    s8 err = -1;
    fe_route_pad_state(cpu, &buttons, &stick_x, &stick_y, &err, &just_pressed);
    bool changed = ready != s_last_ready;
    for (u32 i = 0; i < 4u; ++i)
        changed |= sides[i] != s_last_sides[i];
    if (changed || fe_route_should_log(cpu, buttons, just_pressed, &s_last_frame)) {
        fprintf(stderr,
                "[fe-route] choose-side frame=%llu self=0x%08X disabled=%d "
                "context=%d sides=%d,%d,%d,%d ready=0x%X pad=%04X jp=%04X "
                "stick=%d,%d err=%d%s\n",
                (unsigned long long)(cpu->timebase / 675000ull), self,
                disabled_side, context, sides[0], sides[1], sides[2], sides[3],
                ready, buttons, just_pressed, stick_x, stick_y, err,
                changed ? " side-change" : "");
        for (u32 i = 0; i < 4u; ++i)
            s_last_sides[i] = sides[i];
        s_last_ready = ready;
    }
}

static void notify_IChooseSidePositionController(CPUState* cpu) {
    if (!g_state_log)
        return;
    const u32 self = cpu->gpr[3];
    const s32 pad = (s32)cpu->gpr[4];
    if (pad < 0 || pad >= 4)
        return;
    const s32 side = (s32)mem_read32(cpu, self + (u32)pad * 4u);
    fprintf(stderr,
            "[fe-route] position-controller frame=%llu self=0x%08X pad=%d "
            "side=%d tween=%u vis=%u\n",
            (unsigned long long)(cpu->timebase / 675000ull), self, pad, side,
            cpu->gpr[5] != 0u, cpu->gpr[6] != 0u);
}

static void notify_MovieStart(CPUState* cpu) {
    if (!g_movie_log)
        return;
    char filename[256];
    hle_read_cstr(cpu, hle_arg_u32(cpu, 0), filename, sizeof filename);
    fprintf(stderr, "[movie] MovieStart file='%s' sound=%u loop=%u lr=0x%08X\n",
            filename, hle_arg_u32(cpu, 1), hle_arg_u32(cpu, 2), cpu->lr);
}

static void notify_MovieStop(CPUState* cpu) {
    if (g_movie_log)
        fprintf(stderr, "[movie] MovieStop lr=0x%08X\n", cpu->lr);
}

static void notify_MoviePlay(CPUState* cpu) {
    static unsigned count = 0;
    if (!g_movie_log)
        return;
    ++count;
    if (count <= 120u || (count % 60u) == 0u)
        fprintf(stderr, "[movie] MoviePlay #%u lr=0x%08X\n", count, cpu->lr);
}

static void notify_THPSimpleOpen(CPUState* cpu) {
    if (!g_movie_log)
        return;
    char filename[256];
    hle_read_cstr(cpu, hle_arg_u32(cpu, 0), filename, sizeof filename);
    fprintf(stderr, "[movie] THPSimpleOpen file='%s' lr=0x%08X\n",
            filename, cpu->lr);
}

static void notify_THPSimpleSetBuffer(CPUState* cpu) {
    if (g_movie_log)
        fprintf(stderr, "[movie] THPSimpleSetBuffer buffer=0x%08X lr=0x%08X\n",
                hle_arg_u32(cpu, 0), cpu->lr);
}

static void notify_THPSimplePreLoad(CPUState* cpu) {
    if (g_movie_log)
        fprintf(stderr, "[movie] THPSimplePreLoad loop=%u lr=0x%08X\n",
                hle_arg_u32(cpu, 0), cpu->lr);
}

static void notify_THPSimpleDecode(CPUState* cpu) {
    static unsigned count = 0;
    if (!g_movie_log)
        return;
    ++count;
    if (count <= 120u || (count % 60u) == 0u)
        fprintf(stderr, "[movie] THPSimpleDecode #%u audio-track=%d lr=0x%08X\n",
                count, (s32)hle_arg_u32(cpu, 0), cpu->lr);
}

static void notify_THPVideoDecode(CPUState* cpu) {
    static unsigned count = 0;
    if (!g_movie_log)
        return;
    ++count;
    if (count <= 120u || (count % 60u) == 0u)
        fprintf(stderr,
                "[movie] THPVideoDecode #%u frame=0x%08X y=0x%08X "
                "u=0x%08X v=0x%08X work=0x%08X lr=0x%08X\n",
                count, hle_arg_u32(cpu, 0), hle_arg_u32(cpu, 1),
                hle_arg_u32(cpu, 2), hle_arg_u32(cpu, 3),
                hle_arg_u32(cpu, 4), cpu->lr);
}

static u32 movie_plane_hash(CPUState* cpu, u32 guest_data, u32 bytes) {
    u32 hash_bytes = bytes;
    return guest_hash(cpu, guest_data, hash_bytes, &hash_bytes);
}

static void log_thp_simple_control(CPUState* cpu, const char* tag,
                                   unsigned count) {
    const u32 base = THP_SIMPLE_CONTROL;
    const u32 y = mem_read32(cpu, base + 0x15Cu);
    const u32 u = mem_read32(cpu, base + 0x160u);
    const u32 v = mem_read32(cpu, base + 0x164u);
    const u32 next_decode = mem_read32(cpu, base + 0x7Cu);
    const u32 read_idx = mem_read32(cpu, base + 0x80u);
    const u32 rb_index = next_decode < 16u ? next_decode : 0u;
    const u32 rb = base + 0x9Cu + rb_index * 12u;
    const u32 rb_ptr = mem_read32(cpu, rb + 0u);
    const u32 rb_frame = mem_read32(cpu, rb + 4u);
    const u32 rb_valid = mem_read32(cpu, rb + 8u);
    fprintf(stderr,
            "[movie] %s #%u ret=%d hid2=0x%08X open=%u playing=%u "
            "audio=%u loop=%u audioExist=%u nextDecode=%u readIdx=%u "
            "nextSize=%u frameCount=%u texFrame=%d rb[%u]={ptr=0x%08X "
            "frame=%u valid=%u} y=0x%08X:%08X u=0x%08X:%08X "
            "v=0x%08X:%08X\n",
            tag, count, (s32)cpu->gpr[3], cpu->hid2,
            mem_read32(cpu, base + 0x68u),
            mem_read8(cpu, base + 0x6Cu), mem_read8(cpu, base + 0x6Du),
            mem_read8(cpu, base + 0x6Eu), mem_read8(cpu, base + 0x6Fu),
            next_decode, read_idx, mem_read32(cpu, base + 0x84u),
            mem_read32(cpu, base + 0x88u),
            (s32)mem_read32(cpu, base + 0x168u), rb_index, rb_ptr, rb_frame,
            rb_valid, y, movie_plane_hash(cpu, y, 512u * 416u),
            u, movie_plane_hash(cpu, u, 256u * 208u),
            v, movie_plane_hash(cpu, v, 256u * 208u));
}

static void notify_THPVideoDecodeReturn(CPUState* cpu) {
    static unsigned count = 0;
    if (!g_movie_log)
        return;
    ++count;
    if ((s32)cpu->gpr[3] != 0 || count <= 120u || (count % 60u) == 0u)
        log_thp_simple_control(cpu, "THPVideoDecodeReturn", count);
}

static void notify_THPSimpleDecodeReturn(CPUState* cpu) {
    static unsigned count = 0;
    if (!g_movie_log)
        return;
    ++count;
    const s32 ret = (s32)cpu->gpr[3];
    const bool unusual = ret != 0 && ret != 3;
    if (unusual || count <= 120u || (count % 60u) == 0u)
        log_thp_simple_control(cpu, "THPSimpleDecodeReturn", count);
}

static void notify_LCEnable(CPUState* cpu) {
    if (g_movie_log)
        fprintf(stderr, "[movie] LCEnable entry hid2=0x%08X lr=0x%08X\n",
                cpu->hid2, cpu->lr);
}

static void notify_LCDisable(CPUState* cpu) {
    if (g_movie_log)
        fprintf(stderr, "[movie] LCDisable entry hid2=0x%08X lr=0x%08X\n",
                cpu->hid2, cpu->lr);
}


static const HleAddrEntry kNotify[] = {
    { 0u, notify_OSSleepThread,  "OSSleepThread" },
    { 0u, notify_GXSetDrawSync,  "GXSetDrawSync" },
    { 0u, notify_GXSetDrawDone,  "GXSetDrawDone" },
    { 0u, notify_GXEnableBreakPt, "GXEnableBreakPt" },
    { 0u, notify_PPCMtwpar,       "PPCMtwpar" },
    { 0u, notify_DSPSendMailToDSP, "DSPSendMailToDSP" },
    // B2.1: full certificate-demanded MusyX API surface (notify-only counters).
    { 0u, notify_audio_api, "sndInit" },
    { 0u, notify_audio_api, "sndQuit" },
    { 0u, notify_audio_api, "sndIsInstalled" },
    { 0u, notify_audio_api, "sndSetHooks" },
    { 0u, notify_audio_api, "sndFXStartParaInfo" },
    { 0u, notify_audio_api, "sndFXKeyOff" },
    { 0u, notify_audio_api, "sndFXCheck" },
    { 0u, notify_audio_api, "sndFXCtrl" },
    { 0u, notify_audio_api, "sndFXCtrl14" },
    { 0u, notify_audio_api, "sndFXAssignVolGroup2FXId" },
    { 0u, notify_audio_api, "sndVolume" },
    { 0u, notify_audio_api, "sndSilence" },
    { 0u, notify_audio_api, "sndOutputMode" },
    { 0u, notify_audio_api, "sndSetAuxProcessingCallbacks" },
    { 0u, notify_audio_api, "sndStreamActivate" },
    { 0u, notify_audio_api, "sndStreamDeactivate" },
    { 0u, notify_audio_api, "sndStreamAllocEx" },
    { 0u, notify_audio_api, "sndStreamFree" },
    { 0u, notify_audio_api, "sndStreamMixParameterEx" },
    { 0u, notify_audio_api, "sndStreamARAMUpdate" },
    { 0u, notify_audio_api, "sndStreamLPFParameter" },
    { 0u, notify_audio_api, "sndStreamADPCMParameter" },
    { 0u, notify_audio_api, "sndStreamFrq" },
    { 0u, notify_audio_api, "sndPushGroup" },
    { 0u, notify_audio_api, "sndPopGroup" },
    { 0u, notify_audio_api, "sndSeqPlayEx" },
    { 0u, notify_audio_api, "sndStackSetCurrent" },
    { 0u, notify_audio_api, "sndStackGetSize" },
    { 0u, notify_audio_api, "sndStackAdd" },
    { 0u, notify_audio_api, "sndStackGetARAMAddressRange" },
    { 0u, notify_audio_api, "sndStackGetAvailableSampleMemory" },
    { 0u, notify_audio_api, "sndSetSampleDataUploadCallback" },
    { 0u, notify_audio_api, "sndAddEmitter2StudioPara" },
    { 0u, notify_audio_api, "sndUpdateEmitter" },
    { 0u, notify_audio_api, "sndRemoveEmitter" },
    { 0u, notify_audio_api, "sndCheckEmitter" },
    { 0u, notify_audio_api, "sndEmitterVoiceID" },
    { 0u, notify_audio_api, "sndAddListenerEx" },
    { 0u, notify_audio_api, "sndUpdateListener" },
    { 0u, notify_audio_api, "sndRemoveListener" },
    { 0u, notify_audio_api, "sndPitchUpOne" },
    { 0u, notify_audio_api, "sndGetPitch" },
    { 0u, notify_audio_api, "sndSin" },
    { 0u, notify_audio_api, "sndAuxCallbackPrepareReverbSTD" },
    { 0u, notify_audio_api, "sndAuxCallbackUpdateSettingsReverbSTD" },
    { 0u, notify_audio_api, "sndAuxCallbackShutdownReverbSTD" },
    { 0u, notify_audio_api, "sndAuxCallbackPrepareReverbHI" },
    { 0u, notify_audio_api, "sndAuxCallbackUpdateSettingsReverbHI" },
    { 0u, notify_audio_api, "sndAuxCallbackShutdownReverbHI" },
    { 0u, notify_audio_api, "sndAuxCallbackPrepareDelay" },
    { 0u, notify_audio_api, "sndAuxCallbackUpdateSettingsDelay" },
    { 0u, notify_audio_api, "sndAuxCallbackShutdownDelay" },
    { 0u, notify_audio_api, "sndAuxCallbackPrepareChorus" },
    { 0u, notify_audio_api, "sndAuxCallbackUpdateSettingsChorus" },
    { 0u, notify_audio_api, "sndAuxCallbackShutdownChorus" },
    { 0u, notify_OSResetSystem,    "OSResetSystem" },
    { 0u, notify_LCEnable,          "LCEnable" },
    { 0u, notify_LCDisable,         "LCDisable" },
#ifdef ED_AURORA
    { 0u, notify_GXCopyDisp,      "GXCopyDisp" },
    { 0u, notify_GXCopyTex,       "GXCopyTex" },
    { 0u, notify_GXLoadTexObj,    "GXLoadTexObj" },
    { 0u, notify_GXLoadTlut,      "GXLoadTlut" },
    { 0u, notify_GXLoadPosMtxImm, "GXLoadPosMtxImm" },
    { 0u, notify_VIWaitForRetrace,"VIWaitForRetrace" },
    { 0u, notify_VIConfigure,     "VIConfigure" },
    { 0u, notify_VIFlush,         "VIFlush" },
    { 0u, notify_VISetNextFrameBuffer, "VISetNextFrameBuffer" },
    { 0u, notify_VISetBlack,      "VISetBlack" },
#endif
};

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
// Lowest address holding loaded code. A call below this is a branch through an
// uninitialized/optional function pointer (e.g. a NULL callback hook).
#define HLE_CODE_BASE 0x80003000u
#define HLE_CODE_LIMIT 0x80240000u  // past the end of text in GEDE01 and GEDP01
#define HLE_DISPATCH_COUNT ((HLE_CODE_LIMIT - HLE_CODE_BASE) / 4u)

static HleDispatchEntry g_hle_dispatch[HLE_DISPATCH_COUNT];

// Table entries name their function; the address comes from the symbol table
// built from the user's DOL (tools/symbols.py), so one table serves every build.
static u32 hle_entry_address(u32 address, const char* name) {
    return address != 0u ? address : sdk_symbol_address(name);
}

static HleDispatchEntry* hle_dispatch_entry(u32 address) {
    if (address < HLE_CODE_BASE || address >= HLE_CODE_LIMIT ||
        (address & 3u) != 0u)
        return NULL;
    return &g_hle_dispatch[(address - HLE_CODE_BASE) / 4u];
}

static void initialize_hle_dispatch(void) {
    memset(g_hle_dispatch, 0, sizeof g_hle_dispatch);
    for (size_t i = 0; i < sizeof kTrace / sizeof kTrace[0]; i++)
        kTrace[i].address = sdk_symbol_address(kTrace[i].name);
    for (size_t i = 0; i < sizeof g_audio_api / sizeof g_audio_api[0]; i++)
        g_audio_api[i].address = sdk_symbol_address(g_audio_api[i].name);
#ifdef ED_AURORA
    g_gx_dirty_state_helper = sdk_symbol_address("__GXSetDirtyState");
    g_gx_flush_prim_helper = sdk_symbol_address("__GXSendFlushPrim");
#endif

    for (size_t i = 0; i < sizeof kHandlers / sizeof kHandlers[0]; i++) {
        HleDispatchEntry* entry =
            hle_dispatch_entry(sdk_symbol_address(kHandlers[i].name));
        if (entry != NULL)
            entry->intercept = kHandlers[i].fn;
    }
    // ED_GX_HLE=1 keeps the Strikers host's GX shortcuts (GXSetArray, GXBegin,
    // GXCallDisplayList), which feed the live Aurora gx layer. gxcore, the
    // default renderer, decodes the game's own FIFO stream and resolves arrays
    // and display lists from guest memory, so by default they are left out.
    const char* gx_hle = getenv("ED_GX_HLE");
    const bool use_gx_hle = gx_hle != NULL && gx_hle[0] == '1';
    for (size_t i = 0; i < sizeof kAddrHandlers / sizeof kAddrHandlers[0]; i++) {
        if (!use_gx_hle && (strcmp(kAddrHandlers[i].name, "GXSetArray") == 0 ||
                            strcmp(kAddrHandlers[i].name, "GXBegin") == 0 ||
                            strcmp(kAddrHandlers[i].name, "GXCallDisplayList") == 0))
            continue;
        HleDispatchEntry* entry =
            hle_dispatch_entry(hle_entry_address(kAddrHandlers[i].address, kAddrHandlers[i].name));
        if (entry != NULL)
            entry->intercept = kAddrHandlers[i].fn;
    }
    for (size_t i = 0; i < sizeof kNotify / sizeof kNotify[0]; i++) {
        // The texture and TLUT bindings feed only the live Aurora gx layer
        // (emit_gx_resource_metadata, from the GX shortcuts above). gxcore reads
        // textures from the FIFO stream itself, so without ED_GX_HLE these hooks
        // would only hash every loaded texture on the game thread (and run the
        // Strikers host's THP movie-plane check, which reads a G4QE01 address).
        if (!use_gx_hle && (strcmp(kNotify[i].name, "GXLoadTexObj") == 0 ||
                            strcmp(kNotify[i].name, "GXLoadTlut") == 0))
            continue;
        HleDispatchEntry* entry =
            hle_dispatch_entry(hle_entry_address(kNotify[i].address, kNotify[i].name));
        if (entry != NULL)
            entry->notify = kNotify[i].fn;
    }
}

#ifdef ED_AURORA
// Once a game frame (VIFlush): the frames per second in the window title,
// refreshed each second (ED_NO_FPS=1 leaves the title alone), and the
// in-game menu.
void hle_frame_boundary(void) {
    static int fps_env = -1;
    static unsigned frames;
    static struct timespec mark;
    static bool shown;
    if (fps_env < 0)
        fps_env = getenv("ED_NO_FPS") == NULL;
    const bool fps_on = fps_env && g_settings.fps_counter;
    if (!fps_on && shown) {
        VISetWindowTitle("Eternal Darkness");
        shown = false;
    }
    if (fps_on) {
        shown = true;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        frames++;
        if (mark.tv_sec == 0) {
            mark = now;
            frames = 0;
        } else {
            const double dt = (double)(now.tv_sec - mark.tv_sec) +
                              (double)(now.tv_nsec - mark.tv_nsec) * 1e-9;
            if (dt >= 1.0) {
                char title[64];
                snprintf(title, sizeof title, "Eternal Darkness \u2014 %.0f fps",
                         (double)frames / dt);
                VISetWindowTitle(title);
                mark = now;
                frames = 0;
            }
        }
    }
    // Esc / the controller's Back button opens the in-game menu here, at
    // a frame boundary, with the frame just drawn behind it.
    frontend_frame();
}
#endif

static bool hle_dispatch(CPUState* cpu, u32 address) {
    if (address == HLE_CALLBACK_RETURN && g_irq_callback.stage != 0u) {
        continue_irq_callback(cpu);
        return true;
    }
#ifdef ED_AURORA
    if (address == HLE_DISPLAY_LIST_RETURN && g_display_list_call.active) {
        continue_GXCallDisplayList(cpu);
        return true;
    }
    if (address == HLE_GX_BEGIN_RETURN && g_gx_begin.active) {
        continue_GXBegin(cpu);
        return true;
    }
#endif

    // Call through a NULL (or near-NULL) function pointer. GameCube SDK code
    // calls optional callback hooks (thread-switch callbacks, etc.) that default
    // to NULL; the recompiled `blrl` already saved the return address in lr, so
    // model the absent callback as a no-op and return to the caller instead of
    // halting. Loud + counted so genuine wild branches stay visible.
    if (address < HLE_CODE_BASE) {
        // __OSInitMemoryProtection calls Config24MB (Config48MB on a 48 MB
        // devkit) through its physical address with translation off, to set
        // the BATs. There is no MMU here: skip it, and say so by name.
        if (address != 0u) {
            const u32 cached = address | GC_RAM_BASE;
            const char* config = cached == sdk_symbol_address("Config24MB")   ? "Config24MB"
                                 : cached == sdk_symbol_address("Config48MB") ? "Config48MB"
                                                                              : NULL;
            if (config != NULL) {
                fprintf(stderr, "[hle] %s (real-mode call) skipped\n", config);
                hle_return(cpu);
                return true;
            }
        }
        static unsigned long null_calls = 0;
        if (cpu->lr >= HLE_CODE_BASE) {
            if (g_hle_log || null_calls < 8)
                fprintf(stderr,
                        "[hle] null-pointer call to 0x%08X from lr=0x%08X "
                        "(treating as no-op callback, #%lu)\n",
                        address, cpu->lr, ++null_calls);
            else
                ++null_calls;
            hle_return(cpu);
            return true;
        }
        return false;  // lr also bad -> let the runtime stop and report
    }

    // ED_TRACE_FN=name[,name...]: log each call of those functions with its
    // caller and first arguments, without the full ED_HLE_LOG firehose.
    if (g_trace_fn_count != 0u) {
        for (u32 i = 0; i < g_trace_fn_count; i++)
            if (g_trace_fn[i] == address)
            {
                char name[16];
                const char* sym = sdk_symbol_name(address);
                if (sym == NULL) {
                    snprintf(name, sizeof name, "0x%08X", address);
                    sym = name;
                }
                fprintf(stderr, "[fn] %s lr=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X "
                        "f1=%g f2=%g f3=%g msr.fp=%u",
                        sym, cpu->lr, cpu->gpr[3],
                        cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->fpr[1],
                        cpu->fpr[2], cpu->fpr[3], (cpu->msr >> 13) & 1u);
                // ED_TRACE_R3=off,off..: also print the words at r3+off (hex
                // and as float), for reading fields of the object passed in r3.
                const char* offs = getenv("ED_TRACE_R3");
                for (const char* o = offs; o != NULL && *o != '\0';) {
                    char* end;
                    u32 off = (u32)strtoul(o, &end, 0);
                    u32 ea = cpu->gpr[3] + off;
                    if (ea >= 0x80000000u && ea < 0x81800000u) {
                        u32 w = mem_read32(cpu, ea);
                        float f;
                        memcpy(&f, &w, 4);
                        fprintf(stderr, " [%u]=0x%08X(%g)", off, w, f);
                    }
                    o = *end == ',' ? end + 1 : end;
                    if (end == o && *end != ',') break;
                }
                fputc('\n', stderr);
            }
    }
    // ED_DL_CHECK=1: at each GXCallDisplayList, summarise the list's bytes
    // (non-zero count, draw-opcode count, FNV hash, first bytes).
    {
        static int dl_check = -1;
        static u32 call_dl;
        if (dl_check < 0) {
            dl_check = getenv("ED_DL_CHECK") != NULL;
            call_dl = sdk_symbol_address("GXCallDisplayList");
        }
        if (dl_check && address == call_dl) {
            const u32 base = cpu->gpr[3], size = cpu->gpr[4];
            u32 nonzero = 0, draws = 0;
            u64 h = 1469598103934665603ull;
            u8 head[12] = {0};
            for (u32 i = 0; i < size && i < 0x100000u; i++) {
                const u8 b = mem_read8(cpu, base + i);
                if (i < sizeof head) head[i] = b;
                nonzero += b != 0u;
                h = (h ^ b) * 1099511628211ull;
            }
            // Walk the command stream from the start while it parses simply.
            for (u32 i = 0; i < size;) {
                const u8 c = mem_read8(cpu, base + i);
                if (c == 0x00u) { i++; continue; }
                if (c == 0x08u) { i += 6; continue; }
                if (c == 0x10u) { u32 n = (mem_read16(cpu, base + i + 1) & 0xFu) + 1u; i += 5 + 4 * n; continue; }
                if (c == 0x61u) { i += 5; continue; }
                if ((c & 0xF8u) >= 0x20u && (c & 0xF8u) <= 0x38u) { i += 5; continue; }
                if ((c & 0xC0u) == 0x80u) { draws++; break; }
                break;
            }
            fprintf(stderr, "[dl] lr=0x%08X addr=0x%08X size=0x%X nonzero=%u first_draw=%u fnv=%016llx head=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                    cpu->lr, base, size, nonzero, draws, (unsigned long long)h,
                    head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7]);
        }
    }
#ifdef ED_AURORA
    // With the GX thread the backend runs this on the main thread after the
    // frame is shown (dol_aurora_set_frame_hook).
    if (address == g_call_prof_vi && g_call_prof_vi != 0u &&
        !dol_aurora_gx_threaded())
        hle_frame_boundary();
#endif
    if (g_call_prof_fn != 0u) {
        if (address == g_call_prof_fn) {
            for (u32 i = 0; i < 64u; i++)
                if (g_call_prof[i].lr == cpu->lr || g_call_prof[i].n == 0u) {
                    g_call_prof[i].lr = cpu->lr;
                    g_call_prof[i].n++;
                    break;
                }
        } else if (address == g_call_prof_vi) {
            static u32 frame;
            fprintf(stderr, "[callprof] %u:", frame++);
            for (u32 i = 0; i < 64u && g_call_prof[i].n != 0u; i++)
                fprintf(stderr, " %08X=%u", g_call_prof[i].lr, g_call_prof[i].n);
            fputc('\n', stderr);
            memset(g_call_prof, 0, sizeof g_call_prof);
        }
    }
    HleDispatchEntry* entry = hle_dispatch_entry(address);
    if (entry == NULL)
        return false;

    if (entry->intercept != NULL) {
        if (g_hle_log) {
            const char* name = sdk_symbol_name(address);
            if (name == NULL) {
                for (size_t i = 0;
                     i < sizeof kAddrHandlers / sizeof kAddrHandlers[0]; i++)
                    if (kAddrHandlers[i].address == address) {
                        name = kAddrHandlers[i].name;
                        break;
                    }
            }
            fprintf(stderr, "[hle] %-28s 0x%08X -> host\n",
                    name != NULL ? name : "(unnamed)", address);
        }
        g_hle_tail_call = false;
        g_hle_passthrough = false;
        entry->intercept(cpu);
        if (!g_hle_passthrough) {
            if (!g_hle_tail_call)
                hle_return(cpu);
            return true;
        }
    }

    // Observe-only hooks: drive host state, then fall through so the recompiled
    // function still runs.
    if (entry->notify != NULL)
        entry->notify(cpu);

    if (g_hle_log) {
        const char* tn = trace_name(address);
        if (tn != NULL)
            fprintf(stderr, "[trace] %-22s lr=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X\n",
                    tn, cpu->lr, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
        const char* name = sdk_symbol_name(address);
        if (name != NULL)
            fprintf(stderr, "[hle] %-28s 0x%08X (recompiled)\n", name,
                    address);
    }
    return false;
}

bool hle_card_open(const char* path) {
    hle_card_close();
    DolMemoryCardConfig config;
    memset(&config, 0, sizeof config);
    config.path = path;
    config.size_mbits = 4;
    config.encoding = 0;
    memcpy(config.game_code, ED_GAME_ID, 4);
    memcpy(config.company, ED_GAME_ID + 4, 2);
    g_memory_card = dol_card_open(&config);
    if (g_memory_card == NULL)
        return false;

    u16 size_mbits = 0;
    u32 sector_size = 0;
    if (dol_card_probe(g_memory_card, &size_mbits, &sector_size) !=
        DOL_CARD_RESULT_READY) {
        hle_card_close();
        return false;
    }
    fprintf(stderr,
            "[card] slot A: %s (%u Mbit, %u-byte sectors, serial %016llX)\n",
            path != NULL && path[0] != '\0' ? path : "(memory)",
            size_mbits, sector_size,
            (unsigned long long)dol_card_serial(g_memory_card));
    return true;
}

void hle_card_close(void) {
    dol_card_close(g_memory_card);
    g_memory_card = NULL;
    g_callback_read = 0;
    g_callback_count = 0;
    g_callback_active = false;
    memset(&g_callback_context, 0, sizeof g_callback_context);
}

// Block chaining (DolRecomp cpu.h): generated code stays inside its chunk across
// loops, calls and returns unless the target is in this table. Every address
// hle_dispatch acts on is a stop: intercepts, notify hooks, VIFlush (the frame
// boundary), and the debug hooks (ED_TRACE_FN, ED_CALL_PROFILE, ED_DL_CHECK).
// ED_NO_CHAIN=1, or the full ED_HLE_LOG trace, sends every branch through
// hle_dispatch as before.
static u8 g_chain_stop[HLE_DISPATCH_COUNT];
const u8* dolrecomp_chain_stop;  // read by the generated code (generated.h)
u32 dolrecomp_chain_stop_base;
u32 dolrecomp_chain_stop_count;
u32 dolrecomp_chain_budget;  // shared by nested chunk calls (generated.h)
u32 dolrecomp_call_depth;
// The same table as one byte per word of MEM1, nonzero where a branch may
// chain, for chunks built with DOLRECOMP_STATIC_CPU (a constant target is one
// byte test). Only the HLE code range is ever written; the rest stays zero.
#define CHAIN_TABLE_SIZE 0x600000u  // generated.h DOLRECOMP_CHAIN_TABLE_SIZE
u8 dolrecomp_chain_table[CHAIN_TABLE_SIZE];

// Chain stops are read once, by publish_codegen_options (which also fills
// dolrecomp_chain_table): an intercept, notify or debug hook installed after
// it must call it again, or chunks keep chaining past the new hook.
static void chain_stop_at(u32 address) {
    if (address >= HLE_CODE_BASE && address < HLE_CODE_LIMIT)
        g_chain_stop[(address - HLE_CODE_BASE) / 4u] = 1u;
}

// Fast RAM access in the generated code (generated.h): bit 0 loads, bit 1
// stores, bit 2 write-gather pipe stores (mmio.c dolrecomp_wgp_write).
// ED_NO_FASTMEM=1 sends every access through mem_read / mem_write;
// ED_STORE_WATCH keeps RAM stores there so the watch sees them.
u8 dolrecomp_fast_ram;

static void publish_codegen_options(void) {
    dolrecomp_fast_ram = getenv("ED_NO_FASTMEM") != NULL ? 0u
                         : getenv("ED_STORE_WATCH") != NULL ? 5u : 7u;
    dolrecomp_chain_stop = NULL;
    u8* const chain_ok = &dolrecomp_chain_table[(HLE_CODE_BASE - 0x80000000u) / 4u];
    memset(chain_ok, 0, HLE_DISPATCH_COUNT);
    if (getenv("ED_NO_CHAIN") != NULL || g_hle_log)
        return;
    for (u32 i = 0; i < HLE_DISPATCH_COUNT; i++)
        g_chain_stop[i] = g_hle_dispatch[i].intercept != NULL ||
                          g_hle_dispatch[i].notify != NULL;
    chain_stop_at(g_call_prof_vi);
    chain_stop_at(g_call_prof_fn);
    chain_stop_at(sdk_symbol_address("GXCallDisplayList"));
    for (u32 i = 0; i < g_trace_fn_count; i++)
        chain_stop_at(g_trace_fn[i]);
    dolrecomp_chain_stop_base = HLE_CODE_BASE;
    dolrecomp_chain_stop_count = HLE_DISPATCH_COUNT;
    dolrecomp_chain_stop = g_chain_stop;
    for (u32 i = 0; i < HLE_DISPATCH_COUNT; i++)
        chain_ok[i] = g_chain_stop[i] == 0u;
}

// MSL's memcpy, memset and a forward byte copy sit at the start of text in this
// game and copy one byte per loop pass (lbzu/stbu: four guest instructions a
// byte, ~4 M passes a second in the cutscene that follows the "rats" quote, where
// the main thread then starves the Vorbis speech decoder). Done natively when the
// whole range is in MEM1; anywhere else, byte by byte through the bus as before.
// Found by their instructions, so either disc's DOL works.
static bool mem1_span(const CPUState* cpu, u32 address, u32 size) {
    return guest_ram_range_valid(cpu, address, size);
}

// memcpy(dst, src, n): copies backwards when src < dst, else forwards (memmove).
static void hle_msl_memcpy(CPUState* cpu) {
    const u32 dst = hle_arg_u32(cpu, 0);
    const u32 src = hle_arg_u32(cpu, 1);
    const u32 n = hle_arg_u32(cpu, 2);
    if (n != 0u) {
        if (mem1_span(cpu, dst, n) && mem1_span(cpu, src, n))
            memmove(cpu->ram + (dst - GC_RAM_BASE), cpu->ram + (src - GC_RAM_BASE), n);
        else if (src < dst)
            for (u32 i = n; i-- > 0u;)
                mem_write8(cpu, dst + i, mem_read8(cpu, src + i));
        else
            for (u32 i = 0; i < n; i++)
                mem_write8(cpu, dst + i, mem_read8(cpu, src + i));
    }
    hle_set_u32(cpu, dst);
}

// The forward-only copy: a byte loop, so an overlapping dst inside the source
// repeats the leading bytes; keep that.
static void hle_msl_copy_forward(CPUState* cpu) {
    const u32 dst = hle_arg_u32(cpu, 0);
    const u32 src = hle_arg_u32(cpu, 1);
    const u32 n = hle_arg_u32(cpu, 2);
    if (n != 0u) {
        if (mem1_span(cpu, dst, n) && mem1_span(cpu, src, n) &&
            (dst <= src || dst - src >= n))
            memmove(cpu->ram + (dst - GC_RAM_BASE), cpu->ram + (src - GC_RAM_BASE), n);
        else
            for (u32 i = 0; i < n; i++)
                mem_write8(cpu, dst + i, mem_read8(cpu, src + i));
    }
}

// memset(dst, c, n) (the wrapper around __fill_mem): returns dst.
static void hle_msl_memset(CPUState* cpu) {
    const u32 dst = hle_arg_u32(cpu, 0);
    const u8 c = (u8)hle_arg_u32(cpu, 1);
    const u32 n = hle_arg_u32(cpu, 2);
    if (n != 0u) {
        if (mem1_span(cpu, dst, n))
            memset(cpu->ram + (dst - GC_RAM_BASE), c, n);
        else
            for (u32 i = 0; i < n; i++)
                mem_write8(cpu, dst + i, c);
    }
    hle_set_u32(cpu, dst);
}

static bool code_matches(CPUState* cpu, u32 address, const u32* words, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (mem_read32(cpu, address + (u32)i * 4u) != words[i])
            return false;
    return true;
}

static void install_msl_memory_hle(CPUState* cpu) {
    if (getenv("ED_NO_MEM_HLE") != NULL)
        return;
    static const u32 kCopyForward[] = {0x3884FFFFu, 0x38C3FFFFu, 0x38A50001u, 0x4800000Cu,
                                       0x8C040001u, 0x9C060001u, 0x34A5FFFFu, 0x4082FFF4u,
                                       0x4E800020u};
    static const u32 kMemcpyHead[] = {0x7C041840u, 0x41800028u};
    static const u32 kMemcpyTail[] = {0x7C842A14u, 0x7CC32A14u, 0x38A50001u, 0x4800000Cu,
                                      0x8C04FFFFu, 0x9C06FFFFu, 0x34A5FFFFu, 0x4082FFF4u,
                                      0x4E800020u};
    static const u32 kMemsetWrapper[] = {0x9421FFF0u, 0x7C0802A6u, 0x90010014u, 0x93E1000Cu,
                                         0x7C7F1B78u, 0x4800001Du};
    static const u32 kFillMemHead[] = {0x28050020u, 0x5484063Eu, 0x38C3FFFFu};
    u32 found_memcpy = 0, found_memset = 0, found_forward = 0;
    for (u32 a = HLE_CODE_BASE; a < HLE_CODE_BASE + 0x2000u; a += 4u) {
        if (!found_memcpy && code_matches(cpu, a, kMemcpyHead, 2) &&
            code_matches(cpu, a + 8u, kCopyForward, 9) &&
            code_matches(cpu, a + 0x2Cu, kMemcpyTail, 9))
            found_memcpy = a;
        else if (!found_memset && code_matches(cpu, a, kMemsetWrapper, 6) &&
                 code_matches(cpu, a + 0x30u, kFillMemHead, 3))
            found_memset = a;
        else if (!found_forward && mem_read32(cpu, a - 4u) == 0x4E800020u &&
                 code_matches(cpu, a, kCopyForward, 9) &&
                 (found_memcpy == 0u || a != found_memcpy + 8u))
            found_forward = a;
    }
    struct { u32 address; HleHandler fn; } found[] = {
        {found_memcpy, hle_msl_memcpy},
        {found_memset, hle_msl_memset},
        {found_forward, hle_msl_copy_forward},
    };
    for (size_t i = 0; i < sizeof found / sizeof found[0]; i++) {
        HleDispatchEntry* entry = hle_dispatch_entry(found[i].address);
        if (found[i].address != 0u && entry != NULL && entry->intercept == NULL)
            entry->intercept = found[i].fn;
    }
    fprintf(stderr, "[hle] native memcpy 0x%08X memset 0x%08X forward copy 0x%08X\n",
            found_memcpy, found_memset, found_forward);
}

// The prologue's book shows a photo of Alex that the game makes by copying a
// frame to an XFB and reading it back on the CPU (fn_801F50EC in the USA
// decomp: YUYV to a texture, r3 = the XFB). Our copies reach RAM a frame or
// two late, so the photo was an older frame: the book page on the Steam Deck,
// a flash of the unfinished scene, eyes out of line with the 3D Alex. Before
// it reads, wait for that copy and write it (twice in the sequence).
static void hle_xfb_reader_notify(CPUState* cpu) {
#ifdef ED_AURORA
    dol_aurora_xfb_ram_sync(cpu->gpr[3]);
#else
    (void)cpu;
#endif
}

static void install_xfb_reader_sync(CPUState* cpu) {
    if (getenv("ED_NO_XFB_SYNC") != NULL)
        return;
    static const u32 kHead[] = {0x9421FFC0u, 0x7C0802A6u, 0x90010044u, 0xBF010020u,
                                0x7C982378u, 0x7C7B1B78u, 0x3B800000u};
    const u32 address = strcmp(ED_GAME_ID, "GEDP01") == 0 ? 0x801E9920u : 0x801F50ECu;
    HleDispatchEntry* entry = hle_dispatch_entry(address);
    if (entry == NULL || entry->notify != NULL || !code_matches(cpu, address, kHead, 7)) {
        fprintf(stderr, "[hle] XFB reader not found at 0x%08X: the book's photo may lag\n",
                address);
        return;
    }
    entry->notify = hle_xfb_reader_notify;
}

static bool callback_pending(u32 address, s32 result) {
    if (g_callback_active && g_irq_callback.pending.address == address &&
        g_irq_callback.pending.result == result)
        return true;
    for (u32 i = 0; i < g_callback_count; i++) {
        const HlePendingCallback* c =
            &g_callback_queue[(g_callback_read + i) % HLE_CALLBACK_QUEUE_CAPACITY];
        if (c->address == address && c->result == result)
            return true;
    }
    return false;
}

// The disc stream slots of DVD_CALLBACK_VISITS: three of 132 bytes, each with
// its DVDFileInfo at +20, "read in flight" at +116 and "close once the read is
// in" at +120; the read's callback finds its slot by the file info. States
// saved before reads waited for their callbacks hold slots marked in flight
// with no read anywhere. Clear the mark: the slot reads again, or closes.
static void unstick_disc_streams(CPUState* cpu) {
    const bool pal = strcmp(ED_GAME_ID, "GEDP01") == 0;
    const u32 callback = pal ? 0x801A294Cu : 0x801AE6B0u;
    const u32 slots = pal ? 0x805DB410u : 0x8060B430u;
    // The callback's start, ending in lis/addi of the slot table.
    const u32 head[] = {0x9421FFF0u, 0x7C0802A6u, 0x90010014u, 0x93E1000Cu, 0x38A00000u,
                        0x3C600000u | ((slots + 0x8000u) >> 16),
                        0x38630000u | (slots & 0xFFFFu)};
    if (!code_matches(cpu, callback, head, 7))
        return;
    for (u32 i = 0; i < 3u; i++) {
        const u32 slot = slots + i * 132u;
        if (mem_read8(cpu, slot) == 0u || mem_read32(cpu, slot + 116u) == 0u ||
            callback_pending(callback, (s32)(slot + 20u)))
            continue;
        mem_write32(cpu, slot + 116u, 0u);
        fprintf(stderr, "[hle] disc stream %u: cleared a stuck read (state from an older build)\n",
                i);
    }
}

void hle_state_loaded(CPUState* cpu) {
    for (u32 i = 0; i < g_callback_count; i++)
        delay_callback(cpu, (g_callback_read + i) % HLE_CALLBACK_QUEUE_CAPACITY);
    unstick_disc_streams(cpu);
}

void hle_install(CPUState* cpu) {
    g_hle_log = getenv("ED_HLE_LOG") != NULL;
    g_dvd_log = g_hle_log || getenv("ED_DVD_LOG") != NULL;
    g_skip_intro = getenv("ED_SKIP_INTRO") != NULL;
    g_skip_intro_done = false;
    g_skip_intro_armed = false;
    g_skip_intro_memcard_text = 0;
    g_trace_fn_count = 0;
    if (getenv("ED_TRACE_FN") != NULL) {
        char names[512];
        snprintf(names, sizeof names, "%s", getenv("ED_TRACE_FN"));
        for (char* n = strtok(names, ","); n != NULL && g_trace_fn_count < 16u;
             n = strtok(NULL, ","))
            if (sdk_symbol_address(n) != 0u)
                g_trace_fn[g_trace_fn_count++] = sdk_symbol_address(n);
            else if (n[0] == '0' && n[1] == 'x')  // raw guest address
                g_trace_fn[g_trace_fn_count++] = (u32)strtoul(n, NULL, 16);
    }
    g_call_prof_fn = getenv("ED_CALL_PROFILE") != NULL
                         ? sdk_symbol_address(getenv("ED_CALL_PROFILE")) : 0u;
    g_call_prof_vi = sdk_symbol_address("VIFlush");
    g_audio_log = getenv("ED_AUDIO_LOG") != NULL;
    g_graphics_log = getenv("ED_GFX_LOG") != NULL;
    g_movie_log = getenv("ED_MOVIE_LOG") != NULL;
    g_movie_cadence_log = getenv("ED_MOVIE_CADENCE_LOG") != NULL;
    g_card_log = getenv("ED_CARD_LOG") != NULL;
    g_auto_input = getenv("ED_AUTO_INPUT") != NULL;
    g_mash_to_gameplay = getenv("ED_MASH_TO_GAMEPLAY") != NULL;
    g_mash_side_assigned = false;
    g_mash_prematch = false;
    g_mash_route_complete = false;
    memset(&g_ball_physics_probe, 0, sizeof g_ball_physics_probe);
    memset(&g_ball_sor_probe, 0, sizeof g_ball_sor_probe);
    memset(g_physics_body_probes, 0, sizeof g_physics_body_probes);
    g_physics_body_probe_count = 0;
    g_physics_sor_failure_reported = false;
    g_physics_set_failure_reported = false;
    g_auto_skip_card_prompt = getenv("ED_AUTO_SKIP_CARD_PROMPT") != NULL;
    g_state_log = getenv("ED_STATE_LOG") != NULL;
    if (g_mash_to_gameplay) {
        fprintf(stderr,
                "[input] mashing A + Left + Start (8 frames held, 8 released)\n");
    }
#ifdef ED_AURORA
    {
        const char* mav = getenv("ED_MAX_ARRAY_VERTS");
        if (mav) {
            unsigned long v = strtoul(mav, NULL, 0);
            if (v >= 16u && v <= 65536u)
                g_max_array_verts = (u32)v;
        }
    }
#endif
    g_auto_input_last_pulse = ~(u64)0;
    g_auto_skip_card_last_pulse = ~(u64)0;
    g_auto_input_once_sent = ~(u64)0;
    g_auto_input_once_frame = 0;
    g_callback_read = 0;
    g_callback_count = 0;
    g_callback_active = false;
    memset(&g_callback_context, 0, sizeof g_callback_context);
#ifdef ED_AURORA
    memset(&g_display_list_call, 0, sizeof g_display_list_call);
    memset(&g_gx_begin, 0, sizeof g_gx_begin);
#endif
    initialize_hle_dispatch();
    const char* once_frame = getenv("ED_AUTO_INPUT_ONCE");
    if (once_frame != NULL && once_frame[0] != '\0')
        g_auto_input_once_frame = strtoull(once_frame, NULL, 0);
    parse_input_script(getenv("ED_INPUT_SCRIPT"));
#ifdef ED_AURORA
    memset(g_textures, 0, sizeof g_textures);
    memset(g_tluts, 0, sizeof g_tluts);
    memset(g_array_address, 0, sizeof g_array_address);
    memset(g_array_stride, 0, sizeof g_array_stride);
    g_gx_begin_count = 0;
    g_gx_array_count = 0;
    g_frame_array_bytes = 0;
    g_frame_array_calls = 0;
    g_frame_array_bytes_peak = 0;
    g_array_budget_warned = false;
    g_vi_wait_count = 0;
    g_movie_texture_load_count = 0;
    g_movie_cadence_present_count = 0;
    g_movie_cadence_texframe_changes = 0;
    g_movie_cadence_start_time = 0.0;
    g_movie_cadence_last_texframe = -1;
    g_movie_cadence_started = false;
#endif
    cpu->host_call = hle_dispatch;
    install_msl_memory_hle(cpu);
    install_xfb_reader_sync(cpu);
    publish_codegen_options();
}

// ---------------------------------------------------------------------------
// Quick state (host/quickstate.c)
// ---------------------------------------------------------------------------
// The host halves of guest operations in flight: queued and running completion
// callbacks (with the context a running one returns to), the DVD current
// directory, skip-intro progress, and whether the card is mounted. The card's
// contents are not part of a state: loading one never rewrites saves.
typedef struct {
    HlePendingCallback callback_queue[HLE_CALLBACK_QUEUE_CAPACITY];
    u32 callback_read;
    u32 callback_count;
    bool callback_active;
    HleSavedContext callback_context;
    u32 irq_stage;
    u32 irq_context;
    HlePendingCallback irq_pending;
    char dvd_cwd[sizeof g_dvd_cwd];
    bool skip_intro_done;
    bool skip_intro_armed;
    bool card_mounted;
} HleQuickState;

size_t hle_state_size(void) {
    return sizeof(HleQuickState);
}

void hle_state_save(void* out) {
    HleQuickState s;
    memset(&s, 0, sizeof s);
    memcpy(s.callback_queue, g_callback_queue, sizeof s.callback_queue);
    s.callback_read = g_callback_read;
    s.callback_count = g_callback_count;
    s.callback_active = g_callback_active;
    memcpy(&s.callback_context, &g_callback_context, sizeof s.callback_context);
    s.irq_stage = g_irq_callback.stage;
    s.irq_context = g_irq_callback.context;
    s.irq_pending = g_irq_callback.pending;
    snprintf(s.dvd_cwd, sizeof s.dvd_cwd, "%s", g_dvd_cwd);
    s.skip_intro_done = g_skip_intro_done;
    s.skip_intro_armed = g_skip_intro_armed;
    s.card_mounted = dol_card_mounted(g_memory_card);
    memcpy(out, &s, sizeof s);
}

void hle_state_load(const void* in) {
    HleQuickState s;
    memcpy(&s, in, sizeof s);
    memcpy(g_callback_queue, s.callback_queue, sizeof g_callback_queue);
    g_callback_read = s.callback_read % HLE_CALLBACK_QUEUE_CAPACITY;
    g_callback_count = s.callback_count <= HLE_CALLBACK_QUEUE_CAPACITY
                           ? s.callback_count
                           : 0u;
    memset(g_callback_wait, 0, sizeof g_callback_wait);
    // Stage 4 is inside __OSReschedule: over (see g_irq_callback), though
    // older builds saved it as still running.
    g_callback_active = s.callback_active && s.irq_stage < 4u;
    memcpy(&g_callback_context, &s.callback_context, sizeof g_callback_context);
    g_irq_callback.stage = s.irq_stage;
    g_irq_callback.context = s.irq_context;
    g_irq_callback.pending = s.irq_pending;
    s.dvd_cwd[sizeof s.dvd_cwd - 1u] = '\0';
    snprintf(g_dvd_cwd, sizeof g_dvd_cwd, "%s",
             s.dvd_cwd[0] == '/' ? s.dvd_cwd : "/");
    g_skip_intro_done = s.skip_intro_done;
    g_skip_intro_armed = s.skip_intro_armed;
    // The guest's CARD library remembers mounting; match the host card to it.
    if (g_memory_card != NULL && dol_card_mounted(g_memory_card) != s.card_mounted) {
        if (s.card_mounted)
            dol_card_mount(g_memory_card);
        else
            dol_card_unmount(g_memory_card);
    }
}

// The multi-stage GX shortcuts (ED_GX_HLE=1) exist only in the windowed build,
// so their stage state is a section of its own. NULL: the state has none.
#ifdef ED_AURORA
typedef struct {
    HleDisplayListCall display_list_call;
    HleGxBegin gx_begin;
} HleGxQuickState;

size_t hle_gx_state_size(void) {
    return sizeof(HleGxQuickState);
}

void hle_gx_state_save(void* out) {
    HleGxQuickState s;
    memset(&s, 0, sizeof s);
    memcpy(&s.display_list_call, &g_display_list_call, sizeof s.display_list_call);
    memcpy(&s.gx_begin, &g_gx_begin, sizeof s.gx_begin);
    memcpy(out, &s, sizeof s);
}

void hle_gx_state_load(const void* in) {
    HleGxQuickState s;
    if (in != NULL)
        memcpy(&s, in, sizeof s);
    else
        memset(&s, 0, sizeof s);
    g_display_list_call = s.display_list_call;
    g_gx_begin = s.gx_begin;
}
#else
size_t hle_gx_state_size(void) {
    return 0;
}

void hle_gx_state_save(void* out) {
    (void)out;
}

void hle_gx_state_load(const void* in) {
    (void)in;
}
#endif

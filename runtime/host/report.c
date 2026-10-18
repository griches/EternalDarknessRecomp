// Problem reports. See report.h.
//
// The log: stdout and stderr go through pipes to threads that pass every
// byte on to the original descriptors and keep the last 64 KB in a ring, so
// a report carries what led up to the problem on any device (the Mac app,
// the Deck, the PS5 and the PC have no log file). ED_NO_LOG_TEE=1 leaves
// them alone.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // glibc: dladdr, for the crash report's image base
#endif
#include "host/report.h"

#include "gxruntime/aurora_backend.h"
#include "host/compat.h"
#include "host/frontend.h"
#include "host/quickstate.h"

#include "ed_build_id.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#endif
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
#include <dlfcn.h>
#include <execinfo.h>
#define HAVE_BACKTRACE 1
#endif

#define REPORT_DIR "saves/reports"
#define LOG_RING (64u * 1024u)

#if defined(_WIN32)
#define SYSTEM_NAME "Windows"
#elif defined(__APPLE__)
#define SYSTEM_NAME "macOS"
#elif defined(__PROSPERO__) || defined(PS5)
#define SYSTEM_NAME "PlayStation 5"
#elif defined(__linux__)
#define SYSTEM_NAME "Linux"
#else
#define SYSTEM_NAME "unknown"
#endif

// --- Threads and locks --------------------------------------------------------

#ifdef _WIN32
typedef CRITICAL_SECTION Lock;
static void lock_init(Lock* l) { InitializeCriticalSection(l); }
static void lock(Lock* l) { EnterCriticalSection(l); }
static void unlock(Lock* l) { LeaveCriticalSection(l); }
typedef unsigned(__stdcall* ThreadFn)(void*);
#define THREAD_RET unsigned __stdcall
#define THREAD_END return 0
static bool start_thread(ThreadFn fn, void* arg) {
    const uintptr_t h = _beginthreadex(NULL, 0, fn, arg, 0, NULL);
    if (h == 0)
        return false;
    CloseHandle((HANDLE)h);
    return true;
}
static void sleep_ms(unsigned ms) { Sleep(ms); }
#define dup _dup
#define dup2 _dup2
#define close _close
#define read _read
#define write _write
#else
typedef pthread_mutex_t Lock;
static void lock_init(Lock* l) { pthread_mutex_init(l, NULL); }
static void lock(Lock* l) { pthread_mutex_lock(l); }
static void unlock(Lock* l) { pthread_mutex_unlock(l); }
typedef void* (*ThreadFn)(void*);
#define THREAD_RET void*
#define THREAD_END return NULL
static bool start_thread(ThreadFn fn, void* arg) {
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg) != 0)
        return false;
    pthread_detach(t);
    return true;
}
static void sleep_ms(unsigned ms) {
    struct timespec ts = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    nanosleep(&ts, NULL);
}
#endif

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// --- The log's tail -----------------------------------------------------------

static Lock g_ring_lock;
static char g_ring[LOG_RING];
static unsigned long long g_ring_total;  // bytes ever added

static void ring_add(const char* p, size_t n) {
    lock(&g_ring_lock);
    for (size_t i = 0; i < n; i++)
        g_ring[(g_ring_total + i) % LOG_RING] = p[i];
    g_ring_total += n;
    unlock(&g_ring_lock);
}

// The tail in order, into out (at least LOG_RING bytes); returns its length.
static size_t ring_copy(char* out) {
    lock(&g_ring_lock);
    const size_t n = g_ring_total < LOG_RING ? (size_t)g_ring_total : LOG_RING;
    for (size_t i = 0; i < n; i++)
        out[i] = g_ring[(g_ring_total - n + i) % LOG_RING];
    unlock(&g_ring_lock);
    return n;
}

typedef struct {
    int from;  // the pipe's read end
    int to;    // the original descriptor
} Tee;
static Tee g_tee[2];

static THREAD_RET tee_main(void* arg) {
    const Tee* t = (const Tee*)arg;
    char buf[4096];
    for (;;) {
        const int n = (int)read(t->from, buf, sizeof buf);
        if (n <= 0)
            break;
        (void)!write(t->to, buf, (unsigned)n);
        ring_add(buf, (size_t)n);
    }
    THREAD_END;
}

// fd (1 or 2) through a pipe to its tee thread; untouched if anything fails.
static void log_through_pipe(int fd, Tee* t) {
    int fds[2];
#ifdef _WIN32
    if (_pipe(fds, 65536, _O_BINARY) != 0)
        return;
#else
    if (pipe(fds) != 0)
        return;
#endif
    t->from = fds[0];
    t->to = dup(fd);
    if (t->to < 0 || !start_thread(tee_main, t)) {
        close(fds[0]);
        close(fds[1]);
        if (t->to >= 0)
            close(t->to);
        t->to = -1;
        return;
    }
    if (dup2(fds[1], fd) < 0)
        t->to = -1;  // the thread waits on a pipe nobody writes: harmless
    close(fds[1]);
}

// At exit and on a crash: the descriptors back to where they were, so the
// last lines are not left in a pipe.
static void untee(void) {
    fflush(stdout);
    fflush(stderr);
    for (int i = 0; i < 2; i++)
        if (g_tee[i].to >= 0) {
            dup2(g_tee[i].to, i + 1);
            g_tee[i].to = -1;
        }
}

// --- Reports ------------------------------------------------------------------

static Lock g_write_lock;

static void stamp(char* out, size_t cap) {
    const time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm) != NULL)
        strftime(out, cap, "%Y%m%d-%H%M%S", &tm);
    else
        snprintf(out, cap, "%lld", (long long)now);
}

static bool file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return false;
    fclose(f);
    return true;
}

static void copy_file_into(FILE* out, const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(out, "(%s not found)\n", path);
        return;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        fwrite(buf, 1, n, out);
    fclose(f);
}

static char g_last_report[256];
static char g_last_state[256];
static bool g_state_saved;

const char* report_write(const char* kind, const char* what, bool with_state) {
    lock(&g_write_lock);
    char when[32], date[32];
    stamp(when, sizeof when);
    {
        const time_t now = time(NULL);
        struct tm tm;
        if (localtime_r(&now, &tm) == NULL || strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", &tm) == 0)
            snprintf(date, sizeof date, "%s", when);
    }
    mkdir("saves", 0755);
    mkdir(REPORT_DIR, 0755);
    snprintf(g_last_report, sizeof g_last_report, REPORT_DIR "/%s-%s.txt", kind, when);
    g_last_state[0] = '\0';
    g_state_saved = false;
    if (with_state) {
        // The run loop saves it at its next block boundary, if it still runs
        // guest code.
        snprintf(g_last_state, sizeof g_last_state, REPORT_DIR "/%s-%s.state", kind, when);
        quickstate_request_save(g_last_state);
        for (int i = 0; i < 60 && !(g_state_saved = file_exists(g_last_state)); i++)
            sleep_ms(100);
    }
    fprintf(stderr, "[report] %s: %s -> %s\n", kind, what, g_last_report);
    FILE* out = fopen(g_last_report, "w");
    if (out == NULL) {
        unlock(&g_write_lock);
        return NULL;
    }
    const char* settings = getenv("ED_SETTINGS");
    fprintf(out,
            "Eternal Darkness Recomp: problem report\n"
            "Kind:      %s\n"
            "What:      %s\n"
            "When:      %s\n"
            "Build:     %s (built %s %s)\n"
            "System:    %s\n"
            "Game:      %s\n"
            "Renderer:  %u frame(s) it could not decode\n",
            kind, what, date, ED_BUILD_COMMIT, __DATE__, __TIME__, SYSTEM_NAME,
            ed_game_id(), dol_aurora_gx_decode_errors());
    if (with_state)
        fprintf(out, "State:     %s%s\n", g_last_state,
                g_state_saved ? "" : " (not saved: the game was not running code)");
    fprintf(out, "\nPlease send this file%s with a few words on what you were doing.\n",
            g_state_saved ? " and the .state file beside it" : "");
    fprintf(out, "\n== Guest ==\n");
    main_describe_guest(out);
    fprintf(out, "\n== Settings ==\n");
    copy_file_into(out, settings != NULL && settings[0] != '\0' ? settings : "settings.ini");
    static char tail[LOG_RING];
    const size_t n = ring_copy(tail);
    fprintf(out, "\n== Log (last %zu bytes) ==\n", n);
    fwrite(tail, 1, n, out);
    fclose(out);
    unlock(&g_write_lock);
    return g_last_report;
}

// --- Crashes ------------------------------------------------------------------

// The host's own call stack at a crash, after the report's text: symbol
// names where the executable has them, and the image's load address, so
// `atos -l <base>` / addr2line can name the rest.
static void append_backtrace(void) {
#ifdef HAVE_BACKTRACE
    if (g_last_report[0] == '\0')
        return;
    const int fd = open(g_last_report, O_WRONLY | O_APPEND);
    if (fd < 0)
        return;
    void* frames[64];
    const int n = backtrace(frames, 64);
    Dl_info info;
    char line[256];
    const int len = snprintf(line, sizeof line, "\n== Host stack (image base %p) ==\n",
                             dladdr((void*)append_backtrace, &info) ? info.dli_fbase : NULL);
    (void)!write(fd, line, (size_t)len);
    backtrace_symbols_fd(frames, n, fd);
    close(fd);
#endif
}

#ifdef _WIN32
static LONG WINAPI on_exception(EXCEPTION_POINTERS* e) {
    static volatile LONG once;
    if (InterlockedExchange(&once, 1) == 0) {
        char what[160];
        const char* base = (const char*)GetModuleHandleA(NULL);
        const char* at = (const char*)e->ExceptionRecord->ExceptionAddress;
        snprintf(what, sizeof what, "exception 0x%08lX at %p (EternalDarkness.exe+0x%llx)",
                 (unsigned long)e->ExceptionRecord->ExceptionCode, (const void*)at,
                 (unsigned long long)(at - base));
        report_write("crash", what, false);
        untee();
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void on_abort(int sig) {
    (void)sig;
    report_write("crash", "abort", false);
    untee();
}
#else
static void on_crash(int sig, siginfo_t* info, void* context) {
    (void)context;
#if defined(__APPLE__) || defined(__linux__)
    alarm(5);  // never hang in here: a broken heap could block the report
#endif
    const char* name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS"
                       : sig == SIGILL ? "SIGILL" : sig == SIGFPE ? "SIGFPE"
                       : sig == SIGABRT ? "SIGABRT" : "signal";
    char what[128];
    snprintf(what, sizeof what, "%s (%d) at address %p", name, sig,
             info != NULL ? info->si_addr : NULL);
    report_write("crash", what, false);
    append_backtrace();
    untee();
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

static void install_crash_handlers(void) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(on_exception);
    signal(SIGABRT, on_abort);
#else
    // Its own stack, so a stack overflow on this thread still reports.
    static char alt[64 * 1024];
    stack_t ss = {0};
    ss.ss_sp = alt;
    ss.ss_size = sizeof alt;
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    static const int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (size_t i = 0; i < sizeof kSignals / sizeof kSignals[0]; i++)
        sigaction(kSignals[i], &sa, NULL);
#endif
}

void report_init(void) {
    lock_init(&g_ring_lock);
    lock_init(&g_write_lock);
    g_tee[0].to = g_tee[1].to = -1;
    if (getenv("ED_NO_LOG_TEE") == NULL) {
        log_through_pipe(1, &g_tee[0]);
        log_through_pipe(2, &g_tee[1]);
        atexit(untee);
    }
    install_crash_handlers();
}

// --- The watchdog ---------------------------------------------------------------

static atomic_llong g_last_frame;  // 0: no game frame yet (not armed)
static atomic_int g_pauses;
enum { PROBLEM_NONE, PROBLEM_PENDING, PROBLEM_SHOWN };
static atomic_int g_problem;
static char g_problem_text[512];
static long long g_hang_ms = 6000;

void report_frame(void) {
    atomic_store(&g_last_frame, now_ms());
}

void report_pause(bool paused) {
    atomic_fetch_add(&g_pauses, paused ? 1 : -1);
    if (!paused)
        atomic_store(&g_last_frame, now_ms());
}

static THREAD_RET watchdog_main(void* arg) {
    (void)arg;
    long long last_tick = now_ms();
    bool gx_reported = false;
    for (;;) {
        sleep_ms(500);
        const long long now = now_ms();
        // A gap in the watchdog's own ticks: the process was suspended (rest
        // mode, a sleeping Mac), which is no hang.
        if (now - last_tick > 3000 && atomic_load(&g_last_frame) != 0)
            atomic_store(&g_last_frame, now);
        last_tick = now;
        if (!gx_reported && dol_aurora_gx_decode_errors() != 0) {
            gx_reported = true;
            report_write("gx", "the renderer could not decode a frame (it dropped it and went on)",
                         true);
        }
        const long long last = atomic_load(&g_last_frame);
        if (last == 0 || atomic_load(&g_pauses) > 0 ||
            atomic_load(&g_problem) != PROBLEM_NONE || now - last < g_hang_ms)
            continue;
        char what[96];
        snprintf(what, sizeof what, "no new frame for %lld seconds", g_hang_ms / 1000);
        main_request_pcdump();
        sleep_ms(1000);
        const char* path = report_write("hang", what, true);
        // Two lines fit under the page's rows.
        if (path == NULL)
            snprintf(g_problem_text, sizeof g_problem_text,
                     "No new frame for %lld seconds. The report could not be written.",
                     g_hang_ms / 1000);
        else
            snprintf(g_problem_text, sizeof g_problem_text,
                     "No new frame for %lld seconds. Saved a report%s: %.*s%s. Please send %s.",
                     g_hang_ms / 1000, g_state_saved ? " and this moment" : "",
                     (int)(strlen(path) - 4u), path, g_state_saved ? " (.txt, .state)" : ".txt",
                     g_state_saved ? "both" : "it");
        atomic_store(&g_problem, PROBLEM_PENDING);
    }
    THREAD_END;
}

void report_start_watchdog(void) {
    const char* env = getenv("ED_HANG_SECONDS");
    if (env != NULL && atoi(env) > 0)
        g_hang_ms = (long long)atoi(env) * 1000;
    if (env != NULL && atoi(env) == 0 && env[0] == '0')
        return;  // ED_HANG_SECONDS=0: no watchdog
    start_thread(watchdog_main, NULL);
}

static THREAD_RET exit_later(void* arg) {
    (void)arg;
    sleep_ms(4000);
    untee();
    _exit(0);
    THREAD_END;
}

void report_idle(void) {
    if (atomic_load(&g_problem) != PROBLEM_PENDING)
        return;
    atomic_store(&g_problem, PROBLEM_SHOWN);
    if (frontend_problem("Not responding", g_problem_text, true)) {
        atomic_store(&g_last_frame, now_ms());
        atomic_store(&g_problem, PROBLEM_NONE);
        return;
    }
    // Quit: the game thread may be stuck where it never sees the request.
    dol_aurora_request_quit();
    start_thread(exit_later, NULL);
}

void report_game_stopped(const char* reason) {
    char what[160];
    snprintf(what, sizeof what, "the game stopped: %s", reason);
    const char* path = report_write("stopped", what, false);
    char text[512];
    snprintf(text, sizeof text, "%s. Saved a report: %s. Please send it.", reason,
             path != NULL ? path : "(could not be written)");
    if (text[0] >= 'a' && text[0] <= 'z')
        text[0] = (char)(text[0] - 'a' + 'A');
    frontend_problem("The game stopped", text, false);
}

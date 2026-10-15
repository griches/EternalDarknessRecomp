#pragma once
// Windows stand-ins for the few POSIX calls the host uses (monotonic clock,
// environment, mkdir). Elsewhere this only includes the POSIX headers.
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
// QueryPerformanceCounter; the clock id is ignored (always monotonic).
int ed_clock_gettime(int clock_id, struct timespec* ts);
#define clock_gettime ed_clock_gettime

static inline int ed_setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && getenv(name) != NULL)
        return 0;
    return _putenv_s(name, value);
}
static inline int ed_unsetenv(const char* name) { return _putenv_s(name, ""); }
#define setenv ed_setenv
#define unsetenv ed_unsetenv

#define mkdir(path, mode) _mkdir(path)
#define localtime_r(t, out) (localtime_s((out), (t)) == 0 ? (out) : NULL)

#ifdef __cplusplus
}
#endif
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

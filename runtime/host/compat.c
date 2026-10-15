// Windows stand-ins for POSIX calls (see compat.h).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "host/compat.h"

int ed_clock_gettime(int clock_id, struct timespec* ts) {
    (void)clock_id;
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    ts->tv_sec = (time_t)(now.QuadPart / freq.QuadPart);
    ts->tv_nsec = (long)((now.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
    return 0;
}

int ed_rename_replace(const char* from, const char* to) {
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
               ? 0
               : -1;
}
#else
// Nothing to stand in for: the POSIX calls exist.
typedef int ed_compat_unused;
#endif

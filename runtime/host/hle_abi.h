#ifndef ED_HOST_HLE_ABI_H
#define ED_HOST_HLE_ABI_H

// PowerPC GameCube EABI helpers for HLE handlers.
//
// Integer arguments arrive in r3..r10 (gpr[3]..gpr[10]); argument index 0 is
// r3. Floating-point arguments arrive in f1..f13 (fpr[1]..fpr[13]). Integer
// return values go in r3, floating-point returns in f1. A handler "returns" to
// its caller by setting pc = lr (the recompiled body is never entered).
#include "core/cpu.h"

#include <stddef.h>

// Integer argument `i` (i == 0 -> r3). Arguments past r10 live on the caller's
// stack; handlers needing >8 integer args should read them explicitly.
static inline u32 hle_arg_u32(CPUState* c, unsigned i) {
    return (i < 8) ? c->gpr[3 + i] : 0u;
}

// Floating-point argument `i` (i == 0 -> f1).
static inline f64 hle_arg_f64(CPUState* c, unsigned i) {
    return (i < 13) ? c->fpr[1 + i] : 0.0;
}

static inline void hle_set_u32(CPUState* c, u32 v) { c->gpr[3] = v; }
static inline void hle_set_f64(CPUState* c, f64 v) { c->fpr[1] = v; }

// Return from an intercepted call to its caller.
static inline void hle_return(CPUState* c) { c->pc = c->lr; }

// Copy a NUL-terminated guest string at `gaddr` into `buf` (host), at most
// `cap` bytes including the terminator. Returns `buf`. A zero/!mapped address
// yields an empty string.
char* hle_read_cstr(CPUState* c, u32 gaddr, char* buf, size_t cap);

#endif // ED_HOST_HLE_ABI_H

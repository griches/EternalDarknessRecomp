#pragma once
// HD texture packs in Dolphin's format (texpack.cpp).
#ifdef __cplusplus
#include <cstddef>
extern "C" {
#endif

// Index the pack folder and hook it into the renderer (no-op without one).
void texpack_init(void);

#ifdef __cplusplus
}
// Textures the pack offers (0: no pack).
size_t texpack_count();
#endif

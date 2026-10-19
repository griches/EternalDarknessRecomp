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
#include <cstdint>
#include <vector>
// Textures the pack offers (0: no pack).
size_t texpack_count();
// Images being read, or read and not yet taken by the renderer (photo mode
// lets the game draw until there are none).
bool texpack_busy();
// The pack's image of a texture as RGBA8, its base level only (a DDS file's
// BC blocks decoded), for a host redraw that starts from it. False with the
// pack off or without that texture. Reads the file: call it off the renderer's
// thread.
bool texpack_read_rgba(uint32_t format, uint32_t width, uint32_t height, uint64_t hash,
                       std::vector<uint8_t>* rgba, uint32_t* out_width, uint32_t* out_height);
#endif

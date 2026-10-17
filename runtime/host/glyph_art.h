#pragma once
// Textures with a GameCube button painted in, redrawn with the player's
// (glyph_art.cpp). Hooked into the texture override beside the button prompts
// (buttons.cpp).
#include <cstdint>

// Asked by format and size, then by Dolphin's name hash: only the textures it
// redraws are claimed, so others of their size keep their HD pack versions.
bool art_claims(uint32_t format, uint32_t width, uint32_t height);
bool art_claims_hash(uint64_t hash);
// A texture override answer for the texture with that name hash: 1 redrawn, 2
// while a worker draws it (the game's shows meanwhile), 0 otherwise.
int art_apply(const uint8_t* rgba, uint32_t width, uint32_t height, uint64_t hash,
              uint8_t** out_rgba, uint32_t* out_width, uint32_t* out_height);
// Changes with the layout, the bindings, the controller family and HD textures.
uint64_t art_generation();

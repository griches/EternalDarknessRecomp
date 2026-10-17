#pragma once
// The rooms' baked light and shadow, upscaled as they load (lightmaps.cpp).
// Hooked into the texture override beside the button prompts and the font
// sheets (buttons.cpp), as its claim for textures drawn through the second
// UV set.
#include <cstdint>

bool lightmaps_claims_uv2(uint32_t format, uint32_t width, uint32_t height);
// A texture override answer (DolAuroraTextureOverride.apply): 1 with the
// upscaled light map, 2 while a worker makes it (the original shows meanwhile).
int lightmaps_apply(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t** out_rgba,
                    uint32_t* out_width, uint32_t* out_height);
uint64_t lightmaps_generation();

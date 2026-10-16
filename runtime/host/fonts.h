#pragma once
// The game's text font sheets redrawn sharp (fonts.cpp). Hooked into the
// texture override beside the button prompts (buttons.cpp).
#include <cstdint>

// settings game_font
constexpr int kGameFontSharp = 0;
constexpr int kGameFontTome = 1;
constexpr int kGameFontOriginal = 2;

bool fonts_claims(uint32_t format, uint32_t width, uint32_t height);
// A texture override answer (DolAuroraTextureOverride.apply): 1 with the sheet
// rebuilt, 2 while a worker builds it (the game's own sheet shows meanwhile),
// 0 for any other texture.
int fonts_apply(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t** out_rgba,
                uint32_t* out_width, uint32_t* out_height);
// Loads the current style's letter sheets on a worker, ahead of the game's text.
void fonts_preload();
// Changes when the style does, so the sheets are rebuilt.
uint64_t fonts_generation();

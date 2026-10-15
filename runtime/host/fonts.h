#pragma once
// The game's text font sheets redrawn sharp (fonts.cpp). Hooked into the
// texture override beside the button prompts (buttons.cpp).
#include <cstdint>

// settings game_font
constexpr int kGameFontSharp = 0;
constexpr int kGameFontTome = 1;
constexpr int kGameFontOriginal = 2;

bool fonts_claims(uint32_t format, uint32_t width, uint32_t height);
bool fonts_apply(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t** out_rgba,
                 uint32_t* out_width, uint32_t* out_height);
// Changes when the style does, so the sheets are rebuilt.
uint64_t fonts_generation();

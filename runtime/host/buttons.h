#pragma once
// The game's button icons, drawn for the player's controller and layout.
#ifdef __cplusplus
extern "C" {
#endif
void buttons_init(void);
#ifdef __cplusplus
}
#include <cstdint>
#include <string>
#include <vector>
// Load a PNG file as RGBA8.
bool load_png_rgba(const std::string& path, std::vector<uint8_t>* rgba, uint32_t* width,
                   uint32_t* height);
#endif

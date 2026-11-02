#ifndef ED_HOST_PAD_SOUND_H
#define ED_HOST_PAD_SOUND_H

#include "core/types.h"

#include <stdbool.h>

// Sounds from the controller's own speaker (pad_sound.c): the door knocks (the
// level scripts' stream 14), the muttering of a character losing their mind
// (595-599) and the voice on the lobby phone (479), played by the DualSense instead of the TV where the machine can
// reach its speaker (the PS5, so far).

// Read and decode those streams from the disc image (after dvd_open_image).
void pad_sound_init(const char* disc_image_path);

// Whether any can go to the controller (hle.c hooks the game's stream start
// only then).
bool pad_sound_available(void);

// The game starts stream `stream` at `volume` (0-127): true when the
// controller plays it, and the game's own should stay silent.
bool pad_sound_stream(u32 stream, u32 volume);

// A stream's decoded sound (mono), on any machine: the muttering for the
// recomp's own insanity effects (insanity.cpp). False if not read.
bool pad_sound_pcm(u32 stream, const s16** pcm, u32* frames, u32* rate);

#endif

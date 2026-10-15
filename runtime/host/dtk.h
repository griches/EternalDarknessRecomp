#ifndef ED_HOST_DTK_H
#define ED_HOST_DTK_H

#include "core/types.h"

// Disc audio streaming, the drive feature the DTK library drives: the game
// prepares a byte range of an .adp file (DVDPrepareStream*), starts the AI
// stream, and the drive plays it as 48/32 kHz ADPCM straight into the mixer.
// The host decodes the range from the disc image and mixes it into AID output.

void dtk_init(const char* disc_image_path);
void dtk_prepare(u32 disc_offset, u32 length);
void dtk_cancel(void);
void dtk_stop_at_end(void);
bool dtk_active(void);
// Current play position as a disc byte offset (DVDGetStreamPlayAddr).
u32 dtk_play_address(void);
// Mix `frames` stereo frames of the stream (at stream_rate) into `out`
// (interleaved L/R at out_rate), scaled by the AI stream volume (0-255).
// Returns the number of stream samples consumed.
u32 dtk_mix(s16* out, u32 frames, u32 out_rate, u32 stream_rate,
            u32 volume_left, u32 volume_right);

#endif // ED_HOST_DTK_H

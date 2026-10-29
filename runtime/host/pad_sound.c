// Sounds from the controller's own speaker (pad_sound.h): the door knock,
// /audio/stream/0014, and the muttering of a character losing their mind,
// 0595-0599 (the character sound picker 0x80047558 gives them for event 61 by
// sanity level). Each is a stream of its own on the disc: Nintendo DSP ADPCM
// with the standard 0x60-byte header (big-endian: samples, nibbles, rate, ...,
// 16 coefficients at 0x1C, history at 0x40), then 8-byte frames of 14
// samples. They are read and decoded once here; when the game starts one
// (hle.c), the PS5 port's pad_speaker_play plays it on player 1's DualSense.
#include "host/pad_sound.h"

#include "gxruntime/dvd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__PROSPERO__)
#include <stdint.h>
// ps5/src/platform.c: false when there is no controller or speaker.
bool pad_speaker_play(const int16_t* samples, uint32_t count, uint32_t rate, float volume);
#endif

typedef struct {
    u32 stream;
    s16* pcm;
    u32 frames, rate;
} PadSound;

static PadSound g_sounds[] = {{14u}, {595u}, {596u}, {597u}, {598u}, {599u}};
#define PAD_SOUNDS (sizeof g_sounds / sizeof g_sounds[0])
static bool g_any;

static u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static s16 be16(const u8* p) { return (s16)(p[0] << 8 | p[1]); }

static bool decode_dsp(const u8* data, u32 size, s16** out, u32* frames, u32* rate) {
    if (size < 0x60u)
        return false;
    const u32 n = be32(data), hz = be32(data + 8);
    if (n == 0u || hz == 0u || n > (size - 0x60u) / 8u * 14u + 14u)
        return false;
    s16 coef[16];
    for (int i = 0; i < 16; i++)
        coef[i] = be16(data + 0x1C + 2 * i);
    s32 h1 = be16(data + 0x40), h2 = be16(data + 0x42);
    s16* pcm = (s16*)malloc(n * sizeof *pcm);
    if (pcm == NULL)
        return false;
    u32 done = 0;
    for (u32 p = 0x60u; done < n && p + 8u <= size; p += 8u) {
        const u32 pred = data[p] >> 4, scale = 1u << (data[p] & 15u);
        const s32 c1 = coef[(pred * 2u) & 15u], c2 = coef[(pred * 2u + 1u) & 15u];
        for (u32 i = 0; i < 14u && done < n; i++) {
            const u8 b = data[p + 1u + i / 2u];
            s32 nib = (i % 2u) == 0u ? b >> 4 : b & 15;
            if (nib >= 8)
                nib -= 16;
            s32 s = ((nib * (s32)scale) * 2048 + c1 * h1 + c2 * h2 + 1024) >> 11;
            s = s < -32768 ? -32768 : s > 32767 ? 32767 : s;
            pcm[done++] = (s16)s;
            h2 = h1;
            h1 = s;
        }
    }
    *out = pcm;
    *frames = done;
    *rate = hz;
    return true;
}

void pad_sound_init(const char* disc_image_path) {
#if defined(__PROSPERO__)
    FILE* f = disc_image_path != NULL ? fopen(disc_image_path, "rb") : NULL;
    if (f == NULL)
        return;
    for (size_t i = 0; i < PAD_SOUNDS; i++) {
        PadSound* s = &g_sounds[i];
        char path[32];
        snprintf(path, sizeof path, "/audio/stream/%04u", s->stream);
        u32 start = 0, length = 0;
        const s32 entry = dvd_path_to_entrynum(path);
        if (entry < 0 || !dvd_entry_info(entry, &start, &length) || length > (1u << 22))
            continue;
        u8* data = (u8*)malloc(length);
        if (data != NULL && fseek(f, (long)start, SEEK_SET) == 0 && fread(data, 1, length, f) == length &&
            decode_dsp(data, length, &s->pcm, &s->frames, &s->rate))
            g_any = true;
        free(data);
    }
    fclose(f);
    if (g_any)
        fprintf(stderr, "[pad sound] the door knock and the muttering go to the controller's speaker\n");
#else
    (void)disc_image_path;
    (void)decode_dsp;
#endif
}

bool pad_sound_available(void) { return g_any; }

bool pad_sound_stream(u32 stream, u32 volume) {
    for (size_t i = 0; i < PAD_SOUNDS; i++) {
        const PadSound* s = &g_sounds[i];
        if (s->stream != stream || s->pcm == NULL)
            continue;
#if defined(__PROSPERO__)
        const float v = volume >= 127u ? 1.f : (float)volume / 127.f;
        return pad_speaker_play(s->pcm, s->frames, s->rate, v);
#else
        (void)volume;
        return false;
#endif
    }
    return false;
}

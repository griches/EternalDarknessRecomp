// Disc audio streaming (DTK): the drive's ADPCM music, played from the image.
#include "host/dtk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One 32-byte block: left and right predictor/scale bytes, two unused bytes,
// then 28 bytes each holding a left (low nibble) and right (high nibble) sample.
#define BLOCK_BYTES 32u
#define BLOCK_SAMPLES 28u
#define READ_BLOCKS 256u

typedef struct {
    u32 start;   // disc offset of the stream
    u32 length;  // bytes
    bool valid;
} DtkStream;

static DtkStream g_current;
static DtkStream g_next;       // prepared while the current one plays
static u32 g_position;         // bytes into g_current
static bool g_stop_at_end;
static s32 g_hist_l1, g_hist_l2, g_hist_r1, g_hist_r2;

static u8 g_blocks[READ_BLOCKS * BLOCK_BYTES];
static u32 g_blocks_start = 0xFFFFFFFFu;  // disc offset held in g_blocks
static s16 g_pcm[BLOCK_SAMPLES * 2u];
static u32 g_pcm_pos = BLOCK_SAMPLES;      // next frame in g_pcm
static u32 g_phase;                       // 16.16 resampler position
static bool g_log;
static FILE* g_image;

// Dolphin's StreamADPCM::ADPDecodeSample.
static s16 decode_sample(s32 bits, s32 q, s32* hist1, s32* hist2) {
    s32 hist = 0;
    switch (q >> 4) {
    case 1: hist = *hist1 * 0x3c; break;
    case 2: hist = *hist1 * 0x73 - *hist2 * 0x34; break;
    case 3: hist = *hist1 * 0x62 - *hist2 * 0x37; break;
    default: break;
    }
    hist = (hist + 0x20) >> 6;
    if (hist < -0x200000) hist = -0x200000;
    if (hist > 0x1fffff) hist = 0x1fffff;
    s32 cur = (((s16)(bits << 12) >> (q & 0xf)) << 6) + hist;
    *hist2 = *hist1;
    *hist1 = cur;
    cur >>= 6;
    if (cur < -0x8000) cur = -0x8000;
    if (cur > 0x7fff) cur = 0x7fff;
    return (s16)cur;
}

static void reset_decoder(void) {
    g_hist_l1 = g_hist_l2 = g_hist_r1 = g_hist_r2 = 0;
    g_pcm_pos = BLOCK_SAMPLES;
    g_phase = 0;
}

static bool read_image(void* out, u32 offset, u32 bytes) {
    return g_image != NULL && fseek(g_image, (long)offset, SEEK_SET) == 0 &&
           fread(out, 1, bytes, g_image) == bytes;
}

void dtk_init(const char* disc_image_path) {
    if (g_image != NULL)
        fclose(g_image);
    g_image = disc_image_path ? fopen(disc_image_path, "rb") : NULL;
    memset(&g_current, 0, sizeof g_current);
    memset(&g_next, 0, sizeof g_next);
    g_position = 0;
    g_stop_at_end = false;
    g_blocks_start = 0xFFFFFFFFu;
    reset_decoder();
    g_log = getenv("ED_DTK_LOG") != NULL;
}

void dtk_prepare(u32 disc_offset, u32 length) {
    DtkStream s = {disc_offset, length, true};
    if (g_log)
        fprintf(stderr, "[dtk] prepare disc=0x%08X len=%u%s\n", disc_offset,
                length, g_current.valid ? " (queued)" : "");
    if (g_current.valid) {
        g_next = s;
        return;
    }
    g_current = s;
    g_position = 0;
    g_stop_at_end = false;
    reset_decoder();
}

void dtk_cancel(void) {
    if (g_log)
        fprintf(stderr, "[dtk] cancel\n");
    g_current.valid = false;
    g_next.valid = false;
    reset_decoder();
}

void dtk_stop_at_end(void) {
    g_stop_at_end = true;
    g_next.valid = false;
}

bool dtk_active(void) {
    return g_current.valid;
}

u32 dtk_play_address(void) {
    return g_current.valid ? g_current.start + g_position : 0;
}

// Next decoded block of the current stream, moving on to the queued one (or
// stopping) at its end. False when there is nothing to play.
static bool decode_block(void) {
    if (!g_current.valid)
        return false;
    if (g_position + BLOCK_BYTES > g_current.length) {
        // The drive moves on to a stream prepared while this one played (the
        // DTK library queues the next or repeated track that way); otherwise
        // it stops at the end.
        if (g_next.valid && !g_stop_at_end) {
            g_current = g_next;
            g_next.valid = false;
        } else {
            if (g_log)
                fprintf(stderr, "[dtk] end of stream\n");
            g_current.valid = false;
            return false;
        }
        g_position = 0;
        g_hist_l1 = g_hist_l2 = g_hist_r1 = g_hist_r2 = 0;
    }
    const u32 offset = g_current.start + g_position;
    if (offset < g_blocks_start ||
        offset + BLOCK_BYTES > g_blocks_start + sizeof g_blocks) {
        if (!read_image(g_blocks, offset, sizeof g_blocks))
            return false;
        g_blocks_start = offset;
    }
    const u8* block = g_blocks + (offset - g_blocks_start);
    for (u32 i = 0; i < BLOCK_SAMPLES; i++) {
        const u8 b = block[BLOCK_BYTES - BLOCK_SAMPLES + i];
        g_pcm[i * 2u] = decode_sample(b & 0xf, block[0], &g_hist_l1, &g_hist_l2);
        g_pcm[i * 2u + 1u] = decode_sample(b >> 4, block[1], &g_hist_r1, &g_hist_r2);
    }
    g_position += BLOCK_BYTES;
    g_pcm_pos = 0;
    return true;
}

static s16 clamp16(s32 v) {
    return (s16)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

u32 dtk_mix(s16* out, u32 frames, u32 out_rate, u32 stream_rate,
            u32 volume_left, u32 volume_right) {
    if (!g_current.valid || out_rate == 0)
        return 0;
    const u32 step = (u32)(((u64)stream_rate << 16) / out_rate);
    u32 consumed = 0;
    for (u32 f = 0; f < frames; f++) {
        if (g_pcm_pos >= BLOCK_SAMPLES && !decode_block())
            return consumed;
        const s16* frame = g_pcm + g_pcm_pos * 2u;
        out[f * 2u] = clamp16(out[f * 2u] + (frame[0] * (s32)volume_left) / 255);
        out[f * 2u + 1u] =
            clamp16(out[f * 2u + 1u] + (frame[1] * (s32)volume_right) / 255);
        g_phase += step;
        while (g_phase >= 0x10000u) {
            g_phase -= 0x10000u;
            g_pcm_pos++;
            consumed++;
            if (g_pcm_pos >= BLOCK_SAMPLES && f + 1u < frames && !decode_block())
                return consumed;
        }
    }
    return consumed;
}

// ---------------------------------------------------------------------------
// Quick state (host/quickstate.c)
// ---------------------------------------------------------------------------
// The streams and decoder position. The block buffer is only a read cache of
// the image, so a load invalidates it instead.
typedef struct {
    DtkStream current;
    DtkStream next;
    u32 position;
    bool stop_at_end;
    s32 hist_l1, hist_l2, hist_r1, hist_r2;
    s16 pcm[BLOCK_SAMPLES * 2u];
    u32 pcm_pos;
    u32 phase;
} DtkQuickState;

size_t dtk_state_size(void) {
    return sizeof(DtkQuickState);
}

void dtk_state_save(void* out) {
    DtkQuickState s;
    memset(&s, 0, sizeof s);
    s.current = g_current;
    s.next = g_next;
    s.position = g_position;
    s.stop_at_end = g_stop_at_end;
    s.hist_l1 = g_hist_l1;
    s.hist_l2 = g_hist_l2;
    s.hist_r1 = g_hist_r1;
    s.hist_r2 = g_hist_r2;
    memcpy(s.pcm, g_pcm, sizeof s.pcm);
    s.pcm_pos = g_pcm_pos;
    s.phase = g_phase;
    memcpy(out, &s, sizeof s);
}

void dtk_state_load(const void* in) {
    DtkQuickState s;
    memcpy(&s, in, sizeof s);
    g_current = s.current;
    g_next = s.next;
    g_position = s.position;
    g_stop_at_end = s.stop_at_end;
    g_hist_l1 = s.hist_l1;
    g_hist_l2 = s.hist_l2;
    g_hist_r1 = s.hist_r1;
    g_hist_r2 = s.hist_r2;
    memcpy(g_pcm, s.pcm, sizeof g_pcm);
    g_pcm_pos = s.pcm_pos <= BLOCK_SAMPLES ? s.pcm_pos : BLOCK_SAMPLES;
    g_phase = s.phase;
    g_blocks_start = 0xFFFFFFFFu;
}

#include "host/settings.h"
#include "host/audio.h"
#include "host/dtk.h"
#include "game_data.h"

#include "gxruntime/audio_dma.h"
#include "gxruntime/ax_host.h"
#include "gxruntime/platform.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#define DSP_BASE 0xCC005000u
#define DSP_SIZE 0x40u
#define AI_BASE 0xCC006C00u
#define AI_SIZE 0x20u

#define AI_CONTROL_OFF DOL_AUDIO_DMA_AI_CONTROL_OFF

// salDspIsDone, MusyX's DSP completion flag for the host AX path (tools/gamedata.py).
#define MUSYX_DSP_DONE ED_MUSYX_DSP_DONE

// AI streaming registers (Dolphin AudioInterface): the part of the AI the DTK
// library uses for disc music. AICR bit 0 PSTAT plays the stream, bit 1 AISFR
// picks 48 kHz (1) or 32 kHz, bit 2 AIINTMSK, bit 3 AIINT (write 1 to clear),
// bit 5 SCRESET clears the sample counter. AIVR holds the left (bits 0-7) and
// right (8-15) volumes; AISCNT counts stream samples while playing and raises
// AIINT when it passes AIIT. GXRuntime's AI model covers only the AID rate.
#define AICR_PSTAT 0x01u
#define AICR_AISFR 0x02u
#define AICR_AIINTMSK 0x04u
#define AICR_AIINT 0x08u
#define AICR_SCRESET 0x20u
static u32 g_ai_stream_control;
static u32 g_ai_volume;
static u32 g_ai_sample_counter;
static u32 g_ai_interrupt_timing;
static u32 g_ai_counter_fraction;  // 16.16 stream samples owed by AID chunks

static DolAudioDma g_audio_dma;
static DolAxHost g_ax_host;
static bool g_waiting_for_command_list;
static bool g_log;
static u64 g_aid_chunks;

static bool g_fast_forward;
static bool g_muted;

void audio_set_fast_forward(bool on) {
    g_fast_forward = on;
}

void audio_set_muted(bool muted) {
    g_muted = muted;
}

void audio_init(void) {
    dol_ax_host_init(&g_ax_host);
    dol_audio_dma_init(&g_audio_dma);
    // Device debounces AI control writes; mirror the apploader AID rate
    // explicitly once at boot so the platform HAL starts at 32 kHz.
    dol_platform_audio_set_sample_rate(DOL_AUDIO_DMA_32KHZ);
    g_waiting_for_command_list = false;
    g_aid_chunks = 0;
    g_log = getenv("ED_AUDIO_LOG") != NULL;
    dol_ax_host_set_log(&g_ax_host, g_log);
    // Eternal Darkness runs the launch-era AX ucode (see ax_host.h).
    dol_ax_host_set_old_ucode(&g_ax_host, true);
}

void audio_shutdown(void) {
    if (g_log) {
        const DolAxHostStats* st = dol_ax_host_stats(&g_ax_host);
        fprintf(stderr,
                "[audio] mixer summary: frames=%llu nonzero_frames=%llu "
                "aid_chunks=%llu peak_voices=%u last_voices=%u peak_mix=%u "
                "unknown_ax=%u\n",
                (unsigned long long)(st ? st->frames : 0ull),
                (unsigned long long)(st ? st->nonzero_frames : 0ull),
                (unsigned long long)g_aid_chunks,
                st ? st->peak_voices : 0u, st ? st->active_voices : 0u,
                st ? st->peak_mix : 0u, st ? st->unknown_cmd_count : 0u);
    }
    dol_audio_dma_write_control(&g_audio_dma, 0);
    dol_audio_dma_ack_interrupt(&g_audio_dma);
}

void audio_set_vi_clock(u64 work_units_per_frame, u32 refresh_hz) {
    dol_audio_dma_set_vi_clock(&g_audio_dma, work_units_per_frame, refresh_hz);
}

void audio_use_wall_clock(void) {
    // A work rate of one unit per second makes every audio_poll call one chunk.
    dol_audio_dma_set_work_rate(&g_audio_dma, 1);
}

u32 audio_chunks_per_second(void) {
    const u32 rate = dol_audio_dma_sample_rate(&g_audio_dma);
    return (rate != 0u ? rate : 32000u) / 8u;  // 8 stereo frames per chunk
}

bool audio_mmio_contains(u32 address) {
    return (address >= DSP_BASE && address < DSP_BASE + DSP_SIZE) ||
           (address >= AI_BASE && address < AI_BASE + AI_SIZE);
}

u64 audio_mmio_read(u32 address, u8 size) {
    u64 value = 0;
    if (address >= AI_BASE && address < AI_BASE + AI_SIZE) {
        switch ((address - AI_BASE) & ~3u) {
        case 0x04u: return g_ai_volume;
        case 0x08u: return g_ai_sample_counter;
        case 0x0Cu: return g_ai_interrupt_timing;
        default: break;
        }
    }
    if (address >= DSP_BASE && address < DSP_BASE + DSP_SIZE)
        dol_audio_dma_dsp_mmio_read(&g_audio_dma, address - DSP_BASE, size,
                                    &value);
    else
        dol_audio_dma_ai_mmio_read(&g_audio_dma, address - AI_BASE, size,
                                   &value);
    if (address >= AI_BASE && ((address - AI_BASE) & ~3u) == 0u)
        value = (value & ~(u64)(AICR_PSTAT | AICR_AISFR | AICR_AIINTMSK |
                                AICR_AIINT)) |
                g_ai_stream_control;
    return value;
}

void audio_mmio_write(u32 address, u8 size, u64 value) {
    if (address >= DSP_BASE && address < DSP_BASE + DSP_SIZE) {
        dol_audio_dma_dsp_mmio_write(&g_audio_dma, address - DSP_BASE, size,
                                     value);
        return;
    }
    const u32 offset = address - AI_BASE;
    switch (offset & ~3u) {
    case 0x00u: {
        const u32 v = (u32)value;
        u32 next = (g_ai_stream_control & AICR_AIINT) |
                   (v & (AICR_PSTAT | AICR_AISFR | AICR_AIINTMSK));
        if (v & AICR_AIINT)
            next &= ~AICR_AIINT;
        if (v & AICR_SCRESET)
            g_ai_sample_counter = 0;
        if (g_log && ((next ^ g_ai_stream_control) & AICR_PSTAT))
            fprintf(stderr, "[audio] AI stream %s\n",
                    (next & AICR_PSTAT) ? "play" : "stop");
        g_ai_stream_control = next;
        break;
    }
    case 0x04u: g_ai_volume = (u32)value & 0xFFFFu; return;
    case 0x08u: return;  // AISCNT is read-only
    case 0x0Cu: g_ai_interrupt_timing = (u32)value; return;
    default: break;
    }
    dol_audio_dma_ai_mmio_write(&g_audio_dma, offset, size, value);
    if (g_log && offset < AI_CONTROL_OFF + 4u &&
        offset + size > AI_CONTROL_OFF) {
        u64 control = 0;
        dol_audio_dma_ai_mmio_read(&g_audio_dma, AI_CONTROL_OFF, 4, &control);
        fprintf(stderr, "[audio] AI control=0x%08llX ais=%u aid=%u\n",
                (unsigned long long)control,
                (control & 0x00000002u) != 0u ? 48000u : 32000u,
                (control & 0x00000040u) != 0u ? 32000u : 48000u);
    }
}

// A sound of the host's own (insanity.cpp: a trophy's chime), mixed into the
// game's output as it goes out, once: mono s16 at its own rate, stepped to
// the AID rate. Set and played on the game thread.
static const s16* g_host_sound;
static u32 g_host_sound_count;
static double g_host_sound_at, g_host_sound_step_rate;
static float g_host_sound_volume;

void audio_play_host_sound(const s16* samples, u32 count, u32 rate, float volume) {
    g_host_sound_count = 0u;
    g_host_sound = samples;
    g_host_sound_at = 0.0;
    g_host_sound_step_rate = (double)rate;
    g_host_sound_volume = volume;
    g_host_sound_count = count;
}

static void mix_host_sound(s16* output, u32 frames, u32 aid_rate) {
    if (g_host_sound_count == 0u || aid_rate == 0u)
        return;
    const double step = g_host_sound_step_rate / (double)aid_rate;
    for (u32 i = 0; i < frames; i++) {
        const u32 at = (u32)g_host_sound_at;
        if (at >= g_host_sound_count) {
            g_host_sound_count = 0u;
            return;
        }
        const s32 v = (s32)((float)g_host_sound[at] * g_host_sound_volume);
        for (u32 c = 0; c < 2u; c++) {
            const s32 mixed = output[i * 2u + c] + v;
            output[i * 2u + c] = (s16)(mixed > 32767 ? 32767 : mixed < -32768 ? -32768 : mixed);
        }
        g_host_sound_at += step;
    }
}

// The last of what went out (the game's mix with ours, before muting), for
// audio_play_stuck: a ring of stereo frames, g_recent_at the next to write.
#define RECENT_FRAMES 4096u
static s16 g_recent[RECENT_FRAMES * 2u];
static u32 g_recent_at;

// ED_PCM_OUT=path: append every chunk as raw s16 stereo at the AID rate,
// to inspect the mix offline.
static FILE* g_pcm_out;
static int g_pcm_checked;
// ED_PCM_WALL=1 too: silence written for the time nothing came (the game
// held by a menu), so the file runs with the clock (to put under a
// screen recording; the time it starts is logged).
static int g_pcm_wall;
static double g_pcm_last_s;

// `frames` of stereo at `rate` on their way out: to ED_PCM_OUT's file, then
// the speakers (silence when muted, nothing while fast-forwarding).
static void send_out(s16* output, u32 frames, u32 rate) {
    if (!g_pcm_checked) {
        g_pcm_checked = 1;
        const char* path = getenv("ED_PCM_OUT");
        if (path != NULL && path[0] != '\0')
            g_pcm_out = fopen(path, "wb");
        g_pcm_wall = g_pcm_out != NULL && getenv("ED_PCM_WALL") != NULL;
        if (g_pcm_wall) {
            struct timespec now;
            timespec_get(&now, TIME_UTC);
            fprintf(stderr, "[audio] pcm out starts at %.3f, %u Hz\n", now.tv_sec + now.tv_nsec / 1e9, rate);
        }
    }
    if (g_pcm_out != NULL && g_pcm_wall) {
        struct timespec ts;
        timespec_get(&ts, TIME_UTC);
        const double now = ts.tv_sec + ts.tv_nsec / 1e9;
        if (g_pcm_last_s != 0.0 && now - g_pcm_last_s > 0.1 && rate != 0u) {
            static const s16 silence[16 * 2] = {0};
            u64 gap = (u64)((now - g_pcm_last_s) * rate);
            for (; gap >= 16u; gap -= 16u)
                fwrite(silence, sizeof silence, 1, g_pcm_out);
        }
        // Past what this stretch covers, so a long one is not followed by
        // silence for its own length.
        const double after = now + (rate != 0u ? (double)frames / rate : 0.0);
        g_pcm_last_s = after > g_pcm_last_s ? after : g_pcm_last_s;
    }
    if (g_pcm_out != NULL) {
        fwrite(output, sizeof(s16) * 2u, frames, g_pcm_out);
        if ((g_aid_chunks & 1023u) == 0u)
            fflush(g_pcm_out);
    }
    static int mute = -1;  // ED_MUTE=1: keep pacing, send silence (test runs)
    if (mute < 0)
        mute = getenv("ED_MUTE") != NULL;
    if (mute || g_muted)
        memset(output, 0, sizeof(s16) * 2u * frames);
    if (g_fast_forward)
        return;
    dol_platform_audio_push(output, frames);
}

void audio_poll(CPUState* cpu) {
    u32 physical = 0;
    if (!dol_audio_dma_poll(&g_audio_dma, &physical))
        return;

    s16 output[8u * 2u];
    u32 peak = 0;
    const u32 source = GC_RAM_BASE | physical;
    for (u32 i = 0; i < 8u; i++) {
        const s16 right = (s16)mem_read16(cpu, source + i * 4u);
        const s16 left = (s16)mem_read16(cpu, source + i * 4u + 2u);
        output[i * 2u] = left;
        output[i * 2u + 1u] = right;
        const u32 left_magnitude =
            left == INT16_MIN ? 32768u : (u32)abs(left);
        const u32 right_magnitude =
            right == INT16_MIN ? 32768u : (u32)abs(right);
        if (left_magnitude > peak)
            peak = left_magnitude;
        if (right_magnitude > peak)
            peak = right_magnitude;
    }
    // Player volume for the game's own mix (effects, voices, sequenced music).
    if (g_settings.effects_volume < 100) {
        for (u32 i = 0; i < 16u; i++)
            output[i] = (s16)((s32)output[i] * g_settings.effects_volume / 100);
    }
    // Disc music: mix the stream into this AID chunk and run the AI sample
    // counter at the stream rate while the stream is playing.
    if (g_ai_stream_control & AICR_PSTAT) {
        const u32 aid_rate = dol_audio_dma_sample_rate(&g_audio_dma);
        const u32 stream_rate =
            (g_ai_stream_control & AICR_AISFR) ? 48000u : 32000u;
        u32 samples;
        if (dtk_active()) {
            samples = dtk_mix(output, 8u, aid_rate ? aid_rate : 32000u,
                              stream_rate,
                              (g_ai_volume & 0xFFu) * (u32)g_settings.music_volume / 100u,
                              ((g_ai_volume >> 8) & 0xFFu) * (u32)g_settings.music_volume / 100u);
        } else {
            g_ai_counter_fraction +=
                (u32)(((u64)stream_rate << 16) * 8u / (aid_rate ? aid_rate : 32000u));
            samples = g_ai_counter_fraction >> 16;
            g_ai_counter_fraction &= 0xFFFFu;
        }
        const u32 old = g_ai_sample_counter;
        g_ai_sample_counter += samples;
        if (samples != 0u &&
            g_ai_interrupt_timing - old <= g_ai_sample_counter - old)
            g_ai_stream_control |= AICR_AIINT;
    }
    mix_host_sound(output, 8u, dol_audio_dma_sample_rate(&g_audio_dma));
    for (u32 i = 0; i < 8u; i++, g_recent_at = (g_recent_at + 1u) % RECENT_FRAMES) {
        g_recent[g_recent_at * 2u] = output[i * 2u];
        g_recent[g_recent_at * 2u + 1u] = output[i * 2u + 1u];
    }
    g_aid_chunks++;
    if (g_log && (g_aid_chunks <= 10u || g_aid_chunks % 400u == 0u))
        fprintf(stderr, "[audio] AID chunk %llu: source=%08x peak=%u\n",
                (unsigned long long)g_aid_chunks, physical, peak);
    send_out(output, 8u, dol_audio_dma_sample_rate(&g_audio_dma));
}

// The game's sound stuck (insanity.cpp, a graphics card failing): the last
// `loop_seconds` that went out, over and over for `seconds`, as a machine
// that has hung buzzes. Sent at once; on the game thread, while it is held.
void audio_play_stuck(double loop_seconds, double seconds) {
    const u32 rate = dol_audio_dma_sample_rate(&g_audio_dma);
    if (rate == 0u || seconds <= 0.0)
        return;
    u32 loop = (u32)(loop_seconds * rate);
    loop = loop < 64u ? 64u : loop > RECENT_FRAMES ? RECENT_FRAMES : loop;
    // In a quiet place the loop is near silence: a buzz of its own under it.
    u32 peak = 0;
    for (u32 f = 0; f < loop; f++)
        for (u32 c = 0; c < 2u; c++) {
            const s16 v = g_recent[((g_recent_at + RECENT_FRAMES - loop + f) % RECENT_FRAMES) * 2u + c];
            const u32 m = v == INT16_MIN ? 32768u : (u32)abs(v);
            peak = m > peak ? m : peak;
        }
    const u32 total = (u32)(seconds * rate);
    s16 chunk[256u * 2u];
    for (u32 done = 0; done < total;) {
        const u32 n = total - done < 256u ? total - done : 256u;
        for (u32 i = 0; i < n; i++) {
            const u32 at = (done + i) % loop;
            const u32 frame = (g_recent_at + RECENT_FRAMES - loop + at) % RECENT_FRAMES;
            for (u32 c = 0; c < 2u; c++) {
                s32 v = g_recent[frame * 2u + c];
                if (peak < 2000u)
                    v += (s32)(at * 2u < loop ? 2600 : -2600) * (s32)(2000u - peak) / 2000;
                chunk[i * 2u + c] = (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        }
        send_out(chunk, n, rate);
        done += n;
    }
}

// The rate sound goes out at (0 before the game has started its own).
u32 audio_out_rate(void) { return dol_audio_dma_sample_rate(&g_audio_dma); }

// Stereo frames of the host's own at audio_out_rate, straight out at the
// effects volume (insanity.cpp: a whisper while the game is held, sent a
// little at a time as the held frames pass).
void audio_send_frames(s16* stereo, u32 frames) {
    const u32 rate = dol_audio_dma_sample_rate(&g_audio_dma);
    if (rate == 0u || frames == 0u)
        return;
    if (g_settings.effects_volume < 100)
        for (u32 i = 0; i < frames * 2u; i++)
            stereo[i] = (s16)((s32)stereo[i] * g_settings.effects_volume / 100);
    send_out(stereo, frames, rate);
}

bool audio_ai_interrupt_pending(void) {
    return (g_ai_stream_control & (AICR_AIINT | AICR_AIINTMSK)) ==
           (AICR_AIINT | AICR_AIINTMSK);
}

bool audio_interrupt_pending(void) {
    return dol_audio_dma_dsp_interrupt_pending(&g_audio_dma);
}

void audio_dsp_mail(CPUState* cpu, u32 mail) {
    // MusyX BABE-prefixed command-list size, then absolute list address.
    if ((mail & 0xFFFF0000u) == 0xBABE0000u) {
        g_waiting_for_command_list = true;
        return;
    }
    if (!g_waiting_for_command_list)
        return;

    g_waiting_for_command_list = false;
    {
        // ED_AX_DUMP=N: dump command list N (and its first PB) once, to
        // compare this game's AX layout with the one ax_host expects.
        static int dump_at = -2;
        static int seen;
        if (dump_at == -2)
            dump_at = getenv("ED_AX_DUMP") ? atoi(getenv("ED_AX_DUMP")) : -1;
        // N=999999 dumps the first list that names a PB (command 2).
        bool has_pb = false;
        for (u32 i = 0; i < 40u && !has_pb; i++)
            has_pb = mem_read16(cpu, mail + i * 2u) == 2u;
        static int pb_lists;
        static int dump_after = -2;
        if (dump_after == -2)
            dump_after = getenv("ED_AX_DUMP_AFTER") ? atoi(getenv("ED_AX_DUMP_AFTER")) : -1;
        if (has_pb && dump_after >= 0 && pb_lists++ == dump_after)
            dump_at = seen;
        if (dump_at == 999999 && has_pb) {
            dump_at = seen;
        }
        if (dump_at >= 0 && seen++ == dump_at) {
            fprintf(stderr, "[ax] command list 0x%08X:", mail);
            for (u32 i = 0; i < 48u; i++)
                fprintf(stderr, " %04X", mem_read16(cpu, mail + i * 2u));
            fputc('\n', stderr);
            for (u32 i = 0; i < 40u; i += 1u) {
                if (mem_read16(cpu, mail + i * 2u) == 2u) {
                    const u32 pb = (mem_read16(cpu, mail + i * 2u + 2u) << 16) |
                                   mem_read16(cpu, mail + i * 2u + 4u);
                    fprintf(stderr, "[ax] first PB 0x%08X:", pb);
                    for (u32 j = 0; j < 0xC0u; j++)
                        fprintf(stderr, "%s%04X", (j % 16u) ? " " : "\n  ",
                                mem_read16(cpu, pb + j * 2u));
                    fputc('\n', stderr);
                    break;
                }
            }
        }
    }
    dol_ax_host_process_command_list(&g_ax_host, cpu, mail);
    mem_write32(cpu, MUSYX_DSP_DONE, 1);
}

void audio_skip_dsp_init(CPUState* cpu) {
    // What dspInitCallback would set once the DSP task booted.
    mem_write32(cpu, MUSYX_DSP_DONE, 1);
    mem_write32(cpu, ED_MUSYX_DSP_INIT_DONE, 1);
}

// ---------------------------------------------------------------------------
// Quick state (host/quickstate.c)
// ---------------------------------------------------------------------------
// The AI/DSP device and the AI stream registers. The AX mixer keeps nothing
// between command lists (voice state lives in the guest's parameter blocks),
// so it is not saved. The DMA pacing (blocks or wall clock) is this process's.
typedef struct {
    DolAudioDma dma;
    u32 ai_stream_control;
    u32 ai_volume;
    u32 ai_sample_counter;
    u32 ai_interrupt_timing;
    u32 ai_counter_fraction;
    bool waiting_for_command_list;
} AudioQuickState;

size_t audio_state_size(void) {
    return sizeof(AudioQuickState);
}

void audio_state_save(void* out) {
    AudioQuickState s;
    memset(&s, 0, sizeof s);
    memcpy(&s.dma, &g_audio_dma, sizeof s.dma);
    s.ai_stream_control = g_ai_stream_control;
    s.ai_volume = g_ai_volume;
    s.ai_sample_counter = g_ai_sample_counter;
    s.ai_interrupt_timing = g_ai_interrupt_timing;
    s.ai_counter_fraction = g_ai_counter_fraction;
    s.waiting_for_command_list = g_waiting_for_command_list;
    memcpy(out, &s, sizeof s);
}

void audio_state_load(const void* in) {
    AudioQuickState s;
    memcpy(&s, in, sizeof s);
    const DolAudioDma live = g_audio_dma;
    g_audio_dma = s.dma;
    g_audio_dma.work_units_per_second = live.work_units_per_second;
    g_audio_dma.work_units_per_frame = live.work_units_per_frame;
    g_audio_dma.refresh_hz = live.refresh_hz;
    // Recomputes the per-chunk work for this process's pacing.
    dol_audio_dma_set_sample_rate(&g_audio_dma, s.dma.sample_rate);
    if (g_audio_dma.work_counter >= g_audio_dma.work_units_per_chunk)
        g_audio_dma.work_counter = 0;
    dol_platform_audio_set_sample_rate(g_audio_dma.sample_rate);
    g_ai_stream_control = s.ai_stream_control;
    g_ai_volume = s.ai_volume;
    g_ai_sample_counter = s.ai_sample_counter;
    g_ai_interrupt_timing = s.ai_interrupt_timing;
    g_ai_counter_fraction = s.ai_counter_fraction;
    g_waiting_for_command_list = s.waiting_for_command_list;
}

/* SPDX-License-Identifier: ISC
 * Lightweight procedural Switch 2 tactile UI SFX engine (non-blocking audout).
 */
#include <math.h>
#include <malloc.h>
#include <stdint.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "switch2_sfx.h"

#define SFX_SAMPLE_RATE 48000
#define SFX_CHANNELS    2
#define SFX_SLOTS       6
#define SFX_MAX_FRAMES  4800 /* 100ms max per sound */
#define SFX_BUF_BYTES   (((SFX_MAX_FRAMES * SFX_CHANNELS * 2) + 0xfff) & ~0xfff)

#ifdef __SWITCH__
static bool s_sfx_ready = false;
static bool s_sfx_enabled = true;

static int16_t *s_samples[S2Sfx_Count];
static uint32_t s_frame_counts[S2Sfx_Count];

static AudioOutBuffer s_slots[SFX_SLOTS];
static uint8_t *s_slot_mem[SFX_SLOTS];
static bool s_slot_busy[SFX_SLOTS];

/* Ultra-short, soft Switch 2-style percussive micro-tick (low volume, warm woody/bubble feel) */
static void synthMicroTick(int16_t *out, uint32_t frames, float f0, float f1,
                           float decay_ms, float amp) {
    const float sr = (float)SFX_SAMPLE_RATE;
    const float att = 0.0008f * sr; /* 0.8ms soft transient */
    const float dec = decay_ms * 0.001f * sr;
    float phase = 0.0f;
    float phase_sub = 0.0f;

    for (uint32_t i = 0; i < frames; i++) {
        float t = (float)i / (float)(frames > 1 ? frames - 1 : 1);
        /* Fast pitch drop/rise curve for tactile "pop/tock" instead of electronic beep */
        float ease = 1.0f - (1.0f - t) * (1.0f - t);
        float freq = f0 + (f1 - f0) * ease;
        phase += 6.2831853f * freq / sr;
        phase_sub += 6.2831853f * (freq * 0.5f) / sr;

        float env = ((float)i < att) ? ((float)i / att) : expf(-((float)i - att) / (dec > 1.0f ? dec : 1.0f));
        if (i + 48 > frames)
            env *= (float)(frames - i) / 48.0f;

        float s = (0.78f * sinf(phase) + 0.22f * sinf(phase_sub)) * env * amp;
        if (s > 0.9f) s = 0.9f;
        if (s < -0.9f) s = -0.9f;
        int16_t v = (int16_t)(s * 32767.0f);
        out[i * 2 + 0] = v;
        out[i * 2 + 1] = v;
    }
}

static void synthSoftDoubleTick(int16_t *out, uint32_t frames, float f_a, float f_b,
                                uint32_t split_frame, float amp) {
    if (split_frame > frames) split_frame = frames / 2;
    synthMicroTick(out, split_frame, f_a, f_a * 1.06f, 5.5f, amp * 0.8f);
    synthMicroTick(out + split_frame * 2, frames - split_frame, f_b, f_b * 1.04f, 8.5f, amp);
}

void switch2SfxInit(void) {
    if (s_sfx_ready)
        return;

    if (R_FAILED(audoutInitialize()))
        return;
    if (R_FAILED(audoutStartAudioOut())) {
        audoutExit();
        return;
    }

    /* Pre-allocate 4KB-aligned playback slots */
    for (int i = 0; i < SFX_SLOTS; i++) {
        s_slot_mem[i] = memalign(0x1000, SFX_BUF_BYTES);
        if (s_slot_mem[i]) {
            memset(s_slot_mem[i], 0, SFX_BUF_BYTES);
            s_slots[i].next = NULL;
            s_slots[i].buffer = s_slot_mem[i];
            s_slots[i].buffer_size = SFX_BUF_BYTES;
            s_slots[i].data_size = 0;
            s_slots[i].data_offset = 0;
        }
        s_slot_busy[i] = false;
    }

    /* Ultra-discreet Switch 2 tactile micro-ticks (short, warm, low volume) */
    s_frame_counts[S2Sfx_Move]    = 768;  /* 16ms soft tactile tick */
    s_frame_counts[S2Sfx_Pill]    = 672;  /* 14ms crisp light tap */
    s_frame_counts[S2Sfx_Confirm] = 1536; /* 32ms gentle double pop */
    s_frame_counts[S2Sfx_Back]    = 960;  /* 20ms soft hollow tock */
    s_frame_counts[S2Sfx_Star]    = 1728; /* 36ms delicate sparkle tap */

    for (int i = 0; i < S2Sfx_Count; i++) {
        s_samples[i] = malloc(s_frame_counts[i] * SFX_CHANNELS * sizeof(int16_t));
    }

    if (s_samples[S2Sfx_Move])
        synthMicroTick(s_samples[S2Sfx_Move], s_frame_counts[S2Sfx_Move], 420.0f, 640.0f, 4.2f, 0.085f);
    if (s_samples[S2Sfx_Pill])
        synthMicroTick(s_samples[S2Sfx_Pill], s_frame_counts[S2Sfx_Pill], 580.0f, 780.0f, 3.8f, 0.075f);
    if (s_samples[S2Sfx_Confirm])
        synthSoftDoubleTick(s_samples[S2Sfx_Confirm], s_frame_counts[S2Sfx_Confirm], 540.0f, 760.0f, 680, 0.095f);
    if (s_samples[S2Sfx_Back])
        synthMicroTick(s_samples[S2Sfx_Back], s_frame_counts[S2Sfx_Back], 460.0f, 290.0f, 5.5f, 0.085f);
    if (s_samples[S2Sfx_Star])
        synthSoftDoubleTick(s_samples[S2Sfx_Star], s_frame_counts[S2Sfx_Star], 740.0f, 1080.0f, 760, 0.090f);

    s_sfx_ready = true;
}

void switch2SfxExit(void) {
    if (!s_sfx_ready)
        return;

    audoutStopAudioOut();
    audoutExit();

    for (int i = 0; i < SFX_SLOTS; i++) {
        free(s_slot_mem[i]);
        s_slot_mem[i] = NULL;
        s_slot_busy[i] = false;
    }
    for (int i = 0; i < S2Sfx_Count; i++) {
        free(s_samples[i]);
        s_samples[i] = NULL;
    }
    s_sfx_ready = false;
}

void switch2SfxSetEnabled(bool enabled) {
    s_sfx_enabled = enabled;
}

static void reclaimReleasedSlots(void) {
    AudioOutBuffer *released = NULL;
    u32 count = 0;
    while (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&released, &count)) && count > 0 && released) {
        for (int i = 0; i < SFX_SLOTS; i++) {
            if (&s_slots[i] == released) {
                s_slot_busy[i] = false;
                break;
            }
        }
    }
}

void switch2SfxPlay(S2SfxId id) {
    if (!s_sfx_ready || !s_sfx_enabled || id < 0 || id >= S2Sfx_Count || !s_samples[id])
        return;

    reclaimReleasedSlots();

    int slot = -1;
    for (int i = 0; i < SFX_SLOTS; i++) {
        if (!s_slot_busy[i] && s_slot_mem[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return;

    uint32_t data_bytes = s_frame_counts[id] * SFX_CHANNELS * sizeof(int16_t);
    if (data_bytes > SFX_BUF_BYTES) data_bytes = SFX_BUF_BYTES;
    memcpy(s_slot_mem[slot], s_samples[id], data_bytes);
    s_slots[slot].data_size = data_bytes;
    s_slots[slot].data_offset = 0;

    if (R_SUCCEEDED(audoutAppendAudioOutBuffer(&s_slots[slot])))
        s_slot_busy[slot] = true;
}
#else
void switch2SfxInit(void) {}
void switch2SfxExit(void) {}
void switch2SfxSetEnabled(bool enabled) { (void)enabled; }
void switch2SfxPlay(S2SfxId id) { (void)id; }
#endif

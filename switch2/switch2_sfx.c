/* SPDX-License-Identifier: ISC
 * Real-time Switch 2 audio mixer:
 * - Procedural tactile Switch 2 UI SFX (built-in default & fallback)
 * - Custom .zip Sound Packs (via minizip) with smart filename matching
 * - Streaming BGM (WAV, MP3 via dr_mp3, OGG via libvorbisfile) from SD or ZIP
 * - Smooth launch fade-out and multi-voice SFX mixing on a dedicated audout thread
 */
#include <ctype.h>
#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#include <minizip/unzip.h>
#include <vorbis/vorbisfile.h>

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_SIMD
#include "third_party/dr_mp3.h"
#endif

#include "switch2_sfx.h"

#define MIX_SAMPLE_RATE  48000
#define MIX_CHANNELS     2
#define MIX_BUF_FRAMES   1024 /* ~21.3 ms per buffer */
#define MIX_BUF_BYTES    (((MIX_BUF_FRAMES * MIX_CHANNELS * 2) + 0xfff) & ~0xfff)
#define MIX_NUM_BUFS     3
#define MAX_SFX_VOICES   4
#define MAX_PACK_SFX_SEC 3
#define MAX_ZIP_BGM_MEM  (16 * 1024 * 1024)

#ifdef __SWITCH__

/* ---------- Case-insensitive string helpers ---------- */

static int strcasecmp_local(const char *a, const char *b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb)
            return ca - cb;
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static bool hasExtCI(const char *name, const char *ext) {
    size_t nl = strlen(name);
    size_t el = strlen(ext);
    if (nl < el)
        return false;
    return strcasecmp_local(name + nl - el, ext) == 0;
}

static const char *baseNameOf(const char *path) {
    const char *s1 = strrchr(path, '/');
    const char *s2 = strrchr(path, '\\');
    const char *s = s1 > s2 ? s1 : s2;
    return s ? s + 1 : path;
}

/* ---------- WAV Header & Decoder ---------- */

typedef struct {
    uint16_t audio_format; /* 1 = PCM, 3 = IEEE float */
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint32_t data_offset;
    uint32_t data_size;
} WavInfo;

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool parseWavFromMem(const uint8_t *buf, size_t len, WavInfo *out) {
    if (!buf || len < 44)
        return false;
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
        return false;
    memset(out, 0, sizeof(*out));
    size_t pos = 12;
    bool got_fmt = false, got_data = false;
    while (pos + 8 <= len) {
        const uint8_t *hdr = buf + pos;
        uint32_t csize = rd32(hdr + 4);
        pos += 8;
        if (pos + csize > len)
            csize = (uint32_t)(len - pos);
        if (memcmp(hdr, "fmt ", 4) == 0 && csize >= 16) {
            out->audio_format = rd16(buf + pos + 0);
            out->channels = rd16(buf + pos + 2);
            out->sample_rate = rd32(buf + pos + 4);
            out->bits_per_sample = rd16(buf + pos + 14);
            got_fmt = true;
        } else if (memcmp(hdr, "data", 4) == 0) {
            out->data_offset = (uint32_t)pos;
            out->data_size = csize;
            got_data = true;
            break;
        }
        pos += csize + (csize & 1);
    }
    if (!got_fmt || !got_data)
        return false;
    if (out->channels < 1 || out->channels > 2 || out->sample_rate < 4000 || out->sample_rate > 192000)
        return false;
    if (out->audio_format == 1) {
        return out->bits_per_sample == 8 || out->bits_per_sample == 16 || out->bits_per_sample == 24;
    }
    if (out->audio_format == 3) {
        return out->bits_per_sample == 32;
    }
    return false;
}

static bool parseWavFromFile(FILE *f, WavInfo *out) {
    uint8_t riff[12];
    if (fread(riff, 1, 12, f) != 12)
        return false;
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0)
        return false;
    memset(out, 0, sizeof(*out));
    bool got_fmt = false, got_data = false;
    uint8_t chdr[8];
    while (fread(chdr, 1, 8, f) == 8) {
        uint32_t csize = rd32(chdr + 4);
        if (memcmp(chdr, "fmt ", 4) == 0 && csize >= 16) {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, f) != 16)
                return false;
            out->audio_format = rd16(fmt + 0);
            out->channels = rd16(fmt + 2);
            out->sample_rate = rd32(fmt + 4);
            out->bits_per_sample = rd16(fmt + 14);
            got_fmt = true;
            if (csize > 16)
                fseek(f, (long)(csize - 16), SEEK_CUR);
        } else if (memcmp(chdr, "data", 4) == 0) {
            long pos = ftell(f);
            if (pos < 0) return false;
            out->data_offset = (uint32_t)pos;
            out->data_size = csize;
            got_data = true;
            break;
        } else {
            fseek(f, (long)csize, SEEK_CUR);
        }
        if (csize & 1)
            fseek(f, 1, SEEK_CUR);
    }
    if (!got_fmt || !got_data)
        return false;
    if (out->channels < 1 || out->channels > 2 || out->sample_rate < 4000 || out->sample_rate > 192000)
        return false;
    if (out->audio_format == 1)
        return out->bits_per_sample == 8 || out->bits_per_sample == 16 || out->bits_per_sample == 24;
    if (out->audio_format == 3)
        return out->bits_per_sample == 32;
    return false;
}

static bool parseWavFromUnz(unzFile uf, WavInfo *out) {
    uint8_t riff[12];
    if (unzReadCurrentFile(uf, riff, 12) != 12)
        return false;
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0)
        return false;
    memset(out, 0, sizeof(*out));
    uint32_t offset = 12;
    bool got_fmt = false, got_data = false;
    uint8_t chdr[8];
    uint8_t skipbuf[512];
    while (unzReadCurrentFile(uf, chdr, 8) == 8) {
        offset += 8;
        uint32_t csize = rd32(chdr + 4);
        if (memcmp(chdr, "fmt ", 4) == 0 && csize >= 16) {
            uint8_t fmt[16];
            if (unzReadCurrentFile(uf, fmt, 16) != 16)
                return false;
            offset += 16;
            out->audio_format = rd16(fmt + 0);
            out->channels = rd16(fmt + 2);
            out->sample_rate = rd32(fmt + 4);
            out->bits_per_sample = rd16(fmt + 14);
            got_fmt = true;
            uint32_t rem = csize - 16;
            while (rem > 0) {
                uint32_t chunk = rem > sizeof(skipbuf) ? (uint32_t)sizeof(skipbuf) : rem;
                if (unzReadCurrentFile(uf, skipbuf, chunk) != (int)chunk)
                    return false;
                rem -= chunk;
                offset += chunk;
            }
        } else if (memcmp(chdr, "data", 4) == 0) {
            out->data_offset = offset;
            out->data_size = csize;
            got_data = true;
            break;
        } else {
            uint32_t rem = csize;
            while (rem > 0) {
                uint32_t chunk = rem > sizeof(skipbuf) ? (uint32_t)sizeof(skipbuf) : rem;
                if (unzReadCurrentFile(uf, skipbuf, chunk) != (int)chunk)
                    return false;
                rem -= chunk;
                offset += chunk;
            }
        }
        if (csize & 1) {
            uint8_t pad;
            if (unzReadCurrentFile(uf, &pad, 1) != 1)
                return false;
            offset += 1;
        }
    }
    if (!got_fmt || !got_data)
        return false;
    if (out->channels < 1 || out->channels > 2 || out->sample_rate < 4000 || out->sample_rate > 192000)
        return false;
    if (out->audio_format == 1)
        return out->bits_per_sample == 8 || out->bits_per_sample == 16 || out->bits_per_sample == 24;
    if (out->audio_format == 3)
        return out->bits_per_sample == 32;
    return false;
}

static inline int16_t decodeSample(const uint8_t *p, uint16_t fmt, uint16_t bps) {
    if (fmt == 1) {
        if (bps == 16)
            return (int16_t)rd16(p);
        if (bps == 8)
            return (int16_t)(((int)p[0] - 128) << 8);
        if (bps == 24) {
            int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)(int8_t)p[2] << 16);
            return (int16_t)(v >> 8);
        }
    } else if (fmt == 3 && bps == 32) {
        float f;
        memcpy(&f, p, 4);
        if (f > 1.0f) f = 1.0f;
        if (f < -1.0f) f = -1.0f;
        return (int16_t)(f * 32767.0f);
    }
    return 0;
}

static void decodeWavBytesToStereoS16(const uint8_t *src, size_t num_frames,
                                      const WavInfo *info, int16_t *dst) {
    const uint32_t sb = info->bits_per_sample / 8;
    const uint32_t fb = sb * info->channels;
    for (size_t i = 0; i < num_frames; i++) {
        const uint8_t *fp = src + i * fb;
        int16_t l = decodeSample(fp, info->audio_format, info->bits_per_sample);
        int16_t r = (info->channels == 2) ? decodeSample(fp + sb, info->audio_format, info->bits_per_sample) : l;
        dst[i * 2 + 0] = l;
        dst[i * 2 + 1] = r;
    }
}

/* Decode an entire WAV in memory into a 48kHz stereo S16 buffer (capped at max_out_frames) */
static int16_t *decodeWavMemTo48k(const uint8_t *buf, size_t len, uint32_t max_out_frames, uint32_t *out_frames) {
    WavInfo info;
    if (!parseWavFromMem(buf, len, &info))
        return NULL;
    uint32_t frame_bytes = (info.bits_per_sample / 8) * info.channels;
    if (frame_bytes == 0)
        return NULL;
    uint32_t src_frames = info.data_size / frame_bytes;
    if (src_frames == 0)
        return NULL;

    double ratio = (double)MIX_SAMPLE_RATE / (double)info.sample_rate;
    uint32_t dst_frames = (uint32_t)((double)src_frames * ratio);
    if (dst_frames == 0)
        return NULL;
    if (dst_frames > max_out_frames)
        dst_frames = max_out_frames;

    int16_t *out = malloc(dst_frames * MIX_CHANNELS * sizeof(int16_t));
    if (!out)
        return NULL;

    const uint8_t *pcm = buf + info.data_offset;
    const uint32_t sb = info.bits_per_sample / 8;
    for (uint32_t i = 0; i < dst_frames; i++) {
        double src_pos = (double)i / ratio;
        uint32_t idx0 = (uint32_t)src_pos;
        if (idx0 >= src_frames) idx0 = src_frames - 1;
        uint32_t idx1 = (idx0 + 1 < src_frames) ? idx0 + 1 : idx0;
        float frac = (float)(src_pos - (double)idx0);

        const uint8_t *p0 = pcm + (size_t)idx0 * frame_bytes;
        const uint8_t *p1 = pcm + (size_t)idx1 * frame_bytes;
        float l0 = (float)decodeSample(p0, info.audio_format, info.bits_per_sample);
        float r0 = (info.channels == 2) ? (float)decodeSample(p0 + sb, info.audio_format, info.bits_per_sample) : l0;
        float l1 = (float)decodeSample(p1, info.audio_format, info.bits_per_sample);
        float r1 = (info.channels == 2) ? (float)decodeSample(p1 + sb, info.audio_format, info.bits_per_sample) : l1;

        out[i * 2 + 0] = (int16_t)(l0 + (l1 - l0) * frac);
        out[i * 2 + 1] = (int16_t)(r0 + (r1 - r0) * frac);
    }
    *out_frames = dst_frames;
    return out;
}

/* ---------- OGG Vorbis Memory Callbacks ---------- */

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} OggMemStream;

static size_t oggMemRead(void *ptr, size_t size, size_t nmemb, void *datasource) {
    OggMemStream *ms = (OggMemStream *)datasource;
    size_t want = size * nmemb;
    size_t avail = ms->size - ms->pos;
    if (want > avail) want = avail;
    if (size > 0) want = (want / size) * size;
    if (want > 0) {
        memcpy(ptr, ms->data + ms->pos, want);
        ms->pos += want;
    }
    return size > 0 ? (want / size) : 0;
}

static int oggMemSeek(void *datasource, ogg_int64_t offset, int whence) {
    OggMemStream *ms = (OggMemStream *)datasource;
    ogg_int64_t npos = 0;
    if (whence == SEEK_SET) npos = offset;
    else if (whence == SEEK_CUR) npos = (ogg_int64_t)ms->pos + offset;
    else if (whence == SEEK_END) npos = (ogg_int64_t)ms->size + offset;
    else return -1;
    if (npos < 0 || (size_t)npos > ms->size) return -1;
    ms->pos = (size_t)npos;
    return 0;
}

static long oggMemTell(void *datasource) {
    OggMemStream *ms = (OggMemStream *)datasource;
    return (long)ms->pos;
}

static const ov_callbacks s_ogg_mem_cb = {
    .read_func = oggMemRead,
    .seek_func = oggMemSeek,
    .close_func = NULL,
    .tell_func = oggMemTell,
};

/* ---------- Engine State ---------- */

typedef enum {
    BgmType_None = 0,
    BgmType_WavFile,
    BgmType_WavZip,
    BgmType_Mp3File,
    BgmType_Mp3Mem,
    BgmType_OggFile,
    BgmType_OggMem,
} BgmType;

typedef struct {
    const int16_t *samples;
    uint32_t total_frames;
    uint32_t pos;
    bool active;
} SfxVoice;

static bool s_audio_ready = false;
static volatile bool s_thread_run = false;
static Thread s_audio_thread;
static Mutex s_audio_lock;

static AudioOutBuffer s_out_bufs[MIX_NUM_BUFS];
static int16_t *s_out_mem[MIX_NUM_BUFS];

/* Built-in procedural SFX & optional custom ZIP pack overrides */
static int16_t *s_builtin_sfx[S2Sfx_Count];
static uint32_t s_builtin_frames[S2Sfx_Count];

static int16_t *s_pack_sfx[S2Sfx_Count];
static uint32_t s_pack_frames[S2Sfx_Count];
static bool s_pack_has_bgm = false;
static char s_pack_bgm_entry[256];

static SfxVoice s_voices[MAX_SFX_VOICES];
static bool s_sfx_enabled = true;
static float s_sfx_gain = 1.0f;
static float s_bgm_gain = 0.5f;

/* Fade-out state (for smooth launch transition) */
static float s_fade_gain = 1.0f;
static float s_fade_step = 0.0f;

/* Active BGM stream */
static BgmType s_bgm_type = BgmType_None;
static uint32_t s_bgm_src_rate = MIX_SAMPLE_RATE;
static FILE *s_bgm_file = NULL;
static WavInfo s_bgm_wav;
static uint32_t s_bgm_wav_rem = 0;

static unzFile s_bgm_zip = NULL;
static char s_bgm_zip_path[512];
static char s_bgm_zip_entry[256];

static drmp3 s_bgm_mp3;
static OggVorbis_File s_bgm_ogg;
static uint8_t *s_bgm_mem = NULL;
static OggMemStream s_bgm_ogg_mem;

/* Resampler buffer for BGM */
#define BGM_SRC_CHUNK 1024
static int16_t s_bgm_src_buf[BGM_SRC_CHUNK * 2];
static uint32_t s_bgm_src_count = 0;
static double s_bgm_src_pos = 0.0;
static int16_t s_bgm_last_l = 0;
static int16_t s_bgm_last_r = 0;

/* ---------- Built-in Procedural Switch 2 Tactile SFX ---------- */

static void synthMicroTick(int16_t *out, uint32_t frames, float f0, float f1,
                           float decay_ms, float amp) {
    const float sr = (float)MIX_SAMPLE_RATE;
    const float att = 0.0008f * sr;
    const float dec = decay_ms * 0.001f * sr;
    float phase = 0.0f;
    float phase_sub = 0.0f;

    for (uint32_t i = 0; i < frames; i++) {
        float t = (float)i / (float)(frames > 1 ? frames - 1 : 1);
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

static void synthLaunchChime(int16_t *out, uint32_t frames) {
    /* Soft 3-step rising Switch 2 launch snap */
    uint32_t s1 = frames / 3;
    uint32_t s2 = frames / 3;
    uint32_t s3 = frames - s1 - s2;
    synthMicroTick(out, s1, 520.0f, 660.0f, 6.0f, 0.085f);
    synthMicroTick(out + s1 * 2, s2, 680.0f, 880.0f, 7.0f, 0.095f);
    synthMicroTick(out + (s1 + s2) * 2, s3, 920.0f, 1180.0f, 16.0f, 0.105f);
}

/* ---------- BGM Stream Lifecycle (caller holds s_audio_lock) ---------- */

static void bgmCloseLocked(void) {
    switch (s_bgm_type) {
    case BgmType_WavFile:
        if (s_bgm_file) { fclose(s_bgm_file); s_bgm_file = NULL; }
        break;
    case BgmType_WavZip:
        if (s_bgm_zip) {
            unzCloseCurrentFile(s_bgm_zip);
            unzClose(s_bgm_zip);
            s_bgm_zip = NULL;
        }
        break;
    case BgmType_Mp3File:
    case BgmType_Mp3Mem:
        drmp3_uninit(&s_bgm_mp3);
        break;
    case BgmType_OggFile:
    case BgmType_OggMem:
        ov_clear(&s_bgm_ogg);
        break;
    default:
        break;
    }
    if (s_bgm_mem) {
        free(s_bgm_mem);
        s_bgm_mem = NULL;
    }
    s_bgm_type = BgmType_None;
    s_bgm_src_count = 0;
    s_bgm_src_pos = 0.0;
    s_bgm_last_l = 0;
    s_bgm_last_r = 0;
}

static uint32_t bgmReadRawFramesLocked(int16_t *dst, uint32_t max_frames) {
    if (s_bgm_type == BgmType_None || max_frames == 0)
        return 0;

    if (s_bgm_type == BgmType_WavFile) {
        uint32_t fb = (s_bgm_wav.bits_per_sample / 8) * s_bgm_wav.channels;
        if (fb == 0 || !s_bgm_file) return 0;
        uint8_t raw[BGM_SRC_CHUNK * 8];
        uint32_t want_bytes = max_frames * fb;
        if (want_bytes > sizeof(raw)) want_bytes = (sizeof(raw) / fb) * fb;
        if (s_bgm_wav_rem == 0) {
            fseek(s_bgm_file, (long)s_bgm_wav.data_offset, SEEK_SET);
            s_bgm_wav_rem = s_bgm_wav.data_size;
        }
        if (want_bytes > s_bgm_wav_rem)
            want_bytes = (s_bgm_wav_rem / fb) * fb;
        if (want_bytes == 0) {
            fseek(s_bgm_file, (long)s_bgm_wav.data_offset, SEEK_SET);
            s_bgm_wav_rem = s_bgm_wav.data_size;
            return 0;
        }
        size_t got = fread(raw, 1, want_bytes, s_bgm_file);
        uint32_t frames = (uint32_t)(got / fb);
        s_bgm_wav_rem = (got <= s_bgm_wav_rem) ? (s_bgm_wav_rem - (uint32_t)got) : 0;
        if (frames == 0) {
            fseek(s_bgm_file, (long)s_bgm_wav.data_offset, SEEK_SET);
            s_bgm_wav_rem = s_bgm_wav.data_size;
            return 0;
        }
        decodeWavBytesToStereoS16(raw, frames, &s_bgm_wav, dst);
        return frames;
    }

    if (s_bgm_type == BgmType_WavZip) {
        uint32_t fb = (s_bgm_wav.bits_per_sample / 8) * s_bgm_wav.channels;
        if (fb == 0 || !s_bgm_zip) return 0;
        uint8_t raw[BGM_SRC_CHUNK * 8];
        if (s_bgm_wav_rem < fb) {
            /* Loop by reopening entry */
            unzCloseCurrentFile(s_bgm_zip);
            if (unzLocateFile(s_bgm_zip, s_bgm_zip_entry, 2) == UNZ_OK &&
                unzOpenCurrentFile(s_bgm_zip) == UNZ_OK &&
                parseWavFromUnz(s_bgm_zip, &s_bgm_wav)) {
                s_bgm_wav_rem = s_bgm_wav.data_size;
            } else {
                bgmCloseLocked();
                return 0;
            }
        }
        uint32_t want_bytes = max_frames * fb;
        if (want_bytes > sizeof(raw)) want_bytes = (sizeof(raw) / fb) * fb;
        if (want_bytes > s_bgm_wav_rem) want_bytes = (s_bgm_wav_rem / fb) * fb;
        int got = unzReadCurrentFile(s_bgm_zip, raw, want_bytes);
        if (got <= 0) {
            s_bgm_wav_rem = 0;
            return 0;
        }
        uint32_t frames = (uint32_t)got / fb;
        s_bgm_wav_rem = ((uint32_t)got <= s_bgm_wav_rem) ? (s_bgm_wav_rem - (uint32_t)got) : 0;
        decodeWavBytesToStereoS16(raw, frames, &s_bgm_wav, dst);
        return frames;
    }

    if (s_bgm_type == BgmType_Mp3File || s_bgm_type == BgmType_Mp3Mem) {
        int16_t tmp[BGM_SRC_CHUNK * 2];
        uint32_t want = max_frames > BGM_SRC_CHUNK ? BGM_SRC_CHUNK : max_frames;
        drmp3_uint64 got = drmp3_read_pcm_frames_s16(&s_bgm_mp3, want, tmp);
        if (got == 0) {
            drmp3_seek_to_pcm_frame(&s_bgm_mp3, 0);
            got = drmp3_read_pcm_frames_s16(&s_bgm_mp3, want, tmp);
            if (got == 0) return 0;
        }
        if (s_bgm_mp3.channels == 1) {
            for (drmp3_uint64 i = 0; i < got; i++) {
                dst[i * 2 + 0] = tmp[i];
                dst[i * 2 + 1] = tmp[i];
            }
        } else {
            memcpy(dst, tmp, (size_t)got * 2 * sizeof(int16_t));
        }
        return (uint32_t)got;
    }

    if (s_bgm_type == BgmType_OggFile || s_bgm_type == BgmType_OggMem) {
        vorbis_info *vi = ov_info(&s_bgm_ogg, -1);
        if (!vi || vi->channels < 1 || vi->channels > 2) return 0;
        int16_t tmp[BGM_SRC_CHUNK * 2];
        int bytes_per_frame = vi->channels * 2;
        int want_bytes = (int)(max_frames > BGM_SRC_CHUNK ? BGM_SRC_CHUNK : max_frames) * bytes_per_frame;
        int bitstream = 0;
        long got = ov_read(&s_bgm_ogg, (char *)tmp, want_bytes, 0, 2, 1, &bitstream);
        if (got <= 0) {
            ov_pcm_seek(&s_bgm_ogg, 0);
            got = ov_read(&s_bgm_ogg, (char *)tmp, want_bytes, 0, 2, 1, &bitstream);
            if (got <= 0) return 0;
        }
        uint32_t frames = (uint32_t)(got / bytes_per_frame);
        if (vi->channels == 1) {
            for (uint32_t i = 0; i < frames; i++) {
                dst[i * 2 + 0] = tmp[i];
                dst[i * 2 + 1] = tmp[i];
            }
        } else {
            memcpy(dst, tmp, frames * 2 * sizeof(int16_t));
        }
        return frames;
    }

    return 0;
}

static void bgmMixIntoLocked(int32_t *mix, uint32_t out_frames) {
    if (s_bgm_type == BgmType_None || (s_bgm_gain <= 0.001f && s_fade_step == 0.0f))
        return;

    const double step = (double)s_bgm_src_rate / (double)MIX_SAMPLE_RATE;

    for (uint32_t i = 0; i < out_frames; i++) {
        if (s_fade_step > 0.0f) {
            s_fade_gain -= s_fade_step;
            if (s_fade_gain < 0.0f) {
                s_fade_gain = 0.0f;
                s_fade_step = 0.0f;
            }
        }
        float gain = s_bgm_gain * s_fade_gain;

        while (s_bgm_src_pos >= (double)s_bgm_src_count) {
            if (s_bgm_src_count > 0) {
                s_bgm_last_l = s_bgm_src_buf[(s_bgm_src_count - 1) * 2 + 0];
                s_bgm_last_r = s_bgm_src_buf[(s_bgm_src_count - 1) * 2 + 1];
                s_bgm_src_pos -= (double)s_bgm_src_count;
            } else {
                s_bgm_src_pos = 0.0;
            }
            s_bgm_src_count = bgmReadRawFramesLocked(s_bgm_src_buf, BGM_SRC_CHUNK);
            if (s_bgm_src_count == 0)
                return;
        }

        uint32_t idx = (uint32_t)s_bgm_src_pos;
        float frac = (float)(s_bgm_src_pos - (double)idx);
        int16_t l0 = s_bgm_src_buf[idx * 2 + 0];
        int16_t r0 = s_bgm_src_buf[idx * 2 + 1];
        int16_t l1 = (idx + 1 < s_bgm_src_count) ? s_bgm_src_buf[(idx + 1) * 2 + 0] : l0;
        int16_t r1 = (idx + 1 < s_bgm_src_count) ? s_bgm_src_buf[(idx + 1) * 2 + 1] : r0;

        float l = (float)l0 + ((float)l1 - (float)l0) * frac;
        float r = (float)r0 + ((float)r1 - (float)r0) * frac;
        mix[i * 2 + 0] += (int32_t)(l * gain);
        mix[i * 2 + 1] += (int32_t)(r * gain);

        s_bgm_src_pos += step;
    }
}

/* ---------- Mixer Thread ---------- */

static void fillAudioBuffer(int16_t *out, uint32_t frames) {
    int32_t mix[MIX_BUF_FRAMES * 2];
    memset(mix, 0, frames * 2 * sizeof(int32_t));

    mutexLock(&s_audio_lock);

    /* Mix BGM */
    bgmMixIntoLocked(mix, frames);

    /* Mix active SFX voices */
    if (s_sfx_enabled && s_sfx_gain > 0.001f) {
        for (int v = 0; v < MAX_SFX_VOICES; v++) {
            if (!s_voices[v].active || !s_voices[v].samples)
                continue;
            uint32_t rem = s_voices[v].total_frames - s_voices[v].pos;
            uint32_t n = rem < frames ? rem : frames;
            const int16_t *src = s_voices[v].samples + s_voices[v].pos * 2;
            for (uint32_t i = 0; i < n; i++) {
                mix[i * 2 + 0] += (int32_t)((float)src[i * 2 + 0] * s_sfx_gain);
                mix[i * 2 + 1] += (int32_t)((float)src[i * 2 + 1] * s_sfx_gain);
            }
            s_voices[v].pos += n;
            if (s_voices[v].pos >= s_voices[v].total_frames)
                s_voices[v].active = false;
        }
    }

    mutexUnlock(&s_audio_lock);

    for (uint32_t i = 0; i < frames * 2; i++) {
        int32_t s = mix[i];
        if (s > 32767) s = 32767;
        else if (s < -32768) s = -32768;
        out[i] = (int16_t)s;
    }
}

static void audioThreadMain(void *arg) {
    (void)arg;
    /* Prime all output buffers */
    for (int i = 0; i < MIX_NUM_BUFS; i++) {
        fillAudioBuffer(s_out_mem[i], MIX_BUF_FRAMES);
        s_out_bufs[i].data_size = MIX_BUF_FRAMES * MIX_CHANNELS * sizeof(int16_t);
        s_out_bufs[i].data_offset = 0;
        audoutAppendAudioOutBuffer(&s_out_bufs[i]);
    }

    while (s_thread_run) {
        AudioOutBuffer *released = NULL;
        u32 count = 0;
        Result rc = audoutWaitPlayFinish(&released, &count, 100000000ULL);
        if (!s_thread_run)
            break;
        if (R_SUCCEEDED(rc) && released && count > 0) {
            AudioOutBuffer *cur = released;
            for (u32 i = 0; i < count && cur; i++) {
                AudioOutBuffer *next = cur->next;
                cur->next = NULL;
                fillAudioBuffer((int16_t *)cur->buffer, MIX_BUF_FRAMES);
                cur->data_size = MIX_BUF_FRAMES * MIX_CHANNELS * sizeof(int16_t);
                cur->data_offset = 0;
                audoutAppendAudioOutBuffer(cur);
                cur = next;
            }
        }
    }
}

/* ---------- Public API ---------- */

void switch2SfxInit(void) {
    if (s_audio_ready)
        return;

    mutexInit(&s_audio_lock);

    if (R_FAILED(audoutInitialize()))
        return;
    if (R_FAILED(audoutStartAudioOut())) {
        audoutExit();
        return;
    }

    for (int i = 0; i < MIX_NUM_BUFS; i++) {
        s_out_mem[i] = memalign(0x1000, MIX_BUF_BYTES);
        if (!s_out_mem[i]) {
            audoutStopAudioOut();
            audoutExit();
            return;
        }
        memset(s_out_mem[i], 0, MIX_BUF_BYTES);
        s_out_bufs[i].next = NULL;
        s_out_bufs[i].buffer = s_out_mem[i];
        s_out_bufs[i].buffer_size = MIX_BUF_BYTES;
        s_out_bufs[i].data_size = 0;
        s_out_bufs[i].data_offset = 0;
    }

    /* Built-in tactile Switch 2 micro-ticks */
    s_builtin_frames[S2Sfx_Move]    = 768;  /* 16ms */
    s_builtin_frames[S2Sfx_Pill]    = 672;  /* 14ms */
    s_builtin_frames[S2Sfx_Confirm] = 1536; /* 32ms */
    s_builtin_frames[S2Sfx_Back]    = 960;  /* 20ms */
    s_builtin_frames[S2Sfx_Star]    = 1728; /* 36ms */
    s_builtin_frames[S2Sfx_Launch]  = 2880; /* 60ms */

    for (int i = 0; i < S2Sfx_Count; i++) {
        s_builtin_sfx[i] = malloc(s_builtin_frames[i] * MIX_CHANNELS * sizeof(int16_t));
        s_pack_sfx[i] = NULL;
        s_pack_frames[i] = 0;
    }

    if (s_builtin_sfx[S2Sfx_Move])
        synthMicroTick(s_builtin_sfx[S2Sfx_Move], s_builtin_frames[S2Sfx_Move], 420.0f, 640.0f, 4.2f, 0.085f);
    if (s_builtin_sfx[S2Sfx_Pill])
        synthMicroTick(s_builtin_sfx[S2Sfx_Pill], s_builtin_frames[S2Sfx_Pill], 580.0f, 780.0f, 3.8f, 0.075f);
    if (s_builtin_sfx[S2Sfx_Confirm])
        synthSoftDoubleTick(s_builtin_sfx[S2Sfx_Confirm], s_builtin_frames[S2Sfx_Confirm], 540.0f, 760.0f, 680, 0.095f);
    if (s_builtin_sfx[S2Sfx_Back])
        synthMicroTick(s_builtin_sfx[S2Sfx_Back], s_builtin_frames[S2Sfx_Back], 460.0f, 290.0f, 5.5f, 0.085f);
    if (s_builtin_sfx[S2Sfx_Star])
        synthSoftDoubleTick(s_builtin_sfx[S2Sfx_Star], s_builtin_frames[S2Sfx_Star], 740.0f, 1080.0f, 760, 0.090f);
    if (s_builtin_sfx[S2Sfx_Launch])
        synthLaunchChime(s_builtin_sfx[S2Sfx_Launch], s_builtin_frames[S2Sfx_Launch]);

    memset(s_voices, 0, sizeof(s_voices));
    s_pack_has_bgm = false;
    s_pack_bgm_entry[0] = '\0';

    s_thread_run = true;
    if (R_SUCCEEDED(threadCreate(&s_audio_thread, audioThreadMain, NULL, NULL, 0x20000, 0x2C, -2))) {
        threadStart(&s_audio_thread);
        s_audio_ready = true;
    } else {
        s_thread_run = false;
        audoutStopAudioOut();
        audoutExit();
    }
}

void switch2SfxExit(void) {
    if (!s_audio_ready)
        return;

    s_thread_run = false;
    threadWaitForExit(&s_audio_thread);
    threadClose(&s_audio_thread);

    mutexLock(&s_audio_lock);
    bgmCloseLocked();
    for (int i = 0; i < MAX_SFX_VOICES; i++)
        s_voices[i].active = false;
    for (int i = 0; i < S2Sfx_Count; i++) {
        free(s_pack_sfx[i]);
        s_pack_sfx[i] = NULL;
        s_pack_frames[i] = 0;
        free(s_builtin_sfx[i]);
        s_builtin_sfx[i] = NULL;
        s_builtin_frames[i] = 0;
    }
    mutexUnlock(&s_audio_lock);

    audoutStopAudioOut();
    audoutExit();

    for (int i = 0; i < MIX_NUM_BUFS; i++) {
        free(s_out_mem[i]);
        s_out_mem[i] = NULL;
    }
    s_audio_ready = false;
}

void switch2SfxSetEnabled(bool enabled) {
    mutexLock(&s_audio_lock);
    s_sfx_enabled = enabled;
    mutexUnlock(&s_audio_lock);
}

void switch2SfxSetVolume(int volume_pct) {
    if (volume_pct < 0) volume_pct = 0;
    if (volume_pct > 100) volume_pct = 100;
    mutexLock(&s_audio_lock);
    s_sfx_gain = (float)volume_pct / 100.0f;
    mutexUnlock(&s_audio_lock);
}

void switch2SfxPlay(S2SfxId id) {
    if (!s_audio_ready || id < 0 || id >= S2Sfx_Count)
        return;

    mutexLock(&s_audio_lock);
    if (!s_sfx_enabled || s_sfx_gain <= 0.001f) {
        mutexUnlock(&s_audio_lock);
        return;
    }

    const int16_t *samples = s_pack_sfx[id] ? s_pack_sfx[id] : s_builtin_sfx[id];
    uint32_t frames = s_pack_sfx[id] ? s_pack_frames[id] : s_builtin_frames[id];
    if (!samples || frames == 0) {
        mutexUnlock(&s_audio_lock);
        return;
    }

    /* Pick a free voice, or steal the oldest voice (fixes FIFO backlog!) */
    int slot = -1;
    uint32_t max_pos = 0;
    int oldest = 0;
    for (int i = 0; i < MAX_SFX_VOICES; i++) {
        if (!s_voices[i].active) {
            slot = i;
            break;
        }
        if (s_voices[i].pos >= max_pos) {
            max_pos = s_voices[i].pos;
            oldest = i;
        }
    }
    if (slot < 0)
        slot = oldest;

    s_voices[slot].samples = samples;
    s_voices[slot].total_frames = frames;
    s_voices[slot].pos = 0;
    s_voices[slot].active = true;

    mutexUnlock(&s_audio_lock);
}

/* ---------- ZIP Sound Pack Loader ---------- */

static uint8_t *readZipEntryToMem(unzFile uf, const char *entry_name, size_t max_bytes, size_t *out_size) {
    if (unzLocateFile(uf, entry_name, 2) != UNZ_OK)
        return NULL;
    unz_file_info fi;
    if (unzGetCurrentFileInfo(uf, &fi, NULL, 0, NULL, 0, NULL, 0) != UNZ_OK)
        return NULL;
    if (fi.uncompressed_size == 0 || fi.uncompressed_size > max_bytes)
        return NULL;
    if (unzOpenCurrentFile(uf) != UNZ_OK)
        return NULL;
    uint8_t *buf = malloc(fi.uncompressed_size);
    if (!buf) {
        unzCloseCurrentFile(uf);
        return NULL;
    }
    int got = unzReadCurrentFile(uf, buf, (unsigned)fi.uncompressed_size);
    unzCloseCurrentFile(uf);
    if (got <= 0) {
        free(buf);
        return NULL;
    }
    *out_size = (size_t)got;
    return buf;
}

static bool findZipEntryByNames(unzFile uf, const char *const *names, int count, char *out_entry, size_t out_sz) {
    if (unzGoToFirstFile(uf) != UNZ_OK)
        return false;
    int best_prio = count + 1;
    char best_name[256] = {0};

    do {
        char fname[256];
        unz_file_info fi;
        if (unzGetCurrentFileInfo(uf, &fi, fname, sizeof(fname) - 1, NULL, 0, NULL, 0) != UNZ_OK)
            continue;
        fname[sizeof(fname) - 1] = '\0';
        const char *base = baseNameOf(fname);
        if (!base || base[0] == '\0' || base[0] == '.')
            continue;
        for (int i = 0; i < count && i < best_prio; i++) {
            if (strcasecmp_local(base, names[i]) == 0) {
                best_prio = i;
                snprintf(best_name, sizeof(best_name), "%.255s", fname);
                break;
            }
        }
    } while (unzGoToNextFile(uf) == UNZ_OK);

    if (best_prio <= count && best_name[0]) {
        strncpy(out_entry, best_name, out_sz - 1);
        out_entry[out_sz - 1] = '\0';
        return true;
    }
    return false;
}

bool switch2SfxLoadPack(const char *zip_path) {
    if (!s_audio_ready)
        return false;

    int16_t *new_sfx[S2Sfx_Count] = {0};
    uint32_t new_frames[S2Sfx_Count] = {0};
    bool has_bgm = false;
    char bgm_entry[256] = {0};

    if (zip_path && zip_path[0]) {
        unzFile uf = unzOpen(zip_path);
        if (!uf)
            return false;

        static const char *const kMoveNames[]    = { "move.wav", "cursor_move.wav", "cursor.wav", "tick.wav" };
        static const char *const kPillNames[]    = { "pill.wav", "dock_enter.wav", "options.wav", "cursor_move.wav", "move.wav" };
        static const char *const kConfirmNames[] = { "confirm.wav", "enter_menu.wav", "ok.wav", "select.wav" };
        static const char *const kBackNames[]    = { "back.wav", "cancel.wav", "back_menu.wav" };
        static const char *const kStarNames[]    = { "star.wav", "notification.wav", "favorite.wav", "confirm.wav" };
        static const char *const kLaunchNames[]  = { "launch.wav", "startup.wav", "confirm.wav" };
        static const char *const kBgmNames[]     = { "bgm.wav", "bgm.mp3", "bgm.ogg" };

        const char *const *lists[S2Sfx_Count] = {
            kMoveNames, kPillNames, kConfirmNames, kBackNames, kStarNames, kLaunchNames
        };
        const int counts[S2Sfx_Count] = { 4, 5, 4, 3, 4, 3 };

        for (int i = 0; i < S2Sfx_Count; i++) {
            char entry[256];
            if (findZipEntryByNames(uf, lists[i], counts[i], entry, sizeof(entry))) {
                size_t sz = 0;
                uint8_t *raw = readZipEntryToMem(uf, entry, 4 * 1024 * 1024, &sz);
                if (raw) {
                    uint32_t nf = 0;
                    new_sfx[i] = decodeWavMemTo48k(raw, sz, MIX_SAMPLE_RATE * MAX_PACK_SFX_SEC, &nf);
                    new_frames[i] = nf;
                    free(raw);
                }
            }
        }

        if (findZipEntryByNames(uf, kBgmNames, 3, bgm_entry, sizeof(bgm_entry)))
            has_bgm = true;

        unzClose(uf);
    }

    mutexLock(&s_audio_lock);
    /* Stop voices referencing old pack samples before freeing */
    for (int i = 0; i < MAX_SFX_VOICES; i++)
        s_voices[i].active = false;
    for (int i = 0; i < S2Sfx_Count; i++) {
        free(s_pack_sfx[i]);
        s_pack_sfx[i] = new_sfx[i];
        s_pack_frames[i] = new_frames[i];
    }
    s_pack_has_bgm = has_bgm;
    strncpy(s_pack_bgm_entry, bgm_entry, sizeof(s_pack_bgm_entry) - 1);
    s_pack_bgm_entry[sizeof(s_pack_bgm_entry) - 1] = '\0';
    mutexUnlock(&s_audio_lock);
    return true;
}

bool switch2SfxPackHasBgm(void) {
    mutexLock(&s_audio_lock);
    bool r = s_pack_has_bgm;
    mutexUnlock(&s_audio_lock);
    return r;
}

/* ---------- BGM Public API ---------- */

void switch2BgmSetVolume(int volume_pct) {
    if (volume_pct < 0) volume_pct = 0;
    if (volume_pct > 100) volume_pct = 100;
    mutexLock(&s_audio_lock);
    s_bgm_gain = (float)volume_pct / 100.0f;
    mutexUnlock(&s_audio_lock);
}

void switch2BgmStop(void) {
    if (!s_audio_ready) return;
    mutexLock(&s_audio_lock);
    bgmCloseLocked();
    mutexUnlock(&s_audio_lock);
}

void switch2BgmStartFadeOut(float duration_sec) {
    if (!s_audio_ready) return;
    if (duration_sec < 0.02f) duration_sec = 0.02f;
    mutexLock(&s_audio_lock);
    s_fade_step = s_fade_gain / (duration_sec * (float)MIX_SAMPLE_RATE);
    mutexUnlock(&s_audio_lock);
}

void switch2BgmResetFade(void) {
    if (!s_audio_ready) return;
    mutexLock(&s_audio_lock);
    s_fade_gain = 1.0f;
    s_fade_step = 0.0f;
    mutexUnlock(&s_audio_lock);
}

bool switch2BgmPlayFile(const char *path) {
    if (!s_audio_ready || !path || !path[0])
        return false;

    mutexLock(&s_audio_lock);
    bgmCloseLocked();
    s_fade_gain = 1.0f;
    s_fade_step = 0.0f;

    bool ok = false;
    if (hasExtCI(path, ".wav")) {
        FILE *f = fopen(path, "rb");
        if (f) {
            if (parseWavFromFile(f, &s_bgm_wav)) {
                s_bgm_file = f;
                s_bgm_wav_rem = s_bgm_wav.data_size;
                s_bgm_src_rate = s_bgm_wav.sample_rate;
                s_bgm_type = BgmType_WavFile;
                ok = true;
            } else {
                fclose(f);
            }
        }
    } else if (hasExtCI(path, ".mp3")) {
        if (drmp3_init_file(&s_bgm_mp3, path, NULL)) {
            s_bgm_src_rate = s_bgm_mp3.sampleRate ? s_bgm_mp3.sampleRate : MIX_SAMPLE_RATE;
            s_bgm_type = BgmType_Mp3File;
            ok = true;
        }
    } else if (hasExtCI(path, ".ogg")) {
        if (ov_fopen(path, &s_bgm_ogg) == 0) {
            vorbis_info *vi = ov_info(&s_bgm_ogg, -1);
            s_bgm_src_rate = (vi && vi->rate > 0) ? (uint32_t)vi->rate : MIX_SAMPLE_RATE;
            s_bgm_type = BgmType_OggFile;
            ok = true;
        }
    }

    mutexUnlock(&s_audio_lock);
    return ok;
}

bool switch2BgmPlayPack(const char *zip_path) {
    if (!s_audio_ready || !zip_path || !zip_path[0])
        return false;

    mutexLock(&s_audio_lock);
    if (!s_pack_has_bgm || !s_pack_bgm_entry[0]) {
        mutexUnlock(&s_audio_lock);
        return false;
    }
    char entry[256];
    strncpy(entry, s_pack_bgm_entry, sizeof(entry) - 1);
    entry[sizeof(entry) - 1] = '\0';

    bgmCloseLocked();
    s_fade_gain = 1.0f;
    s_fade_step = 0.0f;

    bool ok = false;
    unzFile uf = unzOpen(zip_path);
    if (uf) {
        if (hasExtCI(entry, ".wav")) {
            if (unzLocateFile(uf, entry, 2) == UNZ_OK &&
                unzOpenCurrentFile(uf) == UNZ_OK &&
                parseWavFromUnz(uf, &s_bgm_wav)) {
                s_bgm_zip = uf;
                snprintf(s_bgm_zip_path, sizeof(s_bgm_zip_path), "%.511s", zip_path);
                snprintf(s_bgm_zip_entry, sizeof(s_bgm_zip_entry), "%.255s", entry);
                s_bgm_wav_rem = s_bgm_wav.data_size;
                s_bgm_src_rate = s_bgm_wav.sample_rate;
                s_bgm_type = BgmType_WavZip;
                ok = true;
            } else {
                unzClose(uf);
            }
        } else if (hasExtCI(entry, ".mp3")) {
            size_t sz = 0;
            uint8_t *mem = readZipEntryToMem(uf, entry, MAX_ZIP_BGM_MEM, &sz);
            unzClose(uf);
            if (mem) {
                if (drmp3_init_memory(&s_bgm_mp3, mem, sz, NULL)) {
                    s_bgm_mem = mem;
                    s_bgm_src_rate = s_bgm_mp3.sampleRate ? s_bgm_mp3.sampleRate : MIX_SAMPLE_RATE;
                    s_bgm_type = BgmType_Mp3Mem;
                    ok = true;
                } else {
                    free(mem);
                }
            }
        } else if (hasExtCI(entry, ".ogg")) {
            size_t sz = 0;
            uint8_t *mem = readZipEntryToMem(uf, entry, MAX_ZIP_BGM_MEM, &sz);
            unzClose(uf);
            if (mem) {
                s_bgm_ogg_mem.data = mem;
                s_bgm_ogg_mem.size = sz;
                s_bgm_ogg_mem.pos = 0;
                if (ov_open_callbacks(&s_bgm_ogg_mem, &s_bgm_ogg, NULL, 0, s_ogg_mem_cb) == 0) {
                    s_bgm_mem = mem;
                    vorbis_info *vi = ov_info(&s_bgm_ogg, -1);
                    s_bgm_src_rate = (vi && vi->rate > 0) ? (uint32_t)vi->rate : MIX_SAMPLE_RATE;
                    s_bgm_type = BgmType_OggMem;
                    ok = true;
                } else {
                    free(mem);
                }
            }
        } else {
            unzClose(uf);
        }
    }

    mutexUnlock(&s_audio_lock);
    return ok;
}

#else
void switch2SfxInit(void) {}
void switch2SfxExit(void) {}
void switch2SfxSetEnabled(bool enabled) { (void)enabled; }
void switch2SfxSetVolume(int volume_pct) { (void)volume_pct; }
void switch2SfxPlay(S2SfxId id) { (void)id; }
bool switch2SfxLoadPack(const char *zip_path) { (void)zip_path; return false; }
bool switch2SfxPackHasBgm(void) { return false; }
void switch2BgmSetVolume(int volume_pct) { (void)volume_pct; }
bool switch2BgmPlayFile(const char *path) { (void)path; return false; }
bool switch2BgmPlayPack(const char *zip_path) { (void)zip_path; return false; }
void switch2BgmStop(void) {}
void switch2BgmStartFadeOut(float duration_sec) { (void)duration_sec; }
void switch2BgmResetFade(void) {}
#endif

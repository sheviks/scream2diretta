// Implementation of pcm_startup.h. See header for design.
//
// PCM sample decoding is little-endian signed integer at 16/24/32 bits.
// Channels are interleaved. The analyser keeps a tiny per-channel
// previous-sample cache so it can report max sample-to-sample delta
// across the configured window without buffering the audio.
//
// The fader writes back in place. For 24-bit it preserves the 3-byte
// packing. Cosine shape uses an equal-power-ish curve scaled to [0,1].

#include "pcm_startup.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cmath>

namespace {

constexpr int kMaxCh = 8;

inline int32_t read_sample_le(const uint8_t* p, uint32_t bits) {
    if (bits == 16) {
        int16_t s = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        return (int32_t)s;
    } else if (bits == 24) {
        int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
        if (v & 0x00800000) v |= ~0x00FFFFFF;
        return v;
    } else if (bits == 32) {
        return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)
                       | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
    }
    return 0;
}

inline void write_sample_le(uint8_t* p, uint32_t bits, int32_t v) {
    if (bits == 16) {
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        uint16_t u = (uint16_t)(int16_t)v;
        p[0] = (uint8_t)(u & 0xff);
        p[1] = (uint8_t)((u >> 8) & 0xff);
    } else if (bits == 24) {
        const int32_t kMax = 0x7FFFFF;
        const int32_t kMin = -0x800000;
        if (v > kMax) v = kMax;
        if (v < kMin) v = kMin;
        uint32_t u = (uint32_t)v & 0x00FFFFFFu;
        p[0] = (uint8_t)(u & 0xff);
        p[1] = (uint8_t)((u >> 8) & 0xff);
        p[2] = (uint8_t)((u >> 16) & 0xff);
    } else if (bits == 32) {
        uint32_t u = (uint32_t)v;
        p[0] = (uint8_t)(u & 0xff);
        p[1] = (uint8_t)((u >> 8) & 0xff);
        p[2] = (uint8_t)((u >> 16) & 0xff);
        p[3] = (uint8_t)((u >> 24) & 0xff);
    }
}

// Multiply a sample by a gain in [0, 1]. Use 64-bit intermediate to avoid
// overflow on 32-bit input * gain near 1.0.
inline int32_t scale_sample(int32_t v, double gain) {
    double scaled = (double)v * gain;
    if (scaled >  2147483647.0) return  2147483647;
    if (scaled < -2147483648.0) return -2147483648;
    return (int32_t)scaled;
}

inline double sample_full_scale(uint32_t bits) {
    if (bits == 16) return 32768.0;
    if (bits == 24) return 8388608.0;
    if (bits == 32) return 2147483648.0;
    return 1.0;
}

void emit_analyzer_summary(const pcm_startup_analyzer_t* a) {
    if (!a || !a->enabled || !a->armed || a->bytes_per_frame == 0) return;
    const uint32_t ch = (a->channels > kMaxCh) ? kMaxCh : a->channels;
    const double fs = sample_full_scale(a->bits_per_sample);
    const uint64_t total_samples = a->sample_count_sq;
    double rms_norm = 0.0;
    if (total_samples > 0 && fs > 0.0) {
        const double mean_sq = (double)a->sum_sq / (double)total_samples;
        rms_norm = std::sqrt(mean_sq) / fs;
    }
    const double peak_norm = (fs > 0.0) ? ((double)a->peak_abs / fs) : 0.0;
    const double delta_norm = (fs > 0.0) ? ((double)a->max_abs_delta / fs) : 0.0;
    const double max_delta_time_ms =
        (a->sample_rate > 0)
            ? ((double)a->max_abs_delta_frame * 1000.0 / (double)a->sample_rate)
            : 0.0;
    const double first_nonsilent_ms =
        (a->first_nonsilent_frame >= 0 && a->sample_rate > 0)
            ? ((double)a->first_nonsilent_frame * 1000.0 / (double)a->sample_rate)
            : -1.0;

    // first-sample list per channel
    char first_buf[160];
    int  off = 0;
    first_buf[0] = '\0';
    for (uint32_t c = 0; c < ch; ++c) {
        int n = std::snprintf(first_buf + off, sizeof(first_buf) - off,
                              "%s%d", (c == 0 ? "" : ","),
                              (int)a->first_sample_per_ch[c]);
        if (n < 0 || (size_t)(off + n) >= sizeof(first_buf)) break;
        off += n;
    }

    std::fprintf(stderr,
        "[startup-analyze:%s] summary: rate=%u bits=%u ch=%u window_ms=%d "
        "frames=%llu first_samples=[%s] first_nonsilent_frame=%lld "
        "first_nonsilent_time_ms=%.3f first_nonsilent_ch=%d "
        "first_nonsilent_value=%d max_abs_delta=%d max_abs_delta_norm=%.6f "
        "max_abs_delta_frame=%llu max_abs_delta_time_ms=%.3f "
        "max_abs_delta_ch=%d peak_abs=%d peak_norm=%.6f rms_norm=%.6f\n",
        a->tag, a->sample_rate, a->bits_per_sample, a->channels, a->max_ms,
        (unsigned long long)a->frames_seen,
        first_buf,
        (long long)a->first_nonsilent_frame,
        first_nonsilent_ms,
        a->first_nonsilent_ch,
        (int)a->first_nonsilent_value,
        (int)a->max_abs_delta,
        delta_norm,
        (unsigned long long)a->max_abs_delta_frame,
        max_delta_time_ms,
        a->max_abs_delta_ch,
        (int)a->peak_abs, peak_norm, rms_norm);
}

} // namespace

extern "C" {

// ----- analyser -----

void pcm_startup_analyzer_init(pcm_startup_analyzer_t* a,
                               const char* tag,
                               int max_ms,
                               int verbosity) {
    if (!a) return;
    std::memset(a, 0, sizeof(*a));
    std::snprintf(a->tag, sizeof(a->tag), "%s", tag ? tag : "pcm");
    a->max_ms = max_ms;
    a->verbosity = verbosity;
    a->enabled  = (max_ms > 0) ? 1 : 0;
    a->first_nonsilent_frame = -1;
}

void pcm_startup_analyzer_configure(pcm_startup_analyzer_t* a,
                                    uint32_t sample_rate,
                                    uint32_t channels,
                                    uint32_t bits_per_sample) {
    if (!a || !a->enabled) return;
    if (sample_rate == 0 || channels == 0 ||
        (bits_per_sample != 16 && bits_per_sample != 24 && bits_per_sample != 32)) {
        a->armed = 0;
        return;
    }
    a->sample_rate     = sample_rate;
    a->channels        = channels;
    a->bits_per_sample = bits_per_sample;
    a->bytes_per_frame = channels * (bits_per_sample / 8u);
    a->window_frames   = ((uint64_t)sample_rate * (uint64_t)a->max_ms) / 1000ULL;
    a->frames_seen     = 0;
    a->have_prev_sample = 0;
    a->have_first_sample = 0;
    for (int i = 0; i < kMaxCh; ++i) {
        a->prev_sample_per_ch[i] = 0;
        a->first_sample_per_ch[i] = 0;
    }
    a->first_nonsilent_frame = -1;
    a->first_nonsilent_ch    = -1;
    a->first_nonsilent_value = 0;
    a->max_abs_delta         = 0;
    a->max_abs_delta_frame   = 0;
    a->max_abs_delta_ch      = 0;
    a->peak_abs              = 0;
    a->sum_sq                = 0;
    a->sample_count_sq       = 0;
    a->armed = 1;

    std::fprintf(stderr,
        "[startup-analyze:%s] armed: rate=%u bits=%u ch=%u window_ms=%d "
        "window_frames=%llu\n",
        a->tag, sample_rate, bits_per_sample, channels, a->max_ms,
        (unsigned long long)a->window_frames);
}

void pcm_startup_analyzer_feed(pcm_startup_analyzer_t* a,
                               const void* data,
                               size_t bytes) {
    if (!a || !a->enabled || !a->armed) return;
    if (a->bytes_per_frame == 0 || bytes < a->bytes_per_frame) return;
    if (a->frames_seen >= a->window_frames) return;

    const uint32_t bps    = a->bits_per_sample;
    const uint32_t bpsamp = bps / 8u;
    const uint32_t ch_all = a->channels;
    const uint32_t ch_lim = (ch_all > kMaxCh) ? kMaxCh : ch_all;
    const uint8_t* p = static_cast<const uint8_t*>(data);

    const uint64_t remaining_frames = a->window_frames - a->frames_seen;
    uint64_t avail_frames = bytes / a->bytes_per_frame;
    if (avail_frames > remaining_frames) avail_frames = remaining_frames;

    for (uint64_t f = 0; f < avail_frames; ++f) {
        for (uint32_t c = 0; c < ch_all; ++c) {
            const uint8_t* sp = p + c * bpsamp;
            int32_t v = read_sample_le(sp, bps);
            if (c < ch_lim) {
                if (!a->have_first_sample) {
                    a->first_sample_per_ch[c] = v;
                }
                // first non-silent frame: pick the FIRST channel that is
                // non-zero in the earliest frame seen so far.
                if (a->first_nonsilent_frame < 0 && v != 0) {
                    a->first_nonsilent_frame = (int64_t)a->frames_seen;
                    a->first_nonsilent_ch    = (int)c;
                    a->first_nonsilent_value = v;
                }
                // sample-to-sample delta per channel
                if (a->have_prev_sample) {
                    int64_t d = (int64_t)v - (int64_t)a->prev_sample_per_ch[c];
                    if (d < 0) d = -d;
                    if (d > (int64_t)a->max_abs_delta) {
                        a->max_abs_delta = (int32_t)((d > (int64_t)0x7FFFFFFF)
                                                     ? 0x7FFFFFFF : d);
                        a->max_abs_delta_frame = a->frames_seen;
                        a->max_abs_delta_ch    = (int)c;
                    }
                }
                a->prev_sample_per_ch[c] = v;
            }
            int32_t av = (v < 0) ? -v : v;
            if (av < 0) av = 0x7FFFFFFF;
            if (av > a->peak_abs) a->peak_abs = av;
            // accumulate sum-of-squares for RMS
            double dv = (double)v;
            a->sum_sq += (uint64_t)(dv * dv);
            a->sample_count_sq += 1;
        }
        if (!a->have_first_sample) a->have_first_sample = 1;
        a->have_prev_sample = 1;
        a->frames_seen += 1;
        p += a->bytes_per_frame;
    }

    if (a->frames_seen >= a->window_frames && a->armed) {
        emit_analyzer_summary(a);
        a->armed = 0;
    }
}

int pcm_startup_analyzer_done(const pcm_startup_analyzer_t* a) {
    if (!a || !a->enabled) return 1;
    return (a->armed == 0) ? 1 : 0;
}

// ----- fader -----

void pcm_startup_fader_init(pcm_startup_fader_t* f,
                            const char* tag,
                            int fade_ms,
                            pcm_fade_shape_t shape) {
    if (!f) return;
    std::memset(f, 0, sizeof(*f));
    std::snprintf(f->tag, sizeof(f->tag), "%s", tag ? tag : "fade");
    f->fade_ms = fade_ms;
    f->shape   = shape;
    f->enabled = (fade_ms > 0) ? 1 : 0;
}

void pcm_startup_fader_configure(pcm_startup_fader_t* f,
                                 uint32_t sample_rate,
                                 uint32_t channels,
                                 uint32_t bits_per_sample) {
    if (!f || !f->enabled) return;
    if (sample_rate == 0 || channels == 0 ||
        (bits_per_sample != 16 && bits_per_sample != 24 && bits_per_sample != 32)) {
        f->armed = 0;
        return;
    }
    f->sample_rate     = sample_rate;
    f->channels        = channels;
    f->bits_per_sample = bits_per_sample;
    f->bytes_per_frame = channels * (bits_per_sample / 8u);
    f->fade_frames     = ((uint64_t)sample_rate * (uint64_t)f->fade_ms) / 1000ULL;
    f->frames_applied  = 0;
    f->armed           = (f->fade_frames > 0) ? 1 : 0;
    f->logged_done     = 0;

    std::fprintf(stderr,
        "[startup-fade:%s] armed: rate=%u bits=%u ch=%u fade_ms=%d "
        "fade_frames=%llu shape=%s\n",
        f->tag, sample_rate, bits_per_sample, channels, f->fade_ms,
        (unsigned long long)f->fade_frames,
        (f->shape == PCM_FADE_COSINE) ? "cosine" : "linear");
}

size_t pcm_startup_fader_apply(pcm_startup_fader_t* f,
                               void* data,
                               size_t bytes) {
    if (!f || !f->enabled || !f->armed) return 0;
    if (f->bytes_per_frame == 0 || bytes < f->bytes_per_frame) return 0;
    if (f->frames_applied >= f->fade_frames) {
        if (!f->logged_done) {
            std::fprintf(stderr,
                "[startup-fade:%s] complete: frames_faded=%llu fade_ms=%d\n",
                f->tag, (unsigned long long)f->frames_applied, f->fade_ms);
            f->logged_done = 1;
            f->armed = 0;
        }
        return 0;
    }

    const uint32_t bps    = f->bits_per_sample;
    const uint32_t bpsamp = bps / 8u;
    const uint32_t ch_all = f->channels;
    uint8_t* p = static_cast<uint8_t*>(data);

    const uint64_t remaining = f->fade_frames - f->frames_applied;
    uint64_t avail_frames    = bytes / f->bytes_per_frame;
    if (avail_frames > remaining) avail_frames = remaining;

    const double total = (double)f->fade_frames;
    for (uint64_t i = 0; i < avail_frames; ++i) {
        const uint64_t idx = f->frames_applied + i;
        double g;
        if (f->shape == PCM_FADE_COSINE) {
            // raised cosine 0..1
            const double x = (double)idx / total;
            g = 0.5 - 0.5 * std::cos(3.14159265358979323846 * x);
        } else {
            g = ((double)idx + 1.0) / total;  // linear, never exactly 0 on first frame
        }
        if (g < 0.0) g = 0.0;
        if (g > 1.0) g = 1.0;
        for (uint32_t c = 0; c < ch_all; ++c) {
            uint8_t* sp = p + c * bpsamp;
            int32_t v = read_sample_le(sp, bps);
            v = scale_sample(v, g);
            write_sample_le(sp, bps, v);
        }
        p += f->bytes_per_frame;
    }

    f->frames_applied += avail_frames;
    if (f->frames_applied >= f->fade_frames && !f->logged_done) {
        std::fprintf(stderr,
            "[startup-fade:%s] complete: frames_faded=%llu fade_ms=%d\n",
            f->tag, (unsigned long long)f->frames_applied, f->fade_ms);
        f->logged_done = 1;
        f->armed = 0;
    }
    return (size_t)avail_frames;
}

int pcm_startup_fader_done(const pcm_startup_fader_t* f) {
    if (!f || !f->enabled) return 1;
    return (f->armed == 0) ? 1 : 0;
}

} // extern "C"

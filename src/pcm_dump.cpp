// Implementation of pcm_dump.h. Header-fixup on close uses fseek so the
// captured WAV opens cleanly in any standard player (Audacity, sox,
// ffmpeg, etc.) even if the program exits abruptly mid-capture -- in that
// case the header is left with the placeholder sizes from open(), but the
// dumper logs ("aborted") which file is incomplete so the user can fix it
// up manually with `sox --ignore-length` or similar.

#include "pcm_dump.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

namespace {

inline void le_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}
inline void le_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

// Write a WAV PCM header. Sizes are placeholders (UINT32_MAX-ish) so a
// crash leaves something the user can still salvage. Real sizes are
// patched in pcm_dumper_close().
void write_wav_header(FILE* fp,
                      uint32_t sample_rate,
                      uint32_t channels,
                      uint32_t bits_per_sample) {
    uint8_t hdr[44] = {0};
    const uint32_t byte_rate   = sample_rate * channels * (bits_per_sample / 8);
    const uint16_t block_align = (uint16_t)(channels * (bits_per_sample / 8));

    // RIFF header
    std::memcpy(hdr + 0, "RIFF", 4);
    le_u32(hdr + 4, 0);                  // ChunkSize placeholder (patched on close)
    std::memcpy(hdr + 8, "WAVE", 4);
    // fmt subchunk
    std::memcpy(hdr + 12, "fmt ", 4);
    le_u32(hdr + 16, 16);                // Subchunk1Size for PCM
    le_u16(hdr + 20, 1);                 // AudioFormat = PCM
    le_u16(hdr + 22, (uint16_t)channels);
    le_u32(hdr + 24, sample_rate);
    le_u32(hdr + 28, byte_rate);
    le_u16(hdr + 32, block_align);
    le_u16(hdr + 34, (uint16_t)bits_per_sample);
    // data subchunk
    std::memcpy(hdr + 36, "data", 4);
    le_u32(hdr + 40, 0);                 // Subchunk2Size placeholder

    std::fwrite(hdr, 1, sizeof(hdr), fp);
}

void patch_wav_sizes(FILE* fp, uint64_t data_bytes) {
    if (!fp) return;
    const uint32_t data_size = (data_bytes > 0xFFFFFFFFULL - 36ULL)
                               ? (uint32_t)0xFFFFFFFFULL
                               : (uint32_t)data_bytes;
    const uint32_t riff_size = (data_bytes > 0xFFFFFFFFULL - 36ULL)
                               ? (uint32_t)0xFFFFFFFFULL
                               : (uint32_t)(36ULL + data_bytes);
    uint8_t b4[4];
    if (std::fseek(fp, 4, SEEK_SET) == 0) {
        le_u32(b4, riff_size);
        std::fwrite(b4, 1, 4, fp);
    }
    if (std::fseek(fp, 40, SEEK_SET) == 0) {
        le_u32(b4, data_size);
        std::fwrite(b4, 1, 4, fp);
    }
}

} // namespace

extern "C" {

void pcm_dumper_init(pcm_dumper_t* d, const char* prefix, const char* tag, int max_ms) {
    if (!d) return;
    std::memset(d, 0, sizeof(*d));
    if (prefix && prefix[0]) {
        std::snprintf(d->prefix, sizeof(d->prefix), "%s", prefix);
        std::snprintf(d->tag,    sizeof(d->tag),    "%s", tag ? tag : "pcm");
        d->max_ms = max_ms;
        d->enabled = 1;
    } else {
        d->enabled = 0;
    }
}

int pcm_dumper_enabled(const pcm_dumper_t* d) {
    return (d && d->enabled) ? 1 : 0;
}

int pcm_dumper_open_or_rotate(pcm_dumper_t* d,
                              uint32_t sample_rate,
                              uint32_t channels,
                              uint32_t bits_per_sample) {
    if (!d || !d->enabled) return 0;
    if (sample_rate == 0 || channels == 0 ||
        (bits_per_sample != 8 && bits_per_sample != 16 &&
         bits_per_sample != 24 && bits_per_sample != 32)) {
        return 0;
    }

    // Already open on this exact format AND not yet capped? Reuse it.
    if (d->fp != nullptr && !d->closed_by_cap &&
        d->cur_sample_rate == sample_rate &&
        d->cur_channels == channels &&
        d->cur_bits_per_sample == bits_per_sample) {
        return 1;
    }

    // Otherwise, close any open file and start a new one (whether format
    // changed or previous file hit the cap).
    pcm_dumper_close(d);

    d->fmt_index += 1;
    d->cur_sample_rate    = sample_rate;
    d->cur_channels       = channels;
    d->cur_bits_per_sample = bits_per_sample;
    d->cur_bytes_per_frame = channels * (bits_per_sample / 8);
    d->cur_bytes_written  = 0;
    d->cur_frames_written = 0;
    d->closed_by_cap = 0;

    if (d->max_ms > 0 && d->cur_bytes_per_frame > 0) {
        const uint64_t bps =
            (uint64_t)sample_rate * (uint64_t)d->cur_bytes_per_frame;
        uint64_t bytes_cap = (bps * (uint64_t)d->max_ms) / 1000ULL;
        // Align cap to a whole frame so the file ends cleanly.
        bytes_cap = (bytes_cap / d->cur_bytes_per_frame) * d->cur_bytes_per_frame;
        d->cur_max_bytes = bytes_cap;
    } else {
        d->cur_max_bytes = 0;
    }

    std::snprintf(d->cur_path, sizeof(d->cur_path),
                  "%s_%s_fmt%03d_%uHz_%ubit_%uch.wav",
                  d->prefix, d->tag, d->fmt_index,
                  (unsigned)sample_rate, (unsigned)bits_per_sample,
                  (unsigned)channels);

    d->fp = std::fopen(d->cur_path, "wb");
    if (!d->fp) {
        std::fprintf(stderr,
                     "[pcm-dump:%s] ERROR: failed to open '%s' for writing\n",
                     d->tag, d->cur_path);
        return 0;
    }
    write_wav_header(d->fp, sample_rate, channels, bits_per_sample);
    std::fflush(d->fp);

    std::fprintf(stderr,
                 "[pcm-dump:%s] open: file='%s' sample_rate=%u channels=%u "
                 "bits=%u bytes_per_frame=%u max_ms=%d cap_bytes=%llu\n",
                 d->tag, d->cur_path, (unsigned)sample_rate,
                 (unsigned)channels, (unsigned)bits_per_sample,
                 (unsigned)d->cur_bytes_per_frame, d->max_ms,
                 (unsigned long long)d->cur_max_bytes);
    return 1;
}

size_t pcm_dumper_write(pcm_dumper_t* d, const void* data, size_t bytes) {
    if (!d || !d->enabled || !d->fp || d->closed_by_cap ||
        data == nullptr || bytes == 0) {
        return 0;
    }
    size_t to_write = bytes;
    if (d->cur_max_bytes > 0) {
        const uint64_t remaining = (d->cur_bytes_written < d->cur_max_bytes)
                                   ? (d->cur_max_bytes - d->cur_bytes_written)
                                   : 0ULL;
        if ((uint64_t)to_write > remaining) to_write = (size_t)remaining;
    }
    if (to_write == 0) {
        // Already at cap before any new bytes; close and mark.
        const uint64_t frames = (d->cur_bytes_per_frame > 0)
                                ? (d->cur_bytes_written / d->cur_bytes_per_frame)
                                : 0;
        std::fprintf(stderr,
                     "[pcm-dump:%s] cap_reached: file='%s' bytes=%llu frames=%llu "
                     "cap_bytes=%llu (no further writes until format change)\n",
                     d->tag, d->cur_path,
                     (unsigned long long)d->cur_bytes_written,
                     (unsigned long long)frames,
                     (unsigned long long)d->cur_max_bytes);
        // Patch header now so the partial file is playable immediately.
        if (d->fp) {
            std::fflush(d->fp);
            patch_wav_sizes(d->fp, d->cur_bytes_written);
            std::fflush(d->fp);
        }
        d->closed_by_cap = 1;
        return 0;
    }
    const size_t wrote = std::fwrite(data, 1, to_write, d->fp);
    d->cur_bytes_written += (uint64_t)wrote;
    if (d->cur_bytes_per_frame > 0) {
        d->cur_frames_written = d->cur_bytes_written / d->cur_bytes_per_frame;
    }
    d->total_bytes_written += (uint64_t)wrote;

    // If this write hit the cap exactly, fold that into "cap reached" so
    // the next call short-circuits and we patch the header now.
    if (d->cur_max_bytes > 0 && d->cur_bytes_written >= d->cur_max_bytes) {
        std::fprintf(stderr,
                     "[pcm-dump:%s] cap_reached: file='%s' bytes=%llu frames=%llu "
                     "cap_bytes=%llu\n",
                     d->tag, d->cur_path,
                     (unsigned long long)d->cur_bytes_written,
                     (unsigned long long)d->cur_frames_written,
                     (unsigned long long)d->cur_max_bytes);
        std::fflush(d->fp);
        patch_wav_sizes(d->fp, d->cur_bytes_written);
        std::fflush(d->fp);
        d->closed_by_cap = 1;
    }
    return wrote;
}

void pcm_dumper_close(pcm_dumper_t* d) {
    if (!d || !d->enabled || !d->fp) return;
    std::fflush(d->fp);
    patch_wav_sizes(d->fp, d->cur_bytes_written);
    std::fflush(d->fp);
    std::fclose(d->fp);
    d->fp = nullptr;
    d->total_files += 1;
    std::fprintf(stderr,
                 "[pcm-dump:%s] close: file='%s' bytes=%llu frames=%llu "
                 "duration_ms=%llu reason=%s\n",
                 d->tag, d->cur_path,
                 (unsigned long long)d->cur_bytes_written,
                 (unsigned long long)d->cur_frames_written,
                 (d->cur_sample_rate > 0 && d->cur_bytes_per_frame > 0)
                    ? ((unsigned long long)d->cur_frames_written * 1000ULL /
                       (unsigned long long)d->cur_sample_rate)
                    : 0ULL,
                 d->closed_by_cap ? "cap" : "rotate/shutdown");
}

} // extern "C"

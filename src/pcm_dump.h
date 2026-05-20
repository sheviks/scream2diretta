// PCM WAV dumper for the diagnostics.
//
// Writes raw PCM samples into a WAV (RIFF/PCM) file with auto-rotation on
// format change. Two independent instances are used by the Diretta backend:
//
//   * ingress: PCM as written into the unified queue (post frame-alignment,
//              post partial-carry) -- captures what came off the wire.
//   * egress:  PCM as popped from the unified queue and handed to the SDK
//              -- captures what the Diretta layer is about to send to the
//              target. Silence emitted by the prefill / startup-real-delay
//              gates is NOT written; only real-PCM bytes returned by
//              popOrSilence().
//
// The dumper is single-producer per instance (ingress is written only from
// the receive thread; egress only from the SDK send thread) so no locking
// is needed inside.
//
// Output WAV is little-endian, format tag 1 (PCM). Header sample size and
// rate reflect the active Scream/Diretta format at the moment the file was
// opened; if the upstream format changes mid-capture, the dumper closes
// the current file (with fixed-up sizes) and opens a new one named with
// an incremented format-change counter, e.g.:
//
//   <prefix>_fmt001_48000Hz_32bit_2ch.wav
//   <prefix>_fmt002_44100Hz_16bit_2ch.wav
//
// A configurable per-file byte cap (derived from --dump-ms and the format)
// stops the writer after the cap is hit; the cap re-arms on each format
// change.

#ifndef SCREAM_PCM_DUMP_H
#define SCREAM_PCM_DUMP_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pcm_dumper_s {
    // Configuration (set once via pcm_dumper_init).
    char prefix[256];
    char tag[16];        // "ingress" or "egress"
    int  max_ms;         // per-file capture duration cap (0 = unlimited)
    int  enabled;        // 0 = disabled (init never called); 1 = armed

    // Live file state.
    FILE* fp;
    int   fmt_index;     // increments on every open (1, 2, 3, ...)
    uint32_t cur_sample_rate;
    uint32_t cur_channels;
    uint32_t cur_bits_per_sample;
    uint32_t cur_bytes_per_frame;
    uint64_t cur_max_bytes;   // computed from max_ms + active format; 0 = unlimited
    uint64_t cur_bytes_written;
    uint64_t cur_frames_written;
    int      closed_by_cap;   // 1 if this open already hit the cap
    char     cur_path[512];

    // Aggregate counters across all files for end-of-run log.
    uint64_t total_files;
    uint64_t total_bytes_written;
} pcm_dumper_t;

// Configure (does not open a file). prefix == NULL or empty disables the
// dumper. max_ms <= 0 means "unlimited" -- be careful, files can grow
// without bound until shutdown.
void pcm_dumper_init(pcm_dumper_t* d, const char* prefix, const char* tag, int max_ms);

// Returns 1 if pcm_dumper_init was called with a non-empty prefix.
int  pcm_dumper_enabled(const pcm_dumper_t* d);

// Open (or re-open on format change) a new WAV file for the active format.
// Safe to call repeatedly with the same format -- it is a no-op then.
// Returns 1 on success, 0 on failure (logs to stderr).
int  pcm_dumper_open_or_rotate(pcm_dumper_t* d,
                               uint32_t sample_rate,
                               uint32_t channels,
                               uint32_t bits_per_sample);

// Write bytes to the active file. Auto-closes when the per-file cap is hit.
// Returns the number of bytes actually written (clamped if the cap fires).
size_t pcm_dumper_write(pcm_dumper_t* d, const void* data, size_t bytes);

// Flush + close + fix up the WAV header. Safe to call on a closed dumper.
void pcm_dumper_close(pcm_dumper_t* d);

// Returns 1 if a file is currently open AND the cap has not yet fired.
int  pcm_dumper_is_writing(const pcm_dumper_t* d);

// Returns 1 if the cap has been hit for the current open. Once true, no
// further writes go through until a format change re-opens the file.
int  pcm_dumper_cap_reached(const pcm_dumper_t* d);

#ifdef __cplusplus
}
#endif

#endif // SCREAM_PCM_DUMP_H

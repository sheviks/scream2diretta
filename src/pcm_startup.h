// Startup PCM diagnostics + optional egress fade.
//
// Two parts:
//
//   1. pcm_startup_analyzer_t -- single-shot per format/open analyser. Runs
//      over the first <N> ms of REAL PCM (ingress OR egress, separate
//      instances). Records: first sample per channel, first non-silent
//      frame index/time, max absolute sample-to-sample delta and where in
//      the window it occurred, plus peak/RMS for the window. Emits a single
//      stderr summary line when the window completes. Default ON whenever
//      a dump is enabled or verbosity >= 2; configurable via
//      --startup-analyze-ms (0 disables).
//
//   2. pcm_startup_fader_t -- single-shot per format/open egress fader.
//      When fade_ms > 0, scales the first fade_ms of real PCM at egress by
//      a linearly (or cosine) ramp 0..1, applied in-place on the SDK's
//      cycle buffer. Default fade_ms = 0 (no fade -> bit-purity preserved).
//
// Both work on interleaved PCM in 16-, 24-, or 32-bit signed little-endian
// (which is what Scream / Diretta carry). 8-bit unsigned is intentionally
// out of scope; this build pipeline only sees signed-int PCM.

#ifndef SCREAM_PCM_STARTUP_H
#define SCREAM_PCM_STARTUP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ----- analyser -----

typedef struct pcm_startup_analyzer_s {
    // config
    char     tag[16];          // "ingress" or "egress"
    int      max_ms;           // analysis window in ms; 0 = disabled
    int      enabled;          // 0 = disabled (init never called); 1 = armed
    int      verbosity;        // log level required for the summary line (0 = always log)

    // active format
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits_per_sample;
    uint32_t bytes_per_frame;
    uint64_t window_frames;    // (sample_rate * max_ms) / 1000
    int      armed;            // 1 between configure and summary emit

    // running state
    uint64_t frames_seen;
    int      have_prev_sample;
    int32_t  prev_sample_per_ch[8];     // last-seen sample per channel (up to 8 channels)
    int32_t  first_sample_per_ch[8];
    int      have_first_sample;
    int64_t  first_nonsilent_frame;     // -1 until seen
    int      first_nonsilent_ch;
    int32_t  first_nonsilent_value;
    int32_t  max_abs_delta;             // sample-to-sample max |x[n]-x[n-1]| seen
    uint64_t max_abs_delta_frame;       // frame index where it occurred
    int      max_abs_delta_ch;
    int32_t  peak_abs;                  // max |x| seen in the window
    uint64_t sum_sq;                    // sum of x^2 over samples in window (for RMS)
    uint64_t sample_count_sq;           // number of samples summed (for averaging)
} pcm_startup_analyzer_t;

// Configure (does not arm). max_ms <= 0 disables. tag is for logging.
void pcm_startup_analyzer_init(pcm_startup_analyzer_t* a,
                               const char* tag,
                               int max_ms,
                               int verbosity);

// Arm for a fresh format/open. Resets running state and recomputes
// window_frames. Safe to call repeatedly: if the format is unchanged it
// still re-arms (so a re-open after a cap also gets a fresh analysis).
void pcm_startup_analyzer_configure(pcm_startup_analyzer_t* a,
                                    uint32_t sample_rate,
                                    uint32_t channels,
                                    uint32_t bits_per_sample);

// Process up to <bytes> of frame-aligned PCM. Returns silently once the
// window is exhausted (subsequent calls are no-ops until the next
// configure()). bytes must be a multiple of bytes_per_frame.
void pcm_startup_analyzer_feed(pcm_startup_analyzer_t* a,
                               const void* data,
                               size_t bytes);

// Returns 1 if the analyser has completed its window since the last
// configure() and emitted its summary, 0 otherwise.
int  pcm_startup_analyzer_done(const pcm_startup_analyzer_t* a);

// ----- fader -----

typedef enum pcm_fade_shape_e {
    PCM_FADE_LINEAR = 0,
    PCM_FADE_COSINE = 1,
} pcm_fade_shape_t;

typedef struct pcm_startup_fader_s {
    char     tag[16];
    int      fade_ms;          // 0 = disabled
    int      enabled;          // 0 = disabled (fade_ms <= 0); 1 = armed
    pcm_fade_shape_t shape;

    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits_per_sample;
    uint32_t bytes_per_frame;
    uint64_t fade_frames;
    uint64_t frames_applied;
    int      armed;            // 1 between configure and exhaust
    int      logged_done;
} pcm_startup_fader_t;

void pcm_startup_fader_init(pcm_startup_fader_t* f,
                            const char* tag,
                            int fade_ms,
                            pcm_fade_shape_t shape);

void pcm_startup_fader_configure(pcm_startup_fader_t* f,
                                 uint32_t sample_rate,
                                 uint32_t channels,
                                 uint32_t bits_per_sample);

// Apply the fade to a frame-aligned PCM buffer IN PLACE. Returns the
// number of frames whose amplitude was scaled (the rest pass through
// unchanged once the fade window is exhausted).
size_t pcm_startup_fader_apply(pcm_startup_fader_t* f,
                               void* data,
                               size_t bytes);

int  pcm_startup_fader_done(const pcm_startup_fader_t* f);

#ifdef __cplusplus
}
#endif

#endif // SCREAM_PCM_STARTUP_H

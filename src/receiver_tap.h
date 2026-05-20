// Receiver-side diagnostic taps.
//
// Two backend-independent diagnostic facilities, both default OFF:
//
//   1. Common receiver-payload tap: captures data->audio EXACTLY as the
//      output backend would have received it, in scream.c's main loop
//      AFTER receiver_rcv_fn() returns and BEFORE output_send_fn() is
//      invoked. Writes a WAV (source-correct), runs an optional startup
//      analyser over the first N ms of REAL PCM, and (optionally) feeds
//      one side of an in-RAM byte comparator.
//
//   2. Raw-stdout tap: parallel facility wired into raw_output_send().
//      Mirrors the byte stream that fwrite(data->audio, ..., stdout)
//      would emit, wrapped as a WAV, also with a startup analyser. Feeds
//      the other side of the comparator when -o stdout is the active
//      backend.
//
// The receiver-payload comparator is shared between -o stdout and -o
// diretta runs. In both runs the receiver-payload side captures the same
// stream (post-network-parse, pre-backend dispatch). The "other" side is
// the active backend tap:
//
//   -o stdout : receiver_payload vs raw_stdout
//   -o diretta: receiver_payload vs diretta_raw_entry
//
// All public functions are safe to call from a single producer thread.
// The comparator is single-producer per side and emits its summary line
// the moment both sides have filled their window.

#ifndef SCREAM_RECEIVER_TAP_H
#define SCREAM_RECEIVER_TAP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// One-shot global init from main(). prefix == NULL/empty disables the
// corresponding facility entirely; the call still succeeds.
//
//   payload_prefix  -> --dump-receiver-payload-wav
//   raw_prefix      -> --dump-raw-stdout-wav
//   dump_ms         -> --dump-ms (per-file cap, 0 = uncapped)
//   analyze_ms      -> --startup-analyze-ms (0 = disabled)
//   compare_ms      -> --compare-receiver-tap-ms (0 = disabled)
//   active_backend  -> 0 = none, 1 = raw/stdout, 2 = diretta raw-entry
//
// active_backend selects which downstream tap the receiver-payload buffer
// is compared against. Only the receiver-payload side is fed from
// receiver_tap_payload_feed(); the "other" side is fed by either
// receiver_tap_raw_feed() (from raw.c) or receiver_tap_diretta_feed()
// (from diretta.cpp's raw-entry tap).
void receiver_tap_init(const char* payload_prefix,
                       const char* raw_prefix,
                       int dump_ms,
                       int analyze_ms,
                       int compare_ms,
                       int active_backend);

// Called from scream.c main loop AFTER receiver_rcv_fn returns and BEFORE
// the active output_send_fn. rate_hz/bits/channels MUST reflect the
// declared source format for this packet (the same conversion raw.c and
// the Diretta path use). Bytes are written verbatim and unconditionally
// (matching what every backend ultimately sees as data->audio).
//
// rate_hz == 0 means "format not yet valid"; the tap silently drops the
// packet in that case (same policy as raw.c skipping fwrite when its
// ro_data.rate is 0).
void receiver_tap_payload_feed(const void* data, size_t bytes,
                               uint32_t rate_hz,
                               uint32_t bits,
                               uint32_t channels);

// Called from raw_output_send() just before its fwrite(stdout). The
// arguments mirror the receiver-payload variant; the raw tap is the
// "byte-perfect" raw_stdout source-of-truth.
void receiver_tap_raw_feed(const void* data, size_t bytes,
                           uint32_t rate_hz,
                           uint32_t bits,
                           uint32_t channels);

// Called from diretta_output_send()'s raw-entry tap so we can compare
// receiver_payload vs diretta_raw_entry in the same run. The diretta raw-entry
// tap continues to write its own WAV via the existing
// --dump-raw-entry-wav code path; this is a NON-FILE feed that only
// populates the comparator buffer.
void receiver_tap_diretta_raw_entry_feed(const void* data, size_t bytes,
                                      uint32_t rate_hz,
                                      uint32_t bits,
                                      uint32_t channels);

// Flush WAVs and emit a final aggregate summary line. Safe to call even
// when nothing was enabled.
void receiver_tap_shutdown(void);

// Returns 1 iff the receiver-payload tap is armed (CLI prefix non-empty).
int  receiver_tap_payload_enabled(void);
int  receiver_tap_raw_enabled(void);

#ifdef __cplusplus
}
#endif

#endif // SCREAM_RECEIVER_TAP_H

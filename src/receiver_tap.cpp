// Receiver-side diagnostic taps. See receiver_tap.h.

#include "receiver_tap.h"

#include "pcm_dump.h"
#include "pcm_startup.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <vector>

// C-linkage gate flag declared in receiver_tap.h. Read by the static
// inline receiver_tap_any_armed() at call sites for branch-predictor-
// friendly early-out. Writes are confined to receiver_tap_init().
extern "C" int g_receiver_tap_any_armed_flag = 0;

namespace {

struct ReceiverTapState {
    // -- payload tap (common, scream.c main loop) --
    pcm_dumper_t           payload_dumper{};
    pcm_startup_analyzer_t payload_analyzer{};
    bool                   payload_enabled = false;

    // -- raw_stdout tap (raw.c) --
    pcm_dumper_t           raw_dumper{};
    pcm_startup_analyzer_t raw_analyzer{};
    bool                   raw_enabled = false;

    // Active source format tracked across feeds (re-arms analyser on
    // every format change so each format/open gets its own summary).
    uint32_t payload_rate = 0;
    uint32_t payload_bits = 0;
    uint32_t payload_chans = 0;

    uint32_t raw_rate = 0;
    uint32_t raw_bits = 0;
    uint32_t raw_chans = 0;

    // -- in-RAM byte comparator (single producer per side) --
    //
    // Side A == receiver_payload. Side B == active backend tap (either
    // raw_stdout or diretta_raw_entry depending on active_backend).
    //
    // The comparator buffers are sized lazily on the first feed once a
    // valid source format is known. They re-arm on format change so each
    // format/open gets its own one-line summary.
    int                    compare_ms = 0;
    int                    active_backend = 0; // 0 none, 1 raw, 2 diretta

    bool                   cmp_armed = false;
    bool                   cmp_summary_emitted = false;

    std::vector<uint8_t>   cmp_payload_buf;
    std::vector<uint8_t>   cmp_other_buf;
    size_t                 cmp_payload_filled = 0;
    size_t                 cmp_other_filled = 0;
    size_t                 cmp_window_bytes = 0;
    uint32_t               cmp_sample_rate = 0;
    uint32_t               cmp_channels = 0;
    uint32_t               cmp_bits_per_sample = 0;
    uint32_t               cmp_bytes_per_frame = 0;
};

ReceiverTapState g_rt{};

const char* backend_name(int b) {
    switch (b) {
        case 1: return "raw_stdout";
        case 2: return "diretta_raw_entry";
        default: return "none";
    }
}

bool format_valid(uint32_t rate, uint32_t bits, uint32_t chans) {
    return rate > 0 && chans > 0 &&
           (bits == 16 || bits == 24 || bits == 32);
}

void cmp_rearm_for_format(uint32_t rate, uint32_t bits, uint32_t chans) {
    if (g_rt.compare_ms <= 0 || g_rt.active_backend == 0) {
        g_rt.cmp_armed = false;
        return;
    }
    if (!format_valid(rate, bits, chans)) {
        g_rt.cmp_armed = false;
        return;
    }
    const uint32_t bytes_per_sample = bits / 8u;
    const uint32_t bpf = bytes_per_sample * chans;
    if (bpf == 0) { g_rt.cmp_armed = false; return; }
    const uint64_t bps = (uint64_t)rate * (uint64_t)bpf;
    uint64_t bytes = (bps * (uint64_t)g_rt.compare_ms) / 1000ULL;
    bytes = (bytes / bpf) * bpf;
    if (bytes == 0) bytes = bpf;
    g_rt.cmp_payload_buf.assign(bytes, 0);
    g_rt.cmp_other_buf.assign(bytes, 0);
    g_rt.cmp_payload_filled = 0;
    g_rt.cmp_other_filled = 0;
    g_rt.cmp_window_bytes = (size_t)bytes;
    g_rt.cmp_sample_rate = rate;
    g_rt.cmp_channels = chans;
    g_rt.cmp_bits_per_sample = bits;
    g_rt.cmp_bytes_per_frame = bpf;
    g_rt.cmp_armed = true;
    g_rt.cmp_summary_emitted = false;
    std::fprintf(stderr,
        "[cmp:receiver-tap] armed: window_ms=%d window_bytes=%zu rate=%u "
        "bits=%u ch=%u bpf=%u sides=receiver_payload_vs_%s\n",
        g_rt.compare_ms, g_rt.cmp_window_bytes,
        rate, bits, chans, bpf, backend_name(g_rt.active_backend));
}

inline void cmp_capture(std::vector<uint8_t>& buf, size_t& filled,
                        const uint8_t* src, size_t bytes) {
    if (!g_rt.cmp_armed || g_rt.cmp_summary_emitted || bytes == 0) return;
    if (filled >= g_rt.cmp_window_bytes) return;
    const size_t space = g_rt.cmp_window_bytes - filled;
    const size_t take = (bytes < space) ? bytes : space;
    std::memcpy(buf.data() + filled, src, take);
    filled += take;
}

int32_t pcm_sample_signed(const uint8_t* p, uint32_t bits) {
    switch (bits) {
        case 16: {
            int16_t v = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
            return (int32_t)v;
        }
        case 24: {
            int32_t v = (int32_t)((uint32_t)p[0] |
                                  ((uint32_t)p[1] << 8) |
                                  ((uint32_t)p[2] << 16));
            if (v & 0x00800000) v |= 0xFF000000;
            return v;
        }
        case 32: {
            int32_t v = (int32_t)((uint32_t)p[0] |
                                  ((uint32_t)p[1] << 8) |
                                  ((uint32_t)p[2] << 16) |
                                  ((uint32_t)p[3] << 24));
            return v;
        }
        default: return 0;
    }
}

void cmp_maybe_emit_summary() {
    if (!g_rt.cmp_armed || g_rt.cmp_summary_emitted) return;
    if (g_rt.cmp_payload_filled < g_rt.cmp_window_bytes) return;
    if (g_rt.cmp_other_filled < g_rt.cmp_window_bytes) return;
    g_rt.cmp_summary_emitted = true;

    const size_t N = g_rt.cmp_window_bytes;
    const uint32_t bps_bytes = g_rt.cmp_bytes_per_frame;
    const uint32_t bits = g_rt.cmp_bits_per_sample;
    const uint32_t bytes_per_sample = bits / 8u;
    const uint32_t sample_rate = g_rt.cmp_sample_rate;

    const uint8_t* a = g_rt.cmp_payload_buf.data();
    const uint8_t* b = g_rt.cmp_other_buf.data();

    size_t first_mismatch_byte = N;
    for (size_t i = 0; i < N; ++i) {
        if (a[i] != b[i]) { first_mismatch_byte = i; break; }
    }
    const bool identical = (first_mismatch_byte == N);

    int64_t max_abs_delta = 0;
    size_t max_abs_delta_frame = 0;
    uint32_t max_abs_delta_ch = 0;
    const size_t total_samples = (bytes_per_sample > 0)
                                 ? (N / bytes_per_sample) : 0;
    for (size_t s = 0; s < total_samples; ++s) {
        const uint8_t* pa = a + s * bytes_per_sample;
        const uint8_t* pb = b + s * bytes_per_sample;
        const int64_t va = (int64_t)pcm_sample_signed(pa, bits);
        const int64_t vb = (int64_t)pcm_sample_signed(pb, bits);
        const int64_t d = (va > vb) ? (va - vb) : (vb - va);
        if (d > max_abs_delta) {
            max_abs_delta = d;
            if (bps_bytes > 0) {
                max_abs_delta_frame = (s * bytes_per_sample) / bps_bytes;
                max_abs_delta_ch =
                    (uint32_t)((s * bytes_per_sample) % bps_bytes) / bytes_per_sample;
            }
        }
    }

    const size_t first_mismatch_frame =
        (first_mismatch_byte < N && bps_bytes > 0)
            ? (first_mismatch_byte / bps_bytes) : 0;
    const double first_mismatch_time_ms =
        (sample_rate > 0)
            ? ((double)first_mismatch_frame * 1000.0 / (double)sample_rate)
            : 0.0;

    const double full_scale =
        (bits == 16) ? 32768.0 :
        (bits == 24) ? 8388608.0 :
        (bits == 32) ? 2147483648.0 : 1.0;
    const double max_abs_delta_norm = (double)max_abs_delta / full_scale;

    std::fprintf(stderr,
        "[cmp:receiver-tap] summary: sides=receiver_payload_vs_%s "
        "window_ms=%d window_bytes=%zu rate=%u bits=%u ch=%u bpf=%u "
        "identical=%s first_mismatch_byte=%zu first_mismatch_frame=%zu "
        "first_mismatch_time_ms=%.3f "
        "max_abs_sample_delta=%lld (norm=%.6f) "
        "max_abs_delta_frame=%zu max_abs_delta_ch=%u\n",
        backend_name(g_rt.active_backend),
        g_rt.compare_ms, N,
        sample_rate, bits, g_rt.cmp_channels, bps_bytes,
        identical ? "yes" : "no",
        (identical ? N : first_mismatch_byte),
        (identical ? (N / (bps_bytes ? bps_bytes : 1)) : first_mismatch_frame),
        identical ? ((sample_rate > 0)
                     ? ((double)(N / (bps_bytes ? bps_bytes : 1)) * 1000.0
                        / (double)sample_rate)
                     : 0.0)
                  : first_mismatch_time_ms,
        (long long)max_abs_delta, max_abs_delta_norm,
        max_abs_delta_frame, max_abs_delta_ch);
}

} // namespace

extern "C" {

void receiver_tap_init(const char* payload_prefix,
                       const char* raw_prefix,
                       int dump_ms,
                       int analyze_ms,
                       int compare_ms,
                       int active_backend) {
#ifdef SCREAM2DIRETTA_NO_DIAGNOSTICS
    // Production build: diagnostics fully compiled out. Warn the user if
    // any diagnostic flag was actually requested so they know to switch
    // to scream2diretta-debug. Keep the function callable so scream.c can
    // call us unconditionally without #ifdef noise at the call site.
    (void)dump_ms; (void)active_backend;
    const bool any_requested =
        (payload_prefix && payload_prefix[0]) ||
        (raw_prefix && raw_prefix[0]) ||
        (analyze_ms > 0) ||
        (compare_ms > 0);
    if (any_requested) {
        std::fprintf(stderr,
            "[receiver-tap] warning: diagnostics not compiled into this "
            "binary (SCREAM2DIRETTA_NO_DIAGNOSTICS). Requested flags "
            "ignored. Rebuild as scream2diretta-debug to enable "
            "--dump-* / --startup-analyze-ms / --compare-receiver-tap-ms.\n");
    }
    return;
#else
    g_rt = ReceiverTapState{};

    if (dump_ms < 0) dump_ms = 0;
    if (analyze_ms < 0) analyze_ms = 0;
    if (compare_ms < 0) compare_ms = 0;
    if (active_backend < 0 || active_backend > 2) active_backend = 0;

    pcm_dumper_init(&g_rt.payload_dumper, payload_prefix, "receiver", dump_ms);
    g_rt.payload_enabled = pcm_dumper_enabled(&g_rt.payload_dumper) ? true : false;

    pcm_dumper_init(&g_rt.raw_dumper, raw_prefix, "raw_stdout", dump_ms);
    g_rt.raw_enabled = pcm_dumper_enabled(&g_rt.raw_dumper) ? true : false;

    // Analysers default ON for every armed tap (raw + payload). The CLI
    // glue in scream.c already applies the diagnostic auto-default policy
    // (auto-100ms when any dump is enabled; user can force-disable).
    pcm_startup_analyzer_init(&g_rt.payload_analyzer,
                              "receiver_payload", analyze_ms, 0);
    pcm_startup_analyzer_init(&g_rt.raw_analyzer,
                              "raw_stdout", analyze_ms, 0);

    g_rt.compare_ms = compare_ms;
    g_rt.active_backend = active_backend;

    // Single-point gate: any of the three diagnostic facilities armed?
    // Read by every per-packet feed call (and the static inline header
    // accessor used at call sites) to early-out without further work
    // when nothing diagnostic is configured.
    const bool analyzer_armed_payload =
        (analyze_ms > 0); // pcm_startup_analyzer_init arms when window > 0
    const bool analyzer_armed_raw     = (analyze_ms > 0);
    const bool comparator_armed       =
        (compare_ms > 0 && active_backend > 0);
    g_receiver_tap_any_armed_flag =
        (g_rt.payload_enabled || g_rt.raw_enabled ||
         analyzer_armed_payload || analyzer_armed_raw ||
         comparator_armed) ? 1 : 0;

    std::fprintf(stderr,
        "[receiver-tap] init: payload_dump=%s raw_dump=%s dump_ms=%d "
        "analyze_ms=%d compare_ms=%d active_backend=%s any_armed=%d\n",
        g_rt.payload_enabled ? payload_prefix : "(off)",
        g_rt.raw_enabled ? raw_prefix : "(off)",
        dump_ms, analyze_ms, compare_ms, backend_name(active_backend),
        g_receiver_tap_any_armed_flag);
#endif // SCREAM2DIRETTA_NO_DIAGNOSTICS
}

int receiver_tap_payload_enabled(void) { return g_rt.payload_enabled ? 1 : 0; }
int receiver_tap_raw_enabled(void)     { return g_rt.raw_enabled ? 1 : 0; }

void receiver_tap_payload_feed(const void* data, size_t bytes,
                               uint32_t rate_hz, uint32_t bits, uint32_t chans) {
    if (__builtin_expect(!g_receiver_tap_any_armed_flag, 1)) return;
    if (data == nullptr || bytes == 0) return;
    if (!format_valid(rate_hz, bits, chans)) return;

    const bool fmt_changed =
        (rate_hz != g_rt.payload_rate ||
         bits    != g_rt.payload_bits ||
         chans   != g_rt.payload_chans);
    if (fmt_changed) {
        g_rt.payload_rate = rate_hz;
        g_rt.payload_bits = bits;
        g_rt.payload_chans = chans;
        // Re-arm analyser on every format change.
        pcm_startup_analyzer_configure(&g_rt.payload_analyzer,
                                       rate_hz, chans, bits);
        // Re-arm comparator on any format change observed at the payload
        // side -- this is the side that triggers a fresh capture window.
        cmp_rearm_for_format(rate_hz, bits, chans);
    }

    if (g_rt.payload_enabled) {
        if (pcm_dumper_open_or_rotate(&g_rt.payload_dumper,
                                      rate_hz, chans, bits)) {
            pcm_dumper_write(&g_rt.payload_dumper, data, bytes);
        }
    }
    if (!pcm_startup_analyzer_done(&g_rt.payload_analyzer)) {
        pcm_startup_analyzer_feed(&g_rt.payload_analyzer, data, bytes);
    }
    cmp_capture(g_rt.cmp_payload_buf, g_rt.cmp_payload_filled,
                (const uint8_t*)data, bytes);
    cmp_maybe_emit_summary();
}

void receiver_tap_raw_feed(const void* data, size_t bytes,
                           uint32_t rate_hz, uint32_t bits, uint32_t chans) {
    if (__builtin_expect(!g_receiver_tap_any_armed_flag, 1)) return;
    if (data == nullptr || bytes == 0) return;
    if (!format_valid(rate_hz, bits, chans)) return;

    const bool fmt_changed =
        (rate_hz != g_rt.raw_rate ||
         bits    != g_rt.raw_bits ||
         chans   != g_rt.raw_chans);
    if (fmt_changed) {
        g_rt.raw_rate = rate_hz;
        g_rt.raw_bits = bits;
        g_rt.raw_chans = chans;
        pcm_startup_analyzer_configure(&g_rt.raw_analyzer,
                                       rate_hz, chans, bits);
    }

    if (g_rt.raw_enabled) {
        if (pcm_dumper_open_or_rotate(&g_rt.raw_dumper,
                                      rate_hz, chans, bits)) {
            pcm_dumper_write(&g_rt.raw_dumper, data, bytes);
        }
    }
    if (!pcm_startup_analyzer_done(&g_rt.raw_analyzer)) {
        pcm_startup_analyzer_feed(&g_rt.raw_analyzer, data, bytes);
    }
    if (g_rt.active_backend == 1) {
        cmp_capture(g_rt.cmp_other_buf, g_rt.cmp_other_filled,
                    (const uint8_t*)data, bytes);
        cmp_maybe_emit_summary();
    }
}

void receiver_tap_diretta_raw_entry_feed(const void* data, size_t bytes,
                                      uint32_t rate_hz, uint32_t bits,
                                      uint32_t chans) {
    if (__builtin_expect(!g_receiver_tap_any_armed_flag, 1)) return;
    if (data == nullptr || bytes == 0) return;
    if (!format_valid(rate_hz, bits, chans)) return;
    if (g_rt.active_backend == 2) {
        cmp_capture(g_rt.cmp_other_buf, g_rt.cmp_other_filled,
                    (const uint8_t*)data, bytes);
        cmp_maybe_emit_summary();
    }
}

void receiver_tap_shutdown(void) {
    if (pcm_dumper_enabled(&g_rt.payload_dumper)) {
        pcm_dumper_close(&g_rt.payload_dumper);
        std::fprintf(stderr,
            "[pcm-dump:receiver] shutdown: files=%llu total_bytes=%llu\n",
            (unsigned long long)g_rt.payload_dumper.total_files,
            (unsigned long long)g_rt.payload_dumper.total_bytes_written);
    }
    if (pcm_dumper_enabled(&g_rt.raw_dumper)) {
        pcm_dumper_close(&g_rt.raw_dumper);
        std::fprintf(stderr,
            "[pcm-dump:raw_stdout] shutdown: files=%llu total_bytes=%llu\n",
            (unsigned long long)g_rt.raw_dumper.total_files,
            (unsigned long long)g_rt.raw_dumper.total_bytes_written);
    }
    if (g_rt.cmp_armed && !g_rt.cmp_summary_emitted) {
        std::fprintf(stderr,
            "[cmp:receiver-tap] shutdown: comparator window did not fill "
            "(payload=%zu/%zu other=%zu/%zu) -- no summary emitted\n",
            g_rt.cmp_payload_filled, g_rt.cmp_window_bytes,
            g_rt.cmp_other_filled, g_rt.cmp_window_bytes);
    }
}

} // extern "C"

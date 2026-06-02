// Single-point armed gate for Diretta-backend-internal diagnostic
// facilities. Parallel in shape to receiver_tap.h's any_armed gate, but
// scoped to facilities owned by the Diretta backend itself:
//
//   - ingress / egress / raw-entry PCM WAV dumpers
//     (--dump-ingress-wav / --dump-egress-wav / --dump-raw-entry-wav)
//   - ingress / egress startup analysers   (--startup-analyze-ms)
//   - egress startup fader                 (--startup-fade-ms / -shape)
//   - ingress-tap byte comparator          (--compare-ingress-taps-ms)
//
// The flag is computed once during diretta_output_init() once all CLI
// values are known, then read-only thereafter. Both the Scream receive
// thread (queue_push_frames / raw-entry tap in diretta_output_send) and
// the Diretta SDK send thread (getNewStream egress feeds in
// diretta_sync.cpp) consult the gate before doing any per-packet
// diagnostic work.
//
// When SCREAM2DIRETTA_NO_DIAGNOSTICS is defined at compile time
// (production binary), the accessor returns a compile-time constant 0.
// The compiler dead-code-eliminates every `if (diretta_diag_armed())`
// guarded block at the call site, so the per-packet hot path costs zero
// instructions for these diagnostics. The diagnostic implementation is
// still linked (shared with the debug binary), but is unreachable in
// the production binary.

#ifndef SCREAM_DIRETTA_DIAG_H
#define SCREAM_DIRETTA_DIAG_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SCREAM2DIRETTA_NO_DIAGNOSTICS
static inline int diretta_diag_armed(void) { return 0; }
#else
extern int g_diretta_diag_armed_flag;
static inline int diretta_diag_armed(void) {
    return g_diretta_diag_armed_flag;
}
#endif

#ifdef __cplusplus
}
#endif

#endif // SCREAM_DIRETTA_DIAG_H

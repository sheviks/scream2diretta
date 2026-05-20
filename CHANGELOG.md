# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

---

## [Unreleased]

### Security

- **F2**: Add UDP source IP validation via `--allowed-source-ip <ip>`.
  `network.c` now rejects Scream packets from unlisted senders at the socket
  layer, mitigating remote format-change DoS by forged UDP packets.
  (`network.h`, `network.c`, `scream.c`)

### Fixed

- **F1**: Fix PcmRing resize race between receiver thread and Diretta SDK
  send thread. `ScreamDirettaSync` now carries an `m_active` atomic gate.
  `getNewStream()` emits silence without touching `m_ring` when the gate is
  closed. `activate()` is called after `play()` succeeds; `deactivate()` +
  `stop()` precede any teardown that may resize the ring.
  (`diretta_sync.h`, `diretta_sync.cpp`, `diretta.cpp`)

- **F3**: Add C/C++ exception barrier around `diretta_output_init()` and
  `diretta_output_send()`. Any `std::bad_alloc` or other C++ exception thrown
  inside the Diretta backend is now caught and translated to a C-style error
  return, preventing undefined behaviour when the exception propagates across
  the C/C++ boundary into `scream.c`.
  (`diretta.cpp`)

- **F4**: Add overflow guard to `PcmRing::roundUpPow2()`. The old loop could
  enter an infinite spin if `v` were near `SIZE_MAX` because `r <<= 1` would
  overflow to zero. A `SIZE_MAX / 2` check now caps the loop before overflow.
  (`diretta_ring.h`)

- **F5**: Block SIGINT/SIGTERM reentry during the signal handler. The
  `sa_mask` in `install_term_handlers()` now includes both `SIGINT` and
  `SIGTERM`, preventing the handler from being interrupted by a second
  instance of the same signal.
  (`scream.c`)

- **F6**: Fix `configTransferAuto` parameter mis-mapping in auto transfer mode.
  The old three-argument call placed `cycle` (200μs) into the "Minimum Sync
  System Time" slot, `Clock()` (0) into "Target Cycle Time", and
  `info_cycle_us` (100ms) into "Maximum Cycle Time (Recovery)". This was
  semantically wrong per SDK 149 `Sync.hpp`.
  The auto path now uses the single-argument `configTransferVarAuto(cycle)`,
  matching DRUP and slim2Diretta, so `cycle` lands directly in the Target
  Cycle Time slot. `info_cycle_us` remains exclusively in `Sync::open()`.
  (`diretta.cpp`)

### Changed

- **Remove `--target-buffer-ms` and `-B` from CLI; default to SDK default**.
  The `target_buffer_ms` field remains in `diretta_config_t` (default 0) and
  is still passed to `Sync::setSink()`, but there is no longer a CLI option.
  This aligns with DRUP's design philosophy (only `thread-mode` and
  `transfer-mode` are mandatory knobs; everything else defaults to SDK
  behaviour). Users who need explicit control can rebuild with a non-zero
  default or re-add the CLI flag.
  (`diretta.h`, `diretta.cpp`, `scream.c`)

- **Keep `--target-profile-limit` default at 0 (SelfProfile)**.
  0 = SelfProfile (host-managed, stable); user can set >0 to enable
  TargetProfile via ProfileMaker. Default left at 0 because direct
  path gives faster connect times and cleaner cycle behaviour on
  tested hardware (e.g. Pi 5 + jumbo frames).
  (`diretta.cpp`, `scream.c`)

- **Replace fixed 200µs fallback with DRUP-style dynamic cycle calculation**.
  When `--cycle-time` is not specified (or 0), s2d now auto-calculates the
  target cycle time from the current audio format and MTU using the DRUP
  formula: `cycle = (MTU - 24) / (sampleRate × channels × bps / 8) × 1e6`,
  clamped to [100, 50000] µs. This gives ~280µs for CD-quality PCM,
  ~130µs for 96k/24, and 100µs (clamped) for very high rates, instead of
  the previous flat 200µs for all formats.
  (`diretta.cpp`)

- **Apply DRUP-style AUTO transfer-mode dispatch**.
  `transfer_mode=auto` (the default) now dynamically selects:
  - `configTransferVarAuto` for low-bitrate formats (≤16-bit / ≤48kHz) or DSD,
  - `configTransferVarMax` for normal / high-bitrate PCM.
  This matches DRUP's `applyTransferMode()` logic exactly.
  (`diretta.cpp`)

- **Startup log refactor**: make the first-run output less noisy and more
  semantically grouped.
  - The data-flow overview line is now prefixed `[pipeline]` instead of
    `[diretta]`, since it describes the whole chain, not just the Diretta
    backend.
  - The single `config:` line is split into two:
    - `[diretta] SDK config:` — parameters actually passed to the Diretta
      SDK (`target_buffer_ms`, `thread_mode`, `cycle_time_us`, etc.).
    - `[pipeline] config:` — receiver-side buffering and sequencing knobs
      (`pcm_buffer_ms`, `pcm_prefill_ms`, `rebuffer_percent`, etc.).
  - All `[diretta-phase]` step-by-step events (`setSink_begin`,
    `connect_begin`, `connectWait_begin`, `play_begin`,
    `receiver_first_packet_after_open_begin`, etc.) are now suppressed in
    normal runs. They only appear when `--diretta-debug` or `-vv` is active.
  - Key milestones (`sync_open_begin`, `play() ok`, `sink connected`,
    `first real PCM`, `open grace ended`) are printed with `DLOG(1, …)` so
    they show up under `-v` without the verbose timestamp-per-step noise.
  - Error and warning events (`underrun_begin`, `startup_overflow_risk`,
    connect failures) continue to print unconditionally.
  (`diretta.cpp`)

---

## [0.1.0] - 2026-05-20

### Summary

Converge the codebase from its multi-experiment history to the current
unified-queue architecture: `SO_RCVBUF -> receiver_data_t ->
diretta_output_send() -> PcmRing -> getNewStream() -> Diretta SDK`.
No new audio features; the focus is aligning code, docs, CLI and diagnostics
with the actual two-thread SPSC design.

### Added

- Define version `0.1` and add `--version` CLI flag.
- Add `ARCHITECTURE_RISKS.md` documenting top-3 risks: SDK lifecycle,
  format-change gap, and diagnostics coupling.
- Add `Q&A.md` for common design questions.
- Add `PROGRESS.md` (now superseded by this CHANGELOG).

### Changed

- Update `CLAUDE.md` to reflect the actual current architecture (v25b):
  `PcmRing` as the unified PCM queue, no separate Detached Packet FIFO,
  no DRUP-style Internal PCM FIFO.
- Normal `--help` now only exposes production-tuning knobs
  (`--pcm-buffer-ms`, `--pcm-prefill-ms`, `--rebuffer-percent`, etc.).
  Diagnostic/startup advanced parameters remain available but are hidden
  from the default help text.
- Rename diagnostic tap naming from historical `legacy` terms to current
  semantic names: `raw-entry` / `receiver-tap`.

### Fixed

- Fix Diretta `Find` initialization failure path: `new Find()` failure now
  calls `cleanup_finder()` to avoid leaking the partially-initialized object.
- Convert Sync timeout cleanup to explicit **bounded abandon**.
  If SDK `disconnect()` / `close()` exceeds a configurable wall-clock budget,
  the blocking thread is detached and the `ScreamDirettaSync` is abandoned
  for process lifetime. This prevents use-after-free when the SDK retains
  internal references, which is safer than forced deletion.

### Removed

- Delete no-op compatibility CLI parameters that no longer map to actual
  pipeline stages:
  - `--packet-fifo`
  - `--detached-capacity`
  - `--staging`
  - `--poststart`
  - `--startup-silence`
  - `--startup-align`

### Decisions

- Keep the minimal transport path: `SO_RCVBUF (4 MiB) -> PcmRing`.
  Do **not** add an intermediate Internal PCM FIFO or a separate Diretta
  worker thread. Extra queueing is only justified if stats prove
  receiver-side ingress is a bottleneck.
- Keep the bounded-abandon strategy for black-box SDK lifecycle.
  Forced cleanup of a Sync that the SDK still references is more dangerous
  than a controlled leak.
- Startup tuning surface remains `--pcm-prefill-ms` and `--rebuffer-percent`.
  Mute / analyzer / fade are development diagnostics, not production knobs.


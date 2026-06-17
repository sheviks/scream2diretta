# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

---

## [Unreleased]

---

## [0.4.0] - 2026-06-17

### Summary

Release focused on the `--transfer-mode=auto` + `--cycle-time` combination,
log accuracy of the `target_cycle` field, and latency-determinism work
carried over from the c00d2c3 / b85bd60 follow-up. Bundles in the
SO_BUSY_POLL / NIC timestamping / mlockall / fast-path changes that
shipped behind `[Unreleased]` since 0.3.0.

### Changed

- **`--transfer-mode=auto` now honours `--cycle-time` when given**.
  When both flags are present, the user's `cycle_us` is treated as primary
  intent and applied via `configTransferFixAuto()` (`auto-fixauto`), BUT
  only if 1 packet/cycle still fits within `effMtu` at the current format.
  The 1-packet bound is `varmax_cycle = calculateCycleTime(...)` with a 3%
  safety margin to absorb overhead-inference jitter. If the user cycle
  exceeds `safe_max`, s2d logs a `[warn]` line and falls back to
  `configTransferVarMax(varmax_cycle)` (`auto-varmax-override`) so that
  `cycle_packets` stays at 1. With no `--cycle-time` given, behaviour is
  unchanged: low-bitrate / DSD → VarAuto, normal/high PCM → VarMax.
  (`diretta.cpp`)

- **Sanity warning when `--cycle-time` < 200µs**. Below this threshold the
  target is unlikely to keep up; the warning fires for any transfer mode
  but the user value is still applied (cycle is not clamped).
  (`diretta.cpp`)

- **Remove undocumented "dense path" from AUTO branch**. An uncommitted
  heuristic added in a prior session compressed normal/high-rate PCM to
  `VarAuto(2000µs)` whenever the natural full-packet cycle exceeded 2000µs.
  It was never released, never documented, and overlapped with the new
  `auto + --cycle-time` semantics. AUTO now matches the historical
  DRUP-style two-branch decision.
  (`diretta.cpp`)

### Fixed

- **`target_cycle` log / stats no longer disagree with the SDK input**.
  The caller previously recomputed `target_cycle` with the old
  `cfg.cycle_us > 0 ? cfg.cycle_us : calculateCycleTime(...)` pattern,
  which silently lied when the AUTO branch routed the request through an
  override path (e.g. `auto-varmax-override` sends `varmax_cycle` to the
  SDK while the caller still showed the user's requested value).
  `apply_transfer_mode()` now returns the effective cycle via an out
  parameter; the open-side logging and `g_st.target_cycle_us` (exposed in
  `diretta_stats_t`) consume that value directly. Verified end-to-end:
  for DSD256 (352.8k container) with `--cycle-time=800`, log now reports
  `mode=auto-varmax-override target_cycle=530us sdk_cycle=531us
  cycle_size=1496B cycle_packets=1` (was `target_cycle=800`).
  (`diretta.cpp`)

### Performance (carried over from 0.3.x follow-up)

- **SO_BUSY_POLL, NIC timestamping, mlockall, and `getNewStream` fast path**.
  Four related changes to reduce wakeup jitter and gate overhead on the
  PcmRing → `getNewStream()` hot path:
  - `--udp-busy-poll-us <us>`: enables `SO_BUSY_POLL` on the Scream UDP socket
    so the receiver thread spins briefly in-kernel for new packets instead of
    scheduling out. 0 (default) preserves prior behaviour.
  - `--enable-nic-timestamp`: enables `SO_TIMESTAMPNS` and threads the per-packet
    NIC arrival timestamp to the underrun reporter as `nic_gap_ms`, separating
    upstream sender silence from local userspace wakeup latency.
  - `--no-mlock`: disable `mlockall(MCL_CURRENT|MCL_FUTURE)` which is now
    called by default. Pins all current and future pages in RAM so the PCM ring
    and SDK buffers cannot be paged out under memory pressure. Non-fatal on
    failure; root or `LimitMEMLOCK=infinity` required.
  - `getNewStream` fast path + branch hints: once all four startup gates have
    passed, an atomic `m_steadyState` latches so subsequent SDK pull cycles skip
    the gate cascade. Gate 3 (underrun) clears the latch with a release store.
  (`network.c`, `network.h`, `scream.c`, `scream.h`, `diretta.cpp`,
  `diretta_sync.h`, `diretta_sync.cpp`)

- **Add `CAP_IPC_LOCK` to systemd capabilities**.
  `AmbientCapabilities` and `CapabilityBoundingSet` now include `CAP_IPC_LOCK`
  so that `mlockall` works under a future non-root service user. Previously
  only `CAP_NET_RAW`, `CAP_NET_ADMIN`, and `CAP_SYS_NICE` were declared.
  (`scripts/scream2diretta.service`)

---

## [0.3.0] - 2026-05-30

### Summary

Production-hardening release focused on the systemd deployment path and a
class of subtle SDK lifecycle / startup-handshake bugs that caused silent
starts and per-target overhead caching to silently fail. Also adds UDP
source-IP validation, Linux capability bounding for the service unit,
and dedicated log-file output with logrotate.

### Security

- **F2**: Add UDP source IP validation via `--allowed-source-ip <ip>`.
  `network.c` now rejects Scream packets from unlisted senders at the socket
  layer, mitigating remote format-change DoS by forged UDP packets.
  (`network.h`, `network.c`, `scream.c`)

- **Add Linux capabilities to systemd service**.
  `scream2diretta.service` now declares `AmbientCapabilities` and
  `CapabilityBoundingSet` for `CAP_NET_RAW`, `CAP_NET_ADMIN`, and
  `CAP_SYS_NICE`, plus `NoNewPrivileges=yes`.  This limits the process
  surface while keeping `User=root` for compatibility, and prepares the
  ground for a future non-root service user.
  (`scripts/scream2diretta.service`)

- **Redirect service logs to `/var/log/scream2diretta.log`**.
  `StandardOutput` and `StandardError` now use `append:` so verbose
  output (enabled with `-vv` in `SCREAM2DIRETTA_OPTS`) is written to a
  dedicated file instead of the systemd journal.  A logrotate config is
  installed automatically by `install.sh` (10M size trigger) to keep
  the file from growing unbounded during extended debug sessions.
  (`scripts/scream2diretta.service`, `scripts/scream2diretta.logrotate`,
  `scripts/install.sh`)

- **Fix `SCREAM2DIRETTA_OPTS` word-splitting in systemd**.
  `ExecStart` now runs through `/bin/sh -c` so spaces inside the
  environment variable are split correctly.  Previously the entire value
  was passed as a single argument, causing "Too long interface name"
  when `-i` was followed by additional flags.
  (`scripts/scream2diretta.service`)

### Fixed

- **R2: Close in-flight `getNewStream()` window with ring-user guard**.
  `m_active=false` only blocked _new_ pull cycles; a cycle that had
  already passed the gate could still be inside `m_ring` access while the
  receiver thread reallocated the ring (UB). Mirrors DRUP's
  `RingAccessGuard` / `beginReconfigure()` pattern: a `std::atomic<int>
  m_ringUsers` counter is incremented under acq_rel inside `getNewStream()`
  after passing the `m_active` check, and `deactivate()` is now two-phase
  (set `m_active=false`, then spin until `m_ringUsers==0`). A second
  occurrence of the same pattern in `finalize_sync_open_on_receiver()`
  (auto-downgrade resize path) was wrapped in
  `deactivate() → resize → configureFormat → activate()`.
  (`diretta_sync.h`, `diretta_sync.cpp`, `diretta.cpp`)

- **Silent startup under systemd**: deterministic SDK disconnect + paced
  handshake. `bounded_disconnect()` now uses `stop() → disconnect_flgset()
  → sleep 50ms` (deterministic, non-blocking, target-protocol-clean)
  instead of `disconnect(true)+detach+abandon`. `open_sync_worker_blocking()`
  now paces `setSink/setSinkConfigure/setConfigTransfer/connectPrepare`
  with 50ms gaps and uses `is_online()` polling (5ms × 100, 500ms cap)
  before `play()`, so the target is always online before the first cycle.
  The abandon/detach path was deleted entirely. Previously a service
  start often produced no audio until the user added `-vv`, because
  fprintf I/O accidentally provided the missing pacing.
  (`diretta.cpp`)

- **Persist inferred overhead regardless of verbosity, and store it under
  `$STATE_DIRECTORY` when running as a systemd service**.
  Two related bugs prevented the per-target overhead cache from working in
  production runs:
  1. The overhead-inference + cache-write block in `open_sync_worker_blocking()`
     was gated under `if (verbosity >= 2)`, so service runs without `-vv` never
     populated the cache on the first successful open against a new target.
  2. The cache directory was hard-coded to `$HOME/.config/scream2diretta`,
     which fails under the hardened systemd unit (`ProtectHome=true` makes
     `/root` read-only) with `mkdir(/root/.config) failed: Permission denied`.
  Fixes:
  - The inference + `save_inferred_overhead()` block now runs unconditionally;
    only the verbose `transfer:` DLOG remains gated by `verbosity >= 2`.
  - `overhead_cache_dir()` now prefers `$STATE_DIRECTORY` (exported by
    systemd when `StateDirectory=scream2diretta` is set) and falls back to
    `$HOME/.config/scream2diretta` for manual invocation, then `/tmp/scream2diretta`.
  - `scream2diretta.service` now declares `StateDirectory=scream2diretta`
    (mode 0750), so systemd auto-creates `/var/lib/scream2diretta/` and the
    cache survives `ProtectHome=true` / `ProtectSystem=strict`.
  - `save_inferred_overhead()` and `ensure_cache_dir()` now log the actual
    path / errno on failure so future regressions are diagnosable from a
    single log line.
  (`diretta.cpp`, `scripts/scream2diretta.service`)

- **Strip empty-string arguments from argv**.
  When `SCREAM2DIRETTA_OPTS` contained empty quoted segments
  (e.g. `"" --foo`), getopt_long() saw a zero-length argument and
  errored out. The wrapper now compacts argv before parsing.
  (`scream.c`)

- **Remove duplicate `cmake` configure call in `install.sh`**.
  (`scripts/install.sh`)

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

- **Silence `-Wformat-zero-length` on clang**.
  (`diretta.cpp`)

- **Skip `sndio` backend when headers are incomplete**.
  (`CMakeLists.txt`, `src/sndio.c`)

### Changed

- **Add `nice(-10)` to async open and cleanup worker threads**.
  Receiver and SDK pull threads use SCHED_FIFO via `--rt-priority`; the
  blocking control-plane workers now run at `nice -10` so they preempt
  ordinary background load without competing with the audio hot path.
  (`diretta.cpp`)

- **Change default `--stats-interval` from 0 to 5 seconds**.
  When `--stats` or `-v`/`-vv` is active, periodic stats now print every
  5 seconds by default instead of being disabled (interval 0 previously
  prevented any periodic output). The old `--stats-interval 1` behaviour
  was too noisy for steady-state monitoring; 5s is a better balance.
  (`diretta.cpp`)

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

## [0.2.0] - 2026-05-26

### Added

- **Add `--rt-priority <1-99>` CLI parameter** for SCHED_FIFO real-time
  scheduling on the audio hot path.
  - Receiver thread (main thread): applies SCHED_FIFO before entering the
    receive loop via `diretta_apply_rt_priority()`.
  - Diretta SDK worker thread: applies SCHED_FIFO inside
    `ScreamDirettaSync::getNewStream()` on its first call, so the priority
    is set on the actual SDK pull thread.
  - Control / async threads are never elevated.
  (`diretta.h`, `diretta.cpp`, `diretta_sync.h`, `diretta_sync.cpp`, `scream.c`)

- **Add `nice(-10)` to async worker threads**.
  The async open worker and the bounded-disconnect cleanup worker both run
  on the CFS scheduler (not SCHED_FIFO). Setting nice to -10 gives them
  priority over ordinary background tasks without starving the audio
  hot-path threads that use `SCHED_FIFO`.
  (`diretta.cpp`)

- **Persist inferred protocol overhead across restarts**.
  On the first successful SDK open, s2d infers the actual overhead from
  `eff_mtu - getCycleSize()` and caches it to
  `~/.config/scream2diretta/overhead-<sanitized-ip>.txt`.
  On subsequent startups the cached value is loaded automatically,
  eliminating the initial VarMax double-packet cycle.
  (`diretta.cpp`)

- **Auto-detect Clang + lld in `install.sh`**.
  If `clang`, `clang++`, and `ld.lld` are all present, the installer
  automatically configures CMake to use the Clang toolchain with lld
  linker (matching AudioLinux's native toolchain). Falls back to the
  system default compiler (GCC) when Clang is unavailable.
  (`scripts/install.sh`)

### Fixed

- **Fix `install.sh` for root-only systems** (e.g. GentooPlayer).
  Add `run_privileged()` helper that runs commands directly when already
  root, falls back to `sudo`, and errors if neither is available.
  (`scripts/install.sh`)

- **Fix build failure on systems with libpcap**.
  Rename local `pcap.c`/`pcap.h` to `pcap_input.c`/`pcap_input.h` to avoid
  an include-guard collision with the system libpcap header (`#define PCAP_H`).
  Also fix a missing `return 0` in `init_pcap()`.
  (`CMakeLists.txt`, `src/pcap_input.c`, `src/pcap_input.h`, `src/scream.c`)

- **Rename `actual_cycle` to `sdk_cycle`** for clarity.
  The value returned by `getCycleTime()` is the SDK-generated transmission
  interval at open time, not a real-time target measurement.
  (`diretta.cpp`, `diretta.h`)

- **Adjust `OVERHEAD` based on MTU**.
  Standard frames (MTU ≤ 2000): overhead = 6B default.
  Jumbo frames (MTU > 2000): overhead = 6B (observed 9014 → 9008).
  The exact value is now inferred from SDK feedback on first open.
  (`diretta.cpp`)

- **Remove `target_cycle` and `sdk_cycle` from periodic stats**.
  These values are fixed at open time and do not change during steady-state
  playback; they are still logged once at open time.
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


# CLAUDE.md - scream2diretta

Guidance for Claude Code when working with this repository.

## Project Overview

**scream2diretta** is a native Scream UDP to Diretta SDK bridge for Linux (aarch64, x86-64).
It receives uncompressed PCM via UDP from a remote Scream sender and forwards it directly
to a Diretta-capable DAC via the Diretta Host SDK — without ALSA, FFmpeg, UPnP, or any
intermediate software layer.

**Design philosophy**: PCM transport, not a media player. No track boundaries, no playlist,
no decode stage. All decoding happens upstream.

## Architecture

```
Scream sender (Windows / Linux)
    │  UDP unicast/multicast  port 4011
    ▼
┌─────────────────────────────────────┐
│  scream2diretta                     │
│  ┌──────────────┐                   │
│  │ UDP Receiver │  ← pinned to --cpu-scream
│  │ SO_RCVBUF=4M │                   │
│  └──────┬───────┘                   │
│         │ receiver_data_t           │
│         ▼                           │
│  diretta_output_send()              │
│  (format detection, frame alignment)│
│         │                           │
│         ▼                           │
│  ┌──────────────┐                   │
│  │   PcmRing    │  ← lock-free SPSC │
│  │  (unified    │     jitter buffer │
│  │   PCM queue) │                   │
│  └──────┬───────┘                   │
│         │ getNewStream()            │
│         ▼                           │
│  ScreamDirettaSync                  │
│  (inherits DIRETTA::Sync)           │
│         │                           │
│         ▼                           │
│  Diretta SDK worker                 │  ← pinned via --cpu-audio
│  └──────────────────────────────────┘
    │  Diretta protocol (UDP/Ethernet)
    ▼
Diretta Target → DAC
```

### Threading Model

| Thread | Role | Affinity |
|--------|------|----------|
| Scream UDP receiver | Push PCM into PcmRing | `--cpu-scream` |
| Diretta SDK worker | Pull PCM via getNewStream() | `--cpu-audio` (via SDK OCCUPIED) |
| Async open/cleanup | Blocking SDK lifecycle | `--cpu-other` |

### Key Design Decisions

- **No separate packet FIFO**: PcmRing is the only userspace FIFO.
- **No decode stage**: Scream packets are already PCM.
- **No ALSA layer**: Direct Diretta SDK integration.
- **Lock-free hot path**: Atomics only, no mutexes in steady state.
- **Cache-line separation**: PcmRing producer/consumer atomics are `alignas(64)`.

## Hot Path Rules

- No heap allocations in steady state.
- No logging in steady state — only on state changes.
- Power-of-2 ring sizes with bitmask arithmetic.
- Explicit `memory_order` on all atomics.

## Buffer Chain

```
SO_RCVBUF kernel buffer      --udp-rcvbuf-bytes  (default 4 MiB)
    ↓
PcmRing unified PCM queue    --pcm-buffer-ms      (default 1000ms)
                               --pcm-prefill-ms     (default 500ms)
                               --rebuffer-percent   (default 50%)
    ↓
ScreamDirettaSync            getNewStream() pull
    ↓
Diretta SDK internal
```

## Format Changes

Detected by changes in Scream header bytes 0-3. Triggers:
1. Validate new format.
2. Resize PcmRing for new frame size.
3. Async teardown of old Sync.
4. Open new Sync with new FormatConfigure.

## Build

```bash
mkdir build && cd build
cmake -DDIRETTA_ENABLE=ON ..
make
```

Output: `scream2diretta-<ARCH_NAME>[-static]`

## Code Style

- C++17
- Classes: PascalCase, Functions: camelCase, Members: m_camelCase
- Constants: `constexpr` in namespace, UPPER_SNAKE_CASE
- Globals: g_camelCase
- 4 spaces, max 120 chars
- Atomics: always explicit `memory_order_acquire` / `release`

# scream2diretta

A native **Scream UDP → Diretta SDK** bridge for Linux (aarch64, x86-64).

Receives a continuous uncompressed PCM stream from a remote machine running ASIOScream / ScreamDriver / scream-alsa over UDP, and forwards it directly to a Diretta-capable DAC via the Diretta Host SDK — **without ALSA, FFmpeg, UPnP, or any intermediate software layer**.

> **scream2diretta is a PCM transport, not a media player.**
> There are no track boundaries, no playlist, no HTTP client, no decode stage. All decoding is offloaded to the upstream Windows or Linux machine.

---

## Architecture

```text
Scream sender (Windows / Linux / macOS)
    │  UDP unicast/multicast  port 4011
    ▼
┌──────────────────────────────────────────────────────┐
│  scream2diretta (Linux host)                         │
│                                                      │
│  ┌─────────────────────────────┐                    │
│  │  Scream UDP Receiver        │  ← --cpu-scream    │
│  │  SO_RCVBUF = 4 MiB          │                    │
│  └──────────────┬──────────────┘                    │
│                 │  frame-aligned PCM                │
│                 ▼                                    │
│  ┌──────────────────────────────┐                   │
│  │  PcmRing (lock-free SPSC)    │  ← unified queue  │
│  │  jitter / prefill buffer     │                    │
│  └──────────────┬───────────────┘                   │
│                 │  getNewStream() callback           │
│                 ▼                                    │
│  ┌──────────────────────────────┐                   │
│  │  ScreamDirettaSync           │  ← inherits       │
│  │  Diretta SDK internal worker │    DIRETTA::Sync  │
│  └──────────────────────────────┘                   │
└──────────────────────────────────────────────────────┘
    │  Diretta protocol (UDP/Ethernet)
    ▼
Diretta Target → DAC
```

### Threading Model

| Thread | Role | Affinity |
|--------|------|----------|
| Scream UDP receiver | Receive packets, push PCM into PcmRing | `--cpu-scream` |
| Diretta SDK worker | Pull PCM via `getNewStream()` | `--cpu-audio` (SDK OCCUPIED) |
| Async open/cleanup | Blocking SDK lifecycle | `--cpu-other` |

The steady-state hot path is a **two-thread SPSC architecture**: receiver pushes PCM into `PcmRing`, SDK worker pulls PCM from the same `PcmRing`.

---

## Why scream2diretta?

[**DirettaRendererUPnP (DRUP)**](https://github.com/cometdom/DirettaRendererUPnP) already provides excellent sound quality for UPnP-based playback (LMS, Roon, Audirvana, BubbleUPnP, etc.). **scream2diretta is designed to complement DRUP**, covering use cases that DRUP cannot handle natively:

### Use Case 1: Non-UPnP Music Services (Desktop, Mobile, Portable Players)

Apple Music, Spotify do not support UPnP streaming. You cannot send an HTTP stream to DRUP from these apps.

**Desktop (Windows / Linux / macOS)**: For apps like Foobar2000 or Spotify Desktop that can use ASIO / ScreamAlsa, output directly to **ScreamAlsa** (or ASIOScream on Windows). The PCM is sent over UDP to a Raspberry Pi running **scream2diretta**.

**Mobile / Portable Players (iPhone, Fiio M21, etc.)**: Output from Apple Music / Spotify to a USB gadget device (typically a Raspberry Pi configured as a USB Audio Class device). The audio can optionally pass through **CamillaDSP** for DSP processing, then feed into **ScreamAlsa**, and finally transmit to **scream2diretta**.

This can run on a **single machine** (USB gadget + ScreamAlsa + scream2diretta on one Pi) or in **dual-machine mode** (USB gadget/ScreamAlsa sender → network → dedicated scream2diretta receiver). Dual-machine is recommended for best sound quality.

```text
Desktop:   Foobar2000 / Spotify → ASIOScream / ScreamAlsa → network → scream2diretta → Diretta Target → DAC

Mobile:    iPhone / Fiio M21 → USB gadget (Raspberry Pi)
                                      ↓
                                CamillaDSP (optional)
                                      ↓
                                ScreamAlsa → network → scream2diretta → Diretta Target → DAC
```

### Use Case 2: HQPlayer / NAA without DirettaAlsaHost

HQPlayer / NAA (Network Audio Adapter) does not natively support Diretta. Traditionally, you would need **DirettaAlsaHost** as an intermediary ALSA plugin to bridge NAA output to a Diretta Target.

**Solution**: Configure NAA to output to **ScreamAlsa** instead. The PCM stream is sent over the network to **scream2diretta**, which bypasses the ALSA layer entirely and feeds the Diretta Target directly.

```text
HQPlayer → NAA → ScreamAlsa → network → scream2diretta → Diretta Target → DAC
```

This eliminates the ALSA period/buffer/wakeup jitter that DirettaAlsaHost introduces.

### Use Case 3: DSP Processing for UPnP Streams

DRUP accepts HTTP streams directly and passes them to the Diretta SDK. However, there is **no convenient hook for DSP processing** in this path.

**Solution**: Use **upmpdcli + MPD** as the UPnP renderer. Configure MPD to output to **ScreamAlsa**, with **CamillaDSP** acting as the PCM pipeline for DSP (EQ, crossover, room correction, etc.). The processed PCM is then sent to **scream2diretta**.

```text
LMS/Roon → upmpdcli → MPD → CamillaDSP → ScreamAlsa
                                               ↓
                                          network
                                               ↓
                                        scream2diretta → Diretta Target → DAC
```

This gives you full UPnP compatibility **with** DSP processing, something that is difficult to achieve with DRUP alone.

After all, You can also use **aprenderer / aplayer** (Windows / Linux) as the Scream sender and play directly to **scream2diretta**.

---

## Building

### Dependencies

- C++17 compiler (gcc 11+, clang 13+)
- CMake 3.7+
- Diretta Host SDK (e.g. `DirettaHostSDK_149`) — see [DirettaRendererUPnP](https://github.com/cometdom/DirettaRendererUPnP) or [slim2Diretta](https://github.com/cometdom/slim2Diretta) for acquisition instructions

If you need to build **ScreamAlsa** from source, see the [ScreamAlsa project](https://github.com/Scream-Projects/scream-alsa).

### Method 1: Install Script (Recommended)

The interactive installer handles dependencies, auto-detects architecture, builds the binary, and optionally installs a systemd service.

```bash
bash scripts/install.sh
```

**What it does:**
- Installs build dependencies (gcc, cmake, pkg-config)
- Auto-detects `DIRETTA_ARCH_SUFFIX` from CPU flags / page size
- Searches for Diretta SDK in project directory, parent, `$HOME`, `/opt`, `/usr/local`
- Builds `scream2diretta`
- Optionally installs systemd service with target selection

**CLI shortcuts (non-interactive):**
```bash
bash scripts/install.sh --full      # Full install with service
bash scripts/install.sh --build     # Build only
bash scripts/install.sh --update    # Rebuild and update installed binary
bash scripts/install.sh --test      # Verify installation and list targets
bash scripts/install.sh --uninstall # Remove binary and service
```

### Method 2: Manual Build

```bash
mkdir build && cd build

# Basic build (auto-detects architecture)
cmake -DDIRETTA_ENABLE=ON -DDIRETTA_SDK_ROOT=../DirettaHostSDK_149 ..
make -j$(nproc)

# Full-featured build (explicit arch, static, no SDK logging)
cmake -DDIRETTA_ENABLE=ON \
      -DDIRETTA_SDK_ROOT=../DirettaHostSDK_149 \
      -DDIRETTA_ARCH_SUFFIX=aarch64-linux-15k16 \
      -DBUILD_STATIC=ON \
      -DNOLOG=1 \
      ..
make -j$(nproc)
```

### Architecture Variants

| `ARCH_NAME` | Platform |
|-------------|----------|
| `x64-linux-15v2` | x86-64 baseline (no AVX2) |
| `x64-linux-15v3` | x86-64 with AVX2 (default) |
| `x64-linux-15v4` | x86-64 with AVX-512 |
| `x64-linux-15zen4` | AMD Zen 4 |
| `aarch64-linux-15` | ARM64, 4KB pages |
| `aarch64-linux-15k16` | ARM64, 16KB pages (RPi 5) |
| `riscv64-linux-15` | RISC-V 64-bit |
| `*-musl*` | musl libc variants (e.g. `aarch64-linux-15-musl`) |
| `*-nolog` | SDK logging disabled (append to any variant) |

#### How to determine your `ARCH_NAME`

**x86-64:**
```bash
# Check CPU flags
grep -m1 '^flags' /proc/cpuinfo
```
- Contains `avx512f` → use `x64-linux-15v4`
- Contains `avx2` but **not** `avx512f` → use `x64-linux-15v3` (most common)
- Neither → use `x64-linux-15v2`

**ARM64:**
```bash
getconf PAGE_SIZE
```
- `16384` → `aarch64-linux-15k16` (Raspberry Pi 5)
- `4096` → `aarch64-linux-15` (most other ARM64 boards)

**musl libc:** If your system uses musl instead of glibc (e.g. Alpine Linux), append `-musl` to the base variant.

**Production build:** Append `-nolog` to disable SDK internal logging (e.g. `aarch64-linux-15-nolog`).

Output: `scream2diretta-<ARCH_NAME>[-static]`

---

## Running

### Basic Usage

```bash
# List available Diretta targets
sudo ./scream2diretta --list-targets

# Run with the first target (default multicast 239.255.77.77:4010)
sudo ./scream2diretta

# Pin to target #1, unicast port 4011, verbose stats every 5s
sudo ./scream2diretta -t 1 -p 4011 -vv --stats --stats-interval 5

# With CPU affinity (recommended for best sound quality)
sudo ./scream2diretta -t 1 -p 4011 \
  --cpu-scream 2 --cpu-audio 3 --cpu-other 1 \
  --thread-mode 1 --transfer-mode auto
```

### Parameter Reference

| Flag | Default | Description |
|------|---------|-------------|
| `--target, -t <index>` | 1 | Select Diretta target by 1-based index |
| `--list-targets` | — | Discover and list targets |
| `-p <port>` | 4010 | UDP port for Scream |
| `-i <iface>` | — | Bind to specific network interface |
| `-g <group>` | 239.255.77.77 | Multicast group address (multicast mode only) |
| `--thread-mode <mask>` | 1 (CRITICAL) | SDK thread mode bitmask |
| `--transfer-mode <mode>` | auto | auto / varmax / varauto / fixauto / random |
| `--mtu <bytes>` | auto | Network MTU (default auto-detect, usually 9000 for jumbo frames) |
| `--pcm-buffer-ms <ms>` | 1000 | PcmRing total size |
| `--pcm-prefill-ms <ms>` | 500 | Fill threshold before first pull |
| `--rebuffer-percent <pct>` | 50 | Resume threshold after underrun |
| `--udp-rcvbuf-bytes <bytes>` | 4194304 | Kernel SO_RCVBUF |
| `--cpu-scream <core>` | — | Pin receiver thread to core |
| `--cpu-audio <core>` | — | Pin SDK worker thread to core |
| `--cpu-other <core>` | — | Pin helper threads to core |
| `--stats --stats-interval <sec>` | off | Periodic stats |
| `-v` / `-vv` | off | Verbose / very verbose |

### Tuning for Sound Quality

1. **Enable chrony on both sender and receiver** to align system clocks and eliminate PCM ring drift:
   ```bash
   sudo apt install chrony
   sudo systemctl enable chrony --now
   ```

2. **Use CPU affinity** to isolate the audio hot path:
   ```bash
   --cpu-scream 2 --cpu-audio 3 --cpu-other 1
   ```

3. **Use jumbo frames** if your network supports it (MTU 9000) for lower packet overhead.

---

## Scream Protocol

Scream sends raw uncompressed PCM over UDP:

```
Byte 0: sample rate code
Byte 1: bit depth (16 / 24 / 32)
Byte 2: channels
Byte 3: channel map
Byte 4+: interleaved PCM samples (little-endian signed)
```

- No sequence numbers, no retransmit — pure UDP.
- Format changes are detected by changes in bytes 0-3.
- Continuous stream, no track boundaries.

---

## Acknowledgments

- **[DirettaRendererUPnP](https://github.com/cometdom/DirettaRendererUPnP)** by cometdom — Primary reference for buffer parameter naming (`--pcm-buffer-ms`, `--pcm-prefill-ms`), `RingAccessGuard` pattern, and CPU affinity design. DRUP sets a high bar for UPnP-based Diretta playback, and scream2diretta exists to complement it for non-UPnP scenarios.
- **[slim2Diretta](https://github.com/cometdom/slim2Diretta)** by cometdom — Primary architecture reference for the `DirettaSync` / `DirettaRingBuffer` / `getNewStream()` pull model.
- **[duncanthrax/scream](https://github.com/duncanthrax/scream)** — The Scream receiver code is derived from the Unix receiver in this project.
- **Yu Harada** — Diretta SDK.

---

## License

See [LICENSE](LICENSE).

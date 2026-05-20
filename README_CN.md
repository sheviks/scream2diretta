# scream2diretta

原生 **Scream UDP → Diretta SDK** 桥接器，适用于 Linux (aarch64, x86-64)。

通过网络 UDP 接收来自远程机器（运行 ASIOScream / ScreamDriver / scream-alsa）的连续无压缩 PCM 流，并直接通过 Diretta Host SDK 转发到支持 Diretta 的 DAC —— **无需 ALSA、FFmpeg、UPnP 或任何中间软件层**。

> **scream2diretta 是 PCM 传输器，不是音乐播放器。**
> 没有曲目边界、没有播放列表、没有 HTTP 客户端、没有解码阶段。所有解码都在上游的 Windows 或 Linux 机器上完成。

---

## 架构

```text
Scream 发送端 (Windows / Linux / macOS)
    │  UDP 单播/组播  端口 4011
    ▼
┌──────────────────────────────────────────────────────┐
│  scream2diretta (Linux 主机)                         │
│                                                      │
│  ┌─────────────────────────────┐                    │
│  │  Scream UDP 接收器          │  ← --cpu-scream    │
│  │  SO_RCVBUF = 4 MiB          │                    │
│  └──────────────┬──────────────┘                    │
│                 │  帧对齐 PCM                       │
│                 ▼                                    │
│  ┌──────────────────────────────┐                   │
│  │  PcmRing (无锁 SPSC)         │  ← 统一队列       │
│  │  抖动/预填充缓冲区             │                    │
│  └──────────────┬───────────────┘                   │
│                 │  getNewStream() 回调              │
│                 ▼                                    │
│  ┌──────────────────────────────┐                   │
│  │  ScreamDirettaSync           │  ← 继承           │
│  │  Diretta SDK 内部工作线程     │    DIRETTA::Sync  │
│  └──────────────────────────────┘                   │
└──────────────────────────────────────────────────────┘
    │  Diretta 协议 (UDP/Ethernet)
    ▼
Diretta Target → DAC
```

### 线程模型

| 线程 | 职责 | 亲和性 |
|------|------|--------|
| Scream UDP 接收器 | 接收数据包，将 PCM 推入 PcmRing | `--cpu-scream` |
| Diretta SDK 工作线程 | 通过 `getNewStream()` 拉取 PCM | `--cpu-audio` (SDK OCCUPIED) |
| 异步打开/清理 | 阻塞式 SDK 生命周期操作 | `--cpu-other` |

稳态热路径是**双线程 SPSC 架构**：接收器将 PCM 推入 `PcmRing`，SDK 工作线程从同一个 `PcmRing` 拉取 PCM。

---

## 为什么需要 scream2diretta？

[**DirettaRendererUPnP (DRUP)**](https://github.com/cometdom/DirettaRendererUPnP) 已经为基于 UPnP 的播放（LMS、Roon、Audirvana、BubbleUPnP 等）提供了出色的音质。**scream2diretta 旨在补充 DRUP**，覆盖 DRUP 原生无法处理的场景：

### 场景 1：非 UPnP 音乐服务（桌面端、移动端、便携播放器）

Apple Music、Spotify 和 Tidal 不支持 UPnP 串流。你无法从这些应用向 DRUP 发送 HTTP 流。

**桌面端（Windows / Linux / macOS）**：对于 Foobar2000、Spotify 桌面版等支持 ASIO / ScreamAlsa 的播放器，直接输出到 **ScreamAlsa**（或 Windows 上的 ASIOScream）。PCM 通过网络 UDP 发送到运行 **scream2diretta** 的设备。

**移动端 / 便携播放器（iPhone、Fiio M21 等）**：将 Apple Music / Spotify 输出到 USB gadget 设备（通常是配置为 USB Audio Class 设备的树莓派）。音频可经过 **CamillaDSP** 进行 DSP 处理，再送入 **ScreamAlsa**，最后传输给 **scream2diretta**。

可以运行在**单机模式**（USB gadget + ScreamAlsa + scream2diretta 在同一台树莓派上），也可以采用**双机模式**（USB gadget/ScreamAlsa 发送端 → 网络 → 独立的 scream2diretta 接收端）。推荐双机模式以获得最佳音质。

```text
桌面端：  Foobar2000 / Spotify → ASIOScream / ScreamAlsa → network → scream2diretta → Diretta Target → DAC

移动端：  iPhone / Fiio M21 → USB gadget (Raspberry Pi)
                                      ↓
                                CamillaDSP (optional)
                                      ↓
                                ScreamAlsa → network → scream2diretta → Diretta Target → DAC
```

### 场景 2：HQPlayer / NAA 无需 DirettaAlsaHost

HQPlayer / NAA（Network Audio Adapter）原生不支持 Diretta。传统上，你需要 **DirettaAlsaHost** 作为 ALSA 插件中间层，将 NAA 输出桥接到 Diretta 目标设备。

**解决方案**：将 NAA 配置为输出到 **ScreamAlsa**。PCM 流通过网络发送到 **scream2diretta**，完全绕过 ALSA 层，直接送入 Diretta 目标设备。

```text
HQPlayer → NAA → ScreamAlsa → network → scream2diretta → Diretta Target → DAC
```

这消除了 DirettaAlsaHost 引入的 ALSA 周期/缓冲区/唤醒抖动。

### 场景 3：UPnP 流的 DSP 处理

DRUP 直接接收 HTTP 流并传递给 Diretta SDK。然而，在这条路径中**没有方便的 DSP 处理钩子**。

**解决方案**：使用 **upmpdcli + MPD** 作为 UPnP 渲染器。配置 MPD 输出到 **ScreamAlsa**，**CamillaDSP** 作为 DSP（EQ、分频、房间校正等）的 PCM 管道。处理后的 PCM 再发送到 **scream2diretta**。

```text
LMS/Roon → upmpdcli → MPD → CamillaDSP → ScreamAlsa
                                               ↓
                                          network
                                               ↓
                                        scream2diretta → Diretta Target → DAC
```

这让你获得完整的 UPnP 兼容性**并支持** DSP 处理，这是仅用 DRUP 难以实现的。

你也可以使用 **aprenderer / aplayer**（Windows / Linux）作为 Scream 发送端，直接播放给 **scream2diretta**。

---

## 编译

### 依赖

- C++17 编译器 (gcc 11+, clang 13+)
- CMake 3.7+
- Diretta Host SDK (如 `DirettaHostSDK_149`) —— 获取方式参见 [DirettaRendererUPnP](https://github.com/cometdom/DirettaRendererUPnP) 或 [slim2Diretta](https://github.com/cometdom/slim2Diretta) 项目主页

如需自行编译 **ScreamAlsa**，请前往 [ScreamAlsa 项目](https://github.com/Scream-Projects/scream-alsa)。

### 编译命令

```bash
mkdir build && cd build

# 基础编译 (自动检测架构)
cmake -DDIRETTA_ENABLE=ON -DDIRETTA_SDK_ROOT=../DirettaHostSDK_149 ..
make -j$(nproc)

# 完整编译 (指定架构、静态链接、关闭 SDK 日志)
cmake -DDIRETTA_ENABLE=ON \
      -DDIRETTA_SDK_ROOT=../DirettaHostSDK_149 \
      -DDIRETTA_ARCH_SUFFIX=aarch64-linux-15k16 \
      -DBUILD_STATIC=ON \
      -DNOLOG=1 \
      ..
make -j$(nproc)
```

### 架构变体

| `ARCH_NAME` | 平台 |
|-------------|------|
| `x64-linux-15v2` | x86-64 基线 (无 AVX2) |
| `x64-linux-15v3` | x86-64 AVX2 (默认) |
| `x64-linux-15v4` | x86-64 AVX-512 |
| `x64-linux-15zen4` | AMD Zen 4 |
| `aarch64-linux-15` | ARM64, 4KB 页 |
| `aarch64-linux-15k16` | ARM64, 16KB 页 (树莓派 5) |
| `riscv64-linux-15` | RISC-V 64-bit |
| `*-musl*` | musl libc 变体 (如 `aarch64-linux-15-musl`) |
| `*-nolog` | 禁用 SDK 内部日志 (附加到任意变体) |

#### 如何确定你的 `ARCH_NAME`

**x86-64:**
```bash
# 查看 CPU 指令集标志
grep -m1 '^flags' /proc/cpuinfo
```
- 包含 `avx512f` → 使用 `x64-linux-15v4`
- 包含 `avx2` 但 **不含** `avx512f` → 使用 `x64-linux-15v3` (最常见)
- 两者皆无 → 使用 `x64-linux-15v2`

**ARM64:**
```bash
getconf PAGE_SIZE
```
- `16384` → `aarch64-linux-15k16` (树莓派 5)
- `4096` → `aarch64-linux-15` (大多数其他 ARM64 开发板)

**musl libc:** 如果你的系统使用 musl 而非 glibc (如 Alpine Linux)，在基础变体后附加 `-musl`。

**生产构建:** 附加 `-nolog` 以禁用 SDK 内部日志 (如 `aarch64-linux-15-nolog`)。

输出：`scream2diretta-<ARCH_NAME>[-static]`

---

## 运行

### 基本用法

```bash
# 列出可用的 Diretta 目标设备
sudo ./scream2diretta --list-targets

# 使用第一个目标设备运行（默认组播 239.255.77.77:4010）
sudo ./scream2diretta

# 绑定目标 #1，单播端口 4011，每 5 秒显示详细统计
sudo ./scream2diretta -t 1 -p 4011 -vv --stats --stats-interval 5

# 使用 CPU 亲和性（推荐用于最佳音质）
sudo ./scream2diretta -t 1 -p 4011 \
  --cpu-scream 2 --cpu-audio 3 --cpu-other 1 \
  --thread-mode 1 --transfer-mode auto
```

### 参数参考

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--target, -t <index>` | 1 | 按 1-based 索引选择 Diretta 目标设备 |
| `--list-targets` | — | 发现并列出目标设备 |
| `-p <port>` | 4010 | Scream UDP 端口 |
| `-i <iface>` | — | 绑定到指定网络接口 |
| `-g <group>` | 239.255.77.77 | 组播组地址（仅组播模式） |
| `--thread-mode <mask>` | 1 (CRITICAL) | SDK 线程模式位掩码 |
| `--transfer-mode <mode>` | auto | auto / varmax / varauto / fixauto / random |
| `--mtu <bytes>` | auto | 网络 MTU（默认自动检测，巨型帧通常为 9000） |
| `--pcm-buffer-ms <ms>` | 1000 | PcmRing 总大小 |
| `--pcm-prefill-ms <ms>` | 500 | 首次拉取前的填充阈值 |
| `--rebuffer-percent <pct>` | 50 | 欠载恢复阈值 |
| `--udp-rcvbuf-bytes <bytes>` | 4194304 | 内核 SO_RCVBUF |
| `--cpu-scream <core>` | — | 将接收线程绑定到指定核心 |
| `--cpu-audio <core>` | — | 将 SDK 工作线程绑定到指定核心 |
| `--cpu-other <core>` | — | 将辅助线程绑定到指定核心 |
| `--stats --stats-interval <sec>` | off | 周期性统计 |
| `-v` / `-vv` | off | 详细 / 非常详细 |

### 音质调优

1. **在发送端和接收端都启用 chrony**，对齐系统时钟，消除 PCM 环漂移：
   ```bash
   sudo apt install chrony
   sudo systemctl enable chrony --now
   ```

2. **使用 CPU 亲和性**隔离音频热路径：
   ```bash
   --cpu-scream 2 --cpu-audio 3 --cpu-other 1
   ```

3. **如果网络支持，使用巨型帧**（MTU 9000）以降低数据包开销。

---

## Scream 协议

Scream 通过 UDP 发送原始无压缩 PCM：

```
Byte 0:    采样率代码
Byte 1:    位深度 (16 / 24 / 32)
Byte 2:    声道数
Byte 3:    声道映射
Byte 4+:   交错 PCM 采样 (小端有符号)
```

- 无序列号，无重传 —— 纯 UDP。
- 格式变化通过字节 0-3 的变化检测。
- 连续流，无曲目边界。

---

## 致谢

- **[DirettaRendererUPnP](https://github.com/cometdom/DirettaRendererUPnP)** by cometdom —— 缓冲区参数命名（`--pcm-buffer-ms`、`--pcm-prefill-ms`）、`RingAccessGuard` 模式和 CPU 亲和性设计的主要参考。DRUP 为基于 UPnP 的 Diretta 播放设立了高标准，scream2diretta 的存在是为了补充 DRUP 在非 UPnP 场景下的使用。
- **[slim2Diretta](https://github.com/cometdom/slim2Diretta)** by cometdom —— `DirettaSync` / `DirettaRingBuffer` / `getNewStream()` 拉取模型的主要架构参考。
- **[duncanthrax/scream](https://github.com/duncanthrax/scream)** —— Scream 接收器代码衍生自该项目的 Unix 接收器。
- **Yu Harada** —— Diretta SDK。

---

## 许可证

见 [LICENSE](LICENSE)。

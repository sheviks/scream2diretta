# VarPrioTime（Host SDK 155）

**状态**: 已接到 CLI。rpi5 / SDK 155 A/B：SelfProfile `--cycle-hz 300`（MemoryPlay 默认）在 44.1 kHz 上 `cy` 锁死；1250 Hz 因 35.28 帧有欠条会呼吸。
**日期**: 2026-10-07
**SDK**: `DirettaHostSDK_155`（`ReleaseNo = 155`；厂商补丁号 155_2）

默认生产路径仍是 `auto` / `varauto` / `fixauto`。`varprio` 为显式 opt-in。150 交叉编译仍可用（选 `varprio` 会在启动时报错）。

四种模式在 Target Profile 下的 `ModeType`、`cy=` / `fs=` 算法见 [transfer-profile-modes.md](transfer-profile-modes.md)。本文写 **155 为什么要加这条 mode**，以及 Hz 格子的内在逻辑。

---

## 1. 要解决的问题

PCM 消费电子里有两族基频，Diretta 一条 Host 进程两边都要播：

| 族 | 基频 | 常见倍数 |
|----|------|----------|
| 44.1 k | 44100 | 88.2 / 176.4 / DSD 容器 |
| 48 k | 48000 | 96 / 192 |

150 及更早的发送配方（`VarAuto` / `FixAuto` / `VarMax` / `Var`）都吃 **`ACQUA::Clock`，s2d 里就是整数微秒**（`--cycle-time`，合法 333–10000）。合同是「每隔 T 微秒发一班」或「把 T 量化成一只整帧盒」。

PCM 一次必须整帧。一拍里的帧数：

\[
N = \text{采样率} \times T_{\mu s} / 10^6
\]

`N` 是整数，包长才唯一，`cy` 才能锁成常数。否则：

- **FixAuto** 守整数微秒边沿，`fs` 带小数，包在相邻整帧之间跳（800 µs @ 44.1 k → `fs=35.28`，35/36 帧）。
- **VarAuto** 把 `N` 升成整帧盒，周期让步（同一 800 µs → 36 帧 / 816.33 µs）。盒子按**当前这一路采样率**算，换 48 k 会另做一盒。它不在两族之间找公倍数。

两族**同时**整帧，等价于 \(T\) 是 \(1/44100\) 和 \(1/48000\) 的公倍数：

\[
\gcd(44100, 48000) = 300 \quad\Rightarrow\quad T = k / 300 \text{ 秒}
\]

再要求 \(T\) 是**整数微秒**：

| \(k\) | \(T\) | CLI `--cycle-time` |
|-------|-------|---------------------|
| 1 | \(3333.\overline{3}\) µs（300 Hz） | 写不进去 |
| 2 | \(6666.\overline{6}\) µs（150 Hz） | 写不进去 |
| 3 | **10000 µs（100 Hz）** | 合法范围里**唯一**能两族整帧的值 |

44.1 k 单独整帧：整数微秒必须是 **10000 的倍数**（441 与 10000 互质）。48 k 单独整帧：必须是 **125 的倍数**。交集在 333–10000 里只剩 10 ms。

于是出现一个空档：**300 Hz 两族都整除（44.1 k = 147 帧，48 k = 160 帧），但 \(1/300\) 秒不是整数微秒。** MemoryPlay 默认 `CyclePrio=300`、`FlexCycle=enable`，Host 应用层打的就是这条格子。整数微秒的旧 API 踩不到它。

这就是 155 要新开一条 mode 的原因：发送节奏改用 **Hz** 当单位，让 Host 按「一秒几班」对齐两族基频，不必再把 \(3333.\overline{3}\) 塞进微秒时钟。

---

## 2. 155 加了什么

```text
bool Sync::configTransferVarPrioTime(std::uint32_t Hz);
bool ProfileMaker::configTransferVarPrioTime(std::uint32_t Hz);
```

注释：`variable-cycle configuration (Priority Cycle Time)`，参数 **Cycle Hertz (Hz)**。

- 仍是 **VARIABLE**（`VarSendSize ≠ 0`，定长变周期，`mode_sdk=variable`，syslog 打 `cy=`）。
- 钉的是**平均班次密度**，不是每一拍边沿（那是 FixAuto），也不是整帧盒优先（那是 VarAuto）。
- s2d：`--transfer-mode varprio --cycle-hz <Hz>`。`--cycle-time` 不参与换算。

时间合同从硬到松：

```text
FixAuto（每一拍边沿） > VarPrioTime（平均班次 Hz） > VarAuto（箱子尺寸优先）
```

---

## 3. 内在逻辑

码率固定时：

\[
\text{包长} = \text{采样率} \times \text{bpf} \times \text{周期},\qquad
\text{Hz} \approx 10^{12} / cy_{\text{ps}}
\]

`VarPrioTime(Hz)` 先定平均周期 \(1/\text{Hz}\)，再按当前格式拆成：

| 字段 | 角色 |
|------|------|
| `VarSendSize` | 普通箱 = \(\lfloor \text{采样率}/\text{Hz} \rfloor\) 整帧 |
| `VarSendRest` | 除不尽的那一截（欠条）。凑满 1 帧，下一趟大一帧 |

**采样率能被 Hz 整除**时，欠条为 0，盒唯一，周期被码率钉死，`cy` 锁成常数——表现像一只按 Hz 裁过的小 varmax。

**除不尽**时，守的是平均班次：多数趟普通箱，少数趟 +1 帧；`cy` 允许在 \(1/\text{Hz}\) 附近呼吸。

s2d CLI 100–3000 里，**两族基频都整帧**的 Hz 只有公约数：

| Hz | 44.1 k 每拍 | 48 k 每拍 | 32-bit/2ch 包长 | 拍长 |
|----|-------------|-----------|-----------------|------|
| 100 | 441 | 480 | 3528 / 3840 B | 10 ms |
| 150 | 294 | 320 | 2352 / 2560 B | 6.67 ms |
| **300** | **147** | **160** | **1176 / 1280 B** | **3.33 ms** |

300 是两边都能整除的**最密**值，也是 MemoryPlay 默认。100 / 150 同样无欠条，但更稀、包更大；MTU 1500 上 32-bit 立体声只有 300 仍能一包。

88.2 / 96 再多一个公约数 **600**；44.1 k 基频是 73.5 帧，会有欠条。

只放一族、想比 300 更密（会牺牲另一族整除）时：44.1 k 可用 882、1225、1470；48 k 可用 800、1000、1200、2000。1250 两边都不整除（35.28 / 38.4）。

FixAuto 没有对等的 300 Hz：`--cycle-time` 写不进 \(3333.\overline{3}\)。两族整帧的 FixAuto 在合法范围内只有 `--cycle-time 10000`（= varprio 100 Hz）。

VarAuto 也不会「动态改包长来兼顾两族」。它为**当前**采样率选一只固定盒子，播放中钉 `VarSendSize`，动的是 `cy`。换格式才重做盒子。

---

## 4. CLI 接线（已定）

两条 CLI 对应 SDK 里两个不同类型的入口，**禁止互转**。

| s2d CLI | SDK | 类型 | 用于 |
|---------|-----|------|------|
| `--cycle-time <us>` | `configTransferVarAuto` / `FixAuto` / `VarMax` / `Random` 的 Target Cycle Time | `ACQUA::Clock`（s2d 里 `Clock::MicroSeconds`） | 现有 mode |
| `--cycle-hz <Hz>` | `configTransferVarPrioTime` | `std::uint32_t` | **仅**新 mode |
| `--transfer-mode varprio` | 上述 PrioTime 调用 | 枚举 `DIRETTA_TM_VARPRIO` | 显式 opt-in |

用法：

```text
--transfer-mode varprio --cycle-hz 300
```

`--cycle-hz 1250` 的平均间隔是 800 µs，但 **不要**写成 `--cycle-time 800` 再除出来。SDK 从不读取 `--cycle-time` 去填 PrioTime。

接线约束：

- `varprio` 缺少 `--cycle-hz`：报错。不要默默拿 `--cycle-time` 换算。
- `varprio` 同时又给了 `--cycle-time`：警告并忽略 `--cycle-time`。
- 非 `varprio` 却给了 `--cycle-hz`：报错。
- `cycle_us == 0` 时的 jumbo `varmax_cycle`（约几十 Hz）**禁止**拿去当 `--cycle-hz`。
- **不要**改现在的 `auto`：`--cycle-time 800` 的 `auto` 仍走 `configTransferVarAuto`。
- `varprio` 不做一包 `safe_max` 回退、也不在 SDK 返回 false 时改走 VarAuto/VarMax。超 MTU 由 Host 一次多包；失败只打日志，`mode=` 仍是 `varprio`。

合法范围 100–3000。`0` 拒收。

`ProfileMaker`（`--target-profile-limit > 0`）上有同名函数，那个分支同样只传 Hz。

---

## 5. VarSendRest：结果，不是开关

`Profile::VarSendRest` 是 155 和 PrioTime 一起出现的字段：

```text
VarSendSize   变周期、定长时每趟字节（普通箱）
VarSendRest   非 0 时表示 VarSendSize 的余数（欠条）
TypicalFrame  fixpoint 帧数（FixAuto 用这个表达 35.28）
```

`configTransferVarPrioTime(Hz)` 之后，Host 按**当前** `setSinkConfigure` 的格式自己写入 Size / Rest。s2d：

- 不提供 `--varsendrest`
- 不手写 `Profile` 再 `setConfigTransfer`
- 调用后继续读 `getCycleTime()` / `getCycleSize()` / `getCyclePackets()` / `getMode()`（预期 `VARIABLE`）

---

## 6. s2d 整帧补偿不要动

`ScreamDirettaSync::getNewStream()` 里按 `getCycleSize()` 截成整帧、余数攒满再多吐一帧：发生在 **PcmRing → SDK** 交界，所有 mode 共用。它守的是环的帧边界和「平均对齐 SDK 给出的整数字节」，不改 Host 发车时刻，也不锁环水位（水位是 prefill / rebuffer）。

VarPrioTime 下若 `getCycleSize() % bpf == 0`（300 Hz @ 44.1 k = 1176 B），这层自然为 0。先留着。

---

## 7. 和 FixAuto / VarAuto 差在哪

同一段 44.1 kHz / 16-bit / 立体声，订「大约 800 µs 一趟」：

| | 你传入 | Host 锁什么 | 箱子 | 赶不上 800 µs 时 |
|---|---|---|---|---|
| VarAuto | `--cycle-time 800` → `Clock(800µs)` | 整帧包长 | 落到 36 帧 / 144 B，生成周期 ~816 µs；live `cy=` 还可晃 | 间隔继续让步 |
| FixAuto | `--cycle-time 800` → `FixAuto(800µs)` | **每一拍** 800 µs | `TypicalFrame=35.28`，每拍 35 或 36 帧（`fs=`） | 守边沿，包长变 |
| VarPrioTime | `--cycle-hz 1250` | **平均** 1250 班/秒 | `VarSendSize=140` + `VarSendRest` 欠条，箱子落在 140/144 | 守班次密度，间隔可喘 |

空闲短窗口里，FixAuto(800) 和 VarPrioTime(1250) 的轨迹可以很像（都是 ~800 µs × 35/36 帧）。一有抖动：FixAuto 守格子，PrioTime 守每秒班次。Hz 作为理想周期信号**可以**隐含等间隔；API 用词仍是 Priority + variable-cycle，**不强锁每一拍边沿**。

---

## 8. 举例

### 8.1 44.1 kHz / 16-bit / 立体声，1250 Hz（有欠条）

- 1 帧 = **4 字节**；每秒 PCM = 176400 字节
- 订 1250 Hz ⇒ 平均 800 µs ⇒ \(44100 \times 0.0008 = 35.28\) 帧 = 141.12 字节

VarAuto(`800µs`) 升到 36 帧 / 144 B / ~816.3 µs（1225 Hz），`VarSendRest` 用不上。

VarPrioTime(1250)：

| 字段 | 本例 |
|------|------|
| `VarSendSize` | 140 B = 35 帧 |
| `VarSendRest` | 0.28 帧 = 1.12 B |

欠条（Bresenham）：每发一趟普通箱记 0.28 帧；凑满 1 帧，下一趟 36 帧（144 B）。约每 25 趟里 7 趟大箱。一秒仍是 44100 帧。

### 8.2 44.1 kHz / 32-bit / 2 ch，300 Hz（无欠条，实测）

SelfProfile `--cycle-hz 300`，jumbo 9000：

| 量 | 值 |
|----|-----|
| \(44100/300\) | **147 帧整除** |
| 包长 | \(147 \times 8 = 1176\) B |
| getter | `sdk_cycle=3333us` `cycle_size=1176B` `mode_sdk=variable` |
| syslog | `cy` 锁在 `3333740235` ps（3333.74 µs，约 299.96 Hz） |
| Rest | `35280/30 = 1176`；偶发 29/31 仍是 1176 B/拍 |

48 k 同一 Hz 是 160 帧 / 1280 B，同样无欠条。这就是第 1 节那个「整数微秒写不进、Hz 写得进」的格子。

### 8.3 上机怎么确认是这条路

1. `mode=` 为 `varprio` 或 `profile-varprio`；`mode_sdk=variable`
2. `cycle_hz=` 等于你给的值；`sdk_cycle` 约为 \(10^6/\text{Hz}\) 微秒
3. 整除时 `cycle_size` 钉死、`cy` 锁常数；除不尽时 `cy` 在 \(1/\text{Hz}\) 附近呼吸，Rest 的 \(A/B\) 仍等于普通箱
4. FixAuto 对照打 `fs=`，不打 `cy=`

对不上数字之前，不要删 leftover 校准，也不要动 s2d 整帧补偿。

---

## 9. 切 155 时 s2d 还要改的硬接口（PrioTime 之外）

`varprio` 本身只是 `apply_transfer_mode()` 多一个分支。对着 155 头文件编，另外两处会挂：

- `Sync::open(...)` 多第 10 参 `bool diswork`（断连 workaround）。**固定传 `false`**，对齐 150 拆除；现网 idle/release 从未出过问题，不启用、不加 CLI、不 A/B。155 头文件没有默认值，不传编不过，所以这是「行为不碰、只为编译补参」。已用 `DIRETTA_SDK_RELEASE>=155` 包起来，150 仍编 9 参。
- `Sync::Info::supportMSmode` 改名为 `SynchroSupport`（`int`：`-1` = 无/未知 Synchro，`0` = 有 Synchro 但无 MS，否则 MS1/2/3 位）。s2d 只在 `-vv` / `--diretta-debug` 用 `checkSinkSupportSynchro()` / `checkSinkSupportMSmode1/2/3()` 打能力表；控制面仍是 `is_MSmode()`。已接到日志。

`Find::Setting::Name` 删了，s2d 没设，无影响。`install.sh` 优先探测 `DirettaHostSDK_155`，其次 150。

`VarSendRest` 当 leftover 替代、`Find::MeasureMtu` 等其它 155 能力，与本 mode 分开评估。`bounded_disconnect`（`stop` + `disconnect_flgset` + 50 ms）保持现状。

---

## 10. 明确不做（本 mode 范围内）

- 不启用 `diswork`（`open` 传 `false`，不加开关）
- 不把 PrioTime 设成 `auto` 默认
- 不用 `--cycle-time` 换算 Hz
- 不把 `configTransferSizeFix` / Rapid Start / `changeWorkMode` / Info 包控制环绑进这次
- 不把手写 `VarSendRest` 暴露成 CLI
- 未在真 Target 上对表之前，不删 s2d leftover 探测和 `getNewStream` 整帧补偿

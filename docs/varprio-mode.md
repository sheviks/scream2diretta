# VarPrioTime 模式备忘（Host SDK 155）

**状态**: 已接到 CLI。rpi5 / SDK 155 A/B：SelfProfile `--cycle-hz 300`（MemoryPlay 默认）在 44.1 kHz 上 `cy` 锁死；1250 Hz 因 35.28 帧有欠条会呼吸。  
**日期**: 2026-10-07  
**SDK**: `DirettaHostSDK_155`（`ReleaseNo = 155`；厂商补丁号 155_2）

默认生产路径仍是 `auto` / `varauto` / `fixauto`。`varprio` 为显式 opt-in。150 交叉编译仍可用（选 `varprio` 会在启动时报错）。

---

## 1. 这条 API 是什么

155 新增：

```text
bool Sync::configTransferVarPrioTime(std::uint32_t Hz);
bool ProfileMaker::configTransferVarPrioTime(std::uint32_t Hz);
```

注释：`variable-cycle configuration (Priority Cycle Time)`，参数 **Cycle Hertz (Hz)**。

它挂在 **VARIABLE** 家族（变周期、定长包），和 `configTransferVarAuto` 同类；**不是** `configTransferFixAuto` 的别名。

时间合同从松到硬：

```text
FixAuto（每一拍边沿） > VarPrioTime（平均班次 Hz） > VarAuto（箱子尺寸优先）
```

包长那根轴相反：VarAuto 最锁箱，FixAuto 每拍都可以变长。

---

## 2. CLI 接线（已定）

两条 CLI 对应 SDK 里两个不同类型的入口，**禁止互转**。

| s2d CLI | SDK | 类型 | 用于 |
|---------|-----|------|------|
| `--cycle-time <us>` | `configTransferVarAuto` / `FixAuto` / `VarMax` / `Random` 的 Target Cycle Time | `ACQUA::Clock`（s2d 里 `Clock::MicroSeconds`） | 现有 mode |
| `--cycle-hz <Hz>` | `configTransferVarPrioTime` | `std::uint32_t` | **仅**新 mode |
| `--transfer-mode varprio` | 上述 PrioTime 调用 | 枚举新增值，如 `DIRETTA_TM_VARPRIO` | 显式 opt-in |

用法：

```text
--transfer-mode varprio --cycle-hz 1250
```

`--cycle-hz 1250` 的平均间隔是 800 µs，但 **不要**写成 `--cycle-time 800` 再除出来。SDK 从不读取 `--cycle-time` 去填 PrioTime。

接线约束：

- `varprio` 缺少 `--cycle-hz`：报错。不要默默拿 `--cycle-time` 换算。
- `varprio` 同时又给了 `--cycle-time`：警告并忽略 `--cycle-time`，或直接拒收。
- 非 `varprio` 却给了 `--cycle-hz`：同样警告或报错。
- `cycle_us == 0` 时的 jumbo `varmax_cycle`（约几十 Hz）**禁止**拿去当 `--cycle-hz`。
- **不要**改现在的 `auto`：`--cycle-time 800` 的 `auto` 仍走 `configTransferVarAuto`。
- `varprio` 不做一包 `safe_max` 回退、也不在 SDK 返回 false 时改走 VarAuto/VarMax。超 MTU 由 Host 一次多包；失败只打日志，`mode=` 仍是 `varprio`。

`--cycle-hz` 已是整数，不存在「µs 除不尽」问题。合法范围接线时可先对齐现有 cycle 窗口的倒数（大约 100–3000）。`0` 拒收。

`ProfileMaker`（`--target-profile-limit`）上有同名函数，那个分支同样只传 Hz。

四种显式模式在 Target Profile 下的 ModeType、`cy=` / `fs=` 算法，见 [transfer-profile-modes.md](transfer-profile-modes.md)。

---

## 3. VarSendRest：结果，不是开关

`Profile::VarSendRest` 是 155 和 PrioTime 一起出现的字段：

```text
VarSendSize   变周期、定长时每趟字节（普通箱）
VarSendRest   非 0 时表示 VarSendSize 的余数（欠条）
TypicalFrame  fixpoint 帧数（FixAuto 用这个表达 35.28）
```

`configTransferVarPrioTime(1250)` 之后，Host 按**当前** `setSinkConfigure` 的格式自己写入 Size / Rest。s2d：

- 不提供 `--varsendrest`
- 不手写 `Profile` 再 `setConfigTransfer`
- 调用后继续读 `getCycleTime()` / `getCycleSize()` / `getCyclePackets()` / `getMode()`（预期 `VARIABLE`）

第一次 A/B 把这四个打进 `-v` 即可。

---

## 4. s2d 整帧补偿不要动

`ScreamDirettaSync::getNewStream()` 里按 `getCycleSize()` 截成整帧、余数攒满再多吐一帧：发生在 **PcmRing → SDK** 交界，所有 mode 共用。它守的是环的帧边界和「平均对齐 SDK 给出的整数字节」，不改 Host 发车时刻，也不锁环水位（水位是 prefill / rebuffer）。

VarPrioTime 下若 Host 已经只要 140/144，`getCycleSize() % bpf == 0`，这层自然为 0。先留着。

---

## 5. 和 FixAuto / VarAuto 差在哪

同一段 44.1 kHz / 16-bit / 立体声，订「大约 800 µs 一趟」：

| | 你传入 | Host 锁什么 | 箱子 | 赶不上 800 µs 时 |
|---|---|---|---|---|
| VarAuto | `--cycle-time 800` → `Clock(800µs)` | 整帧包长 | 落到 36 帧 / 144 B，生成周期 ~816 µs；live `cy=` 还可晃 | 间隔继续让步 |
| FixAuto | `--cycle-time 800` → `FixAuto(800µs)` | **每一拍** 800 µs | `TypicalFrame=35.28`，每拍 35 或 36 帧（`fs=`） | 守边沿，包长变 |
| VarPrioTime | `--cycle-hz 1250` | **平均** 1250 班/秒 | `VarSendSize=140` + `VarSendRest` 欠条，箱子落在 140/144 | 守班次密度，间隔可喘 |

空闲短窗口里，FixAuto(800) 和 VarPrioTime(1250) 的轨迹可以很像（都是 ~800 µs × 35/36 帧）。一有抖动：FixAuto 守格子，PrioTime 守每秒班次。Hz 作为理想周期信号**可以**隐含等间隔；API 用词仍是 Priority + variable-cycle，**不强锁每一拍边沿**。

---

## 6. 举例：44.1 kHz / 16-bit / 立体声

### 6.1 线上

- 1 帧 = L 2 B + R 2 B = **4 字节**（bpf）
- 每秒 PCM = \(44100 \times 4 = 176400\) 字节
- Scream UDP 4010：头 5/6 字节解出格式，后面 `L R L R …`
- 发送端常见约 10 ms 一包：441 帧 = **1764 字节** 进 `PcmRing`
- Diretta 的 `getNewStream()` 按 Host 周期从小环里舀；一大勺进、一小勺出

订 1250 Hz ⇒ 平均间隔 800 µs。800 µs 里「应该」有：

\[
44100 \times 0.0008 = 35.28 \text{ 帧} = 141.12 \text{ 字节}
\]

16-bit 立体声一次必须 4 字节，只能发 **35 帧（140 B）或 36 帧（144 B）**。

### 6.2 今天：VarAuto（箱子优先）

`--transfer-mode auto`（或 `varauto`）+ `--cycle-time 800` → `configTransferVarAuto(800µs)`。

Host 先做标准箱：36 帧 = 144 B。

\[
144 / 176400 \times 10^6 \approx 816.3\ \mu s \approx 1225\ \text{Hz}
\]

之后每趟 144 B、~816 µs。账平：\(1225 \times 36 \approx 44100\)。你订的 1250 Hz 被改成 1225。`VarSendRest` 用不上。816 是生成出来的典型间隔，不是石英；VARIABLE 下 `cy=` 还会晃。

### 6.3 155：VarPrioTime（班次优先）

```text
--transfer-mode varprio --cycle-hz 1250
```

合同：一秒 1250 趟。35.28 帧装不进一只箱子，Host 拆成：

| 字段 | 角色 | 本例 |
|------|------|------|
| `VarSendSize` | 普通箱 | 140 B = 35 帧 |
| `VarSendRest` | 每趟欠的零头 | 0.28 帧 = 1.12 B |

欠条（Bresenham）：每发一趟普通箱记 0.28 帧；凑满 1 帧，下一趟 36 帧（144 B）。

| 趟 | 欠条（发前） | 装 | 字节 | 欠条（发后） |
|----|--------------|----|------|--------------|
| 1 | 0.00 | 35 帧 | 140 | 0.28 |
| 2 | 0.28 | 35 帧 | 140 | 0.56 |
| 3 | 0.56 | 35 帧 | 140 | 0.84 |
| 4 | 0.84 | **36 帧** | **144** | 0.12 |
| 5 | 0.12 | 35 帧 | 140 | 0.40 |

约每 25 趟里 7 趟大箱（\(0.28 \times 25 = 7\)）。一秒：约 900 小箱 + 350 大箱 = 44100 帧，\(1250 \times 800\,\mu s = 1.000\,s\)。

Scream 那包 1764 B（441 帧）大约够 **12.5** 趟 Diretta（\(441 / 35.28\)）。

### 6.4 上机怎么确认是这条路

对同一 DAC、同一 44.1/16：

1. `getCycleTime()` 是否贴 800 µs 量级（相对 VarAuto 的 ~816）
2. `getCycleSize()` 是否在 140 / 144 间跳（定长班车 + 欠条）
3. `getMode()` 是否 `VARIABLE`
4. 忙时 `cy=`：允许散；FixAuto 对照应钉在 800、包长在跳

对不上数字之前，不要删 leftover 校准，也不要动 s2d 整帧补偿。

---

## 7. 切 155 时 s2d 还要改的硬接口（PrioTime 之外）

`varprio` 本身只是 `apply_transfer_mode()` 多一个分支。对着 155 头文件编，另外两处会挂：

- `Sync::open(...)` 多第 10 参 `bool diswork`（断连 workaround）。**固定传 `false`**，对齐 150 拆除；现网 idle/release 从未出过问题，不启用、不加 CLI、不 A/B。155 头文件没有默认值，不传编不过，所以这是「行为不碰、只为编译补参」。已用 `DIRETTA_SDK_RELEASE>=155` 包起来，150 仍编 9 参。
- `Sync::Info::supportMSmode` 改名为 `SynchroSupport`（`int`：`-1` = 无/未知 Synchro，`0` = 有 Synchro 但无 MS，否则 MS1/2/3 位）。s2d 只在 `-vv` / `--diretta-debug` 用 `checkSinkSupportSynchro()` / `checkSinkSupportMSmode1/2/3()` 打能力表；控制面仍是 `is_MSmode()`。已接到日志。

`Find::Setting::Name` 删了，s2d 没设，无影响。`install.sh` 只要旁边还有 `DirettaHostSDK_150` 就会锁 150，切 155 要改 `DIRETTA_SDK_ROOT` / 探测顺序。

`VarSendRest` 当 leftover 替代、`Find::MeasureMtu` 等其它 155 能力，与本 mode 分开评估。`bounded_disconnect`（`stop` + `disconnect_flgset` + 50 ms）保持现状。

---

## 8. 明确不做（本 mode 范围内）

- 不启用 `diswork`（`open` 传 `false`，不加开关）
- 不把 PrioTime 设成 `auto` 默认
- 不用 `--cycle-time` 换算 Hz
- 不把 `configTransferSizeFix` / Rapid Start / `changeWorkMode` / Info 包控制环绑进这次
- 不把手写 `VarSendRest` 暴露成 CLI
- 未在真 Target 上对表之前，不删 s2d leftover 探测和 `getNewStream` 整帧补偿

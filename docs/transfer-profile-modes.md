# Target Profile 下的四种发送模式

**日期**: 2026-10-07
**SDK**: Host SDK 155（`ReleaseNo = 155`）
**A/B**: 同一 Target、jumbo MTU 9000、44.1 kHz / 32-bit / 2 ch（bpf=8，码率 352800 B/s），`--target-profile-limit 200`

本文记录 `varmax` / `varauto` / `fixauto` / `varprio` 在 Target Profile 下的合同、`ModeType`、以及 `--target-info` 里 `cy=` / `fs=` 的算法。`auto` / `autofix` 是 s2d 自己的调度器，不在这四条里。

`varprio` 为什么用 Hz、要解决 44.1 k / 48 k 两族在整数微秒格子上踩不中 300 Hz 的问题，见 [varprio-mode.md](varprio-mode.md)。

---

## 1. 两条配置路径

`--target-profile-limit` 是微秒整数，≥ 0。它只换 **Host 侧谁来填 Profile**，不把调度权交给 Target。

| 值 | 路径 | s2d 做什么 |
|----|------|------------|
| **0**（默认） | SelfProfile | 直接 `Sync::configTransfer*` |
| **> 0** | Target Profile | `getProfileMaker(limitCycle)` → 一个 `configTransfer*` → `setConfigTransfer` |

`limit` 是 Host 允许的**最短发送间隔**（最快发送频率）：200 µs → 上限 5000 Hz。Maker 再和 Target 信封（min/max cycle、ProfileID、最大帧长）以及所选配方求交。Target 只收包、回报 `Stream Rest` / `info rcv`。

`-v` 的 `mode=`：SelfProfile 是 `varauto` / `fixauto` / `varmax` / `varprio`；Target Profile 带 `profile-` 前缀（`profile-varauto` 等）。`mode_sdk=` 是 `Sync::getMode()` 读回的 `Profile::ModeType`。

---

## 2. 四种模式的内涵与 ModeType

SDK `Profile.hpp`：

- `VarSendSize != 0` → **variable-cycle / fix-size** → `ModeType::VARIABLE`
- `VarSendSize == 0` → **fix-cycle / variable-size** → `ModeType::FIX`

恒等式（码率固定时）：

\[
\text{包长（字节）} = \text{采样率} \times \text{bpf} \times \text{周期}
\]

钉住一边，另一边是因变量。

| CLI | limit>0 的 Maker 调用 | `mode=` | `mode_sdk` | 钉住的量 | 因变量 |
|-----|----------------------|---------|------------|----------|--------|
| `varmax` | `configTransferSizeMax()` | `profile-varmax` | **variable** | 满 MTU 包长 | 周期 = 包长 / 码率 |
| `varauto` | `configTransferVarAuto(cycle)` | `profile-varauto` | **variable** | `--cycle-time` 量化成整帧盒 | 周期随盒变 |
| `varprio` | `configTransferVarPrioTime(hz)` | `profile-varprio` | **variable** | 平均 `--cycle-hz` 班次；盒 = 整帧，余数进 `VarSendRest` | 单拍间隔可呼吸 |
| `fixauto` | `configTransferFixAuto(cycle)` | `profile-fixauto` | **fix** | `--cycle-time` 每一拍边沿 | 这一拍帧数（整帧跳） |

时间合同从硬到松：FixAuto（每一拍边沿）> VarPrioTime（平均 Hz）> VarAuto（箱子优先）> SizeMax（满包，周期被几何唯一确定）。

SelfProfile（limit=0）上这四个名字对应的 SDK 调用：

| CLI | limit=0 |
|-----|---------|
| `varauto` | `Sync::configTransferVarAuto(cycle)` |
| `fixauto` | `Sync::configTransferFixAuto(cycle)` |
| `varprio` | `Sync::configTransferVarPrioTime(hz)` |
| `varmax` | `Sync::configTransferVarMax` + s2d 一包 shrink |

varmax 在 Maker 上没有 `VarMax`，满包配方是 `SizeMax()`。SelfProfile 的 VarMax 还走 s2d 的一包上限；Maker 的 SizeMax 不走那层。jumbo 上本来就是一包时，两条路几何相同。

---

## 3. 为什么有的打 `cy=`，有的打 `fs=`

Host 头文件**没有**规定 syslog 字段名。观测上：VARIABLE 族打 `cy=`，FIX 族打 `fs=`。它们是同一恒等式的两个因变量。

| `mode_sdk` | syslog | 含义 | 单位 |
|------------|--------|------|------|
| `variable` | `cy=` | **这一拍实际持续了多久**（班次间隔） | 皮秒 |
| `fix` | `fs=` | **这一拍发出的帧数**（可带小数，对齐 `TypicalFrame`） | 帧 / 拍 |

三个小小数（`info rcv 2  0.0000  0.0000  0.0078`）是相位/误差一类的量。`cy=` 本身不是时间戳：时间戳会一直涨，varmax 的 `cy` 会锁成一个常数。

`Stream Rest A B C` 里 `A/B` 应等于这一拍字节数（varmax 的 `71936/8 = 8992`，varprio 的 `35280/126 = 280`）。B 在相邻整数间跳一格，是 Target 水线差一拍，不是包长在变。

---

## 4. `cy` 的算法（以 varauto 817 µs 为例）

条件：`--transfer-mode varauto --cycle-time 800`，44.1 kHz / 32-bit / 2 ch。

**① 格式**

- 1 帧 = 8 B
- 码率 = 44100 × 8 = 352800 B/s

**② 把 800 µs 变成整帧盒**

\[
44100 \times 0.0008 = 35.28 \text{ 帧}
\]

35 帧的周期是 \(35/44100 = 793.65\) µs，比 800 更短。VarAuto 进到 **36 帧**：

\[
36 \times 8 = 288 \text{ B}
\]

这就是 `-v` 的 `cycle_size=288B`。

**③ 周期由盒和码率唯一确定**

\[
t = \frac{288}{352800} = \frac{36}{44100} = 816.326530\ldots\ \mu s
\]

`getCycleTime()` 报到微秒 → **`sdk_cycle=817us`**。817 是取整，几何真值是 816.33 µs。

**④ `cy=` 就是这个 \(t\)，换成皮秒**

\[
cy = t \times 10^{12} = 816\,326\,531 \text{ ps}
\]

换算：

\[
\text{µs} = cy / 10^{6},\qquad \text{Hz} = 10^{12} / cy
\]

\(10^{12} / 816326531 \approx 1225\) Hz = \(44100 / 36\)。线上 `cy` 会在 815.6～816.8 µs 附近呼吸：VARIABLE 允许周期当因变量，Host 不是石英格子。

varmax 同一公式，盒换成满 MTU：

\[
t = \frac{8992}{352800} = 25487.5\ \mu s \quad\Rightarrow\quad cy \approx 25478666016 \text{ ps}
\]

码率不变则这个常数锁死。varprio 1250 的 getter 是 \(10^6/1250 = 800\) µs，线上 `cy` 呼吸在 ~793 µs（约 1260 Hz），盒钉在 280 B（35 整帧），0.28 帧走 `VarSendRest`。

---

## 5. `fs` 的算法（以 fixauto 800 µs 为例）

条件：`--transfer-mode fixauto --cycle-time 800`，同一格式。

FixAuto 钉 **每一拍 800 µs**。这一拍定额帧数：

\[
fs = 44100 \times 0.0008 = 35.28
\]

这就是 syslog 的 `fs=35.28`，和 `Profile.TypicalFrame`（注释写 fixpoint typical frame count）同一量纲。它是**这一拍的帧数**，不是每秒帧数（每秒帧数是采样率 44100）。

线上包必须整帧，所以实际这一拍是 35 帧（280 B）或 36 帧（288 B）。getter：`mode_sdk=fix`，`sdk_cycle=800us`，`cycle_size=280B`（快照落在 35 整帧）。FIX 族打 `fs=`，因为拍长已钉死，因变量是这一拍几帧。

核对：\(35.28 \times (1 / 0.0008) = 44100\)。

---

## 6. 本次 A/B 读回（limit=200，jumbo 9000）

| CLI | Maker | `mode_sdk` | getter | syslog |
|-----|-------|------------|--------|--------|
| varauto + cycle 800 | `VarAuto(800µs)` | variable | 288 B / 817 µs | `cy≈` 816.3 µs，Rest 122/123 均 = 288 B |
| fixauto + cycle 800 | `FixAuto(800µs)` | fix | 280 B / 800 µs | `fs=35.28` |
| varmax | `SizeMax()` | variable | 8992 B / 25488 µs | `cy` 锁 `25478666016`，Rest `71936/8=8992` |
| varprio + hz 1250 | `VarPrioTime(1250)` | variable | 280 B / getter 800 µs | `cy` 在 792.5–794.4 µs 呼吸，Rest 125/126/127 均 = 280 B |

四条都没有 `min_cycle=`（`getMinCycleTime()` 仍为 0）。limit=200 µs 远短于 800 µs 和 25.5 ms，cap 空闲。

stats：varprio / fixauto 稳态约 6303 cycle / 5.002 s ≈ **1260 Hz**（相对 1250 的 Host 略快，与 varprio 2000 → ~2005 同类）。varmax 约 196 cycle / 5.002 s ≈ **39.2 Hz**。

---

## 7. 为什么 limit=200 和 limit=0 测出来一样

这四条在两条路径上调用的是**同名配方**（varmax 是 VarMax ↔ SizeMax 这一对满包 API）。limit=200 是 5000 Hz 天花板；这次拍长全在 800 µs～25.5 ms，都慢于上限。这台 Target 的 `getMinCycleTime()` 在 limit>0 时仍是 0，信封没有另塞一个更长的最小周期。jumbo 上 varmax 本来就是一包，SelfProfile 的一包 shrink 也打不着。

所以 `-v` 的 `cycle_size` / `sdk_cycle` / `mode_sdk` 和 syslog 的 `cy=` / `fs=` 会重合。能看出走了哪条路的，是 `mode=` 有没有 `profile-` 前缀。

几何会分开的情况：

- **limit ≥ 想要的拍长**。例如 `--cycle-hz 1250`（800 µs）却 `limit=2000`，Maker 应按 2000 µs 上限把频率压下来。要对齐：`limit` ≤ 目标周期（2000 Hz → limit ≤ 500；FixAuto 800 → limit ≤ 800）。
- **`auto` / `autofix`**。limit=0 走 s2d 调度（码率/DSD/`safe_max` 在 VarAuto 与 VarMax 之间切）；limit>0 时 `auto` 调 `configTransferAuto(cycle)`，`autofix` 直接 `FixAuto`。不要用这两条去对照 ModeType。
- **SelfProfile varmax 的一包 shrink 真的触发了**（小 MTU / 高码率），Maker SizeMax 仍按满包。
- **Target 信封真的写入 min cycle / ProfileID**（`min_cycle=` 出现且大于 0）。
- **只存在于 Maker 的配方**（s2d 目前没接线）：无参 `configTransferAuto()`、`Fix`、`Var`、`SizeFix`、`setForceFragment`。

默认保持 `--target-profile-limit 0`。200 是 AlsaHost inf 里常见的 `TargetProfileLimitTime`，作实验值够用，本身不会改这四条的盒子。

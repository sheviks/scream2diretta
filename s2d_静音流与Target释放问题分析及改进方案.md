# s2d 静音流导致 Target 长期占用问题分析与改进方案

**文档版本：0.5**　|　状态：#1 暂停 / #2 释放 / #3 重连崩溃修复 **已解决**；#2b 静音内容检测 **待优化**

## 1. 问题背景与现象

s2d（scream2diretta）作为 Scream UDP 到 Diretta 的持续传输桥，在上游（screamalsa + mpd 等）暂停或停止播放时，存在以下行为：

- 上游停止推送真实 PCM 数据后，PcmRing 逐渐被消费侧耗尽。
- 进入 rebuffer / underrun 状态，`getNewStream()` 持续合成静音（silenceByte）并提供给 Diretta SDK。
- SDK 持续将静音流发送给 Target，导致 Target/DAC 被长期“占用”，即使实际没有音频内容。
- 现象：
  - Target 端（GentooPlayer + closed-source diretta_app_target）USB IRQ（core 0，xhci for DAC）占用率显著升高。
  - 即使是低采样率（44.1kHz）+ 较长 VarMax cycle（4000+ µs），播放时 IRQ 也会异常高。
  - 重启 s2d 或 DAC 只能暂时缓解，**只有重启整个 Target 机器才能恢复正常**。
  - 长时间静音流被怀疑导致 USB 与 ALSA 之间的时钟漂移累积，驱动/端点状态进入“亚健康”。

s2d 设计为“持续桥”，缺乏歌曲边界和主动释放机制，与 DRUP 等有明确播放状态的实现形成对比。

## 2. Diretta 设计哲学回顾

Diretta 核心目标：
- **Averaging of processing and reduction of fluctuations in power consumption**。
- Host 通过“as often as possible at constant short intervals”向 Target 发送数据，让 Target 端负载和功率更均匀。
- 强调避免 bursty（突发）和长时间波动。
- Target 端的电气稳定性（电源轨、EMI、时钟）对音频质量敏感，持续活动或不规律活动都可能引入噪声。

SDK（pull 模式 via getNewStream）本身**不主动生成静音流**，完全由上层应用决定提供什么数据。SDK 只传输上层提供的 buffer。

## 3. s2d 与 DRUP 在“暂停/无真实 PCM”时对 SDK 的指令差异

站在 SDK 视角对比：

### s2d（上游无真实 PCM 时）
- **从不调用 stop()**。
- Sync 保持 active 状态，SDK 按配置的 cycle time 持续调用 `getNewStream()`。
- s2d 在 `getNewStream()` 中：
  - 检测 ring 不足或 rebuffer 条件。
  - 直接 `memset(dest, silenceByte, want)`（或 popOrSilence 补齐静音）。
  - 将静音 buffer 实时提供给 SDK（`s.Data.P = ...; s.Size = ...`）。
- 结果：SDK 持续收到“这是静音数据”的指令，长期向 Target 输出静音流。
- 直到上游恢复真实数据，ring 重新积累到 rebuffer_target 后才切换回真实 PCM。
- **无有限静音 burst，无 stop()，无主动 release**。

### DRUP（Pause / Stop 时）
- **使用包装方法 + 有限指令**：
  - `pausePlayback()` / `stopPlayback(false)`：
    - 调用 `requestShutdownSilence(N)`（有限数量，例如 PCM 10-20 个、DSD 30-50 个）。
    - 在 `getNewStream()` 中：当 `silenceBuffersRemaining > 0` 时，`memset` 静音并递减计数器。
    - 发完有限 buffer 后，调用 `stop()`（SDK 指令，停止活跃数据流）。
  - 设置内部 paused/stopped 标志。
  - `getNewStream()` 后续不再提供持续数据（或 worker 停止）。
- **结果**：
  - SDK 只收到有限个静音 buffer，然后被明确 `stop()`。
  - 不会长期持续输出静音流。
  - 连接可能保持 open（用于 quick resume），但数据流已停止。
- **释放逻辑**：
  - 播放列表结束：立即 `release()`（关闭 SDK 连接）。
  - Stop 后：启动 5 秒 idle 计时器，超时后 `release()`。
  - Pause 时：保持连接（不 release），但不持续喂数据。
- SDK 收到的指令是“发这 N 个静音，然后 stop 流”。

**根本区别**：
- DRUP 通过上层状态机（UPnP Play/Pause/Stop/End）主动控制：有限静音 + stop() + 必要时 release()。
- s2d 因为“持续桥”设计，缺乏状态边界，上游停止时只能被动依赖 ring 状态，持续在 getNewStream 中提供静音。
- 两者都没有让 SDK “自己产生静音流”；静音均由上层应用在 getNewStream buffer 中合成。

## 4. 问题根源与影响

- **长期静音流** 让 Target 端的 USB isochronous 端点 + ALSA 持续处于活跃/轻载状态。
- 没有真实音频反馈时，软件时钟与 DAC USB 时钟容易漂移累积。
- 状态在内核驱动、buffer、IRQ 处理中“脏化”。
- 重启 s2d（流中断）或 DAC（USB 断连）只能临时缓解，**target 端状态需整机重启才能清零**。
- 在 10M + 低码率 + 显式短 cycle 场景下，问题依然存在，因为核心是“连接 + 数据流持续存在”而非速率本身。
- 与 Diretta 哲学冲突：持续可预测的静音仍是一种“持续活动”，而非让 Target 真正 idle。

## 5. DirettaAlsaHost 参考

DirettaAlsaHost **完全没有歌曲边界**，纯基于 **ALSA 活动**：

- 内核创建虚拟 ALSA 声卡，上层应用写数据。
- bridge 监控 ALSA writes、period、underrun 等。
- 配置项：
  - `alsaUnderrun=enable`：underrun 时发静音。
  - `alsaUnderrunSleep`、`alsaUnderrunClear` 等。
- 当播放程序停止写 ALSA（或持续 underrun），bridge 能检测到“不活跃”，停止或收敛向 Diretta 的推送。
- 类似“ALSA 活动检测”而非事件驱动。
- 释放/停止逻辑依赖 ALSA 侧是否有持续写入。

**启发给 s2d**：既然 s2d 输入是 Scream，可基于“真实 PCM 是否被持续推送”来检测“上游活动”，而非固定时间或歌曲事件。

## 6. s2d 可改进的方法（推荐方案）

目标：**在短暂停时快速恢复，在长时间/累积无数据时主动释放 Target**，同时尽量减少静音流时长。

### 核心改进逻辑（可加到 s2d 配置与 producer/consumer 监控中）

1. **检测机制**：
   - 维护 `last_real_push_time`（最后一次 push **真实非静音 PCM** 的时间戳，可基于现有 `last_pcm_packet_at` 增强，过滤纯静音场景）。
   - 同时检测是否处于“只合成静音”状态（rebuffering + producer 未推真实数据）。
   - 结合 `source_gap_ms`（上游多久没给数据）。

2. **触发动作（上游停止发送真实音频时）**：
   - 检测到连续无真实数据超过阈值：
     - 先发**有限 shutdown silence**（模仿 DRUP `requestShutdownSilence(N)`，例如 20-50 个 buffer），flush pipeline。
     - 调用 `stop()`（或等效）停止当前活跃流。
     - 启动 idle release 计时器。
   - 超时后调用 `release()` 彻底释放 Target（关闭 SDK 连接）。

3. **配置参数建议**（添加到命令行/配置文件）：
   ```
   --upstream-idle-timeout-sec 300     # 连续无真实数据多少秒后触发（默认 300=5min，可调 60-600）
   --idle-release-delay-sec 30         # 触发后额外等待多少秒再 release（参考 DRUP 5s）
   --shutdown-silence-buffers 30       # 有限静音 buffer 数量
   --enable-idle-release true          # 是否启用
   ```

4. **状态管理**：
   - **短暂停**（< timeout）：**不 release**，保持连接，支持快速 resume（利用现有 rebuffer 恢复逻辑）。
   - **长时间/累积无数据**（> timeout）：release，让 Target 端 ALSA/USB 有机会 idle/reset。
   - 每次有真实 PCM push 时，重置 idle 计时器（避免“少量多次”直接累积成一次长时）。
   - 暂停期间仍可输出有限静音做 cleanup，但不要进入无限 rebuffer。

5. **恢复流程**：
   - 上游恢复真实数据 → producer 推新 PCM → 如果之前已 stop/release，则重新 open/connect + play()。
   - 利用现有 rebuffer 机制：新数据积累到目标量后切换回真实 PCM。

6. **额外增强**：
   - 在 rebuffer 期间增加日志：记录“连续静音时长”“是否即将 release”。
   - 支持在长时间 idle 时更激进地降低 cycle（或暂停 SDK 侧调用），减少 Target 端活动。
   - 如果想更接近 DirettaAlsaHost，可监控“真实数据 push 速率”，当 push 速率低于阈值一段时间即视为“不活跃”。
   - 考虑在 release 前确保发完 shutdown silence，避免 abrupt 断开。

### 7. 边界设定与累积风险建议

- **连续 idle 时间**作为触发条件，而非累计暂停时长。
  - 每次真实数据 push 重置计时器。
  - 这样“6 次 5 分钟 + 中间有播放”不会直接等同“30 分钟连续”。
- 推荐初始值：
  - 低码率（44.1-96kHz）：300-600 秒（5-10 分钟）。
  - 可做成可配置，甚至支持“按采样率动态”或“按用户场景 profile”。
- 短暂停不触发 release，保留快速恢复优势。
- 释放后，下次需要时 s2d 已有自动 reconnect 能力，影响可控。

### 8. 参考实现与验证

- **DRUP 参考**：`IDLE_RELEASE_TIMEOUT_S = 5` + `m_idleTimerActive` + `release()`；pause 用有限 silence + stop()；playlist end 立即 release。
- **DirettaAlsaHost 参考**：纯 ALSA 活动检测（writes/underrun），无歌曲概念。可在 s2d 侧实现“真实 PCM push 活动检测”。
- **验证方式**：
  - 在 target 端监控 `/proc/interrupts`（xhci 相关）和 ALSA buffer 状态。
  - s2d 侧加详细日志：idle 时长、是否已 release。
  - 测试：短暂停（< timeout）、长暂停、多次短暂停、10M 链路下的表现。
  - 对比使用 DRUP 时的行为。

### 9. 其他补充建议

- **10M + 低码率场景**：天然适合较长 cycle + 主动 release。结合显式 `--cycle-time` 控制节奏，同时用 idle 逻辑避免长期占用。
- **如果不想改 s2d 源码**：外层脚本监控 s2d stats/logs（source_gap、silent_cycles、producer 状态），超时 kill/restart s2d 服务（强制断连接）。
- **潜在副作用**：release 后恢复会有短暂 reconnect 开销（handshake、buffer 预热）。超时不要设太短（建议至少 1-2 分钟）。
- **终极健康状态**：Target 端在无活跃音频时应能真正 idle/suspend USB/ALSA（受闭源限制，host 侧 release 是最有效手段）。

---

## 10. 实现进度与后续增强

### 10.1 已解决：#2 上游 idle 超时释放 Target（commit a56d547）

按本文第 6 节推荐方案的"主动 release"路径实现，已在 RPi5 编译测试通过：

- 新增 CLI `--upstream-idle-timeout-sec`（默认 **120 秒**，0 = 禁用，范围 10..3600）。
- 检测：当 Sync 已进入稳定播放（`stream_started`）且 `now - last_pcm_packet_at >= timeout` 时触发。
- 动作：复用现成的 `teardown_sync_for_runtime()`（`deactivate()` + `stop()` + 异步 `disconnect_flgset/close/delete`），等于彻底停止向 Target 推送任何数据（含静音），让下游 USB/ALSA 有机会 idle。
- 恢复：teardown 时置 `reconnect_pending=true`，PcmRing 保留；上游恢复送数据后走现成的 `try_reconnect_same_format()` 自动重开同格式 Sync（与 sink-lost 自动重连同一路径）。
- 关键实现点：上游**完全停推**时不会有 UDP 包到达，`diretta_output_send()` 不被调用，因此 idle 检测放在接收循环空数据分支的心跳 `diretta_output_tick()` 里，由 network.c 的 500ms `select()` 超时驱动。

**已知盖不住的场景**：见 10.4（#2b）。

### 10.2 已解决：#1 上游 idle 短超时暂停 Target（commit ea0f853）

#2（120s 释放）之前再加一级更短horizon 的"暂停"，对齐 DRUP pause 语义——停止驱动 Target 但保持连接，便于秒级恢复：

- 新增 CLI `--upstream-pause-timeout-sec`（默认 **5 秒**，0 = 禁用，范围 1..600）。
- 检测：同样在 `diretta_output_tick()` 心跳里，`stream_started` 且 idle 达到 pause 阈值、但**未达** release 阈值时触发（`g_st.paused` 保证一个 idle 周期只 pause 一次）。
- 动作：`g_st.sync->stop()`（SDK "stop playback(pause)"，停止 send 线程调 `getNewStream()`）+ `deactivate()`（getNewStream 不再碰 ring）。**连接不拆**，`connectState` 保持 CONNECT。
- 恢复：在 `diretta_output_send()` 收到真实 PCM 包时，若 `g_st.paused` → 重放同一 Sync：`queue.clear()` + `resetGate()`（重新武装 mute/prefill/real-delay 启动门控）+ `activate()` + `play()`，使恢复时先走一小段有界静音再出真实 PCM，模拟 fresh open。
- 升级：若 idle 持续到 `--upstream-idle-timeout-sec`，pause 自动升级为 #2 的完整 release（teardown）。任何 teardown 都会清 `g_st.paused`。
- 已知局限：pause 只 `stop()` 不 `close()`，`connectState` 仍是 CONNECT，SDK keep-alive 让 Target USB 端点保持温热（实测 ~4-5% CPU 占用）。**真正让 USB idle 的是 #2 release**（拆 `connectState`）。USB 占用是"连接层"现象，与"数据层是否在发静音"无关。若主要诉求是降 USB 负载，可把 `--upstream-idle-timeout-sec` 往 DRUP 的 5s 方向调短。

### 10.3 已解决：#3 重连崩溃导致"开头吞音"（commit ea0f853）

**症状**：>2min idle release（#2）后重连，听感"开头明显被吞掉一段"。

**根因**：`diretta_output_send()` 的 `if(!g_st.sync)` reconnect-pending 块里，`try_reconnect_same_format()` **成功**分支只设 `reconnect_backoff_ms=750` 却**没有 `goto ingress_only`**，落到下面 `g_st.sync->is_connect()`。但 reconnect 是**异步** open，`g_st.sync` 此刻仍是 nullptr（要等后续 `poll_async_sync_open()` 安装）→ 空指针解引用 → 进程 SIGSEGV → systemd 重启 → 整个 PcmRing 丢失 → 歌曲头丢掉。

**修复**：成功分支补 `goto ingress_only;`，当周期以 ingress-only 退出，不假设 sync 已就绪。

**说明**：这是 pre-existing bug（a56d547 即存在），format-change-open 路径侥幸没崩（它把 `reconnect_pending=false`，于是 `if(!g_st.sync)` 走安全的 else→goto）；#2 idle release 按固定节奏武装 reconnect_pending，让此 bug 必现。

### 10.4 待优化（pending）：#2b 上游"静音流"内容检测

**问题**：部分支持 Scream 协议的上游软件（如 **Album Player、ap2renderer**）在**暂停**时不会停止发包，而是**持续输送静音 PCM**（全零或近似全零）。

**#2 在此场景失效的原因**：s2d 当前判断"上游是否活跃"的唯一依据是 `last_pcm_packet_at`，它**只要收到 UDP 包就刷新，不检查包内容**（`src/diretta.cpp` 当前 ingress 路径无任何静音/全零检测）。因此上游持续送静音时：
- `last_pcm_packet_at` 一直被刷新 → #2 的 idle 超时**永远不触发**；
- 静音流原样转发给 Target，USB 仍被持续驱动 —— 即最初要解决的问题在"上游主动送静音"下依然存在。

这正是本文第 6.1 节建议把检测升级为 `last_real_push_time`（最后一次推**非静音** PCM 的时间）的动机。

**#2b 设计要点**（若决定实施）：
1. **检测**：静音 PCM = 全零字节（LE 下 16/24/32-bit 静音均为 0x00）。在 `diretta_output_send()` ingress 加一个轻量全零扫描（`memchr`/SIMD，微秒级）。扫到非零 → 刷新 `last_real_push_time`；全零 → 不刷新。
2. **触发改用 `last_real_push_time`**：#2 的超时判断从 `last_pcm_packet_at` 切到 `last_real_push_time`，使"上游送静音 N 分钟"与"上游完全不送 N 分钟"被一视同仁地触发 release。
3. **重连侧也必须门控"非静音"**（关键，否则死循环，见下）：reconnect 触发条件从"收到任何包"改为"收到**非静音** PCM"。
4. **新增独立开关**（如 `--idle-detect-silence`），默认行为保守，避免影响纯 #2 的既有语义。

**上游链路在 release 后会怎样（重要前提）**：Scream 是**纯 UDP 单播，无连接、无握手、无回包、无序列号**。s2d 与上游之间**没有"连接"可断**，与 Target 之间才有 Diretta 协议握手。因此 #2b 释放 Target（断开的是 Diretta 链路）时：
- **上游发送端完全无感**：它是 fire-and-forget，不知道下游是否有人在收，会**继续往 UDP 4011 blast 静音包，不会断开、不报错、不停止**。
- **s2d 的 UDP socket 保持打开**：接收线程照常 recvmsg 收下静音包，只是（#2b 下）识别为静音 → 不刷新 `last_real_push_time`、不重连。
- **Diretta 链路已 teardown**：Target 收到协议层断开通知 → 不再收到任何数据（含静音）→ USB/ALSA 可 idle。这正是目标状态。

**风险与坑**：
- **⚠️ release ↔ reconnect 抖动死循环（必须避免）**：当前重连触发是 `reconnect_pending && have_last_fc && data->audio_size > 0`（`src/diretta.cpp:3547`），即**只要来任何包就重连**。若 #2b 只改"释放"而不改"重连"，会立刻死循环：检测到持续静音 → release → 下一个静音包到达（`audio_size>0`）→ 立即重连 → 又是静音 → 再 release → …。因此 #2b **必须同步把重连条件门控为"收到非静音 PCM 才重连"**，使上游暂停送静音期间 Target 始终保持 release，只有真正恢复播放（出现非零 PCM）的那一帧才触发重连。
- **并非所有"静音"都是全零**：dither / 极低电平噪声 / DC offset 会让"听感静音"仍带非零字节，纯 `==0` 检测只认**数字绝对静音**。Album Player / ap2renderer 暂停时大概率是纯零，但**需先抓包确认**（`tcpdump -X` 或 s2d 现成的 `--dump-raw-entry-wav` tap）。带 dither 则需改用"绝对值 < 阈值"的启发式。
- **必须用"持续 N 秒全零"做触发，不能见零就动**：真实音乐含整段静音（古典弱奏、轨间空白），见零即 release 会导致音乐中途被误断。
- **恢复无缝**：静音切回真实音频时，第一帧非零立即刷新 `last_real_push_time` 并通过上面门控后的 reconnect 路径触发重开（#2 的 teardown/reconnect 机制已具备，#2b 只需把"见包重连"收紧为"见非静音重连"）。

**前置验证步骤**：在 RPi5 上抓一段 Album Player / ap2renderer **暂停时**的 Scream UDP payload，确认是否为纯零，以决定检测能否用简单 `==0` 还是需要阈值启发式。

**决策状态**：**暂缓**。#1（5s pause）/ #2（120s release）/ #3（重连崩溃）已落地，先实测一段时间，观察 Target USB IRQ 的 CPU 占用是否缓解；若"上游送静音"场景仍是主要诱因，再实施 #2b。

通过以上改进，s2d 可以在保留"持续桥 + 快速恢复"优势的同时，显著降低长期静音流对 Target 的影响，使其行为更接近 DRUP 的健康模式。

---

文档生成于 2026 年。基于 s2d/DRUP 源码行为、用户日志观察及 Diretta 哲学分析。如需进一步代码改动细节或测试脚本，可继续讨论。

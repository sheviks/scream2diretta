# CPU Affinity Overlap Detection - Implementation

## 修改文件
`src/diretta.cpp`

## 新增函数
```cpp
static void validate_cpu_affinity_config(const diretta_config_t& cfg)
```

位置：`diretta_config_init()` 之后，`diretta_list_targets()` 之前（约 3445 行）

## 调用位置
`diretta_output_init()` 中，在 `init_syslog()` 之后、target 发现之前（约 3638 行）

---

## 检测逻辑

### 1. 致命冲突：`--cpu-scream == --cpu-audio`

**行为**：
- 打印详细错误信息（无条件输出，不依赖 `-v`）
- `exit(1)` 拒绝启动

**错误信息**：
```
════════════════════════════════════════════════════════════════
  ERROR: CPU Affinity Configuration Conflict
════════════════════════════════════════════════════════════════

  --cpu-scream=3 and --cpu-audio=3 are both set to the same core.

  Why this fails:
    Both threads run at RT priority (SCHED_FIFO) and would compete
    for the same CPU, causing constant context switches (~1-5µs each)
    and audio glitches (underruns, dropouts).

  How to fix:
    1. Auto-configure (recommended):
         sudo ./scripts/diagnose-system.sh

    2. Manual configuration:
         --cpu-scream=3 --cpu-audio=2 --cpu-other=0
       (allocate each hot-path thread to a different core)

    3. Let threads float (removes explicit pinning):
         Remove both --cpu-scream and --cpu-audio
       (threads will be scheduled by the kernel)

════════════════════════════════════════════════════════════════
```

**理由**：
- 两个线程都是 `SCHED_FIFO` RT priority
- 绑在同一核心 = 强制上下文切换
- 必然导致音频问题，无合法使用场景
- 早失败优于晚失败（启动时拒绝 > 运行时性能差）

---

### 2. 非致命冲突：`--cpu-audio == --cpu-other`

**行为**：
- 打印警告信息（无条件输出，不依赖 `-v`）
- **继续启动**（non-fatal）

**警告信息**：
```
[diretta] WARNING: --cpu-audio=2 and --cpu-other=2 overlap
  RT audio worker (SCHED_FIFO) will preempt helper threads (SCHED_OTHER).
  Audio should not be affected, but helpers may be delayed.
  Consider isolating them: e.g. --cpu-audio=2 --cpu-other=0
```

**理由**：
- SDK audio worker 是 `SCHED_FIFO`（RT）
- SDK helper threads 是 `SCHED_OTHER`（CFS）
- RT 线程会抢占 CFS 线程，音频不受影响
- Helper 被饿死不影响音频（它们只做控制平面工作）
- 仅是次优配置，不是错误

---

### 3. 无冲突

**场景**：
- `--cpu-scream` 和 `--cpu-audio` 未同时设置
- 或设置为不同核心

**行为**：
- 静默通过，无任何输出

---

## 测试场景

### 场景 1：正确配置（无输出）
```bash
./scream2diretta -t 1 --cpu-scream=3 --cpu-audio=2 --cpu-other=0
# → 正常启动，无检测输出
```

### 场景 2：未设置 affinity（无输出）
```bash
./scream2diretta -t 1
# → 正常启动，cpu_scream/audio 都是 -1，跳过检测
```

### 场景 3：致命冲突（exit 1）
```bash
./scream2diretta -t 1 --cpu-scream=3 --cpu-audio=3

# 输出：
════════════════════════════════════════════════════════════════
  ERROR: CPU Affinity Configuration Conflict
════════════════════════════════════════════════════════════════
... (完整错误信息)

# 退出码：1
```

### 场景 4：非致命冲突（警告但继续）
```bash
./scream2diretta -t 1 --cpu-audio=2 --cpu-other=2

# 输出：
[diretta] WARNING: --cpu-audio=2 and --cpu-other=2 overlap
  RT audio worker (SCHED_FIFO) will preempt helper threads (SCHED_OTHER).
  ...

# → 继续启动
```

### 场景 5：只设置一个（无输出）
```bash
./scream2diretta -t 1 --cpu-scream=3
# → 正常启动，cpu_audio=-1，无冲突
```

---

## 设计决策

### 为什么检测放在 target 发现之前？

- ✅ **快速失败**：避免用户等待 5-10 秒 target 握手后才看到错误
- ✅ **清晰反馈**：错误信息不被 target 发现日志干扰
- ✅ **一致性**：配置错误应该在配置验证阶段拒绝，而非运行时

### 为什么无条件输出（不依赖 `-v`）？

- ✅ **配置错误必须可见**：用户需要知道为什么启动失败
- ✅ **引导修复**：错误信息包含完整的修复步骤
- ✅ **不是调试信息**：这是用户必须看到的错误，不是可选的 verbose 日志

### 为什么 audio==other 只警告不退出？

- ✅ **技术上可行**：RT 线程抢占 CFS 线程，音频不受影响
- ✅ **向后兼容**：可能有用户已经这样配置（虽然次优）
- ✅ **用户选择**：让用户决定是否需要最优隔离

---

## 与现有代码的集成

### 不影响现有用户

- ✅ **默认不设置 affinity 的用户**：`cpu_scream/audio=-1`，跳过检测
- ✅ **正确配置的用户**：检测通过，无额外输出
- ⚠️ **错误配置的用户**：首次升级后启动失败，需要修复

### 修复路径清晰

错误信息包含三种修复方式（优先级递减）：
1. **自动配置**：`sudo ./scripts/diagnose-system.sh`（推荐）
2. **手动配置**：给出示例命令
3. **移除 affinity**：让内核自动调度

---

## 代码质量

- ✅ **无副作用**：只读 cfg，不修改状态
- ✅ **清晰命名**：`validate_cpu_affinity_config` 表意明确
- ✅ **详细注释**：说明为什么检测、为什么致命/警告
- ✅ **友好错误信息**：包含原因、影响、修复步骤
- ✅ **遵循项目风格**：`std::fprintf(stderr, ...)`，`DLOG` 模式

---

## 与 P0 清单的对应

| P0 任务 | 状态 |
|---------|------|
| IRQ affinity 自动化 | ✅ 完成（脚本系统） |
| SO_BUSY_POLL 失败警告 | ✅ 已正确（无需修改） |
| CPU overlap 检测 | ✅ **完成**（本次提交） |

---

## 测试清单（待 Linux 实测）

- [ ] 正确配置启动正常（无输出）
- [ ] 无 affinity 配置启动正常（无输出）
- [ ] scream==audio 拒绝启动并显示错误
- [ ] audio==other 显示警告但继续启动
- [ ] 错误信息中的 diagnose-system.sh 路径正确
- [ ] exit(1) 导致 systemd 单元失败（符合预期）

---

## Commit Message

```
feat: add CPU affinity overlap detection (P0)

Validate --cpu-scream / --cpu-audio / --cpu-other configuration at
startup and reject fatal conflicts:

- scream == audio: EXIT (both RT threads on same core → glitches)
- audio == other: WARN (RT preempts CFS, audio unaffected)

Detection runs early (before target discovery) for fast failure.
Error messages include diagnose-system.sh reference for easy fix.

Closes P0 item: CPU overlap validation.

Co-Authored-By: Claude <noreply@anthropic.com>
```

---

## 下一步

1. ✅ **代码已完成**（本地语法正确）
2. ⏳ **Linux 编译测试**（树莓派 / x86-64）
3. ⏳ **实际运行测试**（触发各种检测场景）
4. ⏳ **Git commit**（与脚本系统一起提交）

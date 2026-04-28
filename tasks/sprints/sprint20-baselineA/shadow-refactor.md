# Baseline Shadow 重构方案（Sprint20 BaselineA，极简版）

## 1. 目标

1. 进入 `shadow` 需要更严格，降低分钟级噪声误触发。  
2. `shadow` 与 `candidate` 解耦：进入 `shadow` 只保护，不立即训练 `candidate`。  
3. `candidate` 仅在“数据足够 + 漂移证据成立 + 到了重试周期”时触发。  
4. `shadow` 长时间不收敛时明确告警，但不强制切换状态。

---

## 2. 统一默认参数（仅保留这 7 个）

- `z_shift_confirm_min = 1.44`  
- `c_rebuild_min = 5`  
- `z_win_shift_threshold = 2.2`  
- `min_shadow_points_for_candidate = 1440`（`Ω_fit`）  
- `min_shadow_holdout_points = 180`（`Ω_val`）  
- `retry_cooldown_points = 180`  
- `shadow_stuck_alert_points = 4320`

说明（分钟级）：

- `Ω_fit=1440` 约 1 天。  
- `Ω_val=180` 约 3 小时。  
- `retry_cooldown_points=180` 表示每 3 小时最多重试一次。  
- `shadow_stuck_alert_points=4320` 表示 `shadow` 连续 3 天未切回 `formal` 时告警。
- 无方向慢漂移窗口长度复用 `min_shadow_holdout_points`（默认 180）。  
- 无方向慢漂移阈值使用 `z_win_shift_threshold`（默认 2.2，单窗口触发）。
- `value/ratio` 的 candidate 验证尾段长度统一使用 `Ω_val=180`，不再使用 `n_val_switch=16`。  
- `n_val_switch` 仅保留给 legacy/非本方案路径，不作为本方案门禁与验证长度参数。

---

## 3. 状态机（简化）

1. `formal`：正式模型在线。  
2. `shadow_observe`：shadow 在线保护与观测，不训练 candidate。  
3. `candidate_validating`：满足门槛后触发一次重建并验证。  
4. `formal_applied`：验证通过，切回正式模型。

关键原则：**`formal -> shadow` 不等于 `shadow -> candidate`**。

---

## 3.1 状态迁移表（代码现状 + 本方案）

### A. 代码现状（`src/plugins/baseline/task/value_task.cpp` + `value_detector_core.cpp`）

| 序号 | 当前状态 | 触发条件 | 关键动作 | 下一状态 |
|---|---|---|---|---|
| 1 | `formal`（`serviceable_self/source`） | 漂移确认成立：`shift_confirmed` 且 `confirm_count >= m_shift`，且未在 `shadow`、未 pending | 激活 `shadow_state`，冻结参考模型，设置 `delta`，`switch_state=shadow_active`，置 `shift_rebuild_pending=true`，生成 `rebuild_intent` | `shadow_active + rebuild_intent` |
| 2 | `shadow_active + rebuild_intent` | enqueue 成功 | `MarkRebuildEnqueued()`，`candidate_state=building`，`switch_state=rebuild_pending` | `rebuild_pending/building` |
| 3 | `shadow_active + rebuild_intent` | enqueue 失败或不可 enqueue | `ClearPendingRebuild()`，保留 shadow，`switch_state` 回到 `shadow_active` | `shadow_observe` |
| 4 | `rebuild_pending/building` | candidate 训练成功 | `MarkCandidateBuilt()` | `rebuild_pending/built` |
| 5 | `rebuild_pending/built` | 进入验证 | `MarkCandidateValidating()`，`switch_state=validating` | `candidate_validating` |
| 6 | `candidate_validating` | 验证通过且 full model 训练成功 | `ApplyFormalModel(accepted)`，`switch_state=formal_applied`，更新 formal，清空 `shadow/drift/candidate` | `formal_applied -> formal` |
| 7 | `candidate_validating` | 验证失败或 full model 训练失败 | `ApplyFormalModel(rejected/failed)`，`shift_rebuild_pending=false`，清空 candidate，**shadow 保留** | `shadow_observe`（但 `switch_state` 可能为 `idle`） |
| 8 | `rebuild_pending/building` | 回放失败/训练失败/数据不足 | `ApplyFormalModel(failed)` 或 `MarkRebuildFailure()`，清空 candidate，通常保留 shadow | `shadow_observe`（或 `rebuild_blocked`） |
| 9 | 任意重建阶段 | `history_reader` 缺失 | `MarkRebuildFailure(unavailable)`，`switch_state=rebuild_blocked` | `rebuild_blocked` |
| 10 | `shadow_observe` | 长间隔断点：`gap > g_reset` | `shadow_state.Reset()`；后续回到常态选 baseline | `formal` 或 `cold_start`（取决于 formal 是否可服务） |

注：当前代码里，“是否处于 shadow”以 `shadow_state.active` 为准；`switch_state` 有时会落到 `idle`，与 `shadow_state.active=true` 并存。

### B. 本方案目标迁移（本次更新）

| 序号 | 当前状态 | 触发条件（目标） | 关键动作 | 下一状态 |
|---|---|---|---|---|
| 1 | `formal` | 满足“现有漂移证据”且满足以下之一：`快偏移`（`|z_t|>=z_shift_confirm_min` 且连续计数 `>=c_rebuild_min`）或 `慢偏移`（`|z_win|>=z_win_shift_threshold`，单窗口） | 进入 `shadow_observe`，仅保护，不立即训练 candidate | `shadow_observe` |
| 2 | `shadow_observe` | 同时满足：`Ω_fit` 足够、`Ω_val` 足够、快/慢偏移证据至少一种成立、且到达 `retry_cooldown_points` | 触发一次 candidate 重建流程 | `candidate_validating` |
| 3 | `shadow_observe` | 快/慢偏移门槛均不满足 | 不触发 candidate，继续在线 shadow | `shadow_observe` |
| 4 | `shadow_observe` | 慢偏移未触发当点 | 继续滚动计算 `z_win`（每点更新滑窗） | `shadow_observe` |
| 5 | `candidate_validating` | 验证通过 | 切回 formal，清空 shadow 漂移上下文 | `formal` |
| 6 | `candidate_validating` | 验证失败 | 保持 shadow，等待下一次冷却后重试 | `shadow_observe` |
| 7 | `shadow_observe` | 持续点数 `> shadow_stuck_alert_points` | 标记 `shadow_stuck` 并告警，不强制切换状态 | `shadow_observe` |

---

## 3.2 第二轮简化（状态语义收敛）

### 1) 统一 `shadow` 真值来源

现状问题：
- `shadow_state.active` 与 `switch_state` 都在表达“是否 shadow”，失败路径下可能不一致（例如 `shadow_state.active=true` 但 `switch_state=idle`）。

简化目标：
- `shadow_state.active` 作为唯一真值。  
- `switch_state` 只表达“重建流程阶段”，不再承担 shadow 真值语义。

落地规则：
- 任何失败回退后，只要 `shadow_state.active=true`，则 `switch_state` 统一写为 `shadow_active`。  
- 退出 shadow 只能通过显式 `shadow_state.Reset()`（如 formal 成功应用，或长 gap 重置）。

验收信号：
- snapshot 中不再出现“`shadow_active=true` 且 `switch_state=idle`”。

### 2) 压平 candidate 细粒度阶段

现状问题：
- `built` 状态在实现中通常是瞬时过渡，紧接着就进入 `validating`，状态数多于业务语义需要。

简化目标：
- 对外将 `built` + `validating` 统一为一个阶段：`candidate_running`（对外视图）。

落地规则：
- 内部仍可保留 `stage_trace(building/built/validating)` 用于排障。  
- 对外 snapshot/report/告警统一映射为：`candidate_running`。

验收信号：
- 业务与运维视角只需要识别：`idle` / `shadow_observe` / `candidate_running` / `formal_applied`。

### 3) 统一失败回退语义

现状问题：
- 同类失败可能回到不同 switch 状态（`idle`/`rebuild_blocked`/`shadow_active`），导致行为解释分叉。

简化目标：
- 失败后转移规则可预测、单一。

落地规则：
- 若 `shadow_state.active=true`：失败后一律回 `shadow_observe`（`switch_state=shadow_active`）。  
- 若 `shadow_state.active=false`：失败后回 `idle`。  
- `rebuild_blocked` 仅用于“外部能力不可用”诊断标签（如 history reader 缺失），不改变 shadow 真值。

验收信号：
- 重建失败后的下一状态仅由“当前是否 shadow”决定，不再随失败分支漂移。

---

## 4. 进入 Shadow 判定（快变 + 慢漂移）

进入 `shadow` 需同时满足：

1. 现有漂移证据条件通过（现有 `p_shift/m_shift` 逻辑）。  
2. 统计确认条件通过，满足以下任一条：
   - 快变确认：`|z_t| >= z_shift_confirm_min` 且同向连续确认计数 `>= c_rebuild_min`。  
   - 慢漂移确认（无方向）：窗口级累计偏差显著（单窗口触发，不再做多次确认）。

慢漂移（无方向）定义（窗口 `W = min_shadow_holdout_points`）：
在 `formal` 与 `shadow_observe` 两个阶段都持续滚动计算，作为进入 shadow 与触发 candidate 的共同证据。

$$
r_i = x_i - \hat{x}_i
$$

$$
\mathrm{err\_sum}_t = \sum_{i=t-W+1}^{t} r_i
$$

$$
\mathrm{std\_sum}_t = \sqrt{\sum_{i=t-W+1}^{t}\sigma_i^2}
$$

$$
z_t^{(\mathrm{win})} = \frac{\mathrm{err\_sum}_t}{\max(\mathrm{std\_sum}_t,\varepsilon)}
$$

判定：

$$
\left|z_t^{(\mathrm{win})}\right| \ge z_{\mathrm{win\_shift\_threshold}}
$$

默认阈值（分钟级）：

- `W = 180`  
- `|z_win| >= 2.2`  
- 单窗口触发（不使用 `c_rebuild_min`）

---

## 5. Candidate 触发判定

处于 `shadow_observe` 且当前无 pending/inflight rebuild 时，检查三项：

1. 数据门槛：  
   - `shadow_point_count >= min_shadow_points_for_candidate`  
   - `shadow_effective_holdout_count >= min_shadow_holdout_points`
2. 漂移门槛（快变或慢漂移至少一种仍成立）：  
   - 快变：`|z| >= z_shift_confirm_min` 的同向连续确认满足 `c_rebuild_min`  
   - 慢漂移：`|z_win| >= z_win_shift_threshold`（单窗口触发）
3. 节流门槛：  
   - 距离上次 candidate 尝试已过 `retry_cooldown_points`

三项全满足才触发一次 `candidate_validating`。

计数口径（精确定义）：

设 `t_enter` 为进入 `shadow_observe` 的 bucket，`t` 为当前 bucket。

$$
\mathrm{shadow\_point\_count}(t)=\#\{i \mid t_{\mathrm{enter}} \le i \le t,\ i\ \text{被接收为 shadow 观测点}\}
$$

$$
\mathrm{shadow\_effective\_holdout\_count}(t)=
\max\left(0,\ \mathrm{shadow\_point\_count}(t)-\Omega_{\mathrm{fit}}\right)
$$

其中：
- $\Omega_{\mathrm{fit}} = \mathrm{min\_shadow\_points\_for\_candidate}$  
- $\Omega_{\mathrm{val}} = \mathrm{min\_shadow\_holdout\_points}$

因此数据门槛等价为：

$$
\mathrm{shadow\_point\_count}(t)\ge \Omega_{\mathrm{fit}}+\Omega_{\mathrm{val}}
$$

在默认值下（`Ω_fit=1440`, `Ω_val=180`），最早满足数据门槛的点数为 `1620`。

candidate 触发总条件（含无 pending/inflight）：

$$
G_{\mathrm{trigger}}=
G_{\mathrm{no\_pending}}
\land G_{\mathrm{data}}
\land G_{\mathrm{drift}}
\land G_{\mathrm{cooldown}}
$$

触发后回放窗口约束（必须满足）：
- `bucket_end` 为当前点。  
- `bucket_start_hint` 至少回退到 `bucket_end - (Ω_fit + Ω_val) + 1`，保证重建窗口覆盖训练段与验证段。  
- candidate 训练/验证切分固定为：训练段 `Ω_fit`，验证段 `Ω_val`（value/ratio 路径不再用 `n_val_switch`）。

---

## 6. Shadow Stuck 语义（明确）

当 `shadow` 连续持续点数超过 `shadow_stuck_alert_points`：

1. 标记 `shadow_stuck = true`，发出告警。  
2. **状态不切换，仍然保持 `shadow` 在线服务**。  
3. 仍按 `retry_cooldown_points` 周期继续尝试 candidate（前提是 `Ω_fit/Ω_val` 与漂移门槛满足）。  
4. 一旦 candidate 验证通过并 `formal_applied`，清除 `shadow_stuck`。

---

## 6.1 Candidate 通过后的 Full 重训时间起点（精确定义）

本方案在流式处理中采用“固定后看窗口起点”定义 full 重训起点，避免歧义：

- 设 `t_switch_gate` 为 candidate 验证通过的当前 bucket（准备进入 full 重训的时刻）。  
- full 重训的窗口下限定义为：

$$
t_{\mathrm{full\_start}} = t_{\mathrm{switch\_gate}} - (\Omega_{\mathrm{fit}} + \Omega_{\mathrm{val}}) + 1
$$

- 其中 `Ω_fit = min_shadow_points_for_candidate`，`Ω_val = min_shadow_holdout_points`。  
- 在默认值下，`Ω_fit + Ω_val = 1620`，即 full 重训最小起始窗口为最近 1620 点。

注意：
- 这是 full 重训的**最小可用起点**，不是“必然已满足所有周期项”的保证。  
- 若 required components 需要更长覆盖（例如周/月周期），则继续在 shadow 中累计新阶段数据，直到覆盖就绪。

---

## 6.2 长重训等待态（仍属 Shadow）

candidate validate 通过后，不立即强制 `formal_applied`；先进入“full 重训等待态”（语义上仍为 `shadow`）：

1. `candidate_passed = true`，冻结“已通过门禁”的结论。  
2. 继续使用 shadow baseline 对外服务（不回退到 formal）。  
3. 按流式到达持续扩展 full 训练数据窗口（起点按 `t_full_start` 定义）。  
4. 当 required components 全部达到 readiness 后，执行 full 训练与 full 验证。  
5. full 通过才 `formal_applied`；full 失败则回到 `shadow_observe`，并重新进入冷却重试机制。

状态要求：
- 该等待态期间，不重复触发新的 candidate validate（避免重复门禁计算与上游回放压力）。  
- `shadow_stuck_alert_points` 仍生效：超阈值只告警，不强切。

---

## 7. 配置模板（简化）

```yaml
baseline:
  runtime_and_rebuild_constants:
    shadow_policy:
      z_shift_confirm_min: 1.44
      c_rebuild_min: 5
      z_win_shift_threshold: 2.2
      min_shadow_points_for_candidate: 1440
      min_shadow_holdout_points: 180
      retry_cooldown_points: 180
      shadow_stuck_alert_points: 4320
```

不再使用任何分档命名。

---

## 8. 可观测性最小集

建议在 snapshot/report 输出：

1. `z_shift_confirm_min`、`c_rebuild_min`、当前确认计数。  
2. `z_win`、`z_win_shift_threshold`、`slow_drift_triggered`。  
3. `shadow_point_count`、`shadow_effective_holdout_count`。  
4. `last_candidate_attempt_bucket`、`retry_cooldown_points`。  
5. `shadow_stuck` 及触发时间。  
6. 最近一次失败 `failure_reason`、`failure_reason_detail`。

---

## 9. 验收标准

1. 进入 shadow 触发次数明显下降。  
2. 进入 shadow 后不再立即 candidate。  
3. candidate 触发时总能解释：为什么满足 `Ω_fit/Ω_val`，以及触发源于快变还是慢漂移。  
4. 超过 4320 点时出现 stuck 告警，但服务不中断、状态保持 shadow。  

---

## 10. 编码实施任务拆解（按开发顺序）

### 任务 1：接入 Shadow 策略配置（7 参数）

涉及代码：
- `src/plugins/baseline/config/runtime_config.h`
- `src/plugins/baseline/config/runtime_config.cpp`
- `src/plugins/baseline/config/baseline-config-template.yaml`

实现逻辑：
1. 在 runtime config 中新增 `shadow_policy` 配置块，承载 7 个参数。  
2. 在 `runtime_and_rebuild_constants` 下解析 `shadow_policy`，并在 strict schema 中放行该节点。  
3. 提供统一 getter，供 detector 热路径读取，禁止 detector 内硬编码阈值。  
4. 模板 YAML 增加 `shadow_policy` 示例，参数默认值与本方案一致。  
5. 标记 value/ratio rebuild 对 `n_val_switch` 的依赖为废弃路径，验证长度统一走 `Ω_val`。

完成判定：
- 无 YAML 时使用默认值；有 YAML 时可覆盖；strict=true 下未知字段报错。

### 任务 2：扩展运行时状态（Shadow Observe 上下文）

涉及代码：
- `src/plugins/baseline/detector/value_detector_core.h`
- `src/plugins/baseline/detector/ratio_detector_core.h`
- `src/plugins/baseline/model/shadow_state.h`（可选，若抽公共结构）

实现逻辑：
1. 为每个 key 增加 `shadow_observe` 上下文字段：  
`shadow_enter_bucket_id`、`shadow_point_count`、`shadow_effective_holdout_count`、`last_candidate_attempt_bucket`、`shadow_stuck`、`z_win`、`slow_drift_triggered`。  
2. 增加慢漂移滑窗状态：`err_sum`、`var_sum`、ring buffer 指针和窗口计数。  
3. 定义统一 reset 规则：formal 成功切换、gap reset、显式退出 shadow 时清空上下文。

完成判定：
- `QuerySeriesSnapshotJson` 可观察以上字段，且在状态切换时值一致。

### 任务 3：实现慢漂移单窗口判定（无方向）

涉及代码：
- `src/plugins/baseline/detector/value_detector_core.cpp`
- `src/plugins/baseline/detector/ratio_detector_core.cpp`

实现逻辑：
1. 在每个 serviceable 点（包含 `formal` 与 `shadow_observe`）计算残差 `r_t` 与标准差尺度 `sigma_t`。  
2. 用固定窗口 `W=min_shadow_holdout_points` 维护滚动和：
$$
\mathrm{err\_sum}_t=\mathrm{err\_sum}_{t-1}+r_t-r_{t-W}
$$
$$
\mathrm{var\_sum}_t=\mathrm{var\_sum}_{t-1}+\sigma_t^2-\sigma_{t-W}^2
$$
$$
z_t^{(\mathrm{win})}=\frac{\mathrm{err\_sum}_t}{\sqrt{\max(\mathrm{var\_sum}_t,\varepsilon)}}
$$
3. 判定 `abs(z_win) >= z_win_shift_threshold` 即慢漂移成立（单窗口触发，不做连续计数）。  
4. 若 `gap > g_reset` 或 baseline 不可服务，重置滑窗状态，避免跨段污染。

完成判定：
- 慢漂移触发与否仅由 `z_win` 当前值决定，且计算复杂度为 `O(1)/点`。

### 任务 4：重构 `formal -> shadow` 触发（只入 shadow，不立即重建）

涉及代码：
- `src/plugins/baseline/detector/value_detector_core.cpp`
- `src/plugins/baseline/detector/ratio_detector_core.cpp`

实现逻辑：
1. 将当前“进入 shadow 同时 enqueue rebuild”的逻辑拆开。  
2. 进入 shadow 的触发改为：现有漂移证据成立，且满足“快偏移或慢偏移”至少一种。  
3. 进入 shadow 时仅执行：冻结参考模型、初始化 shadow 上下文、`switch_state=shadow_active`。  
4. 禁止在该分支直接设置 `enqueue_rebuild=true`。

完成判定：
- 首次进入 shadow 的点，结果可见 `shadow_active`，但 `rebuild_queued` 不立即置位。

### 任务 5：实现 `shadow_observe -> candidate` 门禁触发

涉及代码：
- `src/plugins/baseline/detector/value_detector_core.cpp`
- `src/plugins/baseline/detector/ratio_detector_core.cpp`
- `src/plugins/baseline/task/value_task.cpp`
- `src/plugins/baseline/task/ratio_task.cpp`

实现逻辑：
1. 在 shadow 每个点更新计数：  
`shadow_point_count += 1`，`shadow_effective_holdout_count = max(0, shadow_point_count - min_shadow_points_for_candidate)`。  
2. candidate 触发需同时满足：  
数据门槛（`Ω_fit` + `Ω_val` 足够）、漂移门槛（快或慢至少一种）、冷却门槛（距离上次尝试 >= `retry_cooldown_points`）。  
3. 触发时输出 `rebuild_intent`，并强制 `bucket_start_hint` 覆盖 `Ω_fit + Ω_val`（禁止继续沿用“确认段起点”短窗口）。  
4. value/ratio 重建切分改为固定验证尾段 `Ω_val`：candidate builder 不再使用 `n_val_switch` 计算 holdout。  
5. 仅在实际入队成功后更新 `last_candidate_attempt_bucket`，失败则清理 pending 并保留 shadow。

门槛表达式（实现时建议直接按布尔变量编码）：
- `gate_data_fit = (shadow_point_count >= min_shadow_points_for_candidate)`  
- `gate_data_val = (shadow_effective_holdout_count >= min_shadow_holdout_points)`  
- `gate_data = gate_data_fit && gate_data_val`  
- `gate_drift = gate_fast || gate_slow`  
- `gate_cooldown = (current_bucket - last_candidate_attempt_bucket >= retry_cooldown_points)`  
- `gate_trigger = gate_no_pending && gate_data && gate_drift && gate_cooldown`

完成判定：
- shadow 内重建触发频率受冷却限制；不再“每点都试”。  
- 每次 candidate 重建都能在快照中看到 `last_replay_window.observation_count >= Ω_fit + Ω_val`。

### 任务 6：压平 candidate 细粒度阶段（对外 `candidate_running`）

涉及代码：
- `src/plugins/baseline/detector/value_detector_core.cpp`
- `src/plugins/baseline/detector/ratio_detector_core.cpp`
- `src/plugins/baseline/task/value_task.cpp`
- `src/plugins/baseline/task/ratio_task.cpp`
- `src/plugins/baseline/model/formal_model_state.h`（若需补充阶段映射辅助）

实现逻辑：
1. 保留内部细粒度阶段（`building/built/validating`）和 `stage_trace`，不改慢路径流程。  
2. 新增对外阶段映射：当内部处于 `building|built|validating` 任一阶段时，对外统一输出 `candidate_running`。  
3. `QuerySeriesSnapshotJson` 增加统一阶段字段（建议命名 `rebuild_phase`），供上游与告警系统使用。  
4. 现有 `candidate_state/switch_state` 继续保留，作为排障字段。

完成判定：
- 上游只需识别 `idle` / `shadow_observe` / `candidate_running` / `formal_applied`，无需关心 built/validating 细节。

### 任务 7：统一 `shadow` 真值与外部阶段语义

涉及代码：
- `src/plugins/baseline/detector/value_detector_core.cpp`
- `src/plugins/baseline/detector/ratio_detector_core.cpp`
- `src/plugins/baseline/task/value_task.cpp`
- `src/plugins/baseline/task/ratio_task.cpp`

实现逻辑：
1. 统一规则：`shadow_state.active` 是是否处于 shadow 的唯一真值。  
2. `switch_state` 仅表达重建流程阶段，不承担 shadow 真值。  
3. 外部阶段字段与 shadow 真值一致：`shadow_state.active=true` 时外部阶段必须是 `shadow_observe` 或 `candidate_running`。  
4. 失败、取消、重试等分支都按该规则收敛，避免 `shadow_active=true` 却显示 `idle`。

完成判定：
- snapshot 中不再出现 `shadow_active=true` 与外部阶段 `idle` 的冲突组合。

### 任务 8：统一失败回退与 `shadow_stuck` 告警

涉及代码：
- `src/plugins/baseline/detector/value_detector_core.cpp`
- `src/plugins/baseline/detector/ratio_detector_core.cpp`
- `src/plugins/baseline/model/formal_model_state.h`（若需扩展诊断字段）

实现逻辑：
1. 失败回退统一规则：  
若 `shadow_state.active=true`，回到 `shadow_observe`；若 `shadow_state.active=false`，回到 `idle`。  
2. `rebuild_blocked` 仅作为诊断语义（通过 `failure_reason`/`failure_reason_detail` 输出），不覆盖 shadow 真值。  
3. 维护 `shadow_stuck`：`shadow_point_count > shadow_stuck_alert_points` 时置真并持续告警；状态仍保持 shadow。  
4. formal 成功切换后清空 stuck 状态。

完成判定：
- 重建失败后下一状态只由“当前是否 shadow”决定；stuck 只告警不强切。

### 任务 9：回归测试与用例补齐

涉及代码：
- `src/tests/test_baseline/test_baseline_value_task.cpp`
- `src/tests/test_baseline/test_baseline_ratio_task.cpp`
- `src/tests/test_baseline/test_baseline_rebuild.cpp`
- 必要时新增：`src/tests/test_baseline/test_baseline_shadow_policy.cpp`

实现逻辑：
1. 用例覆盖 `formal -> shadow` 不立即重建。  
2. 用例覆盖慢漂移单窗口触发（`|z_win| >= 2.2`）与快偏移触发。  
3. 用例覆盖 shadow 内冷却重试、数据门槛不足不触发。  
4. 用例覆盖 value/ratio 验证尾段固定 `Ω_val=180`，不再受 `n_val_switch` 影响。  
5. 用例覆盖 candidate 阶段压平映射（内部 built/validating -> 外部 `candidate_running`）。  
6. 用例覆盖失败回退语义统一、stuck 告警触发与清除。  
7. 用例覆盖快照字段：`z_win`、`shadow_point_count`、`shadow_stuck`、统一阶段字段。  
8. 旧断言若与新语义冲突（例如固定断言某 `switch_state`），直接改为断言新语义字段，不为旧测试保留旧行为。

完成判定：
- 新增/修改测试全部通过，且可解释每个状态迁移。

### 任务 10：落地 Candidate 通过后的 Full 重训等待态与起点约束

涉及代码：
- `src/plugins/baseline/task/value_task.cpp`
- `src/plugins/baseline/task/ratio_task.cpp`
- `src/plugins/baseline/rebuild/formal_model_trainer.cpp`
- `src/plugins/baseline/model/readiness_helper.cpp`
- `src/plugins/baseline/model/formal_model_state.h`
- `src/tests/test_baseline/test_baseline_value_task.cpp`
- `src/tests/test_baseline/test_baseline_ratio_task.cpp`
- `src/tests/test_baseline/test_baseline_rebuild.cpp`

实现逻辑：
1. candidate validate 通过后，不直接 `formal_applied`；写入 `candidate_passed` 与 `full_waiting` 语义标记。  
2. 在通过时刻记录 `t_switch_gate`，并固化 full 最小重训起点：  
$$
t_{\mathrm{full\_start}} = t_{\mathrm{switch\_gate}} - (\Omega_{\mathrm{fit}} + \Omega_{\mathrm{val}}) + 1
$$  
3. `full_waiting` 期间保持 `shadow` 对外服务，禁止再次触发 candidate validate（直到 full 完成或被显式重置）。  
4. 每个新点到达时，仅推进“required components readiness”检查；全部就绪后才触发 full 训练与 full 验证。  
5. full 通过才执行 `formal_applied` 并清空 shadow/full_waiting；full 失败则回到 `shadow_observe` 并恢复冷却重试。  
6. stuck 语义不变：`full_waiting` 同样计入 `shadow_point_count`，超 `shadow_stuck_alert_points` 只告警不强切。

完成判定：
- candidate 通过后的状态不再直接切 formal，而是进入可观测的 full 等待过程。  
- snapshot/report 可观察 `candidate_passed`、`full_waiting`、`t_switch_gate`、`t_full_start`。  
- `full_waiting` 期间不会重复 candidate；full 完成后状态迁移符合第 6.2 节定义。

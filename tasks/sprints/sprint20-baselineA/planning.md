# Sprint 20 BaselineA 规划

## Sprint 信息

- **Sprint 周期**：Sprint 20 BaselineA
- **功能设计来源**：
  - [Sprint 19 基线统一设计](../sprint19-baseline/design.md)
  - [Sprint 20 BaselineA 代码实现设计](code-design.md)
- **复盘与审视约束**：
  - [Sprint 19 Baseline 回顾总结](../sprint19-baseline/retrospective.md)
  - [Sprint 19 Baseline 检视记录](../sprint19-baseline/review.md)
- **本轮目标**：
  - 在保留 `baseline` 插件外壳、任务注册表和部分状态对象的前提下，重建 `T1 / T2 / T3` 的正式算法内核，交付与 `design.md`、`code-design.md` 一致的首个可编码、可测试、可验证版本

---

## 1. 规划原则

### 1.1 Story 拆分原则

本轮 Story 按“依赖闭合 + 代码承载边界”拆分，不按单纯目录拆分，也不按“先留壳、后补算法”拆分。

每个 Story 必须同时满足：

1. 依赖它的输入结构、状态对象、接口注入能力，已由前置 Story 真实产出。
2. 本 Story 的验收标准可独立落到测试，不依赖未来 Story 才成立。
3. 本 Story 完成后，仓库仍保持可编译，且没有新增“空壳算法”。
4. 若修改现有文件，必须优先复用既有职责方向，而不是平行再造一套新层次。

### 1.2 本轮成功标准

- [x] 统一输出协议、任务规格、`HistoryReader / BaselineSourceResolver / EventCalendarSpec` 接口全部与设计对齐
- [x] `T1` 的 `Core / monthpos / event` 训练、在线评分、漂移证据、`shadow baseline`、正式重建闭环全部落地
- [x] `T2` 的 `m0 / alpha0 / beta0 / p_smooth / logit / variance layer / rebuild` 全部落地
- [x] `T3` 的 `ServiceBasis / EvalBasis / summary features / routed detector / pattern fusion / key risk` 全部落地
- [x] 旧的 `intercept-only`、常数预测、只保留壳层不保留算法的占位路径全部移除

### 1.3 实施门禁

任一 Story 开工前，先做 4 项自检：

1. 对应的 `design.md` 条款已经在 `code-design.md` 找到唯一代码 owner。
2. 该 Story 不会引入新的待定语义，尤其不能新增“后续再决定”的 `?` 字段。
3. 对现有代码的处理方式已经明确：保留、重构、重写或新增。
4. 验收标准能明确落到单元测试、集成测试或快照测试。

---

## 2. 现有代码利用策略

### 2.1 直接保留并扩字段

以下文件的职责方向成立，优先保留并扩充字段，而不是重写路径：

- `src/plugins/baseline/baseline_plugin.*`
- `src/plugins/baseline/task/task_registry.*`
- `src/plugins/baseline/model/series_state.h`
- `src/plugins/baseline/model/series_store.*`
- `src/plugins/baseline/model/event_calendar_spec.h`
- `src/plugins/baseline/model/drift_state.h`
- `src/plugins/baseline/model/shadow_state.h`
- `src/plugins/baseline/rebuild/rebuild_queue.*`
- `src/plugins/baseline/rebuild/rebuild_request.h`
- `src/plugins/baseline/rebuild/replay_runner.*`
- `src/plugins/baseline/relation/relation_basis.*`

### 2.2 保留并重构

以下文件保留文件身份，但内部结构按新设计重构：

- `src/framework/interfaces/ibaseline_types.h`
- `src/framework/interfaces/ibaseline_service.h`
- `src/plugins/baseline/config_parser.*`
- `src/plugins/baseline/common/result_builder.h`
- `src/plugins/baseline/model/task_spec.h`
- `src/plugins/baseline/model/series_override.h`
- `src/plugins/baseline/model/formal_model_state.h`
- `src/plugins/baseline/rebuild/rebuild_worker.*`
- `src/plugins/baseline/relation/relation_summary_extractor.*`
- `src/plugins/baseline/relation/relation_router.*`
- `src/plugins/baseline/task/baseline_task_base.*`
- `src/plugins/baseline/task/value_task.*`
- `src/plugins/baseline/task/ratio_task.*`

其中 `baseline_task_base.*` 需要特别约束：

- 保留为薄基类文件，不删除其文件身份
- 但必须从中剥离 task-specific 算法、relation 特化逻辑和大段重建实现
- 重构完成后，它只应保留共性的任务壳层能力、少量通用 helper 和基础设施引用

### 2.3 整体重写

以下文件已有实现与正式设计偏差较大，按代码设计整体重写：

- `src/plugins/baseline/model/formal_model.h`
- `src/plugins/baseline/model/formal_predictor.*`
- `src/plugins/baseline/rebuild/formal_model_trainer.*`
- `src/plugins/baseline/rebuild/candidate_builder.*`
- `src/plugins/baseline/rebuild/candidate_validator.*`
- `src/plugins/baseline/solver/solver_backend.*`
- `src/plugins/baseline/detector/value_detector_core.*`
- `src/plugins/baseline/detector/ratio_detector_core.*`

### 2.4 正式新增

以下能力不应再塞回旧文件，按新文件新增：

- `src/plugins/baseline/model/profile_config.h`
- `src/plugins/baseline/model/calendar_feature_helper.*`
- `src/plugins/baseline/model/event_calendar_matcher.*`
- `src/plugins/baseline/model/readiness_helper.*`
- `src/plugins/baseline/fusion/key_risk_fusion.*`
- `src/plugins/baseline/fusion/relation_pattern_fusion.*`
- `src/plugins/baseline/task/relation_task.*`

---

## 3. Story 依赖顺序

建议实施顺序如下：

1. `18.10` 协议与任务规格闭环
2. `18.11` 共享时间 / 事件 / readiness 基础层
3. `18.12` 正式模型 schema 与 predictor
4. `18.13` 插件与 task 编排层重构
5. `18.14` `T1` 训练与重建慢路径
6. `18.15` `T1` 在线评分、漂移证据与 `shadow baseline`
7. `18.16` `T2` 训练与在线评分
8. `18.17` `T3` basis、摘要特征与 routed detector
9. `18.18` 融合层与 key 级风险输出
10. `18.19` `T3` 正式重建、验证与 lineage 切换
11. `18.20` 测试收口、死代码清理与最终一致性审查

依赖图可简化为：

- `18.10 -> 18.11 -> 18.12`
- `18.10 -> 18.13`
- `18.12 + 18.13 -> 18.14 -> 18.15`
- `18.12 + 18.14 -> 18.16`
- `18.13 + 18.15 + 18.16 -> 18.17`
- `18.17 -> 18.18`
- `18.14 + 18.16 + 18.17 -> 18.19`
- `全部 Story -> 18.20`

---

## 4. Story 列表

### Story 18.10：统一协议与任务规格闭环

**优先级**：P0

**设计来源**：

- `design.md`：`5.2`、`5.3`
- `code-design.md`：第 `4`、`5` 章

**复用现有**：

- `ibaseline_types.h`
- `ibaseline_service.h`
- `task_spec.h`
- `series_override.h`
- `event_calendar_spec.h`
- `config_parser.*`

**必须实现**：

- 补齐 `DetectorResult / FusionResult` 正式结构与 `evidence` tagged union
- 补齐 `BaselineTaskSpec / RelationTaskSpec / RelationTaskClockSpec`
- 补齐 `HistoryReader`、`BaselineSourceResolver`、`QueryKeyFusionSnapshotJson` 接口
- 把 task-bound 能力绑定方式正式收口：
  - `HistoryReader` 走 `SetHistoryReader(...)`
  - `EventCalendarSpec`、按 `(key, feature)` 索引的 `BaselineSourceConfig` 集合走创建期静态绑定
  - relation task 的 `RelationTaskClockSpec`、task-bound `EventCalendarSpec`、task-bound `BaselineSourceResolver` 走 `CreateRelationTask(...)` 创建期绑定
- 将 `T1 / T2` 外部任务配置字段固定为复数 `baseline_source_configs`，每个元素按 `key` 生效；顶层单数 `baseline_source_config` 与旧字段 `series_overrides` 必须拒绝
- 把 `IBaselineRelationTask::SubmitBlock(...)` 的同步返回类型与语义正式化为当前 bucket 的 `FusionResult`
- 让 `config_parser.*` 在 task 创建阶段完成所有正式字段校验
- 明确所有 `?` 字段的 present / absent 语义并落到代码结构

**明确不做**：

- 不实现训练、评分、融合数学过程

**禁止事项**：

- 禁止把 `RelationTaskClockSpec`、`BaselineSourceResolver` 等能力藏在 `relation_task.*` 私有实现里
- 禁止继续保留只在 JSON 中存在、但 C++ 结构层缺失的正式字段

**涉及文件**：

- 修改：`src/framework/interfaces/ibaseline_types.h`
- 修改：`src/framework/interfaces/ibaseline_service.h`
- 修改：`src/plugins/baseline/model/task_spec.h`
- 修改：`src/plugins/baseline/model/series_override.h`
- 修改：`src/plugins/baseline/model/event_calendar_spec.h`
- 修改：`src/plugins/baseline/config_parser.*`

**验收标准**：

- [x] `T1 / T2 / T3` 任务规格与 `code-design.md` 一致
- [x] 非法配置在创建 task 阶段被拒绝
- [x] `DetectorResult / FusionResult` 的正式字段不再依赖快照 JSON 补语义
- [x] task-bound 能力绑定方式已写死：只有 `HistoryReader` 走 setter，事件 / 按 `(key, feature)` 索引的来源配置 / 时钟都不允许 post-create 漂移
- [x] `IBaselineRelationTask::SubmitBlock(...)` 同步返回当前 bucket 的 `FusionResult`
- [x] 配置解析测试覆盖按 `(key, feature)` 生效的 `BaselineSourceConfig`、`EventCalendarSpec`、`RelationTaskSpec`
- [x] `baseline_source_configs` 仅对显式列出的 `key` 生效，未列出的运行时 `key` 不继承来源；顶层 `baseline_source_config` 与 `series_overrides` 均被拒绝

### Story 18.11：共享时间、事件与 readiness 基础层

**优先级**：P0

**设计来源**：

- `design.md`：`2.2`、`5.4`、`6.5`、`6.6`、`7.4`
- `code-design.md`：`5.3.4`、`5.4`、`6.5.2`

**复用现有**：

- `event_calendar_spec.h`
- `series_state.h`
- `shadow_state.h`

**必须实现**：

- 新增 `profile_config.h`
- 新增 `calendar_feature_helper.*`
- 新增 `event_calendar_matcher.*`
- 新增 `readiness_helper.*`
- 在 `profile_config.h` 中正式承载共享主参数、`T1b` profile 参数、`T2` profile 参数及其派生值
- 实现 `coverage -> monthpos_enabled -> confidence_base` 的统一 helper
- 实现 task-bound `CompiledEventCalendar` 的 compile / resolve / indicator 构造

**明确不做**：

- 不实现 trainer 和 detector
- 仅允许 detector 接入 `profile_config.h` 的共享参数来源，不改变 detector 的评分 / 漂移语义

**禁止事项**：

- 禁止在 trainer、predictor、task 中各写一套时间相位或事件匹配逻辑
- 禁止 detector 自己推导 readiness / confidence

**涉及文件**：

- 新增：`src/plugins/baseline/model/profile_config.h`
- 新增：`src/plugins/baseline/model/calendar_feature_helper.*`
- 新增：`src/plugins/baseline/model/event_calendar_matcher.*`
- 新增：`src/plugins/baseline/model/readiness_helper.*`
- 修改：`src/plugins/baseline/CMakeLists.txt`
- 修改：`src/plugins/baseline/detector/value_detector_core.cpp`
- 修改：`src/plugins/baseline/detector/ratio_detector_core.cpp`
- 新增：`src/tests/test_baseline/test_baseline_model_helpers.cpp`
- 修改：`src/tests/test_baseline/CMakeLists.txt`

**验收标准**：

- [x] `phase_day_local / phase_week_local / monthpos` 特征构造可单测
- [x] `EventCalendarSpec` 的 `scope_type / alignment_mode` 匹配可单测
- [x] `ReadinessState` 的在线与训练计算共用同一 helper
- [x] `profile_config.h` 已收口共享主参数、`T1b` / `T2` 参数和派生值，不再散落到 detector 或 trainer 的裸常量
- [x] `DST`、事件版本不一致、事件 absent 等边界都有测试

### Story 18.12：正式模型 schema 与 predictor 落地

**优先级**：P0

**设计来源**：

- `design.md`：`6.5`、`6.6`、`7.4`
- `code-design.md`：第 `6.5`、`7.4`

**复用现有**：

- `formal_model_state.h`

**必须实现**：

- 重写 `formal_model.h`
- 重写 `formal_predictor.*`
- 定义 `ValueFormalModel / RatioFormalModel`
- 落地 `CoreBlock / MonthPosBlock / EventBlock / FitBlockDigest`
- 让 predictor 能按 `bucket_id + delta + tz + compiled events` 生成正式预测值

**明确不做**：

- 不引入新的 solver 与训练目标函数
- 仅把现有 full-model 训练路径映射到正式 schema，供 predictor / snapshot / 后续重建链路消费

**禁止事项**：

- 禁止继续保留 `intercept-only` 作为正式模型 schema
- 禁止把 `confidence_base_at_train` 做成可有可无的诊断字段

**涉及文件**：

- 重写：`src/plugins/baseline/model/formal_model.h`
- 重写：`src/plugins/baseline/model/formal_predictor.*`
- 修改：`src/plugins/baseline/rebuild/formal_model_trainer.*`
- 修改：`src/tests/test_baseline/test_baseline_model_helpers.cpp`
- 修改：`src/tests/test_baseline/test_baseline_plugin.cpp`

**验收标准**：

- [x] formal model 可表达 `Core / monthpos / event`
- [x] predictor 能在 value / ratio 两条路径上生成一致预测
- [x] 训练元数据、事件版本、readiness 元数据都进入模型对象

### Story 18.13：插件与 task 编排层重构

**优先级**：P0

**设计来源**：

- `design.md`：`5.3`、`10.1`
- `code-design.md`：第 `3`、`5` 章

**复用现有**：

- `baseline_plugin.*`
- `task_registry.*`
- `baseline_task_base.*`
- `value_task.*`
- `ratio_task.*`

**必须实现**：

- 新增 `relation_task.*`
- 收口 task-bound 能力绑定方式：
  - `SetHistoryReader(...)` 作为唯一 post-create setter
  - `EventCalendarSpec`、按 `(key, feature)` 索引的 `BaselineSourceConfig` 集合、`RelationTaskClockSpec`、`BaselineSourceResolver` 全部固定为创建期绑定
- 收口 per-series runtime 容器，只负责生命周期、状态持有、submit 编排、snapshot
- 补齐 relation snapshot：routed singles、`ServiceBasis / EvalBasis` 摘要，以及供 `18.18` 挂接的 `FusionResult` 快照位
- 瘦身 `baseline_task_base.*`，把 `RelationTask`、`ValueTask`、`RatioTask` 各自的 task-specific 热路径与重建细节移回各自文件
- 把 `RelationTask::SubmitBlock(...)` 的同步调用链和返回通道预留完整：本 Story 只负责 summary -> routed detector -> snapshot / hook 编排，正式 `FusionResult` 产出留给 `18.18`

**明确不做**：

- 不在本 Story 内实现最终数学评分与正式 pattern fusion

**禁止事项**：

- 禁止 task 壳层持有求解器或训练目标函数
- 禁止 relation task 继续隐身在 `baseline_task_base.*` 内部
- 禁止继续把 relation 摘要提取、candidate 验证、formal model 训练或 routed detector 容器塞回 `baseline_task_base.cpp`
- 禁止把 relation task 的正式接口设计回退为“最终只能查快照，不能同步回传 `FusionResult`”

**涉及文件**：

- 修改：`src/plugins/baseline/baseline_plugin.*`
- 修改：`src/plugins/baseline/task/task_registry.*`
- 修改：`src/plugins/baseline/task/baseline_task_base.*`
- 修改：`src/plugins/baseline/task/value_task.*`
- 修改：`src/plugins/baseline/task/ratio_task.*`
- 新增：`src/plugins/baseline/task/relation_task.*`
- 修改：`src/plugins/baseline/CMakeLists.txt`

**验收标准**：

- [x] 三类 task 都能承接完整 task-bound 能力注入
- [x] task 壳层职责只剩编排、状态持有、snapshot、rebuild 请求
- [x] relation task 拥有独立文件与独立 runtime 结构
- [x] `baseline_task_base.*` 已瘦身为薄基类，不再承载 `T1 / T2 / T3` 的 task-specific 算法或 relation 特化逻辑
- [x] relation task 只允许 `HistoryReader` 走 setter；其他 task-bound 能力都在创建期固定
- [x] `RelationTask::SubmitBlock(...)` 的同步调用链与回传通道已经留好，但本 Story 不要求直接产出正式 `FusionResult`
- [x] `18.13` 完成后 relation snapshot 可以挂接 `FusionResult`，但不要求本 Story 直接产出正式融合结果

### Story 18.14：`T1` 训练与正式重建慢路径

**优先级**：P0

**设计来源**：

- `design.md`：`6.6`、`10.2.1`、`10.2.3`
- `code-design.md`：第 `6.6`、`10.2`

**复用现有**：

- `rebuild_queue.*`
- `rebuild_request.h`
- `replay_runner.*`
- `rebuild_worker.*`

**必须实现**：

- 重写 `solver_backend.*`
- 重写 `formal_model_trainer.*`
- 重写 `candidate_builder.*`
- 重写 `candidate_validator.*`
- 在 `formal_model_state.h` 中写入正式 `candidate_state / switch_state` 状态机，并让 `rebuild_worker.*`、task 层与之对齐
- 在 `T1` 路径上实现：
  - `WeightedHuberRidgeBlockSolver`
  - `monthpos` 去中心化与中心化元数据持久化
  - `TrainCoreBlock / TrainMonthPosBlock / TrainEventBlock`
  - `EstimateSigmaMAD`
  - `Ω_rebuild / Ω_fit / Ω_val`
  - `holdout tail validation` 与 `eps_switch`
  - `candidate vs incumbent` 验证

**明确不做**：

- 不实现 `T2` 和 `T3` 的训练分支

**禁止事项**：

- 禁止把 `core / monthpos / event` 缩成只训 `core`
- 禁止用常数或 fake `sigma_ref` 冒充训练产物

**涉及文件**：

- 重写：`src/plugins/baseline/solver/solver_backend.*`
- 重写：`src/plugins/baseline/rebuild/formal_model_trainer.*`
- 重写：`src/plugins/baseline/rebuild/candidate_builder.*`
- 重写：`src/plugins/baseline/rebuild/candidate_validator.*`
- 修改：`src/plugins/baseline/model/formal_model_state.h`
- 修改：`src/plugins/baseline/rebuild/rebuild_worker.*`

**验收标准**：

- [x] `T1` 能基于历史观测训练出完整 formal model
- [x] `candidate_pass / candidate_fail / insufficient_data / unavailable` 都可验证
- [x] `formal_model_state.h` 中的 `candidate_state / switch_state` 已按设计落地，且不再依赖零散布尔标志拼装生命周期
- [x] `rebuild_worker` 在 `T1` 路径闭环工作
- [x] `monthpos` 的去中心化由 trainer 持久化并在 predictor 侧复用，训练 / 预测口径一致
- [x] `Ω_val` 使用尾部有效 bucket，且 `candidate_pass` 真正受 `eps_switch` 约束

### Story 18.15：`T1` 在线评分、漂移证据与 `shadow baseline`

**优先级**：P0

**设计来源**：

- `design.md`：`6.7 ~ 6.10`、`10.2.2`
- `code-design.md`：第 `6.7`、`6.8`、`10.2.2`

**复用现有**：

- `drift_state.h`
- `shadow_state.h`
- `result_builder.h`

**必须实现**：

- 重写 `value_detector_core.*`
- 接入 `formal_predictor`、`ReadinessState`、`DriftState`、`ShadowState`
- 产出正式 `ValueEvidence`
- 产出 `RebuildIntent`
- 将 `ValueTask` 接到新 detector 热路径
- 落实 `shadow baseline` 的 `confirm_count >= 3` 激活保护与 `c_shadow_max` 置信度上限

**明确不做**：

- 不实现 `T2`

**禁止事项**：

- 禁止保留旧常数预测路径
- 禁止把 `shadow baseline` 退化成单纯 provider 标记

**涉及文件**：

- 重写：`src/plugins/baseline/detector/value_detector_core.*`
- 修改：`src/plugins/baseline/common/result_builder.h`
- 修改：`src/plugins/baseline/task/value_task.*`

**验收标准**：

- [x] `T1a / T1b` 热路径都走新 predictor 与新证据结构
- [x] `sample_count -> gate / rho / sigma_eff` 在 `T1b` 正式生效
- [x] `shadow baseline` 能激活、更新、退出，并触发正式重建
- [x] `shadow baseline` 不会在 `confirm_count < 3` 时提前接管
- [x] `shadow` 模式下 `confidence` 会被 `c_shadow_max` 裁剪，且不改写原有 `reason_code` 体系

### Story 18.16：`T2` 训练与在线评分闭环

**优先级**：P0

**设计来源**：

- `design.md`：`7.2 ~ 7.9`
- `code-design.md`：第 `7` 章

**复用现有**：

- `formal_model_trainer.*`、`candidate_builder.*`、`candidate_validator.*` 中已经由 `T1` 打通的共用基础设施
- `ratio_task.*`

**必须实现**：

- 在 trainer 中补齐 ratio 路径：
  - `ComputeM0`
  - `ComputeAlphaBeta`
  - `BuildSmoothedRatioTarget`
  - `BuildRatioTrainWeight`
  - `TrainRatioCore / MonthPos / Event`
- 重写 `ratio_detector_core.*`
- 接入 `p_smooth / logit / sigmoid / phi_over / v_floor / rho_t`
- 接入来源借用、冷启动抑制与 ratio rebuild

**明确不做**：

- 不实现 `T3`

**禁止事项**：

- 禁止直接在热路径拟合裸 `numerator / denominator`
- 禁止把 `phi_over`、`v_floor` 只做配置项，不进正式计算

**涉及文件**：

- 修改：`src/plugins/baseline/rebuild/formal_model_trainer.*`
- 修改：`src/plugins/baseline/rebuild/candidate_builder.*`
- 修改：`src/plugins/baseline/rebuild/candidate_validator.*`
- 重写：`src/plugins/baseline/detector/ratio_detector_core.*`
- 修改：`src/plugins/baseline/task/ratio_task.*`

**验收标准**：

- [x] `T2` 训练、预测、评分、重建全链路闭环
- [x] `rate_core / ratio_bursty` 的 profile 差异真实生效
- [x] 来源借用在 `T2` 上可服务，并能输出 `baseline_source_key`
- [x] 来源借用期间本级训练不会暂停，且 `self` 一旦 ready 会立即切回，不做 blending
- [x] `provider = none` 的冷启动窗口不会输出正式高强度异常
- [x] `configured_source` 的 `confidence_base` 低于本级 `self` 正式模型

### Story 18.17：`T3` basis、摘要特征与 routed detector

**优先级**：P0

**设计来源**：

- `design.md`：`8.2 ~ 8.11`
- `code-design.md`：第 `8` 章

**复用现有**：

- `relation_basis.*`
- `relation_summary_extractor.*`
- `relation_router.*`
- `relation_task.*`

**必须实现**：

- 补齐 `ServiceBasis / EvalBasis / lineage` 结构与兼容判断
- 实现 `entropy_shannon / top1_share / headK_share / out_of_support_share / distinct_group_count / stable_g[i]_share / stable_headK_coverage / stable_headK_mix_drift`
- 让 `RelationRouter::BuildRoutedFeatureSpecs(...)` 真实消费：
  - `RelationTaskClockSpec`
  - task-bound `EventCalendarSpec?`
  - task-bound `BaselineSourceResolver?`
- 让 relation task 真实持有 routed `ValueDetectorCore / RatioDetectorCore`

**明确不做**：

- 不实现模式融合

**禁止事项**：

- 禁止 `T3` 停留在 “block -> summary” 而不接 detector
- 禁止 basis 刷新时重新回到“按当前 topK 动态建模”

**涉及文件**：

- 修改：`src/plugins/baseline/relation/relation_basis.*`
- 修改：`src/plugins/baseline/relation/relation_summary_extractor.*`
- 修改：`src/plugins/baseline/relation/relation_router.*`
- 修改：`src/plugins/baseline/task/relation_task.*`

**验收标准**：

- [x] `T3` 摘要特征定义与 `design.md` 一致
- [x] routed detector 全部接到正式 `T1 / T2` core
- [x] `ServiceBasis / EvalBasis` 可快照、可比较、可切换

### Story 18.18：模式融合层与 key 级风险合成

**优先级**：P0

**设计来源**：

- `design.md`：`9.1 ~ 9.2`
- `code-design.md`：第 `9` 章

**复用现有**：

- `baseline_plugin.*`
- `result_builder.h`

**必须实现**：

- 新增 `relation_pattern_fusion.*`
- 新增 `key_risk_fusion.*`
- 落地：
  - `core_P / support_P / oppose_P`
  - `lambda_sup / lambda_opp / lambda_P(pattern)`
  - `Risk_T1T2 / Risk_single_T3 / Risk_pattern / Risk_T3 / Risk(Key,t)`
  - `FusionSourceId`
  - `DominantSingleProjection / DominantPatternProjection`
  - relation task / value task / ratio task 的统一推送接口

**明确不做**：

- 不做未来增强模式

**禁止事项**：

- 禁止让 `T3` 只输出 routed singles，不输出 pattern 级证据
- 禁止不同 `bucket_id` 串窗融合

**涉及文件**：

- 新增：`src/plugins/baseline/fusion/relation_pattern_fusion.*`
- 新增：`src/plugins/baseline/fusion/key_risk_fusion.*`
- 新增：`src/plugins/baseline/fusion/fusion_types.h`
- 修改：`src/plugins/baseline/baseline_plugin.*`
- 修改：`src/plugins/baseline/task/value_task.*`
- 修改：`src/plugins/baseline/task/ratio_task.*`
- 修改：`src/plugins/baseline/task/relation_task.*`
- 修改：`src/plugins/baseline/CMakeLists.txt`
- 修改：`src/tests/test_baseline/test_baseline_plugin.cpp`
- 修改：`src/tests/test_baseline/test_baseline_model_helpers.cpp`
- 修改：`src/tests/test_baseline/CMakeLists.txt`

**验收标准**：

- [x] `FusionResult` 正式输出可用
- [x] `RelationTask::SubmitBlock(...)` 的同步返回结果与 `QueryKeyFusionSnapshotJson(...)` 中同一 bucket 的 finalized / active 视图一致
- [x] `QueryKeyFusionSnapshotJson` 可返回 `latest_finalized_result`
- [x] `RemoveTaskContributions(task_id)` 能清理 direct / routed / pattern 三类来源
- [x] `dominant_pattern` 至少完整输出 `pattern / feature_base / score_pattern / metrics_hit / supporting_features`

### Story 18.19：`T3` 正式重建、验证与 lineage 切换

**优先级**：P0

**设计来源**：

- `design.md`：`10.2.3` 中 `T3` 特化部分
- `code-design.md`：第 `8.3`、`10.2.3`

**复用现有**：

- `candidate_builder.*`
- `candidate_validator.*`
- `rebuild_worker.*`
- `relation_basis.*`

**必须实现**：

- 在 `candidate_builder.*` 中补齐 `CandidateServiceBasis / CandidateEvalBasis`
- 在 `candidate_validator.*` 中补齐 `EvalBasis` 上的 `T3` 验证
- 在 `candidate_validator.*` 中落实 `holdout tail validation`、`T3 candidate eval model`、以及 `incumbent = shadow` 时的 prequential 计损
- 在 `rebuild_worker.*` 中补齐 `new lineage / rebuild_blocked / candidate_pass / candidate_fail`
- 确保 `shadow baseline` 在 `T3` routed 序列上与正式切换衔接

**明确不做**：

- 不做跨任务 model sharing

**禁止事项**：

- 禁止 `T3` 正式重建直接在新 `ServiceBasis` 上和旧 incumbent 硬比
- 禁止 `group_space_id / group_space_version` 不兼容时仍强行复用旧 `EvalBasis`

**涉及文件**：

- 修改：`src/plugins/baseline/rebuild/candidate_builder.*`
- 修改：`src/plugins/baseline/rebuild/candidate_validator.*`
- 修改：`src/plugins/baseline/rebuild/rebuild_worker.*`
- 修改：`src/plugins/baseline/relation/relation_basis.*`
- 修改：`src/plugins/baseline/task/relation_task.*`

**验收标准**：

- [x] `T3` 的 `candidate vs incumbent` 比较在共同 `EvalBasis` 上完成
- [x] `new lineage` 与兼容 lineage 两条路径都可测试
- [x] relation task 的正式切换与 `shadow` 桥接闭环成立
- [x] `candidate service model` 与 `candidate eval model` 的职责分离可测试
- [x] `incumbent = shadow baseline` 时，验证链路走 prequential replay，而不是冻结 formal 比较

### Story 18.20：测试收口、死代码清理与最终一致性审查

**优先级**：P0

**设计来源**：

- `design.md`：全篇
- `code-design.md`：第 `13`、`14` 章

**复用现有**：

- 当前 `test_baseline_*` 测试框架

**必须实现**：

- 补齐 `T1 / T2 / T3 / fusion / rebuild / relation snapshot / concurrency` 测试
- 删除旧的 `intercept-only`、常数预测、未被新路径使用的死代码
- 对照 `design.md` 与 `code-design.md` 做最终一致性复审
- 回填 `planning.md` 勾选状态与最终结论

**明确不做**：

- 不做 Web / HTTP 管理 API
- 不做调度层集成

**禁止事项**：

- 禁止以“测试通过”替代“设计一致”
- 禁止保留任何已经被正式算法替代的旧占位路径

**涉及文件**：

- 修改：`src/tests/test_baseline_*`
- 删除：本轮确认无用的旧占位代码
- 修改：`tasks/sprints/sprint20-baselineA/review.md`（如需）

**验收标准**：

- [x] 测试矩阵覆盖 `code-design.md` 第 `14` 章要求
- [x] 无旧正式模型占位路径残留
- [x] 最终代码审查能够说明“实现与设计一致”

**完成结论**：

- 已重新编译并执行 `test_baseline`、`test_baseline_value_task`、`test_baseline_ratio_task`、`test_baseline_relation_task`、`test_baseline_model_helpers`、`test_baseline_task_headers`、`test_baseline_rebuild`、`test_baseline_concurrency`
- 已收口 direct task 与 routed detector 的 `CompiledEventCalendar + runtime key` 正式预测链，并移除旧 `PredictFormalModel(... EventCalendarSpec ...)` 兼容入口及残余 helper
- 已完成 `review-fix-plan.md` 中的 `P0 / P1 / P2` 收口：
  - `P0-1`：`RelationTask::SubmitBlock()` 返回结果生命周期修复
  - `P0-2`：`KeyRiskFusion / FusionResult` 固定上限、低分配收口
  - `P0-3`：业务时区语义与 ICU 依赖落地，`TimezoneMutex` 移除
  - `P1-1`：relation routed runtime 改为锁外物化、锁内回写
  - `P1-2`：`ValueDetectorCore / RatioDetectorCore` 收口为固定 shard 锁
  - `P1-3`：高基数 runtime 与 key fusion 已补 idle prune 与可观测性
  - `P2-1`：`14.4` 所需 `building / built / validating` 中间态已真实写入，`candidate` 在线服务死分支已删除
  - `P2-2`：`SeedMetricBasisForTesting` 已下沉为 test-only seam，不再污染生产 ABI
- 最终边界、一致性与测试矩阵结论见 `review.md`
- `14.4` 残余缺口已按 `review-fix-plan.md` 收口：重建状态机词汇、`failure_reason` 语义、relation `candidate_fail / new lineage`、`RemoveTaskContributions(task_id)` 精确清理、状态机中间态观测与 candidate 在线死分支清理均已有实现与测试证据

---

## 5. 测试策略

### 5.1 必测维度

1. 配置解析与 task 创建失败路径
2. `T1`：训练、预测、评分、`shadow`、重建、切换
3. `T2`：平滑比例变换、评分、来源借用、重建
4. `T3`：basis、summary、router、routed detector、pattern fusion、key risk
5. `HistoryReader` 缺失、`insufficient_data`、`unavailable`
6. relation snapshot 与 key fusion snapshot
7. 并发边界：
   - task 关闭与 contribution 清理
   - rebuild queue 并发入队
   - relation task 多 key 状态隔离

### 5.2 测试文件策略

优先保留并扩充现有测试文件，不重新造平行测试目录：

- 保留并补强：
  - `test_baseline_plugin.cpp`
  - `test_baseline_concurrency.cpp`
- 重写或大幅重构：
  - `test_baseline_value_task.cpp`
  - `test_baseline_ratio_task.cpp`
  - `test_baseline_rebuild.cpp`
  - `test_baseline_relation_task.cpp`

### 5.3 验证纪律

- 每个 Story 完成后至少补一组对应该 Story 的单元或集成测试
- 不允许等到 `18.20` 再集中补测
- 声称“某条路径已完成”前，必须有可复核的测试或快照输出

---

## 6. 本轮非目标

以下内容不在 Sprint 20 BaselineA：

- Web / HTTP 管理 API
- 调度层集成
- 分布式模型共享
- `changepoint`
- 未来增强型事件模型
- `new_group_share / other_group_share` 等已被排除的重型增强项

---

## 7. 完成判定

只有当以下条件同时成立，本轮才算完成：

1. `T1 / T2 / T3` 的正式能力全部能在代码中找到唯一承载。
2. `baseline` 插件不再依赖旧的占位型模型实现。
3. `planning.md` 中所有 Story 均已回填完成状态。
4. 最终代码审查能够逐条映射到 `design.md` 与 `code-design.md`，不再出现“只有壳，没有算法”的问题。

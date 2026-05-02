# B1 Optional Bootstrap Engine 阶段设计

## 1. 文档定位

本文档只覆盖 `B1：Optional Bootstrap Engine` 阶段。

本阶段目标是把旧基线中的历史训练能力干净抽离成可选启动器，为后续 `Online Rolling Core` 提供启动 seed 和可验证的未来预测接口。

本文不设计在线滚动更新算法，不设计成熟度自动推进，不设计 `T3` stream-only basis 刷新。

旧基线算法来源可按需定向参考 [Sprint 19 Baseline 设计](../sprint19-baseline/design.md)，但不得全文引用。本文只继承历史训练、预测和 `T3` 初始 basis 能力；`shadow/candidate/rebuild` 在线恢复链路只作为迁移对象，不进入新主路径。

---

## 2. 阶段目标

`B1` 的一句话目标：

```text
把旧 baseline 的历史拟合能力改造成 Optional Bootstrap Engine，并删除旧在线恢复链路对主路径的影响。
```

完成后，旧基线能力只允许以以下形式存在：

1. 使用一次性历史数据训练 bootstrap artifact。
2. 使用 bootstrap artifact 对未来 bucket 输出 baseline band。
3. 导出 `B2 Online Rolling Core` 可消费的 `BootstrapSeed`。
4. 为 `T3` 导出初始 `RelationServiceBasis` seed，以及由历史 relation 摘要训练出的 routed value/ratio seed。

`B1` 不允许继续保留以下主路径假设：

```text
正式模型固定服务
  -> 漂移后进入 shadow baseline
  -> 异步拉历史训练 candidate
  -> candidate validate
  -> full model switch
```

---

## 3. 总体结构

推荐将旧 `rebuild/` 中可复用能力迁移到新的 `bootstrap/` 边界：

```text
historical observations
  -> BootstrapTrainer
  -> BootstrapArtifact
  -> BootstrapPredictor
  -> BootstrapPrediction
  -> BootstrapSeed
```

建议代码结构：

```text
src/plugins/baseline/bootstrap/
  bootstrap_types.h
  bootstrap_engine.h
  bootstrap_engine.cpp
  bootstrap_trainer.h
  bootstrap_trainer.cpp
  bootstrap_predictor.h
  bootstrap_predictor.cpp
```

迁移原则：

- `bootstrap/` 只依赖历史输入、任务规格、profile、solver、calendar 和 relation basis。
- `bootstrap/` 不依赖 `RebuildQueue`、`RebuildWorker`、`HistoryReader`、`ShadowState`、`CandidateValidator`。
- `bootstrap/` 输出 artifact、prediction、seed，不直接修改在线 runtime state。
- 在线任务后续是否消费 seed，由 `B2` 设计决定。

### 3.1 实现任务顺序

本节只定义代码开始顺序，不新增范围。每个任务完成后都应能独立编译，避免把旧 rebuild 语义拖到后续任务。

| 顺序 | 任务 | 主要文件 | 参考章节 | 完成标志 |
| --- | --- | --- | --- | --- |
| `B1-T01` | 定义新接口、公共类型和最小编译桩 | `framework/interfaces/ibaseline_types.h`、`framework/interfaces/ibaseline_service.h`、`task/*`、`baseline_plugin.*` | `7.1`、`7.4`、`11.1` | `BaselineStatus`、序列化格式、bootstrap 输入 / 输出类型、统一任务接口就位；旧实现先用最小桩适配，保证接口变更后可编译 |
| `B1-T02` | 改造 task config、parser 和 calendar registry | `config_parser.*`、`model/task_spec.h`、`config/runtime_config.*`、`baseline_plugin.*` | `7.8`、`7.9`、`7.10`、`8.1`、`8.2`、`8.3` | 新 task config 可解析；`task_id` 唯一性、`clock_spec`、`calendar_ref` 生效；旧 `key/delta/tz/event_calendar_spec/baseline_source_configs` 主路径完成迁移或删除 |
| `B1-T03` | 建立 `bootstrap/` 模块骨架 | `plugins/baseline/bootstrap/*` | `3`、`6.1`、`6.2`、`6.3`、`6.5` | `BootstrapArtifact`、`BootstrapPrediction`、`BootstrapSeed` 类型和空 engine 编译通过 |
| `B1-T04` | 迁移 T1/T2 历史训练能力 | `bootstrap_trainer.*`，参考 `rebuild/formal_model_trainer.*` | `4.1`、`4.2`、`6.1`、`7.2`、`7.3` | `Value` / `Ratio` 历史输入可训练 artifact，不依赖 shadow / candidate / rebuild；Ratio artifact 保留 `m0/alpha0/beta0` |
| `B1-T05` | 实现 T1/T2 预测 band | `bootstrap_predictor.*`，参考 `model/formal_predictor.*` | `4.3`、`6.2`、`7.3`、`7.5`、`7.6` | `PredictBootstrap` 返回原始空间 / 概率空间的 `mu/lower/upper/band_width`；Value 完成 `log1p` 反变换；Ratio 使用概率空间不确定性 |
| `B1-T06` | 实现 series 级 seed / artifact 生成和 task 级全量序列化 | `bootstrap_engine.*`、`bootstrap_types.*` | `6.3`、`6.4`、`6.6`、`6.7`、`7.1`、`11.2`、`11.3` | 可按 `series_key` 获得内存态 `BootstrapSeed`；`ExportBootstrapArtifact`、`LoadBootstrapArtifact`、`ExportBootstrapSeed` 以 task 为单位导出 / 导入全部 series，并做严格兼容性校验 |
| `B1-T07` | 改造 registry、plugin 生命周期和 service snapshot | `task/task_registry.*`、`baseline_plugin.*`、`task/baseline_task_base.*` | `7.4`、`7.10`、`7.11` | registry 使用 config 提供的 `task_id`；`Create*Task` 返回 `shared_ptr`；`Close` 幂等；`QueryServiceSnapshot` 不再暴露 rebuild queue / worker 状态 |
| `B1-T08` | 接入 T1/T2 统一任务模型 | `task/value_task.*`、`task/ratio_task.*`、`baseline_plugin.*` | `7.0`、`7.4`、`7.5`、`7.6` | `Create*Task -> Bootstrap(series_key) -> PredictBootstrap(series_key) -> Export*()` 路径打通；任务内部按 series 保存 artifact / seed；导出 / 导入按 task 全量处理；任务不再持有 `HistoryReader`、`RebuildRuntime`、`KeyRiskFusion` |
| `B1-T09` | 接入 T3 初始 basis 与离线摘要 bootstrap | `relation/relation_basis.*`、`bootstrap_trainer.*`、`task/relation_task.*` | `4.5`、`6.3`、`7.2`、`7.7` | 历史 `RelationBootstrapBlock` 聚合为 `RelationGroupHistoryStat` 后调用 `BuildServiceBasis`，并在该 basis 上离线路由 relation 摘要，导出 routed value/ratio seed；不调用 `BuildEvalBasis`，不恢复旧在线 route |
| `B1-T10` | 删除旧在线恢复链路 | `rebuild/*`、`model/shadow_state.h`、`model/formal_model_state.h`、`task/*` | `5`、`8.3`、`9.3`、`11.3` | 代码中移除 `RequestRebuild`、`SetHistoryReader`、`SubmitObservation`、`SubmitBlock`、shadow / candidate / rebuild 状态和配置 |
| `B1-T11` | 补齐验证 | `tests` / baseline 相关测试 | `10` | 覆盖训练、预测、序列化重载、无旧状态触发、配置清理；编译或测试能证明旧在线恢复符号不再进入 B1 主路径 |
| `B1-T12` | 补充 `BootstrapSeed` 质量自动评价 | `bootstrap_engine.*`、`bootstrap_types.*`、`config/runtime_config.*`、`tests` | `6.3`、`6.3.1`、`8.2`、`10` | `EvaluateBootstrapSeedStatus()` 基于 profile 目标、训练跨度、覆盖率、phase coverage、组件可用性和 `sigma_init` 自动输出 `full/partial/weak/none`；调用方不能直接写入成熟度标签；routed summary seed 使用同一评价规则并补充测试 |

### 3.2 Event 与 MonthPos 收口待办

本节记录 `B1` 已识别的历史拟合组件收口项，用于确认 `Optional Bootstrap Engine` 的历史模型能力与设计一致。

| 编号 | 能力 | 必须完成的代码结果 | 验证要求 | 状态 |
| --- | --- | --- | --- | --- |
| `B1-E01` | 全局 calendar registry | runtime config 解析 `calendars`，插件持有全局 calendar registry；task 创建时通过 `calendar_ref` 解析并绑定匹配的 `CompiledEventCalendar` | 配置中存在 calendar 时，task 能获得匹配 calendar；calendar 缺失或版本不匹配时 event 组件降级而不是误用 | 已完成 |
| `B1-E02` | Event 训练和预测接入 | `TrainValue` / `TrainRatio` 将 task 绑定的 `CompiledEventCalendar` 传给 bootstrap trainer；`PredictBootstrap` 使用 artifact 中的 event block 或匹配 calendar 做事件修正 | 历史事件区间中的观测能训练出 `event_block.enabled = true`；未来同类 event bucket 的预测明显区别于非 event bucket | 已完成 |
| `B1-M01` | MonthPos artifact round-trip | `BootstrapArtifact` JSON 写出并读回 `monthpos_block`；`LoadBootstrapArtifact` 后预测结果不丢失月位置效应 | 训练出 monthpos 后，export/load 前后的同 bucket 预测一致，且 artifact JSON 包含 `monthpos_block` | 已完成 |
| `B1-M02` | MonthPos seed 交接 | `BootstrapSeed` 显式导出 `monthpos_hint`，不要求 B2 读取旧 `model.monthpos_block` 才能获得月位置提示 | seed JSON 包含 `monthpos_hint`；启用时 `seeded_components/enabled_components` 能表达 `monthpos` | 已完成 |

---

## 4. 保留边界

### 4.1 T1/T2 历史拟合模型块

保留旧模型中可直接表达历史结构的块：

- `CoreBlock`：`level/trend/day/week` 参数。
- `MonthPosBlock`：月位置参数。
- `EventBlock`：事件日历参数。
- `FitBlockDigest`：训练诊断摘要。
- `ValueFormalModel` / `RatioFormalModel` 中与历史拟合有关的字段。

迁移后建议改名为：

```text
ValueBootstrapModel
RatioBootstrapModel
BootstrapModelMetadata
```

原 `formal` 命名不再适合，因为 bootstrap model 不是长期正式服务模型。

### 4.2 历史训练能力

保留 `FormalModelTrainer::TrainValue / TrainRatio` 中的数学训练能力：

- `weighted_huber_ridge_irls`
- 分阶段拟合：`core -> monthpos -> event`
- 训练覆盖度评估
- `sigma_ref` / 残差尺度估计
- `confidence_base_at_train`
- `readiness`

迁移后建议改名为：

```text
BootstrapTrainer::TrainValue
BootstrapTrainer::TrainRatio
```

训练输入不再包含 `candidate_model_version`、`holdout_count` 等候选验证语义。

### 4.3 历史预测能力

保留 `FormalPredictor` 中对以下组件的预测逻辑：

- `core`
- `monthpos`
- `event`
- `delta`
- `tz`
- `event_calendar` 版本检查

迁移后建议改名为：

```text
BootstrapPredictor::PredictValue
BootstrapPredictor::PredictRatio
```

必须扩展输出：旧预测只返回中心值和 `sigma_ref`，B1 必须返回 baseline band。

旧代码细节需要注意：

- `Value` 预测当前返回的是 `log1p` 模型空间的中心值，B1 对外输出必须反变换到原始观测空间。
- `Ratio` 预测当前返回的是概率空间中心值，并且 `sigma_ref = 0`；B1 不能照搬 Value 的 band 计算方式，需要单独使用 ratio 平滑先验和覆盖度估计不确定性。

### 4.4 Replay 输入承载

保留 `ValueReplaySeries` / `RatioReplaySeries` 的历史点承载思路，但去掉 `replay/rebuild` 命名。

建议改为：

```text
ValueBootstrapSeries
RatioBootstrapSeries
```

历史输入保持稀疏语义：

- 不补空 bucket。
- 不把 gap 填成 `0`。
- 不在 bootstrap 内部调用外部历史读取器。

### 4.5 T3 初始 basis 与离线摘要路由能力

保留 `RelationBasisBuilder::BuildServiceBasis`。

`B1` 只负责从历史 `RelationBootstrapBlock` 中导出初始：

```text
support_explicit
stable_head
head_proto_q
basis_version
```

同时，`B1` 必须在该 basis 上对历史 relation block 做离线摘要投影，并复用 T1/T2 bootstrap trainer 训练 routed summary artifact / seed。该能力用于让后续 rolling 阶段在 `T3 routed 摘要 -> T1/T2 rolling baseline` 路径上获得历史启动参数。

首版 routed 摘要包括：

```text
value summary:
  entropy_shannon
  distinct_group_count
  stable_headk_mix_drift

ratio summary:
  top1_share
  headk_share
  out_of_support_share
  stable_headk_coverage
  stable_g_share_i
```

`B1` 不做：

- 在线 basis 刷新。
- candidate service basis / eval basis 双视图验证。
- basis 切换验证。
- 旧 `RelationRouter::SubmitBlock` 在线热路径。
- `RelationTask` 当前在首个流式 block 上构造一次 bootstrap basis 的逻辑不能作为 B1 主实现；B1 应从完整历史 `RelationBootstrapInput` 聚合后构造 basis。

---

## 5. 删除边界

原则：只要服务于旧在线恢复生命周期，而不是 bootstrap 训练 / 预测 / seed 导出，就删除。

### 5.1 删除 shadow baseline

删除：

- `ShadowStateT`
- `ValueShadowState`
- `RatioShadowState`
- `shadow_active`
- `shadow_ref_kind`
- `shadow_stuck`
- `slow_drift_triggered`
- `kBaselineFlagShadowActive`
- `BaselineProvider::kShadow`
- `BaselineModelState::kShadow`

`B1` 不再提供“冻结参考模型 + 在线 delta”的桥接状态。

### 5.2 删除 candidate validation

删除：

- `CandidateBuilder`
- `CandidateValidator`
- `CandidateBuildStatus`
- `CandidateValidationStatus`
- `candidate_loss`
- `incumbent_loss`
- `candidate_pass`
- `candidate_model`
- `candidate_generation`

Bootstrap 是一次性历史训练结果，不与 incumbent 做在线切换验证。

### 5.3 删除 rebuild worker 链路

删除：

- `RebuildRequest`
- `RebuildQueue`
- `RebuildWorker`
- `RebuildTaskRuntime`
- `RequestRebuild`
- `SetHistoryReader`
- `HistoryReader.fetch` 作为在线恢复前置条件

### 5.4 删除 rebuild 状态机

删除以下状态：

- `RebuildCandidateState`
- `RebuildSwitchState`
- `RebuildFailureReason`
- `RebuildPhase`
- `RebuildStageTrace`
- `FormalModelState` 中的 candidate / switch / validation 字段

`B1` 只保留 bootstrap 训练状态，例如：

```text
not_trained
trained
insufficient_data
solver_unavailable
train_failed
```

### 5.5 删除旧配置项

删除：

```text
runtime_and_rebuild_constants.candidate_builder
runtime_and_rebuild_constants.shadow_policy
runtime_and_rebuild_constants.candidate_validator
runtime_and_rebuild_constants.relation_rebuild
scoring_and_confidence_constants.confidence_shadow_base
scoring_and_confidence_constants.value_shadow_confidence_cap
scoring_and_confidence_constants.ratio_shadow_confidence_cap
scoring_and_confidence_constants.value_shadow_sigma_scale
scoring_and_confidence_constants.ratio_shadow_score_scale
```

`baseline_source_configs` 默认不进入 B1。除非后续明确需要“用其他 key 的历史作为 bootstrap 来源”，否则 B1 不保留跨 key 借基线能力。

---

## 6. 新增设计

### 6.1 BootstrapArtifact

`BootstrapArtifact` 是 B1 的核心训练产物。

建议结构：

```text
BootstrapArtifact = {
  artifact_kind,          // value | ratio | relation
  train_status,
  model_version,
  series_identity,
  coverage_report,
  enabled_components,
  value_model?,
  ratio_model?,
  relation_basis_by_metric?,
  relation_routed_summary_artifacts?,
  sigma_init,
  uncertainty_init,
  maturity_init,
  diagnostics
}
```

语义：

- `artifact` 可以用于预测，也可以导出 seed。
- `artifact` 不是 `RollingState`。
- `artifact` 不在流式阶段继续更新。
- `artifact` 是 series 级训练产物，必须绑定 `series_key`。同一个 task 下不同 `series_key` 的模型参数、覆盖度、成熟度和不确定性互不共享。

### 6.2 BootstrapPrediction

预测接口必须输出 baseline band，而不是单点。

建议结构：

```text
BootstrapPrediction = {
  status,
  series_key,
  bucket_id,
  baseline_mu,
  baseline_lower,
  baseline_upper,
  band_width,
  confidence,
  uncertainty_source,
  model_space_mu?,
  model_space_lower?,
  model_space_upper?,
  diagnostics
}
```

约束：

- 对外主语义是原始观测空间的 `baseline_mu/lower/upper`。
- 对 `Value`，内部可继续使用 `log1p` 模型空间；band 先在模型空间计算，再对 `mu/lower/upper` 做反变换，输出下界至少裁剪到 `0`。
- 对 `Ratio`，输出应在 `[0, 1]` 概率空间。旧 `RatioFormalModel` 没有 `sigma_ref`，因此不能照搬 Value 的残差尺度；首版 band 需基于 `m0/alpha0/beta0`、训练覆盖度和概率裁剪边界给出概率空间不确定性。
- `band_width` 必须随 `sigma_init`、训练覆盖度和成熟度变化。

### 6.3 BootstrapSeed

`BootstrapSeed` 是 B2 的输入准备，不在 B1 内被在线更新。

建议结构：

```text
BootstrapSeed = {
  seed_status,            // none | weak | partial | full
  algorithm_version,
  source_artifact_version,
  artifact_kind,
  series_identity,
  task_identity.feature_type,
  clock_spec,
  coverage_report,
  seeded_components,
  enabled_components,
  theta_init,
  sigma_init,
  uncertainty_init,
  maturity_init,
  ratio_prior_init?,
  relation_basis_by_metric?,
  relation_routed_summary_seeds?,
  diagnostics
}
```

映射关系：

```text
CoreBlock.beta0          -> level_0
CoreBlock.trend_k        -> trend_0
day_sin/day_cos          -> daily harmonic state
week_sin/week_cos        -> weekly harmonic state
MonthPosBlock            -> monthpos seed
ValueFormalModel.sigma_ref -> sigma_init
RatioFormalModel.m0      -> ratio_prior_init.m0
RatioFormalModel.alpha0  -> ratio_prior_init.alpha0
RatioFormalModel.beta0   -> ratio_prior_init.beta0
confidence_base_at_train -> maturity / confidence hint
RelationServiceBasis     -> relation_basis_by_metric
Relation routed summaries -> relation_routed_summary_seeds
```

以下 C++ 结构体是 baseline plugin 内部类型，建议放在 `plugins/baseline/bootstrap/bootstrap_types.h`，不进入 `framework/interfaces` 公共 ABI。

`theta_init` 首版使用结构化字段，不直接携带旧 bootstrap model block：

```cpp
struct BootstrapHarmonicInit {
    int32_t order = 0;
    double sin = 0.0;
    double cos = 0.0;
};

struct BootstrapThetaInit {
    bool available = false;
    std::string model_space;
    int64_t reference_bucket_id = 0;
    double level = 0.0;
    double trend = 0.0;
    std::vector<BootstrapHarmonicInit> daily_harmonic;
    std::vector<BootstrapHarmonicInit> weekly_harmonic;
};
```

`sigma_init`、`uncertainty_init` 和 `maturity_init` 必须显式导出，避免 `B2` 从旧 `model.core_block` 或训练诊断里反推：

```cpp
struct BootstrapClockSpec {
    int64_t bucket_seconds = 0;
    std::string timezone;
};

struct BootstrapSigmaInit {
    bool available = false;
    double value = 0.0;
    std::string model_space;
    std::string source;
};

struct BootstrapRatioPriorInit {
    bool available = false;
    double m0 = 0.5;
    double alpha0 = 0.0;
    double beta0 = 0.0;
    std::string model_space = "probability";
    std::string source;
};

struct BootstrapComponentUncertaintyInit {
    double level_scale = 1.0;
    double trend_scale = 4.0;
    double daily_scale = 2.0;
    double weekly_scale = 4.0;
};

struct BootstrapUncertaintyInit {
    bool available = false;
    double confidence_base = 0.0;
    double confidence_level = 0.95;
    double coverage_ratio = 0.0;
    double band_z = 1.96;
    std::string band_source;
    std::vector<std::string> uncertainty_source;
    BootstrapComponentUncertaintyInit component_uncertainty;
};

struct BootstrapMaturityInit {
    bool available = false;
    BootstrapSeedStatus seed_status = BootstrapSeedStatus::kNone;
    double confidence = 0.0;
    uint64_t accepted_count = 0;
    uint64_t rejected_count = 0;
    double coverage_ratio = 0.0;
};
```

`BootstrapSeed` 的 JSON 不导出旧 `model/core_block`。完整模型快照只属于 `BootstrapArtifact`；`BootstrapSeed` 只导出 `B2` 直接消费的初始化字段。

`series_identity.series_key` 必须显式导出。`BootstrapSeed` 是某个具体序列的 rolling 初始化参数，不是整个 task 的通用参数；同一 task 下不同 `series_key` 的 `theta_init/sigma_init/uncertainty_init/maturity_init` 可能完全不同，不能互相复用。

`task_identity.feature_type` 必须显式导出，用于区分 `value_basic`、`value_sampled`、`ratio` 和 `relation`。`B2` 不能只依赖 `task_kind` 判断观测语义。

`clock_spec` 必须显式导出 `bucket_seconds` 和 `timezone`。`B2` 计算 day/week phase 和做 seed 兼容性校验时使用 seed 自身的 clock，而不是只依赖任务当前配置。

`seeded_components` 与 `enabled_components` 必须同时在顶层 seed 和 relation routed summary seed 中显式导出：

- `seeded_components` 表示 B1 已经交接了对应组件参数，例如 `level/trend/daily/weekly`。
- `enabled_components` 表示 B2 可以直接启用的组件；首版按训练覆盖时长保守启用，level/trend 直接启用，daily 至少覆盖 1 天，weekly 至少覆盖 14 天。
- 禁止再使用 `"core"` 这种打包名称，否则 B2 无法判断 daily/week 是已启用还是仅有参数。

`ratio_prior_init` 只在 ratio seed 中出现，用于显式交接 `RatioFormalModel.m0/alpha0/beta0`。Value seed 不导出该块。

`uncertainty_init.component_uncertainty` 至少导出组件级 scale 提示。它不是完整 Kalman covariance，但给 B2 的初始 `P` 或等价不确定性构造提供显式依据，避免 B2 完全按默认规则反推。

面向 `B2 Online Rolling Core` 的 seed 能力补充：

- `B1` 不导出完整 Kalman covariance，不承担 online state 的协方差建模职责。
- `B1` 必须保证 `uncertainty_init.component_uncertainty` 中的 scale 为有限正数；若训练过程无法估计组件级不确定性，应使用保守默认值，而不是留空或写 `0`。
- `B1` 必须保证 `seed_status`、`coverage_ratio`、`maturity_init.confidence` 与实际训练支撑一致，供 `B2` 映射初始 `P`、confidence 和 cold/warming 状态。
- `theta_init.model_space`、`sigma_init.model_space` 和任务 transform 必须一致；不一致的 seed 对 `B2` 来说是不可兼容 seed，不能依赖 B2 静默降级。
- `relation_routed_summary_seeds` 中的每个 routed value/ratio seed 也必须满足上述要求，否则 `T3 routed summary -> T1/T2 rolling` 无法获得可靠历史启动参数。

#### 6.3.1 Seed 质量自动评价

`seed_status` 由 `BootstrapEngine` 自动计算，调用方不能在 `Bootstrap` 输入里直接指定 `full/partial/weak`。调用方只声明训练目标和 profile，例如是否要求 day/week 周期、最低训练跨度、最低覆盖率和 harmonic 阶数；实际评价必须基于规范化后真正进入训练的 bucket。

`full` 是相对 profile 的完整，不是全能力完整。若当前 profile 只要求 day/week，则不应因为缺少 month/event 能力而降级；若 profile 要求 weekly，则必须有足够 weekly 覆盖才能评为 `full`。

建议内部函数：

```cpp
BootstrapSeedStatus EvaluateBootstrapSeedStatus(
    const BootstrapArtifact& artifact,
    const BootstrapSeedQualityConfig& config,
    std::vector<std::string>* diagnostics);
```

评价输入：

- `artifact.train_status`、`theta_init.available` 和 `sigma_init.available`。
- `coverage_report.accepted_count`、`rejected_count`、`train_start_bucket`、`train_end_bucket`、`coverage_ratio`。
- `seeded_components` 与 `enabled_components`，尤其是 `daily`、`weekly` 是否可用。
- profile 要求的核心周期，例如 `level_only`、`day`、`day_week`。
- 日相位 / 周相位覆盖率；首版可以按 fixed phase bins 统计，不保存原始历史窗口。

默认判定规则：

```text
none:
  训练失败，或缺少 theta_init / sigma_init。

weak:
  只有 level / sigma 基本可用；
  数据不足一个主要周期，或 coverage_ratio 低于 partial 阈值。

partial:
  有可用 seed，但只满足部分 profile 目标。
  例如 daily 可用但 weekly 覆盖不足，或训练跨度足够但 phase coverage 不足。

full:
  profile 要求的核心组件全部可用；
  训练跨度、accepted_count、coverage_ratio、phase coverage 和 sigma_init 均达标。
```

`BootstrapSeed` 顶层和 relation routed summary seed 必须使用同一评价函数。`maturity_init.seed_status` 与顶层 `seed_status` 必须一致；若评价降级，`diagnostics` 必须说明原因，例如 `insufficient_weekly_span`、`low_coverage_ratio`、`missing_sigma_init`。

原因：

- `BootstrapSeed` 是 `B2 Online Rolling Core` 的初始化输入，不是旧模型结构的搬运容器。
- `B2` 不需要读取 `model.core_block` 才能得到 `level/trend/day/week`。
- `level/trend/day/week` 是 rolling core 可以直接消费的启动状态。
- `monthpos_hint/event_hint` 只作为可选提示，`B2` 不必须完全照搬旧模型结构。
- 不把 `CoreBlock/MonthPosBlock/EventBlock` 直接暴露给 `B2`，避免旧模型内部结构污染后续在线滚动设计。

### 6.4 BootstrapArtifact 与 BootstrapSeed 的职责边界

`BootstrapArtifact` 是“训练产物”，`BootstrapSeed` 是“交接参数”。

整体关系：

```text
history
  -> Optional Bootstrap Engine
  -> BootstrapArtifact
       -> PredictBootstrap(series_key, ...)  // B1 自身预测、验证、审计
       -> Build/CacheBootstrapSeed()   // 给 B2 初始化 rolling state
  -> BootstrapSeed
       -> Initialize RollingState      // B2
```

上图中的 `BootstrapArtifact` 和 `BootstrapSeed` 都是某个 `series_key` 下的实例。task 只是它们的归属容器，不是模型参数颗粒度。

`BootstrapArtifact` 面向 `B1 Optional Bootstrap Engine`：

- 保存历史数据训练出的完整模型结果。
- 支持 `PredictBootstrap(series_key, future_bucket)`，用于验证 bootstrap 基线准确性。
- 支持序列化、重启恢复、离线审计和诊断。
- 可以包含较完整的信息，例如 `core/monthpos/event/delta`、训练覆盖率、`sigma`、组件诊断、profile、calendar 版本。
- 更接近“批处理模型快照”。

`BootstrapSeed` 面向 `B2 Online Rolling Core`：

- 只保留 rolling 初始化真正需要的最小信息。
- 初始化 `level_0`、`trend_0`、day/week 周期系数。
- 初始化 `sigma`、band 宽度、`confidence`、`maturity`。
- 可选携带 `monthpos/event` 的初始提示，但不要求 rolling core 完全照搬旧模型结构。
- 对 `T3`，可携带初始 `basis/support/stable_head` seed。
- 对 `T3 routed summary`，每个 routed value/ratio seed 必须携带可初始化 rolling core 的模型参数，不能只导出 summary 名称。
- 更接近“流式滚动模型启动参数”。

`B1 -> B2` 的主交接路径必须是内存中的 `BootstrapSeed` 结构体：

```text
Bootstrap(history)
  -> task owns BootstrapArtifact by series_key
  -> task owns BootstrapSeed by series_key
  -> B2 SubmitObservation / warm-up consumes BootstrapSeed internally
```

不得把 `ExportBootstrapSeed(JSON)` 再 parse / load 作为 B2 初始化主路径。JSON seed 只用于持久化、审计、跨进程导入、调试和重启恢复。即使未来需要从已保存的 seed 启动，也应先在 B2 core 外部反序列化为内部 `BootstrapSeed` 结构体，再进入 rolling 初始化流程。

两者不合并的原因：

- `BootstrapArtifact` 需要完整、可预测、可诊断、可复现。
- `BootstrapSeed` 需要轻量、稳定，只表达 rolling core 能直接消费的初始化状态。
- 这样可以保留旧算法作为 bootstrap 的价值，同时避免把旧算法内部结构和 `shadow/candidate/rebuild` 语义继续带入 `B2`。

### 6.5 BootstrapEngine

`BootstrapEngine` 是内部实现模块，不作为独立任务创建入口对外暴露。

```text
BootstrapEngine
  TrainValue(input) -> ValueBootstrapArtifact
  TrainRatio(input) -> RatioBootstrapArtifact
  TrainRelation(input) -> RelationBootstrapArtifact
  PredictValue(artifact, bucket_id) -> BootstrapPrediction
  PredictRatio(artifact, bucket_id) -> BootstrapPrediction
  ExportSeed(artifact) -> BootstrapSeed
```

任务创建仍统一由 `IBaselineService` 负责。用户创建的是“某个特征的基线任务”，不是先创建 bootstrap task 再创建 rolling task。

任务内部必须按 `series_key` 管理 bootstrap 状态：

```text
BaselineTask
  owns task config / task identity
  owns BootstrapArtifact[series_key]   // B1
  owns BootstrapSeed[series_key]       // B1 -> B2，内存态主交接
  owns RollingState[series_key]        // B2
```

任务级配置只描述特征、时钟、profile 和 calendar；series 级状态才描述某个具体序列的历史训练参数。禁止用单个 task 级 `BootstrapArtifact` 或 `BootstrapSeed` 覆盖多个 `series_key`。

序列化导出 / 导入是非热路径，按 task 全量处理，不要求调用方逐个传入 `series_key`。导出的 artifact / seed 文档必须包含当前 task 下全部已训练 series；每个 series 条目内部携带自己的 `series_identity`。

对外使用方式：

```text
create_result = IBaselineService.CreateValueTask(config_content, format)
baseline_task_bps = create_result.second
baseline_task_bps.Bootstrap(history)
baseline_task_bps.PredictBootstrap(history.series_key, future_bucket, options)
baseline_task_bps.ExportBootstrapArtifact(format)  // 导出全部 series
```

内部状态边界：

```text
BaselineTask
  owns task config          // task 级
  owns BootstrapArtifact[]  // series 级
  owns BootstrapSeed[]      // series 级
  owns RollingState[]       // series 级，B2
```

`BootstrapArtifact` 与 `RollingState` 不混用。`B1` 只实现 bootstrap artifact / seed；`B2` 再决定如何用 seed 初始化 rolling state。

### 6.6 Artifact 序列化

`BootstrapArtifact` 和 `BootstrapSeed` 都应支持 JSON 序列化，但序列化不是 `B1 -> B2` 的主交接方式。

序列化的价值：

- 历史数据只需要提交一次；任务重启后可以直接加载 artifact。
- 可离线保存 bootstrap 训练结果，便于审计和回放。
- 可用同一批历史数据比较不同配置下的训练效果。
- 可对未来 bucket 做离线预测，验证 bootstrap 准确性。
- 可把 seed 保存为外部文档，供调试、审计或跨进程导入使用。
- 导出 / 导入不是热接口，默认以 task 为单位处理全部 series，避免调用方在恢复或迁移时遗漏某个 `series_key`。

明确限制：

- 同一进程内，`B2` 初始化 rolling state 必须直接消费 `BootstrapSeed` 结构体。
- 不允许把 `ExportBootstrapSeed(JSON) -> parse/load -> rolling 初始化` 作为主路径。
- 重启恢复时优先加载 task 级全量 `BootstrapArtifact`，再在内存中为每个 series 重新生成 `BootstrapSeed`；若只保存了 seed 文档，也必须先在 B2 core 外部反序列化为结构体。
- `LoadBootstrapArtifact(content, format)` 必须原子替换当前 task 的 bootstrap store：任一 series 条目不兼容或重复，整体失败，不能部分导入。

建议区分两个序列化对象：

```text
BootstrapArtifact serialized content
  task 级完整训练产物，包含全部 series，可预测、可诊断、可回放。

BootstrapSeed serialized content
  task 级最小初始化文档，包含全部 series，面向外部保存 / 审计 / 导入，不是进程内主交接对象。
```

### 6.7 最小序列化 schema

`B1` 首版只要求 JSON 格式，但 schema 必须稳定，便于重启恢复和严格兼容性校验。

`BootstrapArtifact` 最小字段：

```json
{
  "schema_version": 1,
  "document_kind": "bootstrap_artifact",
  "artifact_kind": "value|ratio|relation",
  "algorithm_version": "b1-bootstrap-v1",
  "task_identity": {},
  "profile": {},
  "clock_spec": {},
  "calendar_ref": {},
  "series_artifacts": [
    {
      "series_identity": {
        "series_key": "link_001"
      },
      "train_coverage": {},
      "enabled_components": [],
      "model": {},
      "diagnostics": {}
    }
  ],
  "diagnostics": {}
}
```

`model` 字段不能只是非结构化 blob。首版最少保留以下可重建预测和 seed 的字段：

```text
value model:
  transform_name, solver_name, fit_strategy, bucket_seconds, timezone,
  train_start_bucket, train_end_bucket, core_block, monthpos_block,
  event_block, fit_summary, sigma_ref, confidence_base_at_train

ratio model:
  transform_name, solver_name, fit_strategy, bucket_seconds, timezone,
  train_start_bucket, train_end_bucket, core_block, monthpos_block,
  event_block, fit_summary, m0, alpha0, beta0, sigma_ref,
  confidence_base_at_train

relation model:
  basis_by_metric, support_policy, summary_policy, group_space_id,
  group_space_version, valid_bucket_count, routed_summary_artifacts
```

`BootstrapSeed` 最小字段：

```json
{
  "schema_version": 1,
  "document_kind": "bootstrap_seed",
  "algorithm_version": "b1-bootstrap-v1",
  "artifact_kind": "value|ratio|relation",
  "task_identity": {
    "feature_type": "value_basic|value_sampled|ratio|relation"
  },
  "clock_spec": {
    "bucket_seconds": 60,
    "timezone": "Asia/Shanghai"
  },
  "calendar_ref": {
    "calendar_id": "cn-holiday",
    "calendar_version": "2026.1"
  },
  "series_seeds": [
    {
      "series_identity": {
        "series_key": "link_001"
      },
      "seed_status": "none|weak|partial|full",
      "source_artifact_version": 1,
      "seeded_components": [],
      "enabled_components": [],
      "theta_init": {},
      "sigma_init": {},
      "ratio_prior_init": {},
      "uncertainty_init": {
        "component_uncertainty": {
          "level_scale": 1.0,
          "trend_scale": 4.0,
          "daily_scale": 2.0,
          "weekly_scale": 4.0
        }
      },
      "maturity_init": {},
      "relation_basis_by_metric": [],
      "relation_routed_summary_seeds": [],
      "diagnostics": {}
    }
  ],
  "diagnostics": {}
}
```

`LoadBootstrapArtifact` 必须校验 `document_kind`、`artifact_kind`、`schema_version`、`algorithm_version`、`task_identity`、`profile`、`clock_spec` 和 `calendar_ref`。每个 `series_artifacts[]` 条目必须校验 `series_identity`，`series_key` 不能为空且不能重复。不兼容时返回 `kIncompatibleArtifact`。

`value`、`ratio` 和 `relation` artifact 都必须支持 JSON round-trip reload。`relation` reload 后至少能恢复 `relation_basis_by_metric` 与 `relation_routed_summary_artifacts`，并继续导出等价的 `BootstrapSeed`。

---

## 7. 接口定义

本节按“统一任务”模型设计。`IBaselineService` 负责创建任务；`B1` 只暴露 bootstrap 阶段能力，后续 rolling 阶段沿用同一个任务身份再扩展接口。

不新增独立 `IBaselineBootstrapService` 作为任务创建者。

### 7.0 已确认接口结论

`B1` 采用统一任务模型：

- `IBaselineService` 是唯一任务创建入口。
- 用户创建的是“某个数据特征的基线任务”，不是 bootstrap task 与 rolling task 两个任务。
- `BootstrapEngine` 是任务内部实现模块，不对外负责创建任务。
- `BootstrapArtifact`、`BootstrapSeed`、未来的 `RollingState` 归属于同一个 `IBaselineTask`，但实际模型状态按 `series_key` 分桶保存。

`B1` 对外暴露一次性 bootstrap 入口：

```cpp
auto [create_status, task] =
    service->CreateValueTask(config_content, BaselineSerializationFormat::kJson);

BootstrapTrainResult train_result = task->Bootstrap(history);
BootstrapPrediction prediction =
    task->PredictBootstrap(history.series_key, future_bucket, predict_options);

auto [artifact_status, artifact_content] =
    task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);

auto [seed_export_status, seed_content] =
    task->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
```

原因：

- 当前约束是历史数据只在启动阶段批量提交一次。
- 一次性 `Bootstrap(history)` 更符合 B1 的阶段目标，接口更简单。
- `Bootstrap(history)` 通过 `history.series_key` 定位要训练的序列；同一 task 可以依次 bootstrap 多个 `series_key`，互不覆盖。
- 如后续需要支持超大历史数据，可以在内部实现分块累积或工具层导入，但不把 `AppendHistory + TrainBootstrap` 暴露为 B1 主接口。

序列化结论：

- `BootstrapArtifact` 和 `BootstrapSeed` 的序列化导出是 B1 必须提供的能力。
- 内存态 `BootstrapSeed` 是 task 内部状态，不进入 `IBaselineTask` 公共接口；后续 B2 rolling 初始化在同一 task 内部直接消费。
- `ExportBootstrapArtifact(format)`、`LoadBootstrapArtifact(content, format)` 和 `ExportBootstrapSeed(format)` 都是 task 级全量接口，覆盖当前 task 下全部 `series_key`。
- 首版只要求 JSON 序列化，并在 JSON 中保留 `schema_version`、`algorithm_version`、`profile_version` 等版本字段。
- 二进制格式不进入 B1 主路径；如后续有性能或体积压力，再单独增加。
- 函数名称不写死 `Json`，格式通过 `BaselineSerializationFormat` 指定。
- 序列化导出接口返回 `std::pair<BaselineStatus, std::string>`，不使用 `std::string*` 出参。
- 版本、profile、calendar 等元信息写入序列化内容内部，不在导出返回值里重复展开。
- `B2` 不从 `seed_content` 初始化 rolling state；若使用外部保存的 seed 文档，必须先在 B2 core 外部反序列化为内部结构体。

预测结论：

- `PredictBootstrap(series_key, bucket_id)` 不要求调用方每次传入 calendar。
- 任务通过创建配置中的 `calendar_ref` 解析全局 calendar registry。
- `Value` 的预测 band 默认输出到原始观测空间，可附带模型空间调试字段。
- `Ratio` 的预测 band 默认输出到 `[0, 1]` 概率空间。

加载结论：

- `LoadBootstrapArtifact` 默认做严格兼容性校验。
- 至少校验 task kind、feature identity、series identity、profile、clock spec、calendar ref、schema version 和 algorithm version。
- `LoadBootstrapArtifact(content, format)` 必须校验文档内所有 `series_identity.series_key`，拒绝空 key、重复 key 和不兼容 series 条目。
- 不兼容时返回明确错误或诊断，不做静默加载。

### 7.1 公共类型

`B1` 新增接口以 C++17 为基线，可以使用 `std::string_view` 和 structured binding。

序列化返回值采用 `std::pair`，保持接口轻量：

```cpp
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flowsql {

enum class BaselineSerializationFormat : int32_t {
    kJson = 0,
};

enum class BaselineStatus : int32_t {
    kOk = 0,
    kNotTrained = 1,
    kInsufficientData = 2,
    kInvalidArgument = 3,
    kCalendarUnavailable = 4,
    kIncompatibleArtifact = 5,
    kUnsupportedFormat = 6,
    kParseFailed = 7,
    kTrainFailed = 8,
    kPredictFailed = 9,
    kSerializationFailed = 10,
};

enum class BootstrapSeedStatus : int32_t {
    kNone = 0,
    kWeak = 1,
    kPartial = 2,
    kFull = 3,
};

using BaselineSerializedContent = std::string;
using BaselineSerializationResult = std::pair<BaselineStatus, BaselineSerializedContent>;

}  // namespace flowsql
```

约定：

- `BaselineSerializationResult.first` 是状态。
- `BaselineSerializationResult.second` 是序列化内容；当状态不是 `kOk` 时可以为空。
- 调用方使用 C++17 structured binding 读取结果。
- 不使用 `std::tuple` 作为公开接口，避免多个同类型字段造成语义不清。
- `BaselineStatus` 不继续细分到诊断粒度；更细的训练、预测、序列化原因写入 `diagnostics` 或 artifact 序列化内容。

### 7.2 Bootstrap 输入类型

`Bootstrap(...)` 一次性接收历史数据。任务只保存训练得到的 artifact，不保存调用方传入的历史数组。

```cpp
namespace flowsql {

struct BootstrapTrainOptions {
    bool force_replace_existing_artifact = true;
    bool enable_monthpos = true;
    bool enable_event = true;
    bool include_diagnostics = true;
    uint32_t min_observation_count = 0;  // 0 means using profile default.
};

struct ValueBootstrapPoint {
    int64_t bucket_id = 0;
    double value = 0.0;
    uint64_t sample_count = 0;
};

struct RatioBootstrapPoint {
    int64_t bucket_id = 0;
    double numerator = 0.0;
    double denominator = 0.0;
};

struct RelationBootstrapMetric {
    std::string metric;
    double total = 0.0;
    uint32_t active_count = 0;
    std::vector<double> values_by_group;
};

struct RelationBootstrapBlock {
    int64_t bucket_id = 0;
    std::vector<uint32_t> group_idx;
    std::vector<RelationBootstrapMetric> metrics;
};

struct ValueBootstrapInput {
    std::string series_key;
    std::vector<ValueBootstrapPoint> observations;
    BootstrapTrainOptions options;
};

struct RatioBootstrapInput {
    std::string series_key;
    std::vector<RatioBootstrapPoint> observations;
    BootstrapTrainOptions options;
};

struct RelationBootstrapInput {
    std::string series_key;
    std::vector<RelationBootstrapBlock> blocks;
    BootstrapTrainOptions options;
};

}  // namespace flowsql
```

说明：

- `Value` / `Ratio` 使用 owned point，避免把 `BaselineStringRef` 带入 bootstrap 历史输入。
- `Relation` bootstrap 使用 owned block，避免历史输入依赖外部指针生命周期。
- `series_key` 是 bootstrap 训练和预测的 series 级模型身份；不能为空。导出 / 导入按 task 全量处理，但每个序列化条目仍必须保留 `series_identity.series_key`。

输入准入与规范化：

- `Bootstrap` 内部按 `bucket_id` 升序处理，允许调用方提交乱序数组。
- `series_key` 为空时返回 `kInvalidArgument`，不创建匿名 artifact / seed。
- 同一个 `bucket_id` 的重复点必须按任务类型聚合：`Value` 以样本数加权合并，`Ratio` 累加 numerator / denominator，`Relation` 合并同 metric / group 位置。
- gap 不补点，不填 `0`。
- 非有限数值、负样本数、负 numerator、负 denominator 直接拒绝。
- `Ratio` 中 denominator 为 `0` 的点不参与训练，计入 `rejected_count`。
- 有效点数低于 `min_observation_count` 或 profile 默认阈值时返回 `kInsufficientData`。
- `accepted_count`、`coverage_ratio` 和 `maturity_init` 必须基于“规范化且实际进入训练的 bucket”计算；被 `value_sampled.n_train_min` 或 `ratio.d_min_train` 过滤掉的 bucket 计入 `rejected_count`，不能抬高 seed 成熟度。
- `coverage_ratio = accepted_unique_bucket_count / train_bucket_span`，按训练实际使用的 `train_start_bucket/train_end_bucket` 计算，并限制在 `[0, 1]`。
- `Relation` 训练前先按 metric / group 聚合历史 block：`valid_bucket_count` 来自有效历史 bucket 数，`hist_mass` 累加 group 质量，`active_bucket_count` 累加 group 出现的有效 bucket 数；聚合结果再传给 `RelationBasisBuilder::BuildServiceBasis`。

### 7.3 Bootstrap 输出类型

训练结果只表达 bootstrap 训练状态、覆盖度和 seed 成熟度，不携带旧 `candidate/rebuild` 状态。

```cpp
namespace flowsql {

struct BootstrapTrainResult {
    BaselineStatus status = BaselineStatus::kOk;
    BootstrapSeedStatus seed_status = BootstrapSeedStatus::kNone;
    uint64_t accepted_count = 0;
    uint64_t rejected_count = 0;
    int64_t train_start_bucket = 0;
    int64_t train_end_bucket = 0;
    double coverage_ratio = 0.0;
    double confidence = 0.0;
    std::vector<std::string> enabled_components;
    std::string diagnostics;

    bool ok() const { return status == BaselineStatus::kOk; }
};

struct BootstrapPredictionOptions {
    double confidence_level = 0.95;
    bool include_model_space_debug = false;
    bool include_diagnostics = false;
};

struct BootstrapPrediction {
    BaselineStatus status = BaselineStatus::kOk;
    std::string series_key;
    int64_t bucket_id = 0;
    double baseline_mu = 0.0;
    double baseline_lower = 0.0;
    double baseline_upper = 0.0;
    double band_width = 0.0;
    double confidence = 0.0;
    std::vector<std::string> uncertainty_source;

    bool has_model_space = false;
    double model_space_mu = 0.0;
    double model_space_lower = 0.0;
    double model_space_upper = 0.0;

    std::string diagnostics;

    bool ok() const { return status == BaselineStatus::kOk; }
};

}  // namespace flowsql
```

### 7.4 统一任务接口

`IBaselineTask` 保留任务身份、配置和快照查询能力。

`RequestRebuild` 从主接口移除。旧在线恢复入口不再作为 BaselineB 主路径能力。

```cpp
namespace flowsql {

class IBaselineValueTask;
class IBaselineRatioTask;
class IBaselineRelationTask;

interface IBaselineTask {
    virtual ~IBaselineTask() = default;

    virtual const char* Id() const = 0;
    virtual const char* Name() const = 0;
    virtual BaselineTaskKind Kind() const = 0;

    virtual BaselineSerializationResult ExportConfig(
        BaselineSerializationFormat format) const = 0;

    virtual BaselineSerializationResult QueryTaskSnapshot(
        BaselineSerializationFormat format) const = 0;

    virtual BaselineSerializationResult QuerySeriesSnapshot(
        std::string_view series_key,
        BaselineSerializationFormat format) const = 0;

    virtual BaselineStatus Close() = 0;
};

interface IBaselineService {
    virtual ~IBaselineService() = default;

    virtual std::pair<BaselineStatus, std::shared_ptr<IBaselineValueTask>>
    CreateValueTask(std::string_view config_content,
                    BaselineSerializationFormat format) = 0;

    virtual std::pair<BaselineStatus, std::shared_ptr<IBaselineRatioTask>>
    CreateRatioTask(std::string_view config_content,
                    BaselineSerializationFormat format) = 0;

    virtual std::pair<BaselineStatus, std::shared_ptr<IBaselineRelationTask>>
    CreateRelationTask(std::string_view config_content,
                       BaselineSerializationFormat format) = 0;

    virtual BaselineSerializationResult QueryServiceSnapshot(
        BaselineSerializationFormat format) const = 0;
};

}  // namespace flowsql
```

说明：

- 所有序列化输出都通过 `BaselineSerializationFormat` 指定格式，函数名不携带 `Json`。
- 任务创建不使用 `**out` 出参，直接返回 `std::pair<BaselineStatus, std::shared_ptr<Task>>`。
- 任务配置也通过 `ExportConfig(format)` 输出，避免将格式写入函数名。
- `QueryServiceSnapshot(format)` 暴露任务列表和最近一次 service 级错误诊断。

### 7.5 Value 任务接口

```cpp
namespace flowsql {

interface IBaselineValueTask : public IBaselineTask {
    virtual BootstrapTrainResult Bootstrap(const ValueBootstrapInput& input) = 0;

    virtual BootstrapPrediction PredictBootstrap(
        std::string_view series_key,
        int64_t bucket_id,
        const BootstrapPredictionOptions& options) const = 0;

    virtual BaselineSerializationResult ExportBootstrapArtifact(
        BaselineSerializationFormat format) const = 0;

    virtual BaselineStatus LoadBootstrapArtifact(
        std::string_view content,
        BaselineSerializationFormat format) = 0;

    virtual BaselineSerializationResult ExportBootstrapSeed(
        BaselineSerializationFormat format) const = 0;
};

}  // namespace flowsql
```

说明：

- `task_spec` 来自创建任务时的 config，不在每次 `Bootstrap` 调用里重复传入。
- `calendar` 由任务配置中的 `calendar_ref` 解析，不在 `Bootstrap` 输入中嵌入完整 calendar。
- `Bootstrap` 覆盖或刷新 `input.series_key` 对应的 `BootstrapArtifact` 和 `BootstrapSeed`，不能覆盖其他 series。
- `PredictBootstrap` 依赖当前任务中 `series_key` 对应的 artifact 和 `bucket_id`。
- `BootstrapSeed` 是任务内部 series 级状态，不通过 `IBaselineValueTask` 对外暴露；B2 初始化在同一 task 内部按 `series_key` 直接消费。
- `ExportBootstrapArtifact`、`LoadBootstrapArtifact` 和 `ExportBootstrapSeed` 是 task 级全量接口，导出 / 导入当前 task 下全部 series；返回值按值返回，避免手写 `std::move`。
- `B1` 不暴露 rolling 流式提交接口；`SubmitObservation` 留到 `B2` 设计。

### 7.6 Ratio 任务接口

`IBaselineRatioTask` 与 `Value` 同构：

```cpp
namespace flowsql {

interface IBaselineRatioTask : public IBaselineTask {
    virtual BootstrapTrainResult Bootstrap(const RatioBootstrapInput& input) = 0;

    virtual BootstrapPrediction PredictBootstrap(
        std::string_view series_key,
        int64_t bucket_id,
        const BootstrapPredictionOptions& options) const = 0;

    virtual BaselineSerializationResult ExportBootstrapArtifact(
        BaselineSerializationFormat format) const = 0;

    virtual BaselineStatus LoadBootstrapArtifact(
        std::string_view content,
        BaselineSerializationFormat format) = 0;

    virtual BaselineSerializationResult ExportBootstrapSeed(
        BaselineSerializationFormat format) const = 0;
};

}  // namespace flowsql
```

### 7.7 Relation 任务接口

`IBaselineRelationTask` 的 bootstrap 导出初始 basis seed 和历史 relation 摘要训练出的 routed value/ratio seed，不做 relation 分布的未来预测。

```cpp
namespace flowsql {

interface IBaselineRelationTask : public IBaselineTask {
    virtual BootstrapTrainResult Bootstrap(const RelationBootstrapInput& input) = 0;

    virtual BaselineSerializationResult ExportBootstrapArtifact(
        BaselineSerializationFormat format) const = 0;

    virtual BaselineStatus LoadBootstrapArtifact(
        std::string_view content,
        BaselineSerializationFormat format) = 0;

    virtual BaselineSerializationResult ExportBootstrapSeed(
        BaselineSerializationFormat format) const = 0;

    virtual BaselineSerializationResult QueryBootstrapBasis(
        BaselineSerializationFormat format) const = 0;
};

}  // namespace flowsql
```

`RelationBootstrapArtifact` 内部仍包含 `basis_by_metric`、`relation_routed_summary_artifacts`、`coverage_report` 和 `diagnostics`。对外通过 `ExportBootstrapArtifact`、`ExportBootstrapSeed` 或 `QueryBootstrapBasis` 导出；`QueryBootstrapBasis` 是 task 级全量 basis 视图，包含全部已训练 relation `series_key` 的 basis。

`relation_task_spec` 和 `clock_spec` 来自创建任务时的 config。

### 7.8 Calendar 引用

Calendar 不做 task 内嵌完整配置。Baseline 插件维护全局 calendar registry，任务通过 `calendar_ref` 引用某个 calendar 实例。

calendar entries 只描述静态事件本身，保留 `event_code`、`alignment_mode`、`start_ts`、`end_ts` 和可选 `tz`；不携带 `feature`、`key` 或任何 task-scoped 语义。

任务配置示例：

```json
{
  "calendar_ref": {
    "calendar_id": "cn-holiday",
    "calendar_version": "2026.1"
  }
}
```

训练和预测都通过任务的 `calendar_ref` 解析 calendar。

若 artifact 记录的 `calendar_id/calendar_version` 在预测时不可用或不匹配：

```text
event component disabled
confidence lowered
uncertainty_source += calendar_unavailable
```

`PredictBootstrap(series_key, bucket_id)` 不需要每次传入 calendar 上下文；任务是否启用事件修正由 task 自己控制。

### 7.9 Task config 最小 schema

`Create*Task(config_content, format)` 的配置必须包含任务身份、特征身份、时钟和 calendar 引用。

最小字段：

```json
{
  "schema_version": 1,
  "task_id": "baseline_task_bps",
  "task_name": "link bps baseline",
  "task_kind": "value|ratio|relation",
  "feature_id": "bps",
  "feature_type": "value_basic|value_sampled|ratio|relation",
  "profile": "default",
  "clock_spec": {
    "bucket_seconds": 60,
    "timezone": "Asia/Shanghai"
  },
  "calendar_ref": {
    "calendar_id": "cn-holiday",
    "calendar_version": "2026.1"
  }
}
```

`series_key` 属于 bootstrap 输入，不作为 task config 必填项。若部署选择“一任务一序列”，可以在 config 中额外加入 `series_key` 作为业务约束；但 B1 的 artifact / seed 仍必须把 `series_key` 写入 `series_identity`，严格兼容性校验必须同时检查 task 身份与 series 身份。

与旧 config 的迁移关系：

- 旧 `key` 不再作为 task config 必填项；历史序列身份进入 `BootstrapInput.series_key`。
- 旧 `feature_profile` 迁移为 `profile`。
- 旧 `delta/tz` 迁移为 `clock_spec.bucket_seconds/timezone`。
- 旧 task-local `event_calendar_spec` 不再进入 task config；全局 `calendars` 初始化 registry，task 只保存 `calendar_ref`。
- 旧 `baseline_source_configs` 不进入 B1 主路径。
- `feature_type` 必须表达观测语义类型，不折叠为宽泛的 `value`。
- `value_basic` 表示普通数值观测，训练时不使用 `sample_count` 做准入过滤或权重修正。
- `value_sampled` 表示采样数值观测，训练时必须使用 `sample_count`、`n_train_min` 和 `kappa_sample` 做准入过滤与权重修正。
- `profile` 只表达对应 `feature_type` 下的参数档位，不能承担 basic / sampled 类型判定。
- `value_basic` 首版只允许 `profile = "default"`。
- `value_sampled` 首版只允许 `profile` 来自 `value_sampled_profiles`，例如 `cont_core` / `cont_tail`。
- `ratio` 首版要求 `profile` 来自 `ratio_profiles`。
- `relation` 首版使用 `profile = "default"`。
- `Relation` routed value 摘要没有 sampled 观测语义，必须生成 `feature_type = "value_basic"`。

### 7.10 任务生命周期

- `task_id` 必须唯一；重复创建同名未关闭任务返回 `kInvalidArgument`。
- `Create*Task` 成功后，service 将任务登记到 registry，并返回 `shared_ptr`。
- `Close()` 幂等；第一次调用从 registry 移除任务，后续调用仍返回 `kOk`。
- 已关闭任务不再允许 `Bootstrap`、`PredictBootstrap`、`LoadBootstrapArtifact` 或 `ExportBootstrap*`，统一返回 `kInvalidArgument`。
- 插件销毁时关闭 registry 中仍存活的任务；外部持有的 `shared_ptr` 只保留 closed task，不再访问插件内部已释放资源。

### 7.11 错误诊断规则

接口返回 `BaselineStatus` 只表达粗粒度状态。

诊断细节放在以下位置：

- `BootstrapTrainResult.diagnostics`：训练输入、覆盖率、组件拟合失败等信息。
- `BootstrapPrediction.diagnostics`：预测失败、calendar 不可用、band 计算降级等信息。
- `QueryTaskSnapshot(format)`：任务最近一次错误、artifact 状态、seed 状态。
- `QueryServiceSnapshot(format)`：最近一次 task 创建失败、配置解析失败、registry 状态。
- `BootstrapArtifact` 序列化内容：可审计的训练 / 预测诊断，不包含旧 `shadow/candidate/rebuild` 生命周期语义。

---

## 8. 配置改造

### 8.1 保留配置

保留：

```text
parser.tz_default
shared_profile_config
value_sampled_profiles
ratio_profiles
solver_constants
event calendar 编译 / 匹配能力
```

其中：

- `shared_profile_config.daily_harmonic_order/weekly_harmonic_order/dme_max` 继续控制历史拟合结构；`k_day/k_week` 只作为内部兼容别名。
- `value_sampled_profiles` 和 `ratio_profiles` 继续控制训练样本准入和变换语义。
- `solver_constants` 继续控制 deterministic solver。
- 旧 event calendar 的编译 / 匹配逻辑继续复用，但 task-local `event_calendar_spec` 配置字段不保留；全局 `calendars` 用于初始化 calendar registry，calendar entries 只保留静态事件字段，task 侧只引用 `calendar_ref`。

### 8.2 新增配置

建议新增：

```yaml
calendars:
  - calendar_id: "cn-holiday"
    calendar_version: "2026.1"
    entries: []

bootstrap:
  min_train_points: 2
  seed_quality:
    full_min_coverage_ratio: 0.90
    partial_min_coverage_ratio: 0.50
    daily_min_span_days: 1
    weekly_min_span_days: 14
    daily_phase_coverage_ratio: 0.75
    weekly_phase_coverage_ratio: 0.70
  prediction_band:
    z_value: 3.0
    sigma_floor: 1.0e-3
    low_maturity_band_scale: 1.5
  export_seed:
    include_monthpos_when_ready: true
    include_event_when_ready: true
    include_t3_basis_when_ready: true

shared_profile_config:
  daily_harmonic_order: 6
  weekly_harmonic_order: 3
```

说明：

- `calendars` 是全局 calendar registry。
- `min_train_points` 替代旧 `candidate_builder.min_train_point_count`。
- `seed_quality` 控制 `BootstrapSeed.seed_status` 的自动评价阈值；调用方只声明 profile 目标，不能直接覆盖 `full/partial/weak` 标签。
- `prediction_band` 控制 B1 预测条带，不复用 shadow 放宽系数。
- `export_seed` 控制 seed 中哪些组件允许导出。
- `daily_harmonic_order` 和 `weekly_harmonic_order` 控制日 / 周周期 harmonic 阶数；默认日周期 `6` 阶、周周期 `3` 阶。

### 8.3 删除配置

删除：

```text
runtime_and_rebuild_constants.candidate_builder
runtime_and_rebuild_constants.shadow_policy
runtime_and_rebuild_constants.candidate_validator
runtime_and_rebuild_constants.relation_rebuild
scoring_and_confidence_constants.*shadow*
```

`scoring_and_confidence_constants.confidence_formal_base/source_base` 在 B1 中应重新命名，因为 B1 不再存在 formal/source 在线服务语义。

---

## 9. 文件级迁移草案

### 9.1 迁移到 bootstrap

建议迁移或重命名：

```text
rebuild/formal_model_trainer.* -> bootstrap/bootstrap_trainer.*
model/formal_predictor.*       -> bootstrap/bootstrap_predictor.*
model/formal_model.h           -> bootstrap/bootstrap_model.h 或保留模型块后改名
rebuild/replay_runner.*        -> bootstrap/bootstrap_series.*
```

### 9.2 保留原位置

暂时保留：

```text
solver/solver_backend.*
model/calendar_feature_helper.*
model/event_calendar_matcher.*
model/readiness_helper.*
model/profile_config.h
relation/relation_basis.*
```

### 9.3 删除

删除：

```text
rebuild/candidate_builder.*
rebuild/candidate_validator.*
rebuild/rebuild_queue.*
rebuild/rebuild_worker.*
rebuild/rebuild_request.h
model/shadow_state.h
model/formal_model_state.h 中的 rebuild 状态
relation/relation_summary_extractor.*
relation/relation_router.*
task/* 中的 ExecuteRebuild / RequestRebuild / SetHistoryReader
```

`ReplayWindowSummary` 这类纯窗口摘要如果仍被 bootstrap 训练复用，应移动到 `bootstrap/` 或新的公共历史摘要头文件；不要为了保留它而继续保留 `formal_model_state.h` 的 rebuild 状态机。

---

## 10. 验收标准

`B1` 完成时必须满足：

- [ ] 可用完整历史训练出 `ValueBootstrapArtifact` 和 `RatioBootstrapArtifact`。
- [ ] 可对指定 `series_key` 的未来 `bucket_id` 调用预测接口，返回 `baseline_mu/lower/upper/band_width/confidence`。
- [ ] `Value` 预测输出在原始观测空间，不能只暴露 `log1p` 模型空间值。
- [ ] `Ratio` 预测输出在 `[0, 1]` 概率空间。
- [ ] 可从历史 `RelationBootstrapBlock` 导出 `T3` 初始 basis seed 和 routed value/ratio summary seed。
- [ ] 统一任务可按 `series_key` 完成 `Bootstrap -> PredictBootstrap`，并通过 task 级全量 `ExportBootstrapArtifact -> ExportBootstrapSeed` 导出全部 series。
- [ ] 同一 task 下两个不同 `series_key` 连续训练时，artifact / seed / prediction 互不覆盖，导出的 JSON 每个 series 条目均包含正确 `series_identity`。
- [ ] `BootstrapSeed.seed_status` 由 `BootstrapEngine` 自动评价；覆盖 `full/partial/weak/none`、daily-only、day-week、低覆盖、缺 `sigma_init` 和 routed summary seed 场景，调用方不能直接覆盖成熟度标签。
- [ ] `BootstrapArtifact` 序列化内容可重新加载，并保持预测结果一致。
- [ ] artifact / seed 序列化内容包含最小 schema 字段，并通过 task 身份、series 身份、clock、calendar 和版本的严格兼容性校验。
- [ ] 任务通过全局 `calendar_ref` 使用 calendar；预测接口不要求调用方每次传 calendar 上下文。
- [ ] task config 最小 schema 可创建任务；重复 `task_id`、非法配置、关闭后调用均有明确状态和诊断。
- [ ] 历史输入规范化规则覆盖乱序、重复 bucket、gap、非法值和 ratio denominator 为 `0`。
- [ ] 测试证明 bootstrap 训练 / 预测不触发 `shadow/candidate/rebuild` 状态。
- [ ] 编译或静态检查证明 B1 主路径不再引用 `RebuildQueue`、`RebuildWorker`、`SetHistoryReader`、`RequestRebuild`、`ShadowState`、`CandidateValidator`。
- [ ] `IBaselineTask` 主接口不再暴露在线 rebuild 入口。
- [ ] `B1` 接口不暴露 `SubmitObservation` / `SubmitBlock` rolling 流式入口。
- [ ] 配置文件不再包含 shadow / candidate / rebuild 主路径参数。

---

## 11. 已确认决策

### 11.1 `BaselineStatus` 错误码

`BaselineStatus` 不继续细分到日志或诊断粒度。

当前错误码集合只表达调用方需要分支处理的粗粒度状态：

```text
kOk
kNotTrained
kInsufficientData
kInvalidArgument
kCalendarUnavailable
kIncompatibleArtifact
kUnsupportedFormat
kParseFailed
kTrainFailed
kPredictFailed
kSerializationFailed
```

更细原因写入 `diagnostics` 或 artifact 序列化内容，不把 enum 变成日志系统。

### 11.2 `BootstrapSeed` 显式初始化字段

`theta_init` 首版使用结构化字段，不直接携带旧 bootstrap model block。

`BootstrapSeed` 是给 `B2 Online Rolling Core` 的初始化参数，不是旧模型结构的搬运容器。

首版只把 rolling core 能直接消费的信息显式化：

```text
level
trend
daily_harmonic
weekly_harmonic
monthpos_hint
event_hint
sigma_init
uncertainty_init
maturity_init
clock_spec
series_identity
seeded_components
enabled_components
ratio_prior_init
```

其中 `monthpos_hint` 和 `event_hint` 只作为可选提示，后续 rolling core 不必须完全照搬。

`ratio_prior_init` 只服务 ratio seed；`series_identity`、`clock_spec`、`calendar_ref`、`seeded_components` 和 `enabled_components` 是 `B2` 做兼容性校验与初始化启用判断的显式依据，不允许从旧 `model` 字段反推。

Ratio 的 `theta_init` 使用 logit 模型空间时，`sigma_init.model_space` 也必须是 `logit`，并来自 logit residual 的 `sigma_ref`。如需 probability 空间 band 提示，必须另设 observation-space 字段，不能混入 `sigma_init`。

### 11.3 诊断字段边界

`BootstrapArtifact` 序列化内容只保留 bootstrap 训练 / 预测可解释诊断。

允许保留：

- 训练时间范围、样本数、覆盖率。
- 启用组件：`core/day/week/monthpos/event`。
- 各组件训练是否成功。
- 残差尺度、`sigma`、confidence。
- calendar 是否可用、版本是否匹配。
- 输入数据问题：gap、异常值过滤、样本不足。

禁止带回旧在线恢复生命周期语义：

- `shadow_active`
- `candidate_loss`
- `incumbent_loss`
- `candidate_pass`
- `rebuild_phase`
- `switch_state`
- `rebuild_queue`
- `shadow_stuck`

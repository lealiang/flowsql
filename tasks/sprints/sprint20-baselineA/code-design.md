# Sprint 20 BaselineA 代码实现设计

## 1. 文档定位

本文是 Sprint 20 BaselineA 的编码执行文档，唯一目的就是指导后续代码实施。

它不是新的算法设计文档。算法语义、输入输出语义、训练目标、阈值语义、重建与切换规则，全部以 [Sprint 19 基线统一设计](../sprint19-baseline/design.md) 为唯一准绳。

本文只回答 4 个问题：

- `design.md` 的每一条正式设计，由哪一个代码模块承载
- 这些模块之间如何协作，才能形成可编码、可测试、可验证的闭环
- 现有 baseline 代码哪些保留，哪些必须重写
- 参数、状态、快照、验证责任分别归属于哪里

本文直接依赖以下文档：

- [Sprint 19 基线统一设计](../sprint19-baseline/design.md)
- [Sprint 19 Baseline 回顾总结](../sprint19-baseline/retrospective.md)
- [Sprint 19 Baseline 检视记录](../sprint19-baseline/review.md)

若本文与 `design.md` 冲突，以 `design.md` 为准；若实现过程中确认 `design.md` 本身需要调整，必须先更新 `design.md`，再回填本文，禁止在代码层私自改语义。

记号约定：

- 本文中字段后缀 `?` 只表示“显式可选字段”，不表示“待定”或“后续再决定”
- 任何带 `?` 的字段，都必须在首次出现处同时定义 absent 语义
- 若某字段在实现中不允许缺失，则本文不得使用 `?`

## 2. 本轮必须遵守的实现原则

### 2.1 不允许再出现“形式上有模块，实际上无算法”的实现

本轮明确禁止：

- 用 `intercept-only` 或常数模型占位正式模型
- 只实现 task 外壳，不实现 `Core / monthpos / event`
- 只实现 `T3` 摘要提取，不实现模式融合与 `FusionResult`
- 只实现重建请求，不实现 `candidate -> validate -> switch`
- 只把设计条款记在注释里，不落实为可执行状态或代码对象

### 2.2 输入、输出、状态都必须直接服务算法

遵循 [design.md](../sprint19-baseline/design.md) 第 `5`、`6`、`7`、`8`、`9`、`10` 章：

- 运行时输入中的字段，只有在它会影响训练、评分、漂移、重建或切换时，才能进入正式代码接口
- 设计里的正式输出字段，必须有等价代码承载；不允许在代码设计阶段擅自裁剪
- 状态必须分层：时序公共状态、模型状态、漂移状态、影子基线状态、重建状态分别归位，不混在一个“万能结构体”里

### 2.3 可以做编码层优化，但不能改变设计语义

允许的优化只有两类：

- 把 task 绑定的静态字段从热路径观测中移到 `TaskSpec`
- 用 `BaselineStringRef`、枚举、固定数组、预分配容器降低热路径分配成本

但前提是：

- 语义必须与 `design.md` 等价
- 文档中必须写清楚“哪一层省略了什么字段，以及为什么语义不变”

### 2.4 代码注释是正式交付要求

所有基线算法相关代码都必须写清晰中文注释，尤其是：

- 时间相位和 `DST` 处理
- `monthpos` 特征构造
- `T1b` 的 gate 与 `rho_t`
- `T2` 的 `m0 / alpha0 / beta0 / phi_over / v_floor`
- `T3` 的 `ServiceBasis / EvalBasis / StableHeadSet`
- 模式融合中的 `core_P / support_P / oppose_P`
- `shadow baseline` 激活、退出、切换验证

注释必须解释“为什么这样做”和“对应哪一层算法语义”，不能只复述代码字面意思。

## 3. 总体实现架构

### 3.1 总体分层

Baseline 插件按以下层次实现：

```text
外部接口层
  IBaselineService / IBaselineTask / Query*Snapshot

task 编排层
  ValueTask / RatioTask / RelationTask

检测核心层
  ValueDetectorCore / RatioDetectorCore

关系分布层
  RelationBasis / RelationSummaryExtractor / RelationRouter / RelationPatternFusion

模型与状态层
  TaskSpec / ProfileConfig / FormalModel / SeriesState / DriftState / ShadowState / FormalModelState

重建慢路径
  RebuildWorker / ReplayRunner / FormalModelTrainer / CandidateBuilder / CandidateValidator

数学后端
  SolverBackend
```

### 3.2 三类 task 与代码边界

`T1 / T2`：

- 一个 task 对应一个 `feature` 静态算法规格；真正的建模对象仍然是 `Series = (key, feature)`
- task 内部按运行时 `key` 持有该序列的正式模型、影子状态、漂移状态、来源决策与重建状态
- `BaselineSourceConfig` 的生效粒度固定为单个 `(key, feature)`，不能作为 task 下所有 `key` 的全局默认来源
- 热路径入口分别是 `IBaselineValueTask::SubmitObservation` 与 `IBaselineRatioTask::SubmitObservation`

`T3`：

- 一个 task 对应一个关系分布规格 `T3TaskSpec`
- 运行时输入按 `key` 分层，因此 `RelationTask` 内部按 `key` 持有一组 `RelationKeyRuntime`
- task 热路径入口是 `IBaselineRelationTask::SubmitBlock`

`BaselineTaskBase` 的正式定位：

- 它是三类 task 共用的薄基类，只负责 `Id / Name / Kind / ConfigJson / Close / RequestRebuild` 这类通用壳层能力
- 它可以承载极少量所有 task 都共用的基础设施引用，例如注册表句柄、重建队列句柄、通用关闭保护和少量字符串工具
- 它不负责持有 detector core、trainer、candidate validator、relation summary extractor、relation router 或任何 task-specific runtime
- 它不负责实现 `ValueTask / RatioTask / RelationTask` 的热路径、重建 apply 逻辑、relation basis 刷新、routed detector 容器或 task-specific snapshot 组装
- 因此，`baseline_task_base.h` 可以保留为共性接口与少量 helper 的承载；但 `baseline_task_base.cpp` 不得继续演化为混装 `T1 / T2 / T3` 具体算法与 relation 特化逻辑的大实现文件

### 3.3 `T1 / T2` 内部 core 的复用方式

为满足“direct task 与 relation routed task 复用同一检测核心”的要求，本轮明确采用：

- `ValueDetectorCore`：`T1a`、`T1b`、以及 routed 到 `T1a` 的 `T3` 摘要特征统一复用
- `RatioDetectorCore`：`T2`、以及 routed 到 `T2` 的 `T3` 摘要特征统一复用
- `ValueTask / RatioTask` 只做 task 外壳、生命周期、重建编排和快照输出
- `RelationTask` 不重复实现评分逻辑，而是内嵌多组 `ValueDetectorCore / RatioDetectorCore` 实例

这意味着：

- relation task 不能调用 value task / ratio task 的外部接口做二次转发
- relation task 必须直接复用内部 detector core，以避免重复 task 壳层、重复状态和重复快照逻辑

### 3.4 建议目录结构

本轮建议目录收口为：

```text
src/plugins/baseline/
  baseline_plugin.*
  config_parser.*
  common/
    result_builder.h
  detector/
    detector_common.h
    value_detector_core.*
    ratio_detector_core.*
  fusion/
    key_risk_fusion.*
    relation_pattern_fusion.*
  model/
    task_spec.h
    event_calendar_spec.h
    series_override.h
    profile_config.h
    calendar_feature_helper.*
    event_calendar_matcher.*
    readiness_helper.*
    formal_model.h
    formal_predictor.*
    formal_model_state.h
    series_state.h
    series_store.*
    drift_state.h
    shadow_state.h
  rebuild/
    rebuild_request.h
    rebuild_queue.*
    rebuild_worker.*
    replay_runner.*
    formal_model_trainer.*
    candidate_builder.*
    candidate_validator.*
  relation/
    relation_basis.*
    relation_summary_extractor.*
    relation_router.*
  solver/
    solver_backend.*
  task/
    baseline_task_base.*
    task_registry.*
    value_task.*
    ratio_task.*
    relation_task.*
```

其中新增的正式模块是：

- `model/profile_config.h`
- `model/calendar_feature_helper.*`
- `model/event_calendar_matcher.*`
- `model/readiness_helper.*`
- `fusion/key_risk_fusion.*`
- `fusion/relation_pattern_fusion.*`
- `task/relation_task.*`

## 4. `design.md` 到代码模块的总映射

| `design.md` 章节 | 设计主题 | 主代码承载 |
|---|---|---|
| `5.2` | 统一输出协议 | `src/framework/interfaces/ibaseline_types.h`、`src/plugins/baseline/common/result_builder.h` |
| `5.3` | 任务规格、事件日历、历史读取 | `src/framework/interfaces/ibaseline_service.h`、`src/plugins/baseline/model/task_spec.h`、`src/plugins/baseline/model/series_override.h`、`src/plugins/baseline/model/event_calendar_spec.h`、`src/plugins/baseline/model/event_calendar_matcher.*`、`src/plugins/baseline/config_parser.*` |
| `5.4` | 在线更新 / 增量适配 / 正式重建分层 | `src/plugins/baseline/model/series_state.h`、`src/plugins/baseline/model/formal_model_state.h`、`src/plugins/baseline/model/drift_state.h`、`src/plugins/baseline/model/shadow_state.h`、`src/plugins/baseline/model/readiness_helper.*` |
| `6.3 ~ 6.11` | `T1a / T1b` 输入、变换、模型、训练、评分、漂移、输出 | `src/plugins/baseline/model/profile_config.h`、`src/plugins/baseline/model/calendar_feature_helper.*`、`src/plugins/baseline/model/event_calendar_matcher.*`、`src/plugins/baseline/model/readiness_helper.*`、`src/plugins/baseline/model/formal_model.h`、`src/plugins/baseline/model/formal_predictor.*`、`src/plugins/baseline/rebuild/formal_model_trainer.*`、`src/plugins/baseline/detector/value_detector_core.*` |
| `7.2 ~ 7.9` | `T2` 输入、平滑、`logit`、方差层、评分、来源 | `src/plugins/baseline/model/profile_config.h`、`src/plugins/baseline/model/readiness_helper.*`、`src/plugins/baseline/model/formal_model.h`、`src/plugins/baseline/rebuild/formal_model_trainer.*`、`src/plugins/baseline/detector/ratio_detector_core.*` |
| `8.2 ~ 8.11` | `T3TaskSpec`、`T3Block`、basis、摘要提取、路由 | `src/plugins/baseline/model/task_spec.h`、`src/plugins/baseline/relation/relation_basis.*`、`src/plugins/baseline/relation/relation_summary_extractor.*`、`src/plugins/baseline/relation/relation_router.*`、`src/plugins/baseline/task/relation_task.*` |
| `9.1 ~ 9.2` | 单特征标准化、`T3` 模式融合、`Risk_T1T2 / Risk_T3 / Risk(Key,t)` | `src/plugins/baseline/fusion/relation_pattern_fusion.*`、`src/plugins/baseline/fusion/key_risk_fusion.*` |
| `10.2` | 正式重建、`shadow baseline`、候选验证、切换 | `src/plugins/baseline/rebuild/rebuild_request.h`、`src/plugins/baseline/rebuild/rebuild_worker.*`、`src/plugins/baseline/rebuild/replay_runner.*`、`src/plugins/baseline/rebuild/candidate_builder.*`、`src/plugins/baseline/rebuild/candidate_validator.*` |
| `11` | 工程约束 | 全部模块，重点是 `relation/`、`detector/`、`rebuild/` |
| `12` | 参数目录与归属 | `src/plugins/baseline/model/profile_config.h`、`src/plugins/baseline/solver/solver_backend.*`、`src/plugins/baseline/fusion/*.h`、`src/plugins/baseline/common/result_builder.h` |

## 5. 对应 `design.md` 第 5 章：统一接口与任务规格

### 5.1 边界原则的代码落点

对应 [design.md](../sprint19-baseline/design.md) 第 `5.1` 节：

- 检测器层只输出单特征数学异常结果，不做业务判别
- 融合层单独实现，不嵌入 `ValueDetectorCore / RatioDetectorCore`
- 输出协议在 `ibaseline_types.h` 中统一定义

代码上：

- `detector/` 只负责单特征检测
- `fusion/` 只负责多特征、多模式风险合成
- `task/` 只负责编排，不定义新的数学语义

### 5.2 统一输出协议

`src/framework/interfaces/ibaseline_types.h` 必须成为 `DetectorResult` 与 `FusionResult` 的唯一外部协议文件。

必须承载以下正式结构：

#### 5.2.1 `DetectorResult`

```text
DetectorResult = {
  status,
  key,
  feature,
  feature_type,
  ts,
  raw_score,
  normalized_score,
  confidence,
  persistence,
  severity,
  direction,
  provider,
  reason_code,
  flags,
  evidence
}
```

代码要求：

- `key / feature / feature_type` 使用 `BaselineStringRef`，避免热路径字符串拷贝
- `ts` 使用 `int64_t`，语义上等于当前 `bucket_id` 反解后的窗口时间；实现上允许直接回传 `bucket_id` 并在快照层补充时间解释，但字段必须存在
- `evidence` 不能被删除；必须在 C++ 结构层保留正式承载
- `evidence` 必须是固定大小 tagged union / POD 风格结构，不允许在热路径内持有 `std::string`、动态 JSON 或堆分配容器
- `evidence` 中涉及身份类信息时，优先使用枚举、整数 ID、`BaselineStringRef` 或固定小数组，而不是拥有型字符串对象

`evidence` 采用 tagged union 方案收口，至少包含：

- `ValueEvidence`
  - `y_t`
  - `x_t`
  - `baseline_mu_t`
  - `resid_r_t`
  - `z_t`
  - `p_shift_t`
  - `dir_t`
  - `score_point`
  - `score_shift`
  - `sample_count?`
  - `sigma_eff_t?`
  - `baseline_source_kind`
  - `baseline_source_key?`
  - `model_state`
  - `shadow_active`
- `RatioEvidence`
  - `numerator`
  - `denominator`
  - `p_smooth`
  - `x_t`
  - `p_hat_t`
  - `var_eff_t`
  - `r_t`
  - `rho_t`
  - `p_shift_t`
  - `dir_t`
  - `score_point`
  - `score_shift`
  - `baseline_source_kind`
  - `baseline_source_key?`
  - `model_state`
  - `shadow_active`

说明：

- `T1a` 使用 `ValueEvidence`，其中 `sample_count / sigma_eff_t` 标记为 absent
- `T1b` 使用完整 `ValueEvidence`
- `T2` 使用 `RatioEvidence`
- routed `T3` 摘要特征输出的 `DetectorResult` 仍按其路由类型使用上述证据结构
- `baseline_source_key` 仅在 `baseline_source_kind = configured_source` 时填写；其余情况下固定为 absent

#### 5.2.2 `FusionResult`

`FusionResult` 也是正式输出协议的一部分，不允许只存在于 JSON 里。

```text
FusionResult = {
  key,
  ts,
  risk,
  dominant_single[<=3],
  dominant_pattern[<=2]
}
```

代码要求：

- `FusionResult` 结构定义在 `ibaseline_types.h`
- `dominant_single` 与 `dominant_pattern` 采用固定上限数组 + `count` 字段，避免热路径分配
- `dominant_single` 结构必须是源 `DetectorResult` 的子集
- `dominant_pattern` 结构由 `relation_pattern_fusion.*` 构造

必须同时定义以下正式投影结构，避免接口层与融合层各自补字段：

```text
DominantSingleProjection {
  feature,
  dir,
  reason_code,
  a_f,
  normalized_score,
  confidence,
  persistence
}

DominantPatternProjection {
  pattern,
  feature_base,
  score_pattern,
  metrics_hit[<=3],
  supporting_features[<=4]
}
```

约束：

- `metrics_hit` 对应 `M_valid(P, t)` 中真实参与跨指标合成的指标集合
- `supporting_features` 只保留少量高价值摘要特征名，不回传底层完整 `evidence`
- 以上两个结构都必须使用固定上限数组 + `count` 字段，不允许在热路径持有动态容器

#### 5.2.3 输出构造责任

- `common/result_builder.h` 负责把数值分数、方向、阈值、置信度、flags 映射到 `DetectorResult`
- `fusion/relation_pattern_fusion.*` 负责生成 `FusionResult`
- `fusion/key_risk_fusion.*` 负责生成跨任务的最终 `Risk(Key,t)` 快照

### 5.3 任务规格、事件日历与历史读取接口

#### 5.3.1 `IBaselineService / IBaselineTask`

`src/framework/interfaces/ibaseline_service.h` 承载公共接口。

必须满足：

- `CreateValueTask` 对应 `T1a / T1b`
- `CreateRatioTask` 对应 `T2`
- `CreateRelationTask` 对应 `T3`
- `IBaselineTask` 作为外部 `IHandle`

在当前接口基础上，需要补齐：

- `HistoryFetchRequest` 必须包含 `feature` 或等价任务引用，不能只含 `key`
- `IBaselineService` 必须新增按 `key` 查询融合结果的接口，例如：
  - `QueryKeyFusionSnapshotJson(const BaselineStringRef& key, std::string* out_json)`
- `IBaselineTask::QuerySeriesSnapshotJson` 对 relation task 必须能够返回 routed 单特征结果、最近一次 `FusionResult`，以及当前 `ServiceBasis / EvalBasis` 的摘要信息
- `ibaseline_service.h` 必须正式承载 `HistoryReader` 与 `BaselineSourceResolver` 的接口定义或等价函数型接口

接口绑定方式必须正式收口为：

- 运行期 setter 注入：只有 `HistoryReader`
- 创建期静态绑定：`BaselineTaskSpec.baseline_source_configs?`、`BaselineTaskSpec.event_calendar_spec?`
- relation task 的创建期静态绑定：`RelationTaskSpec`、`RelationTaskClockSpec`、task-bound `EventCalendarSpec?`、task-bound `BaselineSourceResolver?`
- 创建完成后，不允许再通过 setter 或隐式全局状态修改 `EventCalendarSpec`、`BaselineSourceResolver`、`RelationTaskClockSpec`
- `BaselineSourceConfig` 允许在创建期随 task spec 批量提供，但必须以 `(key, feature)` 为索引保存；创建后同样不允许漂移

方法级契约必须进一步明确为：

- `IBaselineValueTask::SubmitObservation(const ValueObservation&, DetectorResult* out)`
- `IBaselineRatioTask::SubmitObservation(const RatioObservation&, DetectorResult* out)`
- `IBaselineRelationTask::SubmitBlock(const RelationObservationBlock&, FusionResult* out)`

其中 `IBaselineRelationTask::SubmitBlock(...)` 的同步返回语义收口为：

- 当前 bucket 的摘要提取、routed detector 提交、pattern fusion、key fusion 更新，都必须在本次调用内完成
- `out` 返回的是“当前这次 `RelationObservationBlock` 对应的最终 `FusionResult`”，而不是某个 routed single 的 `DetectorResult`
- routed single 结果、pattern 贡献明细、`ServiceBasis / EvalBasis` 摘要，不通过 `SubmitBlock(...)` 内联回传，统一通过 relation task 的 `QuerySeriesSnapshotJson(...)` 与 service 级 `QueryKeyFusionSnapshotJson(...)` 查询
- 不允许把 `FusionResult` 的同步返回退化成“只更新内部状态，结果等待后续异步快照查询”

创建契约必须进一步明确为：

- `CreateValueTask / CreateRatioTask`：消费 `BaselineTaskSpec`，其中 `baseline_source_configs?` 与 `event_calendar_spec?` 都是创建期静态能力
- `CreateRelationTask`：消费 `RelationTaskSpec + RelationTaskClockSpec + task-bound EventCalendarSpec? + task-bound BaselineSourceResolver?`
- `EventCalendarSpec?` absent 表示该 task 的事件层整体禁用，不允许在 detector 内自行补默认事件表
- `HistoryReader` 由 `SetHistoryReader()` 单独注入；未注入时，该 task 允许在线运行，但正式重建 / candidate build / replay validate 全部禁用
- `BaselineSourceResolver?` absent 表示该 relation task 生成的全部 routed 特征都不配置外部来源，因此每个 routed detector 只能在 `self | none` 之间选源

其中 `BaselineSourceResolver` 的正式语义是：

```text
ResolveBaselineSource(key, feature) -> BaselineSourceConfig?
```

约束：

- 返回值类型复用 `series_override.h` 中的 `BaselineSourceConfig`
- `ResolveBaselineSource(...)` 只运行在 routed 特征规格构造 / basis 刷新 / 冷启动决策所需的轻量路径，不进入 detector 数学内核
- 当返回 absent 时，等价于该逻辑特征未配置外部来源

#### 5.3.2 `BaselineTaskSpec`

`src/plugins/baseline/model/task_spec.h` 中的 `BaselineTaskSpec` 必须严格对齐 `design.md`：

```text
BaselineTaskSpec = {
  name,
  key,
  feature,
  feature_type,
  feature_profile,
  delta,
  tz,
  baseline_source_configs?,
  event_calendar_spec?,
  config_json
}
```

说明：

- `history_reader` 不放进 `TaskSpec`，而是通过 `SetHistoryReader()` 注入
- `baseline_source_configs?` 使用 `series_override.h` 中的来源配置类型，但必须按 `key` 索引；其逻辑语义是 `BaselineSourceConfig(key, feature)`
- `baseline_source_configs?` absent 的正式语义是“该 task 下全部序列均未配置外部候选来源”，因此每个序列的来源选择只能落在 `self | none`
- `baseline_source_configs?` present 时，只对显式列出的 `key` 生效；未列出的 `key` 仍等价于 absent
- `BaselineTaskSpec` 的 C++ 结构必须使用 `std::vector<SeriesBaselineSourceConfig> baseline_source_configs` 或等价容器；不得使用 `std::optional<BaselineSourceConfig> baseline_source_config` 作为 task 字段
- `SeriesBaselineSourceConfig` 至少包含 `key` 与 `BaselineSourceConfig baseline_sources` 两部分，`key + task.feature` 才是完整索引
- 旧的 `SeriesOverride` 名称可以重命名为 `BaselineSourceEntry` 或 `SeriesBaselineSourceConfig`，但不能删除按 `key` 索引这一层
- 禁止把单一 `BaselineSourceConfig` 直接挂到 task 上并让所有 runtime `key` 共享
- `config_parser.*` 必须只接受顶层 `baseline_source_configs`；若出现顶层单数 `baseline_source_config` 或旧字段 `series_overrides`，必须在创建 task 阶段拒绝
- 校验必须覆盖：重复 `key`、空 `baseline_sources`、重复 `source_key`、`source_key == key`
- `event_calendar_spec?` absent 的正式语义是“该序列事件层禁用”，训练与预测都必须按 `h_event(t) = 0` 执行
- `EventCalendarSpec` 与 `baseline_source_configs?` 都属于创建期静态能力，不提供 post-create setter

命名边界：

- 顶层 `baseline_source_configs` 是外部 `T1 / T2` task JSON 字段。
- 内部单序列变量可以命名为 `baseline_source_config`，但它必须来自 `(key, feature)` 查找结果，不能从 task 级全局复制而来。
- `RelationRoutedFeatureSpec.baseline_source_config?` 是 relation task 对某个 routed 逻辑特征 `(key, feature)` 的物化结果，因此允许使用单数。

#### 5.3.3 `RelationTaskSpec`

`RelationTaskSpec` 必须严格对齐 `design.md` 第 `8.2` 节：

```text
RelationTaskSpec = {
  task_id,
  name,
  feature_base,
  group_space_id,
  group_space_version?,
  metric_set_id,
  metrics,
  encode_type,
  support_policy,
  summary_policy,
  config_json
}
```

与之配套的时间规格 `RelationTaskClockSpec = { delta, tz }` 必须定义在同一任务规格层（`task_spec.h` 或 `ibaseline_service.h` 的等价接口层），禁止散落到 `relation_task.*` 私有实现中。

字段责任：

- `task_id` 是 relation task 的静态身份，必须进入快照与重建日志
- `metric_set_id` 不能省略，因为它决定 `RelationObservationBlock.metrics[*]` 的顺序语义
- `encode_type` 必须参与 parser 校验与 relation task 运行时分支
- `group_space_version?` absent 的正式语义是“该 group 空间未单独版本化”；此时 lineage 兼容性只按 `group_space_id` 判断，同一 absence 值视为同一版本上下文

补充约束：

- `RelationTaskSpec` 保持与 `design.md` 的 `T3TaskSpec` 一致，不额外把 `delta / tz` 塞进该结构
- 但 `CreateRelationTask(...)` 或等价 task 注册入口，必须为 relation task 额外绑定一份 task-bound 的时间规格 `RelationTaskClockSpec = { delta, tz }`
- 这份 `RelationTaskClockSpec` 只作为 routed `T1 / T2` 检测器的静态时间语义来源，不在每个 `T3Block` 上重复传输
- 若没有这份 task-bound `delta / tz`，则 relation task 无法合法驱动日 / 周周期、`monthpos`、`shadow delta` 和事件对齐；实现中不允许退化为“默认全局粒度”
- relation task 的事件能力与 routed 来源能力都属于创建期静态绑定，而不是 post-create setter；这与 `design.md` 第 `5.3` 节保持一致
- `history_reader` 是 relation task 唯一允许的 post-create setter 能力
- relation task 的 routed 来源配置也必须通过创建期绑定的 `BaselineSourceResolver` 生成，而不是在 `RelationTaskSpec` 中枚举所有未来 routed 特征

#### 5.3.4 `EventCalendarSpec`

`src/plugins/baseline/model/event_calendar_spec.h` 已有结构定义，可保留。

需要补齐：

- parser 校验 `calendar_id / calendar_version / entries`
- parser 校验 `scope_type / alignment_mode / feature / key / tz` 的组合合法性
- predictor 侧的版本一致性检查
- 训练阶段根据 `EventCalendarSpec` 生成事件 indicator 列

事件命中语义必须由新增的 `src/plugins/baseline/model/event_calendar_matcher.*` 唯一承载。

该模块职责是：

- 对 `EventCalendarSpec` 做一次性 compile，生成 `CompiledEventCalendar`
- 基于 `(task_spec, key, feature, bucket_id, delta, tz)` 判断当前 bucket 命中的事件集合
- 同时服务训练阶段的事件列构造和在线预测阶段的事件命中判断

编码契约必须继续收口为：

- `CompiledEventCalendar` 是 task-bound、只读、可共享的编译结果，在 task 创建或配置装载时生成一次
- `formal_model_trainer.*` 与 `formal_predictor.*` 只消费 `CompiledEventCalendar`，不各自重新解析 `EventCalendarSpec`
- `ValueTask / RatioTask / RelationTask` 负责持有 `CompiledEventCalendar` 的共享引用，并传给训练 / 预测路径
- 正式模型元数据只持久化 `calendar_id / calendar_version / enabled_event_codes`，不把整份事件表复制进模型对象

`event_calendar_matcher.*` 必须至少提供以下方法级契约：

- `CompileEventCalendar(const EventCalendarSpec&, const BaselineTaskSpec&)`
- `ResolveBucketEvents(const CompiledEventCalendar&, const BaselineTaskSpec&, int64_t bucket_id)`
- `BuildEventIndicatorRow(const CompiledEventCalendar&, const BaselineTaskSpec&, int64_t bucket_id, Span<double> out_row)`

硬性约束：

- `scope_type`、`alignment_mode`、`feature`、`key`、`tz` 的过滤逻辑，只允许在 `event_calendar_matcher.*` 中实现一次
- `calendar_feature_helper.*` 只负责通用时间相位和月位置，不负责事件作用域匹配
- `formal_model_trainer.*` 与 `formal_predictor.*` 必须共享同一个 `CompiledEventCalendar`

#### 5.3.5 `HistoryReader`

`ibaseline_service.h` 中的 3 类 history reader 接口继续保留，但 `HistoryFetchRequest` 必须增加 `feature` 字段：

```text
HistoryFetchRequest = {
  key,
  feature,
  bucket_start,
  bucket_end
}
```

对 `T3`：

- `feature` 字段装载 relation task 的静态引用，等价于 `task_id`

### 5.4 更新层次与语义

代码层明确分为 4 类状态：

- `SeriesState`
  - gap、顺序性、持续性、观测数
- `DriftState`
  - `u_t`、`evidence_up/down`、`confirm_count`、`low_count`
- `ReadinessState`
  - `coverage_stats`
  - `monthpos_enabled`
  - `readiness`
  - `confidence_base`
  - `coverage_degraded`
- `FormalModelState`
  - formal / candidate / shadow / rebuild / validate / switch 的生命周期状态

约束：

- 不允许把正式模型参数放进 `SeriesState`
- 不允许把 rebuild 生命周期混进 `DriftState`
- `monthpos` 启停、覆盖率降级、`confidence_base` 计算，必须统一收口到新增的 `src/plugins/baseline/model/readiness_helper.*`
- `task/*_task.*` 中的 per-key runtime 只能组合这些状态，不能重新发明另一套平行状态机

`readiness_helper.*` 必须明确提供方法级契约，而不是只作为“职责 owner”存在。至少需要：

- `UpdateCoverageStats(ReadinessState*, int64_t bucket_id, bool is_valid_bucket)`
- `EvaluateMonthPosEligibility(const ReadinessState&, const SharedProfileConfig&)`
- `ComputeConfidenceBase(const ReadinessState&, ModelReadiness, BaselineSourceKind)`
- `RefreshOnlineReadiness(ReadinessState*, const SharedProfileConfig&, ModelReadiness, BaselineSourceKind)`
- `BuildTrainReadiness(const TrainingCoverageStats&, const SharedProfileConfig&)`

语义约束：

- 训练阶段与在线阶段对 `coverage / month_count / monthpos_enabled / confidence_base` 的计算公式必须共用同一 helper
- detector core 只能读取 `ReadinessState` 的结果，不能各自重写一套 readiness / confidence 规则
- task 层只能触发 helper 更新，不能直接改写 `monthpos_enabled` 或 `confidence_base`

## 6. 对应 `design.md` 第 6 章：`T1` 代码实现设计

### 6.1 `T1` 公共主干

`T1a` 与 `T1b` 统一走：

```text
ValueObservation
  -> transform
  -> formal_predictor
  -> reliability adapter
  -> residual normalization
  -> point score
  -> drift accumulator
  -> DetectorResult
```

代码承载：

- `detector/value_detector_core.*`
- `model/profile_config.h`
- `model/formal_model.h`
- `model/formal_predictor.*`
- `rebuild/formal_model_trainer.*`

### 6.2 `T1a` 适用范围

代码层不需要单独文件承载，仅通过：

- `BaselineTaskSpec.feature_type = "t1a"`
- `ValueDetectorCore` 的分支
- `profile_config.h` 中的 `TransformKind`

完成约束。

### 6.3 `T1a` 输入规格

运行时输入继续使用 `ibaseline_types.h` 中的：

```text
ValueObservation = {
  key,
  bucket_id,
  value,
  sample_count
}
```

实现约定：

- 对 `T1a`，`sample_count` 忽略，调用方可传 `0`
- `feature`、`delta`、`tz` 属于 `BaselineTaskSpec`，不在热路径重复传输
- `key` 是运行时序列身份；`ValueTask` 必须用 `key + task.feature` 查找本序列的 `BaselineSourceConfig?`

`ValueTask` 必须维护以下 per-key 运行时状态：

```text
ValueSeriesRuntime = {
  series_state,
  drift_state,
  shadow_state,
  formal_state,
  readiness_state,
  source_decision,
  service_model,
  candidate_model
}
```

其中 `ReadinessState` 由新增的 `readiness_helper.*` 统一维护，至少包括：

- `valid_bucket_count`
- `first_bucket_id`
- `last_bucket_id`
- `natural_month_bitmap` 或等价月覆盖统计
- `coverage`
- `month_count`
- `monthpos_enabled`
- `readiness`
- `confidence_base`
- `coverage_degraded`

### 6.4 `T1a` 数值变换

`model/formal_model.h` 必须定义：

- `TransformKind`
  - `identity`
  - `log1p`

`ValueFormalModel` 中必须持久化：

- `transform_name`
- `feature_profile`

`ValueDetectorCore` 负责：

- 在线执行 `Transform(y_t)`
- 使用模型中的 `transform_name`
- 从 `ReadinessState` 读取 `confidence_base`

`FormalModelTrainer` 负责：

- 训练时在变换空间建模
- 把 `transform_name` 写入模型

### 6.5 `T1a` 模型结构

#### 6.5.1 模型类型

`src/plugins/baseline/model/formal_model.h` 必须重写为正式结构，而不是当前的 `intercept-only`。

至少包含：

```text
enum class ModelReadiness {
  kColdStart,
  kCoreNoMonthReady,
  kFullReady
}

struct CoreBlock {
  double beta0;
  double trend_k;
  vector<double> day_sin;
  vector<double> day_cos;
  vector<double> week_sin;
  vector<double> week_cos;
}

struct MonthPosBlock {
  bool enabled;
  array<double, 31> dom_coeff;
  vector<double> dme_coeff;     // size = DME_max + 1
  array<double, 7> lwd_coeff;
  array<double, 31> dom_center;
  vector<double> dme_center;    // size = DME_max + 1
  array<double, 7> lwd_center;
}

struct EventBlock {
  bool enabled;
  string calendar_id;
  string calendar_version;
  vector<string> active_event_codes;
  vector<double> coeff;
}

struct FitBlockDigest {
  string block_name;
  string status;                // ok | skipped | degraded
  uint64_t sample_count;
  double objective;
  double condition_est;
}

struct ValueFormalModel {
  metadata;
  readiness;
  transform_name;
  solver_name;
  fit_strategy;
  delta;
  tz;
  feature_profile;
  core_block;
  monthpos_block;
  event_block;
  fit_summary;
  sigma_ref;
  train_start;
  train_end;
  confidence_base_at_train;
}
```

其中 `confidence_base_at_train` 不是可选诊断项，而是正式模型元数据的一部分；它必须由训练阶段的 `BuildTrainReadiness(...)` 计算并持久化，供快照、来源借用和重建对比使用。

#### 6.5.2 时间相位与 `DST`

必须新增 `src/plugins/baseline/model/calendar_feature_helper.*`，专门承载：

- `phase_day_local(bucket_id, delta, tz)`
- `phase_week_local(bucket_id, delta, tz)`
- `day_of_month`
- `days_to_month_end`
- `is_last_weekday_of_month`

该模块是 `design.md` 第 `6.3`、`6.5` 节中“本地日历字段与周期相位”的唯一时间语义入口。

明确约束：

- `bucket_id` 按 `UTC` 绝对窗口解释
- 日 / 周相位按 local wall clock 计算
- `DST` 切换日不做人工补偿
- 事件命中与作用域匹配只能由 `event_calendar_matcher.*` 实现，不能回流到本模块
- 任何别处不得重复实现一套相位逻辑

### 6.6 `T1a` 训练流程与目标

#### 6.6.1 训练责任拆分

`rebuild/formal_model_trainer.*` 负责：

- 构造 `Ω_train`
- 调用 `calendar_feature_helper` 生成 `Core / monthpos / event` 设计矩阵
- 调用 `readiness_helper.*` 判断 `monthpos` 是否启用，以及本次模型的 `readiness`
- 调用 `event_calendar_matcher.*` 生成训练阶段的事件 indicator 列
- 对 `day_of_month / days_to_month_end / last_weekday_of_month` 列做去中心化，并把中心化元数据写入 `MonthPosBlock`
- 分 3 个 stage 执行拟合
- 估计 `sigma_ref`
- 生成 `ValueFormalModel`

`solver/solver_backend.*` 负责：

- `WeightedHuberRidgeBlockSolver`
- `BlockFitSpec`
- `FitBlockResult`
- `weighted_huber_ridge_irls`

#### 6.6.2 统一块求解器契约

`solver_backend.*` 必须对外暴露：

```text
BlockFitSpec {
  block_name,
  y_target,
  X,
  sample_weight,
  ridge_diag,
  init_beta?,
  col_roles
}

BlockSolverConfig {
  solver_name,
  c_huber,
  s_min_fit,
  max_iter_fit,
  tol_obj_rel,
  tol_beta_inf,
  cond_max
}

FitBlockResult {
  status,
  beta,
  objective,
  condition_est,
  iter_count
}
```

其中 `init_beta?` 的 absent 语义固定为“本块从全 0 初值开始求解”；只有显式 warm start 时才填写上一版兼容系数。

#### 6.6.3 `T1a` stage 训练

`FormalModelTrainer` 必须显式实现：

- `TrainCoreBlock(...)`
- `TrainMonthPosBlock(...)`
- `TrainEventBlock(...)`
- `EstimateSigmaMAD(...)`

并把每一步的状态写入 `fit_summary`。

`monthpos` 的去中心化 owner 必须明确为 `FormalModelTrainer`：

- `calendar_feature_helper.*` 只负责生成原始本地日历字段，不负责学习期去中心化
- `FormalModelTrainer` 负责基于 `Ω_train` 计算 `dom_center / dme_center / lwd_center`
- `formal_predictor.*` 必须复用模型内持久化的这些中心化元数据，保证训练 / 预测口径一致

#### 6.6.4 回退契约

回退逻辑必须写在 `formal_model_trainer.*`，不能隐含在 detector 中：

- `core` 失败时：`weighted ridge -> intercept + day/week -> intercept-only`
- `monthpos` 失败时：整块置零，`status = degraded`
- `event` 失败时：整块置零，`status = degraded`

### 6.7 `T1a` 在线评分

`detector/value_detector_core.*` 是 `T1a / T1b` 的唯一在线评分引擎。

`T1a` 分支必须实现：

- `gate_score = gate_train = gate_shift = 1`
- `rho_t = 1`
- `x_t = Transform(y_t)`
- `mu_t = formal_predictor.PredictValue(...)`
- `z_t = (x_t - mu_t) / sigma_ref`
- `score_point`
- `normalized_score`
- `confidence = readiness_state.confidence_base`

在线评分不允许：

- 重新拟合模型
- 重算 `sigma_ref`
- 进行任何 `O(window)` 运算

### 6.8 `T1a` 漂移证据累积器

继续复用 `model/drift_state.h`，但该文件已经不是“最小可用状态”，而是正式 `BOCPD-style` 实现承载。

需要补齐：

- `p_shift_low / p_shift_high / M_shift / G_skip / G_reset`
- `τ_hat = t_confirm - c_t + 1` 所需的 `confirm_count`
- 方向连续确认与 gap 冻结规则

`ValueDetectorCore` 负责：

- 产生 `signed_residual`
- 调用 `UpdateDriftState`
- 基于 `p_shift_t` 决定 `RebuildIntent`

### 6.9 `T1a` 阈值语义

阈值构造统一放在：

- `model/profile_config.h`
- `common/result_builder.h`

责任分工：

- `profile_config.h`
  - 持有共享主参数 `z_warn / z_crit / shift_clip / alpha / lambda_mem / ...`
- `readiness_helper.*`
  - 持有 `coverage_stats -> readiness -> confidence_base` 的计算逻辑
- `result_builder.h`
  - 执行 `score_point`、`score_shift`、`severity` 映射

### 6.10 `T1a` 输出语义与正式重建触发

`ValueDetectorCore` 必须构造：

- `DetectorResult.raw_score = |z_t|`
- `DetectorResult.reason_code = spike | drop | baseline_shift_up | baseline_shift_down`
- `ValueEvidence`

同时产出：

```text
RebuildIntent {
  required,
  reason = shift_confirmed | scheduled | bootstrap,
  rebuild_start_hint = τ_hat,
  bucket_end
}
```

### 6.11 `T1b`：时延 / 连续值类差异规格

`T1b` 不新增新的 detector 文件，也不新增新的 formal model 文件。

差异全部落在以下 3 个位置：

- `model/profile_config.h`
- `rebuild/formal_model_trainer.*`
- `detector/value_detector_core.*`

#### 6.11.1 `T1b` profile 配置

`model/profile_config.h` 必须新增：

```text
T1bProfileConfig {
  profile_name,
  n_train_min,
  transform_name_override?
}
```

其中 `transform_name_override?` absent 时，正式语义固定为使用该 profile 的默认变换；`v1` 默认即 `log1p`，只有少数连续值特征确有必要时才允许覆盖。

并提供派生函数：

- `n_score_min = ceil(0.5 * n_train_min)`
- `n_shift_min = 2 * n_train_min`
- `kappa_sample = n_train_min`

#### 6.11.2 `sample_count` 的代码语义

`ValueObservation.sample_count` 在 `T1b` 中是正式输入，不允许忽略。

`ValueDetectorCore` 必须显式实现：

- `gate_score(n_t)`
- `gate_train(n_t)`
- `gate_shift(n_t)`
- `rho_t = sqrt(1 + kappa_sample / n_t)`
- `sigma_eff_t = sigma_ref * rho_t`

#### 6.11.3 `T1b` 训练差异

`FormalModelTrainer` 中的 value 训练路径必须支持：

- `Ω_train = { t | gate_train(t) = 1 }`
- `sample_weight = 1 / rho_t^2`

不能再把 `T1b` 当成 `T1a` 的同权训练。

`ReadinessState.confidence_base` 在 `T1b` 中仍由 `readiness_helper.*` 计算，`ValueDetectorCore` 只负责再做：

```text
confidence = confidence_base / rho_t
```

#### 6.11.4 `T1b` 输出证据

`ValueEvidence` 对 `T1b` 必须额外回填：

- `sample_count`
- `sigma_eff_t`

这两项不能只存在于快照 JSON 中。

## 7. 对应 `design.md` 第 7 章：`T2` 代码实现设计

### 7.1 `T2` 统一主干

`T2` 统一走：

```text
RatioObservation
  -> m0 / alpha0 / beta0
  -> p_smooth
  -> logit
  -> formal_predictor
  -> variance layer
  -> point score
  -> drift accumulator
  -> DetectorResult
```

代码承载：

- `detector/ratio_detector_core.*`
- `model/profile_config.h`
- `model/readiness_helper.*`
- `model/formal_model.h`
- `rebuild/formal_model_trainer.*`

### 7.2 输入规格

继续使用：

```text
RatioObservation = {
  key,
  bucket_id,
  numerator,
  denominator
}
```

`feature`、`feature_profile`、`delta`、`tz` 由 `BaselineTaskSpec` 提供。
`key` 是运行时序列身份；`RatioTask` 必须用 `key + task.feature` 查找本序列的 `BaselineSourceConfig?`。

`RatioTask` 的 per-key 运行时状态与 `ValueTask` 同构，也必须持有：

- `series_state`
- `drift_state`
- `shadow_state`
- `formal_state`
- `readiness_state`
- `source_decision`
- `service_model`
- `candidate_model`

### 7.3 `feature profile` 与默认分组

`model/profile_config.h` 必须新增：

```text
T2ProfileConfig {
  profile_name,
  s_prior,
  d_min_train,
  phi_over
}
```

并提供派生函数：

- `d_score_min`
- `d_shift_min`
- `kappa_den`

首版只保留：

- `rate_core`
- `ratio_bursty`

### 7.4 变换与训练目标

#### 7.4.1 `RatioFormalModel`

`formal_model.h` 中的 `RatioFormalModel` 必须扩展为正式结构，至少包含：

- `readiness`
- `transform_name = logit`
- `m0`
- `alpha0`
- `beta0`
- `feature_profile`
- `core_block`
- `monthpos_block`
- `event_block`
- `fit_summary`
- `train_start / train_end`
- `confidence_base_at_train`

`RatioFormalModel.confidence_base_at_train` 与 `ValueFormalModel` 一样，必须持久化，不允许省略。

#### 7.4.2 训练实现

`FormalModelTrainer` 的 ratio 路径必须显式实现：

- `ComputeM0(...)`
- `ComputeAlphaBeta(...)`
- `BuildSmoothedRatioTarget(...)`
- `BuildRatioTrainWeight(...)`
- `EvaluateReadiness(...)`
- `TrainRatioCore(...)`
- `TrainRatioMonthPos(...)`
- `TrainRatioEvent(...)`

### 7.5 在线评分与漂移证据累积器

`RatioDetectorCore` 必须显式实现：

- `p_smooth`
- `x_t = logit(...)`
- `p_hat_t = sigmoid(mu_hat_t)`
- `Var_ideal`
- `Var_model = Var_ideal * phi_over`
- `Var_eff_t = max(Var_model, v_floor)`
- `r_t = (numerator - denominator * p_hat_t) / sqrt(Var_eff_t)`
- `rho_t = sqrt(1 + kappa_den / denominator)`
- `confidence = confidence_base / rho_t`
- 其中 `confidence_base` 必须从 `readiness_state` 读取，而不是在 detector 内部自行推断

并复用 `DriftState`：

- 输入值改为 `clip(r_t, -shift_clip, shift_clip)`

### 7.6 阈值语义

`T2` 与 `T1` 共享：

- `z_warn`
- `z_crit`
- `shift_clip`
- `p_shift_low`
- `p_shift_high`
- `M_shift`
- `G_skip`
- `G_reset`

这些参数由 `profile_config.h` 中的全局共享配置承载，而不是散落在 detector 内的 `constexpr` 常量。

### 7.7 输出语义与正式重建触发

`RatioDetectorCore` 必须构造：

- `DetectorResult.raw_score = |r_t|`
- `RatioEvidence`
- `RebuildIntent`

### 7.8 冷启动与基线来源

`model/series_override.h` 继续承载：

- `BaselineSourceConfig`
- `BaselineSourceDecision`

代码约束：

- 源选择在 task 层完成，不在 detector 中解析 JSON
- detector 只消费“当前使用 self / configured_source / none 中哪一个”
- `T2` 使用来源基线时，`phi_over` 仍按当前 `feature_profile` 取值

`Sprint 20 BaselineA` 的正式来源选择契约收口为：

```text
if Serviceable(self_formal):
    use self
else if exists first Serviceable(configured_source_i):
    use configured_source_i
else:
    use none
```

其中：

- `Serviceable(self_formal)` 固定表示“本级 formal model 已达到 `core_no_month_ready | full_ready`，且当前 bucket 可预测”
- `Serviceable(configured_source_i)` 固定表示“该来源的 formal model 当前可服务”；本轮不再把 `candidate model` 视为正式来源
- 来源决策一旦落在 `configured_source_i`，本级序列仍继续累计 `Ω_train` 并训练自己的模型
- 一旦 `self_formal` 进入可服务状态，必须立即切回本级基线，不做多来源混合或渐进 blending

`BaselineSourceDecision` 至少需要承载：

- `selected_kind`
- `selected_source_key?`
- `serviceable`

行为约束：

- 当 `selected_kind = configured_source` 时，`readiness_helper.*` 中的 `ComputeConfidenceBase(..., BaselineSourceKind)` 必须给出低于 `self` 的 `confidence_base`
- 当 `selected_kind = none` 时，task 层只保留观察和状态积累，不允许输出正式高强度异常；输出若仍走统一协议，必须保证 `provider = none` 且 `normalized_score = 0`
- 上述冷启动抑制由 task 层决策，不允许 detector 自己发明第二套冷启动规则

### 7.9 性能约束

`RatioDetectorCore` 的实现必须满足：

- 热路径 `O(1)`
- 不调用 `lgamma`、不完全 `Beta`、精确 `Binomial` 尾概率
- 不做任何逐窗口循环

## 8. 对应 `design.md` 第 8 章：`T3` 代码实现设计

### 8.1 设计原则的代码边界

`T3` 的任务不是直接对原始向量做重型基线，而是：

```text
T3Block
  -> basis
  -> summary features
  -> routed T1/T2 detectors
  -> pattern fusion
```

因此：

- `relation/` 只负责分布到摘要特征的转换
- `detector/` 继续负责时间基线
- `fusion/` 负责模式级和 key 级风险

### 8.2 `T3TaskSpec`

`model/task_spec.h` 中的 `RelationTaskSpec` 必须完整承载：

- `task_id`
- `feature_base`
- `group_space_id`
- `group_space_version`
- `metric_set_id`
- `metrics`
- `encode_type`
- `support_policy`
- `summary_policy`

parser 必须校验：

- `metrics` 非空
- `encode_type ∈ { exact_sparse, topk_other }`
- `K_support >= 1`
- `K_stable >= 1`
- `K_support >= K_stable`
- `K_head >= 1`
- `0 < min_hist_share <= 1`
- `0 < min_active_ratio <= 1`
- `group_space_id / metric_set_id / feature_base` 非空

### 8.3 支持空间与建模依据

`relation/relation_basis.*` 是 `ServiceBasis / EvalBasis / lineage` 的唯一代码承载。

必须承载的正式类型：

- `RelationBasisBuildInput`
- `RelationServiceBasis`
- `RelationEvalBasis`
- `RelationLineageCompatibility`

并补齐以下语义：

- `SupportExplicit_T3(m)` 只在正式重建或 basis 刷新时更新
- `StableHeadSet_T3(m)` 从 `SupportExplicit_T3(m)` 派生
- `head_proto_q` 必须持久化
- `group_space_id / group_space_version` 不兼容时返回 `kNewLineage`

### 8.4 运行时输入：`T3Block`

`ibaseline_types.h` 中继续使用：

```text
RelationObservationBlock {
  key,
  bucket_id,
  nnz,
  group_idx,
  metric_count,
  metrics[metric_count]
}

RelationMetricBlock {
  total,
  flags,
  active_count,
  values
}
```

这里采用 task-bound 编码，与 `design.md` 中的 `T3Block` 语义等价：

- `task_id` 由 `IBaselineRelationTask` 固定绑定，因此不在每个 block 重复传输
- `metric_set_id / metrics` 由 `RelationTaskSpec` 固定绑定，因此运行时只传 `metric_index -> RelationMetricBlock`

其中 `flags` 至少定义：

- `kRelationMetricHasActiveCount`

语义：

- `flags & kRelationMetricHasActiveCount != 0`：表示上游显式提供了 `active_count`
- 否则表示 `active_count` 缺失，不能把 `0` 解释成真实值

这里必须使用显式 presence bit，而不能依赖哨兵值，因为：

- `active_count = 0` 在接口层不是稳定的“永远不可能值”
- `T3 v1` 需要严格区分“字段缺失”和“当前值为 0”

语义约束必须由 relation task 校验：

- `metric_count == spec.metrics.size()`
- `group_idx` 升序
- `total >= sum(values)`
- `flags` 不含 `kRelationMetricHasActiveCount` 时，只禁用 `distinct_group_count`

### 8.5 `topk` 与 `sketch` 的职责边界

代码层不实现 `sketch`。

relation task 只做两件事：

- 根据 `encode_type` 记录该任务的输入契约
- 在快照与诊断中保留“当前输入是否满足 `topk_other` 约束”的静态说明

即：

- `sketch` 是上游预处理能力，不进入 baseline 插件
- baseline 插件不保留 `sketch` 状态，也不依赖 `sketch` API

### 8.6 多指标展开语义

relation task 内部必须把一个 `RelationObservationBlock` 展开为多个逻辑特征：

```text
Feature_T3(m) = (feature_base, metric_m)
```

实现要求：

- `RelationTask` 对每个 `metric_m` 持有独立的 `RelationMetricRuntime`
- 每个 `RelationMetricRuntime` 再持有一组 routed detector core

### 8.7 与统一抽象的关系

relation task 需要显式维护：

```text
RelationKeyRuntime = {
  metrics[M],
  last_fusion_result
}

RelationMetricRuntime = {
  service_basis,
  eval_basis,
  routed_features[N]
}

RelationRoutedFeatureRuntime = {
  spec,
  source_decision,
  compiled_event_calendar?,
  value_core?,
  ratio_core?
}
```

其中：

- `RelationMetricRuntime` 不应把 `spec / source / event / core` 拆成多组松散平行数组，否则很容易在 basis 刷新、task close、快照输出时发生错位
- `RelationRoutedFeatureRuntime.spec` 必须是稳定、可快照化的静态 routed 特征规格
- `value_core` 与 `ratio_core` 二选一，取决于该 routed 特征落到 `T1a` 还是 `T2`
- `compiled_event_calendar?` absent 的正式语义是该 routed 特征事件层禁用；present 时必须来自 task-bound `EventCalendarSpec` 的编译结果，而不是临时生成
- `value_core? / ratio_core?` 的正式约束是“恰有一个 present”；不得同时缺失，也不得同时存在

### 8.8 摘要特征层

`relation/relation_summary_extractor.*` 必须承载：

- `entropy_shannon`
- `top1_share`
- `headK_share`
- `out_of_support_share`
- `distinct_group_count`
- `stable_g[i]_share`
- `stable_headK_coverage`
- `stable_headK_mix_drift`

并保证：

- 单个 `metric_m` 的提取复杂度为 `O(nnz)`
- 不按原始 group 全空间建状态
- 不做堆分配；如需 scratch buffer，由 `RelationKeyRuntime` 预分配

### 8.9 固定身份 `stable head set` 特征

`RelationSummaryExtractor` 必须基于 `RelationServiceBasis.stable_head` 计算：

- `stable_g[i]_share`
- `stable_headK_coverage`
- `stable_headK_mix_drift`

注意：

- routed feature 数量依赖 `K_stable_eff`
- 因此 `RelationRouter::BuildRoutedFeatureSpecs(...)` 不能只接收 `RelationTaskSpec`
- 必须改为接收 `RelationTaskSpec + RelationServiceBasis`

这是本轮必须修正的代码设计点。

### 8.10 摘要特征的路由与实现收口

`relation/relation_router.*` 必须负责两类事情：

#### 8.10.1 routed feature catalog

基于 `RelationTaskSpec + RelationServiceBasis` 生成：

- `entropy_shannon -> T1a`
- `top1_share -> T2(rate_core)`
- `headK_share -> T2(rate_core)`
- `out_of_support_share -> T2(ratio_bursty)`
- `distinct_group_count -> T1a(log1p)`
- `stable_g[i]_share -> T2(rate_core)`
- `stable_headK_coverage -> T2(rate_core)`
- `stable_headK_mix_drift -> T1a(identity)`

并且 `RelationRouter::BuildRoutedFeatureSpecs(...)` 的输入不能只停留在 `RelationTaskSpec + RelationServiceBasis`，还必须显式消费：

- `RelationTaskClockSpec`
- task-bound `EventCalendarSpec?`
- task-bound `BaselineSourceResolver?`

其中：

- `EventCalendarSpec?` 负责生成各 routed 特征的 `event_calendar_spec?`
- `BaselineSourceResolver?` 负责按 `(key, feature)` 生成各 routed 特征的 `baseline_source_config?`
- 若 `BaselineSourceResolver?` absent，则 routed 特征的 `baseline_source_config?` 统一为 absent

#### 8.10.2 routed observation 构造

必须提供：

- `BuildValueObservation(...)`
- `BuildRatioObservation(...)`

其输入是：

- `RelationRoutedFeatureSpec`
- `RelationMetricSummary`
- `key`
- `bucket_id`

`RelationRoutedFeatureSpec` 不能只是“名字到 detector 类型”的映射，至少必须承载：

```text
RelationRoutedFeatureSpec = {
  local_slot,
  feature,
  routed_type,              // t1a | t2
  feature_profile,
  transform_kind?,
  delta,
  tz,
  baseline_source_config?,
  event_calendar_spec?
}
```

约束：

- `local_slot` 在同一 `RelationTask` 内必须稳定，供 `FusionSourceId` 与快照回溯复用
- `delta / tz` 来自 `RelationTaskClockSpec` 的物化拷贝，禁止在 routed detector 内自行猜测
- `transform_kind?` 只允许在 `routed_type = t1a` 时填写；其 absent 语义是沿用 `feature_profile` 的默认变换；对 `t2` 必须固定为 absent
- `baseline_source_config?` 对应逻辑特征 `(key, feature)` 的正式来源配置；absent 时该 routed 特征只能在 `self | none` 之间选源；relation task 只做下发，不在 detector 内解析 JSON
- `event_calendar_spec?` 来源于 relation task 的 task-bound `EventCalendarSpec` 经过 feature 作用域裁剪后的物化结果；absent 时该 routed 特征事件层禁用，训练与预测都必须按 `h_event(t) = 0` 执行

### 8.11 `T3 v1` 首版推荐特征集

relation task 必须支持的必选核心：

- `entropy_shannon`
- `top1_share`
- `headK_share`
- `out_of_support_share`
- `stable_headK_coverage`
- `stable_headK_mix_drift`

可选增强：

- `distinct_group_count`
- `stable_g[i]_share`

由 `RelationRouter` 控制是否启用：

- `distinct_group_count` 依赖 `active_count`
- `stable_g[i]_share` 依赖 `K_stable_eff`

## 9. 对应 `design.md` 第 9 章：分型检测器与统一融合

### 9.1 单特征判定

单特征判定继续由：

- `ValueDetectorCore`
- `RatioDetectorCore`

完成。

这些 detector core 只输出：

- `DetectorResult`
- `RebuildIntent`

不直接计算：

- `score_P`
- `Risk_T3`
- `Risk(Key,t)`

### 9.2 融合风险分

本章必须拆成两个正式模块：

#### 9.2.1 `fusion/relation_pattern_fusion.*`

负责 `T3` 的：

- `a_f = s_f * c_f * π_f`
- `a_f^up / a_f^down`
- `core_P / support_P / oppose_P`
- `score_support_escape`
- `score_head_concentration`
- `score_legacy_head_dilution`
- `score_stable_head_mix_shift`
- 跨指标 `ScorePattern`
- `FusionResult`

需要正式定义：

- `PatternCode`
- `PatternScore`
- `FusionSingleContribution`
- `FusionPatternContribution`
- `FusionResult`

并显式实现：

- `AggCore = GeomMean`
- `AggSup = Top2Mean`
- `AggOpp = Top2Mean 或 max`
- `lambda_sup`
- `lambda_opp`

#### 9.2.2 `fusion/key_risk_fusion.*`

负责：

- `a_f = normalized_score_f * confidence_f * min(1, persistence_f / N_fuse)`
- `Risk_T1T2(Key,t)`
- `Risk_single_T3(Key,t)`
- `Risk_pattern(Key,t)`
- `Risk_T3(Key,t)`
- `Risk(Key,t)`

这是当前代码设计里缺失、但 `design.md` 正式存在的模块，必须新增。

该模块至少需要：

```text
FusionSourceId {
  task_id,
  source_kind,   // direct_single | routed_single | relation_pattern
  local_slot     // direct task 固定为 0；relation routed feature / pattern 在 task 内唯一编号
}
```

说明：

- `FusionSourceId` 是 `KeyRiskFusion` 的唯一来源身份，不能只用 `task_id`
- 对 `ValueTask / RatioTask`，`local_slot = 0`
- 对 `RelationTask`，每个 routed 摘要特征必须分配稳定 `local_slot`
- `dominant_single` 仍以 `(key, feature, ts)` 对外解释，但 `KeyRiskFusion` 内部去重与覆盖必须按 `FusionSourceId`

```text
KeyRiskFusionState {
  windows[<=2],
  latest_finalized_bucket_id,
  latest_finalized_result
}

KeyRiskWindowState {
  bucket_id,
  single_results_by_source_id,
  relation_fusions_by_source_id,
  key_risk,
  finalized
}
```

并提供：

- `UpdateSingleDetectorResult(result.ts, FusionSourceId, DetectorResult)`
- `UpdateRelationFusionResult(fusion.ts, FusionSourceId, FusionResult)`
- `ComputeKeyRisk(...)`
- `FinalizeWindow(bucket_id)`
- `PruneOlderWindows(...)`
- `RemoveTaskContributions(task_id)`
- `QueryKeyFusionSnapshotJson(...)`

服务级 owner 明确为：

- `baseline_plugin.*` 持有全局 `KeyRiskFusion` 注册表
- `ValueTask / RatioTask / RelationTask` 在热路径提交后，把本次 `DetectorResult` 或 `FusionResult` 推送到该注册表
- `IBaselineService::QueryKeyFusionSnapshotJson(...)` 由 `baseline_plugin.*` 直接转发到 `KeyRiskFusion`
- `ValueTask::Close()`、`RatioTask::Close()`、`RelationTask::Close()` 除释放 task 自身资源外，还必须向 `KeyRiskFusion` 调用 `RemoveTaskContributions(task_id)`

时间维度硬性约束：

- `KeyRiskFusion` 只能融合“同一 `key`、同一 `bucket_id`”的结果
- 不允许把不同 `bucket_id` 的 `DetectorResult` 与 `FusionResult` 混到同一个 `Risk(Key,t)`
- 当某个 `key` 收到更大的 `bucket_id` 时，旧窗口必须先 `FinalizeWindow`
- `QueryKeyFusionSnapshotJson(key)` 默认返回 `latest_finalized_result`；若还存在进行中的窗口，可在快照中附带 `active_window`

热路径性能约束：

- 全局注册表必须做分片，例如 `shard by hash(key)`
- 每个 `key` 只保留极小数量的活跃窗口，`v1` 建议上限 `2`
- `RemoveTaskContributions(task_id)` 在 task `Close()` 时必须调用，避免全局注册表长期残留已关闭 task 的贡献
- `RemoveTaskContributions(task_id)` 必须删除该 task 生成的全部 `FusionSourceId` 贡献，包含 routed single 与 relation pattern 两类来源

## 10. 对应 `design.md` 第 10 章：在线执行、`shadow baseline` 与正式重建

### 10.1 在线路径

代码路径固定为：

- `ValueTask::SubmitObservation`
  - `ValueDetectorCore::Submit(...)`
- `RatioTask::SubmitObservation`
  - `RatioDetectorCore::Submit(...)`
- `RelationTask::SubmitBlock`
  - summary extract
  - routed detector submit
  - pattern fusion
  - key fusion update
  - synchronous `FusionResult` return

在线路径必须满足：

- 不调用 `HistoryReader`
- 不持有可重放明细
- 只更新轻量状态
- `RelationTask::SubmitBlock(...)` 返回前，必须已经形成当前 bucket 的最终 `FusionResult`

### 10.2 异步正式重建路径

#### 10.2.1 重建请求

`rebuild/rebuild_request.h` 必须正式承载：

```text
RebuildRequest = {
  key,
  feature,
  rebuild_reason,
  bucket_start_hint,
  bucket_end
}
```

`feature` 对：

- `T1 / T2` 表示 `(key, feature)` 中的 `feature`
- `T3` 表示 `task_id`

#### 10.2.2 `shadow baseline`

`model/shadow_state.h` 已有基础结构，可保留，但要与 `design.md` 对齐。

必须承载：

- `active`
- `ref_kind`
- `ref_source_key`
- `ref_model_version`
- `frozen_ref_model`
- `delta`
- `last_bucket_id`

`ValueDetectorCore / RatioDetectorCore` 负责：

- 激活条件检查
- `delta_0` 初始化
- 在线更新 `delta`
- 使用 `k_shadow_sigma`
- 应用 `confidence = min(confidence_base, c_shadow_max)`
- 设置 `provider = shadow`

`task/*_task.*` 负责：

- `shadow` 的生命周期切换
- 保护性退出
- 正式模型切换成功后的退出

`v1` 的正式激活约束必须继续收口为：

- 只有当 `shift_confirmed = true` 且 `DriftState.confirm_count >= 3` 时，才允许真正激活 `shadow baseline`
- 上述 `confirm_count >= 3` 是“`shadow` 接管 -> `holdout` 验证 -> 正式切换”链路的工程保护，不允许在代码里悄悄省略
- `shadow baseline` 不改变原有 `reason_code` 体系；它只改变当前使用的预测来源和置信度上限

#### 10.2.3 候选验证与正式切换

必须把状态机显式写进 `formal_model_state.h`，至少包含：

- `candidate_state`
  - `none`
  - `building`
  - `built`
  - `validating`
  - `accepted`
  - `rejected`
  - `failed`
- `switch_state`
  - `idle`
  - `shadow_active`
  - `rebuild_pending`
  - `validating`
  - `formal_applied`
  - `rebuild_blocked`

`candidate_builder.*` 负责：

- 从 `HistoryReader` 拉历史
- 计算 `Ω_rebuild`
- 训练 candidate
- 对 `T3` 同时生成 `CandidateServiceBasis` 与 `CandidateEvalBasis`

`candidate_validator.*` 负责：

- 构造 `Ω_fit / Ω_val`
- 计算 `L_val(candidate)` 与 `L_val(incumbent)`
- `T3` 在 `EvalBasis` 上比较
- 输出 `candidate_pass`

候选验证契约必须显式写死为“尾部保留验证（holdout tail validation）”，不能只停留在“有 `Ω_fit / Ω_val`”：

```text
Ω_val = Ω_rebuild 中最后 N_val_switch 个有效 bucket
Ω_fit = Ω_rebuild \ Ω_val
```

约束：

- `candidate model` 只能使用 `Ω_fit` 训练
- 若 `Ω_rebuild` 的有效 bucket 数少于 `2 * N_val_switch`，则本次不做切换验证，保持 `incumbent`
- `Ω_val` 中的“有效 bucket”定义沿用各特征类型自己的 gate 规则；gap 和无效 bucket 不计入尾段长度

`candidate_validator.*` 必须继续显式实现：

- 对 `T1a / T1b`：`w_val(t) = 1`
- 对 `T2`：`w_val(t) = denominator_t / (denominator_t + d_min_train(feature))`
- 对 `T3`：在共同 `EvalBasis` 上重建摘要特征，再对 routed `T1 / T2` 验证损失取简单平均

`T3` 的验证视图必须分成两套对象：

- `candidate service model`：基于 `CandidateServiceBasis` 训练，只用于切换后服务
- `candidate eval model`：基于 `CandidateEvalBasis = EvalBasis` 训练，只用于验证比较
- `incumbent model`：始终按当前 `EvalBasis` 计损

若 `incumbent = shadow baseline`，则验证时必须：

- 从 `Ω_fit` 起点按同一套轻量 `shadow` 规则做一次单遍 replay
- 在 `Ω_val` 上按“先预测、后更新”的 prequential 方式计损
- 禁止直接把 `shadow` 当冻结 formal model 处理

`candidate_pass` 的正式比较规则收口为：

```text
candidate_pass
= I(candidate_core_status = ok)
 * I(L_val(candidate_eval) <= (1 + eps_switch) * L_val(incumbent))
```

`rebuild_worker.*` 负责：

- 驱动上述慢路径
- 成功后原子替换服务模型
- 更新 `FormalModelState`

#### 10.2.4 历史读取、回退与计划重建

`rebuild_worker.*` 必须实现：

- `history_reader` 缺失 -> `rebuild_blocked`
- `insufficient_data` -> 放弃本次正式重建
- `unavailable` -> 标记失败，等待重试

relation task 额外要求：

- `group_space_id / group_space_version` 不兼容时，`RelationBasisBuilder` 返回 `kNewLineage`
- 这种情况下不做 `candidate vs incumbent` 直接对比，按新 lineage 冷启动

## 11. 对应 `design.md` 第 11 章：工程约束的代码落点

### 11.1 高基数控制

必须落在 relation task 及其下游模块中：

- `RelationSummaryExtractor` 复杂度 `O(nnz)`
- `RelationTask` 不保存原始 group 历史明细
- `RelationBasis` 只保存 `support_explicit / stable_head / head_proto_q`
- `KeyRiskFusion` 采用分片注册表，不允许单全局锁串行所有 key 的热路径提交

### 11.2 冷启动策略

必须由 task 层承载，而不是 detector 自己去找来源。

具体为：

- `ValueTask / RatioTask` 持有 `BaselineSourceDecision`
- `RelationTask` 的 routed detector runtime 也各自持有来源决策
- detector 只消费“当前 provider 是 formal / shadow / source / none”
- 来源借用期间，本级训练与 `readiness` 积累不能暂停
- 一旦本级 `formal model` 进入 `core_no_month_ready | full_ready`，必须立即切回本级基线，不做 blending
- 当 `provider = none` 时，只允许观察和状态积累，不允许输出正式高强度异常

### 11.3 缺失值策略

公共 gap 语义只允许在：

- `SeriesState`
- `DriftState`
- `ShadowState`

里处理。

不允许：

- trainer 自行补空 bucket
- detector 自行做插值

### 11.4 历史数据与正式重建约束

对应 `HistoryReader`：

- 在线路径禁止调用
- rebuild / replay 才能调用
- 返回协议必须与在线 observation 一致

### 11.5 算法代码注释约束

所有新增或重写的算法文件，都必须在代码设计里明确写注释目标。

本轮重点文件：

- `model/calendar_feature_helper.*`
- `model/event_calendar_matcher.*`
- `model/readiness_helper.*`
- `model/formal_model.h`
- `detector/value_detector_core.*`
- `detector/ratio_detector_core.*`
- `relation/relation_summary_extractor.*`
- `relation/relation_router.*`
- `fusion/relation_pattern_fusion.*`
- `fusion/key_risk_fusion.*`
- `rebuild/formal_model_trainer.*`
- `rebuild/candidate_builder.*`
- `rebuild/candidate_validator.*`

## 12. 对应 `design.md` 第 12 章：参数归属与默认值承载

### 12.1 参数归属原则

所有参数收口到 4 类 owner：

- task 静态规格：`TaskSpec`
- profile 参数：`profile_config.h`
- 融合参数：`fusion/*.h`
- 求解器 / 数值常量：`solver_backend.*`

禁止把主参数散落为 detector 内的裸 `constexpr`。

### 12.2 `profile_config.h` 的正式职责

必须新增 `src/plugins/baseline/model/profile_config.h`，承载：

#### 12.2.1 共享主参数

- `K_day`
- `K_week`
- `DME_max`
- `M_month_enable`
- `month_cov_min`
- `z_warn`
- `z_crit`
- `shift_clip`
- `alpha`
- `lambda_mem`
- `kappa_shift`
- `u_min`
- `H_shift`
- `p_shift_low`
- `p_shift_high`
- `M_shift`
- `G_skip`
- `G_reset`
- `w_shift`

#### 12.2.2 `T1b` profile 参数

- `n_train_min`
- `transform_name_override`

派生：

- `n_score_min`
- `n_shift_min`
- `kappa_sample`

#### 12.2.3 `T2` profile 参数

- `s_prior`
- `d_min_train`
- `phi_over`

派生：

- `d_score_min`
- `d_shift_min`
- `kappa_den`
- `m0 / alpha0 / beta0`

### 12.3 融合参数归属

`fusion/relation_pattern_fusion.*` 承载：

- `lambda_sup`
- `lambda_opp`
- `lambda_P(pattern)`

`fusion/key_risk_fusion.*` 承载：

- `N`
- `N_fuse`
- `w_f`

### 12.4 求解器与实现常量归属

`solver/solver_backend.*` 承载：

- `solver_name`
- `c_huber`
- `s_min_fit`
- `max_iter_fit`
- `tol_obj_rel`
- `tol_beta_inf`
- `cond_max`

`rebuild/candidate_validator.*` 承载：

- `N_val_switch`
- `eps_switch`

`detector/*` 与 `shadow_state` 承载：

- `c_shadow_max`
- `k_shadow_sigma`

`common/result_builder.h` 承载：

- `severity` 映射阈值

## 13. 当前 baseline 代码的保留 / 重构 / 重写矩阵

### 13.1 保留

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

说明：

- 这些文件的职责方向成立
- 但其中部分结构体字段仍需扩展，不等于“不用改”

### 13.2 保留并重构

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

其中需要额外收口的文件边界：

- `src/plugins/baseline/task/baseline_task_base.*` 只保留薄基类职责，不再承载 relation 摘要提取、candidate 验证、full model 训练、rebuild apply 或 routed detector 管理
- `src/plugins/baseline/task/value_task.*`、`src/plugins/baseline/task/ratio_task.*`、`src/plugins/baseline/task/relation_task.*` 分别承载各自 task-specific 的热路径编排、snapshot 细节与重建切换细节

### 13.3 整体重写

- `src/plugins/baseline/model/formal_model.h`
- `src/plugins/baseline/model/formal_predictor.*`
- `src/plugins/baseline/rebuild/formal_model_trainer.*`
- `src/plugins/baseline/rebuild/candidate_builder.*`
- `src/plugins/baseline/rebuild/candidate_validator.*`
- `src/plugins/baseline/solver/solver_backend.*`
- `src/plugins/baseline/detector/value_detector_core.*`
- `src/plugins/baseline/detector/ratio_detector_core.*`

### 13.4 新增

- `src/plugins/baseline/model/profile_config.h`
- `src/plugins/baseline/model/calendar_feature_helper.*`
- `src/plugins/baseline/model/event_calendar_matcher.*`
- `src/plugins/baseline/model/readiness_helper.*`
- `src/plugins/baseline/fusion/key_risk_fusion.*`
- `src/plugins/baseline/fusion/relation_pattern_fusion.*`
- `src/plugins/baseline/task/relation_task.*`

## 14. 测试与验证矩阵

### 14.1 `T1`

必须覆盖：

- `log1p` 与 `identity` 变换
- `Core / monthpos / event` 三阶段训练
- `EventCalendarSpec` 的 `scope_type / alignment_mode` 命中一致性
- `DST` 日内相位
- `coverage_stats -> readiness -> confidence_base` 转换
- `T1b sample_count -> gate / rho / sigma_eff`
- `shadow baseline`
- `τ_hat` 与 rebuild trigger

### 14.2 `T2`

必须覆盖：

- `m0 / alpha0 / beta0`
- `p_smooth / logit / sigmoid`
- `phi_over / v_floor`
- `readiness_state.confidence_base -> confidence / rho_t`
- `denominator` gate
- 来源基线借用

### 14.3 `T3`

必须覆盖：

- `ServiceBasis` 构建
- `EvalBasis` 兼容性
- `RelationTaskClockSpec(delta / tz)` 绑定与 routed detector 继承
- `BaselineSourceResolver?` 为 routed 特征生成 `baseline_source_config?`
- `topk_other` 输入
- `active_count` 缺失与显式提供两种编码
- `entropy / top1_share / headK_share / out_of_support_share`
- `stable_g[i]_share / stable_headK_coverage / stable_headK_mix_drift`
- routed detector 复用
- `support_policy / summary_policy` 的 parser 合法性校验
- 四类模式融合
- 同一 `RelationTask` 下多个 routed feature 的 `FusionSourceId.local_slot` 唯一且稳定
- relation snapshot 返回 `ServiceBasis / EvalBasis` 摘要

### 14.4 重建与切换

必须覆盖：

- `HistoryReader` 缺失
- `insufficient_data`
- `unavailable`
- `candidate_pass`
- `candidate_fail`
- `new lineage`
- `shadow -> formal` 成功切换
- `KeyRiskFusion` 同 `key` 不同 `bucket_id` 不串窗融合
- `RemoveTaskContributions(task_id)` 会清理 direct / routed / pattern 全部来源

## 15. 本文结论

Sprint 20A 的 `code-design.md` 从这一版开始，必须被当作编码契约使用，而不是“方向性说明”。

后续写代码时，必须满足：

- 每个 `design.md` 的正式能力，都能在本文找到明确代码 owner
- 每个新增模块，都有清晰的输入、输出、状态和参数归属
- 每个实现偏差，都必须先回到 `design.md` 和本文修文，而不能直接落到代码里

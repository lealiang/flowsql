# B4 Relation Routed Rolling and Stream Basis 阶段设计

## 1. 阶段定位

`B4` 的目标是让 Relation 进入 BaselineB 的在线主路径。

核心结论：

```text
Relation block
  -> basis-aware summary projection
  -> routed value/ratio observations
  -> reuse B2/B3 Online Rolling Core
```

Relation 不单独实现新的时间序列基线算法。Relation 分布变化先被投影成一组稳定摘要特征，再按 `value_basic` 或 `ratio` 进入已有 rolling core，复用 B2 的滚动学习、B3 的 band 校准、score trust、maturity 和 forecast 语义。

Relation basis 的职责是定义哪些 group 属于有界 support / stable head，以及这些 group 如何形成可解释摘要。basis 可以由 `B1` 历史训练 seed 初始化，也必须能在无历史时通过流式统计保守成熟。

### 1.1 上游依赖

- `B1` 已提供：
  - `RelationServiceBasis`。
  - `relation_basis_by_metric`。
  - `relation_routed_summary_seeds`。
  - 离线 relation block 到 routed summary seed 的训练能力。
- `B2` 已提供：
  - `RunValueRollingSubmit` / `RunRatioRollingSubmit`。
  - task 内部 seed 批量 warm-up。
  - `QueryRollingTaskSnapshot` / `QueryRollingSeriesSnapshot`。
- `B3` 已提供：
  - score trust。
  - calibrated detection band。
  - maturity / component readiness。
  - `PredictRollingForSeries` 的基础 forecast view。

### 1.2 非目标

B4 不做：

- Relation 专用时间序列模型。
- Relation 分布整体长期 forecast 产品接口。
- 每个 group 一个无界 rolling baseline。
- 每个 bucket 动态切换 support / stable head。
- `shadow/candidate/rebuild`、`HistoryReader.fetch` 或正式重建链路。
- Rolling 反向改写 bootstrap artifact / seed。

---

## 2. 当前代码基线

现有 Relation 代码处于 `B1` 形态：

| 代码位置 | 当前能力 | B4 处理 |
| --- | --- | --- |
| `IBaselineRelationTask` | 只暴露 `Bootstrap` / artifact / seed / basis 查询 | 新增流式提交与 routed forecast 查询 |
| `BaselineRelationTask` | 只维护 `artifacts_by_series_`、`seeds_by_series_` | 新增 basis 在线状态、routed rolling states 和 routed seed store |
| `RelationBasisBuilder` | 从历史 `RelationGroupHistoryStat` 构建 basis | 继续复用，B4 增加有界流式统计生成同构输入 |
| `BootstrapEngine::TrainRelation` | 私有实现 relation summary 提取和 routed bootstrap 训练 | summary 提取逻辑下沉到 relation 模块，供 B1/B4 复用 |
| `rolling_task_runner.*` | 支持 value / ratio rolling submit、predict、snapshot | B4 直接调用，不复制算法 |

设计约束：

- 不能把 B4 写成第二套 rolling core。
- 不能让 RelationTask 在首个流式 block 上一次性冻结 basis 后长期不动。
- 不能把 B1 的 JSON seed 导出再解析回来作为 B4 初始化主路径。

---

## 3. Public ABI 改造

B4 需要让外部可以提交 Relation 流式 block，因此 `IBaselineRelationTask` 必须新增流式入口。初始化细节仍然保持内部 lazy，不暴露 `InitFromEmpty` / `InitFromBootstrap`。

### 3.1 新增 public 类型

放置位置：`src/framework/interfaces/ibaseline_types.h`。

```cpp
struct RelationRollingObservation {
    std::string series_key;
    int64_t bucket_id = 0;
    std::vector<uint32_t> group_idx;
    std::vector<RelationBootstrapMetric> metrics;
};

struct RelationRollingSubmitOptions {
    RollingSubmitOptions routed_options;
    bool allow_basis_update = true;
    bool include_routed_results = true;
    bool include_diagnostics = false;
};

struct RelationRoutedSummaryResult {
    std::string source_series_key;
    std::string routed_series_key;
    std::string metric;
    std::string summary;
    std::string feature_type;
    uint64_t basis_version = 0;
    bool basis_scoped = false;
    RollingBaselineResult rolling;
};

struct RelationRollingResult {
    BaselineStatus status = BaselineStatus::kOk;
    std::string series_key;
    int64_t bucket_id = 0;
    uint64_t basis_version = 0;
    std::string basis_status;
    bool basis_updated = false;
    bool handover_active = false;
    std::vector<RelationRoutedSummaryResult> routed_results;
    std::string diagnostics;

    bool ok() const { return status == BaselineStatus::kOk; }
};

struct RelationRoutedSummaryQuery {
    std::string source_series_key;
    std::string metric;
    std::string summary;
    std::string feature_type;
    uint64_t basis_version = 0;
};
```

说明：

- `RelationRollingObservation` 使用与 `RelationBootstrapBlock` 同构的字段，但补齐 `series_key`，表示单个在线 bucket。
- `RelationBootstrapMetric` 首版复用已有 public 类型，避免 B4 为指标数组引入重复结构。后续若要清理命名，可单独做兼容迁移。
- `RelationRollingSubmitOptions.routed_options` 直接传给 B2/B3 rolling core。
- `include_diagnostics` 默认关闭，避免 Relation fan-out 热路径无条件拼接大 diagnostics。
- `RelationRoutedSummaryResult.rolling` 复用已有 `RollingBaselineResult`，避免为 Relation 复制 band / score / trust 字段。
- `RelationRoutedSummaryQuery` 只用于查询 routed summary forecast / snapshot，不用于初始化；`basis_version = 0` 表示使用当前 active basis。通用摘要忽略该字段，basis-scoped 摘要若传入 `> 0` 则表示精确查询指定 basis version。

### 3.2 `IBaselineRelationTask` 接口

```cpp
interface IBaselineRelationTask : public IBaselineTask {
    virtual RelationRollingResult SubmitObservation(
        const RelationRollingObservation& obs,
        const RelationRollingSubmitOptions& options) = 0;

    virtual RollingPrediction PredictRoutedSummary(
        const RelationRoutedSummaryQuery& query,
        int64_t bucket_id) const = 0;

    virtual BaselineSerializationResult QueryRoutedSummarySnapshot(
        const RelationRoutedSummaryQuery& query,
        BaselineSerializationFormat format) const = 0;

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
```

`SubmitObservation()` 是 Relation rolling 的唯一 public 主入口：

- 第一次提交时，如果同 `source_series_key` 有 B1 basis seed / routed summary seed，内部批量 warm-up。
- 没有 seed 时，内部创建 stream-only basis state，并允许通用摘要空启动。
- 不暴露空启动 / bootstrap 初始化函数。

`PredictRoutedSummary()` 是只读 forecast：

- 只预测指定 routed summary。
- 不更新 state。
- 不触发 lazy init。
- 查不到 routed state 时返回 `kNotTrained` 或等价不可用状态。

`QueryRoutedSummarySnapshot()` 是 dedicated routed snapshot 查询入口，避免把 `QuerySeriesSnapshot(source_series_key)` 的 Relation source 语义和 `QuerySeriesSnapshot(routed_series_key)` 的 rolling 语义混在一起。

---

## 4. 身份规则

Relation task 内部会为一个 `source_series_key` 生成多个 routed summary。B4 必须显式区分 3 层身份。

| 身份 | 示例 | 用途 |
| --- | --- | --- |
| `source_series_key` | `linkA.client_mix` | 外部 Relation 序列身份 |
| `summary_identity` | `bps.top1_share.ratio` | 摘要特征身份 |
| `routed_series_key` | `linkA.client_mix::bps::top1_share::ratio` | rolling state key |

生成规则：

```text
universal summary:
  routed_series_key =
    source_series_key + "::" + metric + "::" + summary + "::" + feature_type

basis-scoped summary:
  routed_series_key =
    source_series_key + "::" + metric + "::" + summary + "::" + feature_type
      + "::basis:" + basis_version
```

`basis_version` 不进入通用摘要的 key，因为 `entropy_shannon`、`distinct_group_count`、`top1_share`、`headk_share` 的定义不依赖 support / stable head。

`basis_version` 必须进入 basis-scoped 摘要的 key，因为 `out_of_support_share`、`stable_headk_coverage`、`stable_g_share_i`、`stable_headk_mix_drift` 的语义随 basis 变化。这样可以避免 basis 切换后把旧语义的 rolling state 当成新语义继续使用。

summary 分类固定如下，不能由运行时动态猜测：

| summary | scope | basis_version 语义 |
| --- | --- | --- |
| `entropy_shannon` | `universal` | 不进入 key，查询时忽略 |
| `distinct_group_count` | `universal` | 不进入 key，查询时忽略 |
| `top1_share` | `universal` | 不进入 key，查询时忽略 |
| `headk_share` | `universal` | 不进入 key，查询时忽略 |
| `out_of_support_share` | `basis_scoped` | 必须进入 key |
| `stable_headk_coverage` | `basis_scoped` | 必须进入 key |
| `stable_g_share_i` | `basis_scoped` | 必须进入 key，因为 `i` 依赖 stable head 顺序 |
| `stable_headk_mix_drift` | `basis_scoped` | 必须进入 key，因为依赖 `head_proto_q` |

内部还需要为每个 routed summary 生成 `BaselineTaskSpec`：

```text
task_id      = relation_task_id + "::" + metric + "::" + summary
feature_id   = feature_base + "." + metric + "." + summary
feature_type = value_basic 或 ratio
profile      = value_basic 使用 default，ratio 使用 rate_core
clock_spec   = RelationTaskClockSpec
calendar_ref = RelationTaskSpec.calendar_ref
```

该规则必须与 B1 `TrainRelation` 生成 routed summary seed 时的规则保持一致。

---

## 5. Routed Summary 投影

B4 首版 summary 集合必须与 B1 保持一致，但实现形态要拆成共享的 relation summary 模块，不能继续只复用当前 `BootstrapEngine::TrainRelation` 内部的 `ExtractRelationBootstrapMetricSummary()`。原因是现有函数强依赖 `RelationServiceBasis`，无法支撑无历史 `no_basis` 冷启动时的通用摘要。

### 5.1 输入模型

新 summary 模块分两类输入。

通用摘要输入：

```text
RelationBootstrapBlock 或 RelationRollingObservation
RelationTaskSpec.summary_policy.k_head
RelationTaskSpec.other_group_idxs
metric index / metric name
```

通用摘要不得要求 `RelationServiceBasis`。

Basis-scoped 摘要输入：

```text
RelationBootstrapBlock 或 RelationRollingObservation
RelationTaskSpec.summary_policy
RelationTaskSpec.other_group_idxs
active RelationServiceBasis
metric index / metric name
```

Basis-scoped 摘要必须要求 active basis；没有 active basis 时不生成对应 routed observation。

### 5.2 通用摘要

无 basis 也可以计算，stream-only 冷启动时立即进入 rolling。

| summary | feature_type | 说明 |
| --- | --- | --- |
| `entropy_shannon` | `value_basic` | 当前 block 的 group 分布熵 |
| `distinct_group_count` | `value_basic` | 当前活跃 group 数 |
| `top1_share` | `ratio` | 最大 group 质量占比 |
| `headk_share` | `ratio` | 当前 top-k group 质量占比，`k = summary_policy.k_head` |

### 5.3 Basis-scoped 摘要

只有 active basis 可用时才计算。basis 未成熟时可以学习，但不得直接输出高置信异常。

| summary | feature_type | 说明 |
| --- | --- | --- |
| `out_of_support_share` | `ratio` | 不在 support 内的质量占比 |
| `stable_headk_coverage` | `ratio` | stable head 覆盖质量占比 |
| `stable_g_share_i` | `ratio` | 第 `i` 个 stable head group 的质量占比 |
| `stable_headk_mix_drift` | `value_basic` | stable head 内部混合比例相对 `head_proto_q` 的 TV distance |

### 5.4 Rolling core 适配协议

B4 复用 rolling core 的边界必须写死：

- RelationTask 构造 `ValueRollingObservation` 后调用 `RunValueRollingSubmit()`。
- RelationTask 构造 `RatioRollingObservation` 后调用 `RunRatioRollingSubmit()`。
- 不允许绕过 public runner 调用 `rolling_task_runner.cpp` 内部的 `SubmitObservedPoint()` 或复制 observation adapter。

Value summary 映射：

```text
ValueRollingObservation.series_key = routed_series_key
ValueRollingObservation.bucket_id = block.bucket_id
ValueRollingObservation.value = summary_value
ValueRollingObservation.sample_count = 1
```

Ratio summary 必须沿用 B1 `AppendRatioPoint()` 的口径：

```text
RatioRollingObservation.series_key = routed_series_key
RatioRollingObservation.bucket_id = block.bucket_id
RatioRollingObservation.numerator = Clamp01(share) * metric.total
RatioRollingObservation.denominator = metric.total
```

这样 B2/B3 的 denominator reliability、低分母 skip、ratio 先验和 band 校准才与 bootstrap seed 的训练口径一致。

### 5.5 投影顺序

每个流式 block 的顺序固定为：

```text
validate / normalize block
  -> ensure relation source state
  -> ensure routed rolling warm-up once
  -> extract summaries with current active basis
  -> submit routed value/ratio observations
  -> update stream basis accumulator
  -> maybe low-frequency refresh basis
  -> return RelationRollingResult
```

先使用当前 active basis 投影和评分，再用当前 block 更新 basis 统计。这样可以避免同一个 bucket 既改变 feature definition 又用新定义给自己评分。

### 5.6 Relation basis gate

B3 的 score trust 只知道某个 routed series 自己是否成熟，不知道这个 series 背后的 Relation basis 是否成熟。因此 B4 必须在 RelationTask 层增加 basis gate。

对 basis-scoped 摘要：

- `basis_status != basis_ready` 时，仍可提交 rolling 更新，但返回结果必须降级：
  - `can_alert = false`
  - `effective_confidence` 不得高于 warming 水平
  - `diagnostics` 追加 `relation_basis_not_ready`
- `handover_warming` 期间，新 basis version 的 routed state 只学习或低可信评分。
- 只有 `basis_ready` 且 routed rolling 自身 `score_trust_status = score_ready` 时，才允许高置信异常输出。

通用摘要不受 basis gate 影响，只受 B2/B3 rolling maturity 和 score trust 约束。

---

## 6. Basis 在线成熟

### 6.1 状态模型

内部新增 `RelationBasisRuntimeState`，按 `(source_series_key, metric)` 维护。

```text
no_basis
  -> collecting
  -> basis_warming
  -> basis_ready
  -> handover_warming
  -> basis_ready
```

状态语义：

- `no_basis`：没有 seed，且流式统计不足。
- `collecting`：正在积累有界 group 统计，只启用通用摘要。
- `basis_warming`：已形成初始 basis，但 stable head 相关摘要只学习，不高置信启用。
- `basis_ready`：basis-scoped 摘要可参与 routed rolling score trust。
- `handover_warming`：新 basis 候选通过 replacement cap，开始 warm-up handover。

### 6.2 有界统计

B4 不保存无界 group 历史。

内部维护 `RelationStreamBasisAccumulator`：

```text
per source_series_key + metric:
  valid_bucket_count
  total_mass
  bounded_group_stats[group_idx] = {
    estimated_mass,
    active_bucket_count,
    last_seen_bucket
  }
```

高基数 group 统计使用有界 heavy-hitter 近似，首版实现建议采用 Space-Saving / Misra-Gries 风格的固定容量表。

默认容量：

```text
basis_stats_max_groups = max(256, 4 * support_policy.k_support)
```

容量达到上限后，只保留质量贡献最高或持续活跃的 group 近似统计。被挤出的 group 不会单独建 rolling state，只会体现在 `out_of_support_share` 等摘要里。

### 6.3 近似统计到 builder 的保守契约

`RelationBasisBuilder` 当前按以下硬阈值筛选 group：

```text
hist_share = hist_mass / total_hist_mass
active_ratio = active_bucket_count / valid_bucket_count

hist_share >= support_policy.min_hist_share
active_ratio >= support_policy.min_active_ratio
```

stream basis accumulator 若使用 Space-Saving / Misra-Gries 近似统计，必须在进入 `BuildServiceBasis()` 前做保守过滤，避免接近阈值的 group 因估计误差反复进出 basis。

同时必须保持 `hist_share` 分母口径不变。当前 `RelationBasisBuilder` 会在内部对传入的 `group_stats.hist_mass` 重新求和作为 `total_hist_mass`，如果 B4 只把过滤后的 tracked group 传进去，分母会被缩小，`hist_share` 会被放大，`support_policy.min_hist_share` 的语义就会被破坏。

B4 采用以下口径：

- `RelationStreamBasisAccumulator.total_mass` 表示该 metric 在有效 bucket 内观察到的全部质量，包括 tracked、evicted 和 untracked group。
- 构造 `RelationBasisBuildInput` 时，candidate group 可以是保守过滤后的有界集合，但 denominator 必须使用全部 `total_mass`。
- 编码实现优先扩展 `RelationBasisBuildInput`，增加可选 `total_hist_mass_denominator` 或等价字段；当该字段 `> 0` 时，`RelationBasisBuilder` 用它计算 `hist_share`，否则保持 B1 历史训练的现有行为。
- 不允许只传过滤后的 candidate group 给 builder，并让 builder 用 candidate group 的 mass 重新当分母。

若实现阶段暂不扩展 builder，只能采用等价的 denominator 保真方案：把 evicted / untracked mass 作为内部 synthetic other 统计传入分母，并确保它不会进入 `support_explicit` / `stable_head` / 对外 basis；该方案需要专门测试，避免污染 `other_group_idxs` 的业务语义。

首版规则：

- 每个 group 记录 `estimated_mass` 和 `mass_error_upper_bound`。
- 计算候选时使用保守下界：

```text
mass_lower = max(0, estimated_mass - mass_error_upper_bound)
hist_share_lower = mass_lower / total_mass
```

- 只有 `hist_share_lower >= min_hist_share * basis_threshold_margin` 才允许进入 builder。
- `active_bucket_count` 必须是实计数；若 group 被近似表挤出后重新进入，active 计数不得伪造补齐。
- 对 `hist_share` 或 `active_ratio` 距阈值太近的 group，默认拒绝进入 candidate basis。
- 排序 tie-breaker 固定为：`hist_share` 降序、`active_ratio` 降序、`last_seen_bucket` 降序、`group_idx` 升序。

建议默认：

```text
basis_threshold_margin = 1.20
basis_min_stable_refresh_count = 2
```

`basis_min_stable_refresh_count` 表示同一 group 至少连续 2 次低频评估满足保守阈值，才允许进入 support / stable head candidate。这个规则优先保证 basis 稳定，代价是新 head group 进入会更慢。

### 6.4 低频刷新

低频刷新只生成 candidate basis，不直接覆盖 active basis。

建议默认值：

| 配置 | 默认值 | 说明 |
| --- | --- | --- |
| `basis_collect_min_buckets` | `day_buckets` | 至少观察 1 天才尝试首个 stream basis |
| `basis_ready_min_buckets` | `3 * day_buckets` | 至少观察 3 天才允许 stable head ready |
| `basis_refresh_interval_buckets` | `day_buckets` | 每 1 天最多评估一次 candidate |
| `basis_candidate_min_coverage_ratio` | `0.60` | 刷新窗口有效覆盖下限 |
| `basis_replacement_cap_ratio` | `0.20` | 单次最多替换 20% support / stable head |
| `basis_replacement_cap_max` | `2` | 单次最多替换 2 个 group |
| `basis_handover_warmup_buckets` | `day_buckets` | 新 basis-scoped routed state 至少 warm-up 1 天 |
| `basis_threshold_margin` | `1.20` | 近似统计进入 builder 的阈值安全边际 |
| `basis_min_stable_refresh_count` | `2` | 连续满足保守阈值的刷新次数 |

`day_buckets = 86400 / bucket_seconds`。

### 6.5 Handover 规则

candidate basis 只有满足以下条件才进入 handover：

- 达到 `basis_refresh_interval_buckets`。
- 统计覆盖达到 `basis_candidate_min_coverage_ratio`。
- support / stable head 变化不超过 replacement cap。
- candidate 的 `head_proto_q` 可计算。

进入 handover 后：

- universal summary 沿用原 routed state。
- basis-scoped summary 使用新 `basis_version` 创建新 routed state。
- 新 state 在 `basis_handover_warmup_buckets` 内只学习或低可信评分。
- 旧 basis-scoped state 保留到新 state ready 后再从 active snapshot 中隐藏。

这避免了旧方案类似 shadow 状态难退出的问题：handover 是有限窗口、有限状态，不依赖完整历史重建。

---

## 7. Bootstrap Seed 接入

B1 的 `BootstrapSeed` 在 Relation task 内部按 `source_series_key` 存储。B4 只在内部消费结构体，不走 JSON 导出再解析。

RelationTask 内部建议维护：

```text
BootstrapSeedStore seeds_by_series_                 # B1 relation seed，按 source_series_key
BootstrapSeedStore routed_seeds_by_series_          # 转换后的 value/ratio seed，按 routed_series_key
RollingStateMap routed_rolling_states_              # routed summary rolling state
RelationBasisStateMap basis_states_                 # source_series_key + metric -> basis runtime state
```

`routed_seeds_by_series_` 是内部派生缓存，不对外导出为独立 task。

### 7.1 Basis seed

`BootstrapSeed.relation_basis_by_metric` 初始化：

```text
RelationBasisRuntimeState.active_basis
RelationBasisRuntimeState.basis_version
RelationBasisRuntimeState.basis_status
RelationBasisRuntimeState.seed_source
```

若 seed_status 为 `full/partial`，basis 可从 `basis_warming` 或 `basis_ready` 起步；但不能绕过 B3 score trust。

若 seed_status 为 `weak/none`，basis 只作为候选提示，不直接启用 stable head 高置信摘要。

### 7.2 Routed summary seed

`BootstrapSeed.relation_routed_summary_seeds` 需要转换为普通 value / ratio `BootstrapSeed` 后交给 rolling core warm-up。

转换规则：

```text
source_series_key = parent BootstrapSeed.series_key
summary_scope = FixedSummaryScope(metric, summary, feature_type)
basis_version =
  summary_scope == universal
    ? 0
    : routed_seed.basis_version if present
      else parent active basis version for same metric
routed_series_key = MakeRoutedSeriesKey(source_series_key, metric, summary, feature_type, basis_version)
task_identity = routed_seed.task_identity
theta_init / sigma_init / uncertainty_init / maturity_init = routed_seed 对应字段
artifact_kind = value 或 ratio
```

注意：

- `RelationRoutedBootstrapSeed` 当前没有显式 `basis_version` / `basis_scoped` 字段。B4 首版必须用固定 summary 分类判断 scope，不能运行时猜测。
- 旧 seed 缺 `basis_version` 时，只能视为“初始 active basis”的 basis-scoped seed。若同 metric 找不到唯一 parent basis，则拒绝 warm-up，并记录 diagnostics。
- `stable_g_share_i` 必须绑定到 parent basis 的 stable head 顺序；缺 parent basis 或 stable head size 不匹配时拒绝 warm-up。
- 后续若修改 B1 seed 序列化，应给 `RelationRoutedBootstrapSeed` 显式增加 `basis_version` 和 `basis_scoped` 字段，并在 JSON 导出 / 导入中稳定写入。
- B4 不能把 routed seed 暴露成外部 task；它只是 RelationTask 内部的 rolling warm-up 输入。
- 初始化完成后，routed rolling state 与 bootstrap seed 解耦，在线更新不能反写 seed。

---

## 8. 热路径锁与 fan-out

B4 的 Relation submit 比 Value / Ratio submit 多一层 fan-out：一个 source block 会生成多个 routed observations，并可能触发 basis accumulator 更新和低频 refresh。若沿用一把 `BaselineTaskBase::mutex_` 包住完整过程，会放大 `baseline-locking-analysis.md` 中已经记录的 task 级锁问题。

### 8.1 锁边界

锁设计目标：

- `BaselineTaskBase::mutex_` 只保护 task 生命周期、artifact / seed store 的结构性替换、关闭状态。
- source ordered lock 使用 `source_series_key` 粒度，保证同一 source 的 bucket 顺序；basis state 内部仍按 `(source_series_key, metric)` 组织。
- routed rolling state 使用 routed shard lock，按 `Hash(routed_series_key) % shard_count` 分片。
- snapshot 不长时间持有热路径锁，优先复制轻量状态后序列化。
- 不允许在释放 task mutex 后继续使用 `BootstrapArtifactStore` / `BootstrapSeedStore` 内部元素指针。当前 store 是 by-value `unordered_map`，`LoadBootstrapArtifactStoreInternal()` 会整体替换 map，`StoreBootstrapArtifact()` 会 `insert_or_assign`，map 内部指针在解锁后可能悬空。

建议内部结构：

```text
RelationTaskRuntime:
  source_states[source_series_key]
  routed_shards[N] = {
    mutex
    routed_seeds_by_series
    routed_rolling_states
  }
```

默认 `N = 16`，后续可配置。

### 8.2 SubmitObservation 锁顺序

固定锁顺序，避免死锁：

```text
task mutex: EnsureOpen + 复制必要 seed/basis 数据
  -> release task mutex
source ordered lock: projection / routed fan-out / accumulator update / candidate decision
routed shard lock: per routed summary submit
```

task mutex 内只能做短操作，但 seed / basis 获取必须是安全快照：

- B4 MVP：在锁内按值复制当前 `BootstrapSeed`、`RelationServiceBasis`、`RelationRoutedBootstrapSeed` 等初始化必需字段，解锁后只使用这些副本。
- 后续优化可把 store 改成 immutable `shared_ptr` 快照并原子替换，但必须保证解锁后持有的是独立不可变对象，不是 map 内部裸指针。

同一 `source_series_key` 的 `SubmitObservation()` 必须保持 bucket 顺序。Rolling core 对单个 routed state 要求 `bucket_id > last_seen_bucket`，因此 source bucket 并发提交会带来交错风险：例如 bucket 101 的某个 routed summary 先更新后，bucket 100 再到会被 rolling core 拒绝。

B4 MVP 采用 per-source ordered lock：同一个 `source_series_key` 的以下步骤必须串行：

```text
validate / normalize block
  -> projection
  -> routed fan-out submit
  -> basis accumulator update
  -> candidate refresh decision
```

这个锁可以覆盖 fan-out，但只覆盖同一个 source series；不同 `source_series_key` 仍可并行。若后续要释放 source lock 再 fan-out，必须先实现 per-source ordered queue，由队列保证同一 source 的 bucket 严格按序执行。

低频 basis refresh 可以在 source ordered lock 内完成 candidate 判断，但不能在持有 source lock 时批量序列化 diagnostics 或长时间输出 JSON。

### 8.3 Snapshot copy 策略

- `QueryTaskSnapshot()` 复制计数、basis version、state size 等轻量字段后释放锁，再生成 JSON。
- `QuerySeriesSnapshot(source_series_key)` 只读取 source relation state 和 routed summary 索引，不直接序列化全部 routed rolling 内部状态。
- `QueryRoutedSummarySnapshot()` 单独锁定对应 routed shard，调用 rolling snapshot 逻辑。

### 8.4 Diagnostics

- `RelationRollingSubmitOptions.include_diagnostics` 默认 `false`。
- 默认结果只输出 status、basis status、basis version、routed summary identity 和 rolling 结果。
- 大字符串 diagnostics 只在测试、调试或错误场景开启。

---

## 9. Snapshot 与查询

### 9.1 Task snapshot

`QueryTaskSnapshot(JSON)` 在原有 task 信息外补充：

```json
{
  "relation_rolling": {
    "source_series_count": 1,
    "routed_state_count": 8,
    "basis_state_count": 1,
    "basis_stats_group_count": 128,
    "basis_handover_active_count": 0
  }
}
```

### 9.2 Series snapshot

`QuerySeriesSnapshot(source_series_key, JSON)` 返回 relation source 视图：

```json
{
  "document_kind": "relation_series_snapshot",
  "series_key": "linkA.client_mix",
  "basis_by_metric": [
    {
      "metric": "bps",
      "basis_version": 2,
      "basis_status": "basis_ready",
      "support_size": 16,
      "stable_head_size": 5,
      "stats_group_count": 128,
      "handover_active": false
    }
  ],
  "routed_summaries": [
    {
      "metric": "bps",
      "summary": "top1_share",
      "feature_type": "ratio",
      "routed_series_key": "linkA.client_mix::bps::top1_share::ratio",
      "basis_version": 0,
      "maturity_status": "weekly_ready",
      "score_trust_status": "score_ready"
    }
  ]
}
```

`QuerySeriesSnapshot()` 在 Relation task 中只接受 `source_series_key`，不再复用为 routed rolling snapshot 查询。若入参匹配 routed key，应返回 `kInvalidArgument`，并提示使用 `QueryRoutedSummarySnapshot()`。

### 9.3 Routed snapshot 查询

`QueryRoutedSummarySnapshot(query, JSON)` 根据 `RelationRoutedSummaryQuery` 构造 routed key，并返回底层 rolling snapshot。

查询规则：

- 通用摘要忽略 `query.basis_version`。
- basis-scoped 摘要且 `query.basis_version = 0` 时，使用当前 active basis version。
- basis-scoped 摘要且 `query.basis_version > 0` 时，精确查询该版本。
- 查不到对应 routed state 时返回 `kNotTrained`。

### 9.4 Forecast 查询

`PredictRoutedSummary(query, bucket_id)` 内部解析 `routed_series_key` 后调用：

```text
PredictRollingForSeries(routed_spec, routed_seed_store, routed_states, routed_series_key, bucket_id)
```

`PredictRoutedSummary()` 不对 Relation 分布整体做预测，只预测某个 routed summary 的未来中心线和 band。

---

## 10. 配置

B4 配置进入 `baseline-config-template.yaml` 的 `baseline.rolling_config.relation_rolling`。

建议结构：

```yaml
relation_rolling:
  enable_routed_rolling: true
  enable_stream_basis: true
  include_universal_summaries_without_basis: true
  basis_stats_max_groups: 256
  basis_collect_min_buckets: 0        # 0 表示 day_buckets
  basis_ready_min_buckets: 0          # 0 表示 3 * day_buckets
  basis_refresh_interval_buckets: 0   # 0 表示 day_buckets
  basis_candidate_min_coverage_ratio: 0.60
  basis_replacement_cap_ratio: 0.20
  basis_replacement_cap_max: 2
  basis_handover_warmup_buckets: 0    # 0 表示 day_buckets
  basis_threshold_margin: 1.20
  basis_min_stable_refresh_count: 2
  routed_state_shard_count: 16
```

配置原则：

- `support_policy` / `summary_policy` 仍属于 Relation task config。
- rolling 学习、score trust、band 校准继续使用 B2/B3 的 rolling 配置。
- B4 只新增 Relation routing 和 basis 成熟相关配置。
- 当前 `BaselineRollingConfig` 是平铺 struct，`ParseRollingConfig()` 和 strict schema 也是平铺白名单。B4-T09 必须补齐：
  - `BaselineRelationRollingConfig` 子结构或等价字段组。
  - `BaselineRollingConfig.relation_rolling` 字段。
  - `ParseRollingConfig()` 对 nested `relation_rolling` 的解析。
  - strict schema whitelist 对 `relation_rolling` 及其内部 key 的允许列表。
  - `baseline-config-template.yaml` 默认值。
  - strict schema 测试和默认值解析测试。

---

## 11. 实现任务顺序

| 任务 | 名称 | 设计引用 | 主要文件 | 完成标准 |
| --- | --- | --- | --- | --- |
| `B4-T01` | 补齐 Relation rolling public ABI | 第 3、9 节 | `ibaseline_types.h`、`ibaseline_service.h`、`relation_task.*` | Relation task 暴露 `SubmitObservation`、`PredictRoutedSummary`、`QueryRoutedSummarySnapshot` stub，编译通过 |
| `B4-T02` | 抽出 relation summary 投影模块 | 第 5.1、5.2、5.3 节 | `relation/relation_summary.*`、`bootstrap_engine.cpp` | B1 `TrainRelation` 改用共享投影逻辑；通用摘要不依赖 basis；原 bootstrap 测试通过 |
| `B4-T03` | 实现 routed identity 与 seed 转换 | 第 4、7 节 | `relation/routed_summary.*`、`relation_task.*` | summary scope 固定；basis-scoped seed 绑定 basis version；relation routed seed 可批量 warm-up 到内部 rolling state，不走 JSON |
| `B4-T04` | 实现 stream basis accumulator | 第 6.1、6.2、6.3 节 | `relation/relation_basis_state.*`、`relation/relation_basis.*` | 高基数 group 统计有固定容量、误差上界和保守阈值过滤；进入 builder 时 denominator 使用全部 observed mass，不被 candidate 过滤缩小 |
| `B4-T05` | 实现 basis 低频刷新与 handover | 第 6.4、6.5 节 | `relation/relation_basis_state.*` | replacement cap、warm-up handover、basis_version 推进、边界 group 防抖有测试 |
| `B4-T06` | 实现 RelationTask 锁边界与 fan-out 运行时 | 第 8 节 | `task/relation_task.*` | source/routed 分片锁、seed/basis 按值安全快照、per-source 有序提交、snapshot copy、diagnostics 开关落地 |
| `B4-T07` | 接入 RelationTask SubmitObservation | 第 5.4、5.5、5.6、7 节 | `task/relation_task.*` | 无历史可提交通用摘要；ratio 按 `share * total / total` 提交；有 seed 可启用 basis-scoped 摘要；basis 未 ready 时不高置信告警 |
| `B4-T08` | 接入 snapshot 与 routed forecast | 第 9 节 | `relation_task.*`、`rolling_task_runner.*` 可选 | source snapshot、dedicated routed snapshot 和 forecast 字段稳定，forecast 只读 |
| `B4-T09` | 补齐配置模板和解析 | 第 10 节 | `rolling_config.*`、`runtime_config.*`、`baseline-config-template.yaml` | struct、parser、strict whitelist、模板、strict schema 测试全部覆盖 |
| `B4-T10` | 自动化测试与回归验证 | 全文 | `src/tests/test_baseline/*` | 覆盖无历史、有 seed、basis 刷新、handover、snapshot、无旧 rebuild |
| `B4-T11` | 闭环 B4 设计细节 | 第 6.3、8、9、10 节 | `relation_basis_state.*`、`relation_task.*`、`test_baseline_*` | 配置开关控制实际行为；`routed_state_shard_count` 不再固定；`basis_min_stable_refresh_count` 落地连续满足计数；source snapshot 输出最小 schema；routed key 误用 `QuerySeriesSnapshot()` 返回 `kInvalidArgument`；source ordered lock 采用可配置 striped lock，保证同 source 有序且锁数量有上界 |

---

## 12. 测试矩阵

| 场景 | 预期 |
| --- | --- |
| 无历史首个 Relation block | 通用摘要 routed rolling 可空启动，summary 提取不依赖 `RelationServiceBasis`，basis 状态为 `collecting` |
| basis 未成熟 | stable head 摘要可以学习，但 `can_alert = false`，diagnostics 可显示 `relation_basis_not_ready` |
| 有 B1 basis seed + routed summary seed | RelationTask 首次 submit 前内部 warm-up，routed rolling state 使用 seed 初始化；旧 seed 缺 basis version 时绑定初始 active basis |
| `stable_g_share_i` seed 缺 parent basis | 拒绝 warm-up，不复用到错误 stable head 顺序 |
| ratio summary 提交 | `numerator = share * total`、`denominator = total`，低分母可靠性与 B2/B3 ratio 规则一致 |
| 高基数 group 流 | `basis_stats_group_count` 不超过配置上限，边界 group 不因近似误差频繁进出 basis |
| basis builder denominator | 过滤 candidate group 后，`hist_share` 分母仍使用全部 observed mass；不会因只传候选 group 放大占比 |
| support 发生小幅变化 | candidate basis 通过 replacement cap，进入有限 handover |
| support 发生大幅变化 | candidate basis 被拒绝或降级，不直接破坏 active basis |
| stable head 未成熟 | `stable_g_share_i`、`stable_headk_mix_drift` 不输出高置信异常 |
| basis 切换后 | basis-scoped routed key 带新 `basis_version`，旧 state 不污染新语义 |
| `PredictRoutedSummary` | 只读预测，不触发 lazy init，不更新 state |
| `QuerySeriesSnapshot(source_series_key)` | 只返回 Relation source 视图 |
| `QueryRoutedSummarySnapshot(query)` | 返回指定 routed summary 的 rolling snapshot，`basis_version = 0` 表示 active basis |
| strict schema | `relation_rolling` nested 配置被 parser 和 whitelist 接受 |
| seed / artifact store 替换 | 解锁后不使用 map 内部裸指针；Load / Store 替换不会造成悬空访问 |
| 同 source 并发提交 | 同一 `source_series_key` 的 bucket 按序完成 projection、fan-out 和 accumulator update，不会让 routed rolling state 先见到后续 bucket |
| 旧链路检查 | Relation rolling 不调用 `HistoryReader.fetch`、`shadow/candidate/rebuild` |

---

## 13. 完成门禁

B4 完成必须满足：

- Relation 流式 block 无历史可启动。
- 通用摘要和 basis-scoped 摘要边界清晰。
- routed summary 全部复用 B2/B3 rolling core。
- basis 在线统计有固定上限。
- stream basis 进入 builder 时，`hist_share` 分母使用完整 observed mass，不因 candidate 过滤而放大 group 份额。
- basis 刷新有 `basis_version`、replacement cap 和 warm-up handover。
- routed seed 的 `basis_scoped` / `basis_version` 口径稳定，旧 seed 缺字段时不会错误复用。
- Relation fan-out 不使用一把 task 级锁包住完整热路径。
- 解锁后不持有 bootstrap store 内部裸指针；seed / basis 快照生命周期安全。
- 同一 `source_series_key` 的流式提交有顺序保证。
- snapshot 能观察 source series、routed summary、basis maturity、score trust，且 source snapshot 与 routed snapshot 查询入口不混用。
- 没有恢复旧 rebuild 链路。

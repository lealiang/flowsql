# B5 Relation Pattern Fusion and Risk Output 阶段设计

## 1. 阶段定位

B5 承接 B4 的 Relation routed summary rolling 结果，补齐 Sprint 19 中 `T3 v1` 的模式融合能力。

B4 已解决：

```text
Relation block
  -> routed summary observations
  -> T1 / T2 Online Rolling Core
  -> RollingBaselineResult
```

B5 解决：

```text
routed summary RollingBaselineResult
  -> fusion evidence
  -> Relation local pattern score
  -> Relation risk output
```

B5 不重新训练时间序列模型，不替代 B4 的 routed summary rolling state。它只在同一个 Relation source 内，把多个摘要特征的数学异常证据组织成结构模式解释。

### 1.1 上游依赖

| 依赖 | B5 消费内容 | 约束 |
| --- | --- | --- |
| B1 Optional Bootstrap | Relation basis seed、routed summary seed、artifact / seed JSON | 仅补齐 fusion metadata，不训练 fusion 模型 |
| B2 Online Rolling Core | `RollingBaselineResult` 的 band、`z_score`、`can_score`、update 语义 | B5 不反向修改 rolling state |
| B3 Detection Trust | `maturity_status`、`score_trust_status`、`effective_confidence`、`can_alert` | 未可信证据必须降权或不可用 |
| B4 Relation Routed Rolling | `RelationRollingResult.routed_results`、`basis_version`、summary identity、source snapshot | B5 只消费 B4 输出，不恢复旧 rebuild 链路 |
| Sprint 19 设计 | `T3 v1` 摘要模式库和融合公式 | 只落地 Relation 内部融合，不实现全局 `Risk(Key,t)` |

### 1.2 非目标

B5 不实现：

- 全局 `Risk(Key,t)` 统一融合引擎。
- Value / Ratio / Relation 跨类型风险合成。
- 攻击、割接、上线、专家规则等业务语义判别。
- 新的 Relation 时间序列模型。
- 旧 `shadow/candidate/rebuild`、`HistoryReader.fetch` 或正式重建。
- Fusion 结果反向写入 routed rolling state、Relation basis 或 bootstrap seed 主路径。
- 每个 group、peer 或关系对的无界状态。

---

## 2. 当前代码基线

| 位置 | 当前能力 | B5 影响 |
| --- | --- | --- |
| `framework/interfaces/ibaseline_types.h` | 已有 `RollingBaselineResult`、`RelationRoutedSummaryResult`、`RelationRollingResult` | 需要新增 fusion 输出 public 类型，并把结果挂到 Relation submit result |
| `framework/interfaces/ibaseline_service.h` | `IBaselineRelationTask` 暴露 `SubmitObservation`、`PredictRoutedSummary`、`QueryRoutedSummarySnapshot` | B5 首版不新增查询接口，source snapshot 输出最近 fusion 结果 |
| `plugins/baseline/task/relation_task.*` | Relation block fan-out 到 routed summary rolling，按 source 有序提交 | B5 在 fan-out 后执行 fusion update，并保存 source 级 last fusion result |
| `plugins/baseline/relation/relation_summary.*` | 摘要投影 | B5 复用 summary 名称，不改摘要数值算法；需要透传 `distinct_group_count` 来源标记 |
| `plugins/baseline/relation/routed_summary.*` | routed key / seed materialization | B5 复用 identity 和 basis-scoped 判断 |
| `plugins/baseline/bootstrap/bootstrap_types.h` | `BootstrapSeed` / `BootstrapArtifact` 包含 relation basis 和 routed summary seed | 需要新增 relation fusion metadata |
| `plugins/baseline/rolling/rolling_config.*` | `BaselineRelationRollingConfig` 已包含 B4 配置 | 需要扩展 Relation fusion 配置 |

当前缺口：

- 没有把 `RollingBaselineResult` 标准化为 Relation fusion evidence 的模块。
- 没有维护 routed summary 的 fusion persistence。
- 没有 Sprint 19 的 `support_escape`、`head_concentration`、`legacy_head_dilution`、`stable_head_mix_shift` 模式计算。
- source snapshot 只能看到 basis 和 routed summary，不能看到 Relation 层结构风险。
- bootstrap artifact / seed 不记录哪些 summary / pattern 可用于 fusion。

---

## 3. Public ABI 改造

### 3.1 新增 public 类型

在 `framework/interfaces/ibaseline_types.h` 中新增以下结构。所有枚举型语义首版使用字符串，避免 public ABI 反复扩 enum。

```cpp
struct RelationFusionSingleEvidence {
    std::string source_series_key;
    std::string routed_series_key;
    std::string feature_base;
    std::string metric;
    std::string summary;
    std::string feature_type;
    uint64_t basis_version = 0;
    bool basis_scoped = false;

    std::string direction;          // "up" / "down" / "none"
    double normalized_score = 0.0;  // [0, 1]
    double confidence = 0.0;        // fusion confidence after B3 / B4 gate
    uint32_t persistence = 0;
    double evidence_strength = 0.0; // normalized_score * confidence * persistence_factor * trust_factor

    bool available = false;
    bool can_alert = false;
    std::string score_trust_status;
    std::string metric_basis_status;
    std::string unavailable_reason;
};

struct RelationFusionPatternScore {
    std::string source_series_key;
    std::string feature_base;
    std::string pattern;
    double score = 0.0;
    double weighted_score = 0.0;
    double pattern_weight = 1.0;
    std::vector<std::string> metrics_hit;
    std::vector<std::string> supporting_features;
    std::string diagnostics;
};

struct RelationFusionResult {
    BaselineStatus status = BaselineStatus::kOk;
    std::string source_series_key;
    std::string feature_base;
    int64_t bucket_id = 0;

    double relation_risk = 0.0;
    double single_risk = 0.0;
    double pattern_risk = 0.0;

    std::vector<RelationFusionSingleEvidence> dominant_single;
    std::vector<RelationFusionPatternScore> dominant_pattern;
    std::vector<RelationFusionPatternScore> pattern_scores;
    std::string diagnostics;
};
```

字段语义：

- `RelationFusionSingleEvidence` 是源 `RollingBaselineResult` 的轻量投影，不复制完整 rolling 结果。
- `RelationFusionSingleEvidence.confidence` 是 fusion 层实际使用的置信度，不等同于 rolling public `confidence` 字段。
- `metric_basis_status` 表示该 evidence 所属 `(source_series_key, metric, basis_version)` 的 basis 状态，不得复用 `RelationRollingResult.basis_status` 顶层字段。
- `evidence_strength` 是 fusion 层实际使用的有效证据强度。
- `available = false` 表示当前 bucket 该 evidence 不参与模式计算。
- `relation_risk` 是 Relation source 内部风险，不等价于全局 `Risk(Key,t)`。
- `dominant_single` 最多 3 个，`dominant_pattern` 最多 2 个。
- `pattern_scores` 输出当前 bucket 可计算的模式分，便于测试和观测。

### 3.2 修改现有 public 类型

扩展 `RelationRollingSubmitOptions`：

```cpp
struct RelationRollingSubmitOptions {
    RollingSubmitOptions routed_options;
    bool allow_basis_update = true;
    bool include_routed_results = true;
    bool include_diagnostics = false;
    bool include_fusion_result = true;
};
```

扩展 `RelationRollingResult`：

```cpp
struct RelationRollingResult {
    BaselineStatus status = BaselineStatus::kOk;
    std::string series_key;
    int64_t bucket_id = 0;
    uint64_t basis_version = 0;
    std::string basis_status;
    bool basis_updated = false;
    bool handover_active = false;
    std::vector<RelationRoutedSummaryResult> routed_results;
    bool has_fusion_result = false;
    RelationFusionResult fusion_result;
    std::string diagnostics;
};
```

设计约束：

- `include_routed_results = false` 不能导致 fusion 无法计算。Relation task 内部仍应收集轻量 evidence 输入，只是不把完整 routed results 返回给调用者。
- `include_fusion_result = false` 只影响 submit 返回值，不关闭内部 fusion state 和 source snapshot 更新。
- B5 首版不新增 `IBaselineRelationTask` 方法；后续若需要历史 fusion 查询，再单独设计 dedicated query。

---

## 4. 身份规则

B5 必须继续沿用 B4 的 3 层身份：

| 身份 | 示例 | B5 用途 |
| --- | --- | --- |
| `source_series_key` | `linkA.client_mix` | Relation source 级融合状态 |
| `summary_identity` | `bps.top1_share.ratio` | 单 evidence 标识 |
| `routed_series_key` | `linkA.client_mix::bps::top1_share::ratio` | 回溯底层 rolling result |

B5 新增 2 个融合身份：

```text
fusion_bundle_key =
  source_series_key + "::" + feature_base + "::" + metric

fusion_pattern_key =
  source_series_key + "::" + feature_base + "::" + pattern
```

其中：

- `feature_base` 来自 `RelationTaskCreateSpec.task_spec.feature_base`。
- 局部模式融合只在同一个 `fusion_bundle_key` 内进行。
- 跨 metric 合成只在同一个 `fusion_pattern_key` 内进行。
- basis-scoped summary 的 evidence identity 必须包含 `basis_version`，保证 basis 切换后 persistence 自动重新开始。

Evidence identity：

```text
universal evidence:
  source_series_key + "::" + feature_base + "::" + metric + "::" + summary

basis-scoped evidence:
  source_series_key + "::" + feature_base + "::" + metric + "::" + summary
    + "::basis:" + basis_version
```

### 4.1 Summary 到模式证据的固定映射

| summary | direction | 证据符号 | 参与模式 |
| --- | --- | --- | --- |
| `out_of_support_share` | `up` | `a_out_of_support_share^up` | `support_escape` core，`legacy_head_dilution` support，`head_concentration` oppose，`stable_head_mix_shift` oppose |
| `entropy_shannon` | `up` | `a_entropy_shannon^up` | `support_escape` support，`legacy_head_dilution` support，`head_concentration` oppose |
| `entropy_shannon` | `down` | `a_entropy_shannon^down` | `head_concentration` support，`support_escape` oppose |
| `distinct_group_count` | `up` | `a_distinct_group_count^up` | `support_escape` support |
| `top1_share` | `up` | `a_top1_share^up` | `head_concentration` core，`support_escape` oppose |
| `headk_share` | `up` | `a_headk_share^up` | `head_concentration` support，`support_escape` oppose |
| `stable_headk_coverage` | `down` | `a_stable_headk_coverage^down` | `legacy_head_dilution` core，`support_escape` support |
| `stable_headk_coverage` | `up` | `a_stable_headk_coverage^up` | `legacy_head_dilution` oppose |
| `stable_headk_mix_drift` | `up` | `a_stable_headk_mix_drift^up` | `stable_head_mix_shift` core |

`stable_g_share_i` 首版只作为 dominant single 候选，不直接进入 4 个模式公式。

`distinct_group_count` 是可选增强 evidence。只有当当前 `RelationBootstrapMetric.active_count` 来自上游明确统计口径时，它才能进入 fusion；如果 B4 只是用当前可见 group 数回退生成该 summary，B5 必须把它标记为 unavailable，`unavailable_reason = distinct_group_count_untrusted`。实现上必须由投影层或 Relation task 热路径透传 `active_count_from_upstream = (metric.active_count > 0)`，不得在 fusion 层重新用 `nnz` 推断。

---

## 5. Fusion Evidence 标准化

### 5.1 输入

B5 内部模块不直接消费原始 Relation block，而是消费 B4 fan-out 后的 routed result 和每个 task metric 的当前 bucket 状态。

投影层为 `distinct_group_count` 增加来源标记：

```cpp
struct RelationProjectedSummary {
    ...
    bool active_count_from_upstream = false;
};
```

规则：

- `summary_name == "distinct_group_count"` 时，`active_count_from_upstream = metric.active_count > 0`。
- 其他 summary 该字段保持 `false`，fusion 层不得把它解释为 summary 可用性。
- 该改动只透传 provenance，不改变 B4 摘要数值计算规则。

Relation task 需要为 fusion 收集 metric 级上下文：

```cpp
struct RelationFusionMetricContext {
    std::string metric;
    bool present = false;
    bool valid = false;
    std::string unavailable_reason; // "metric_missing" / "metric_invalid"
};
```

routed result 输入：

```cpp
struct RelationFusionRoutedInput {
    RelationRoutedSummaryResult routed;
    std::string feature_base;
    std::string metric_basis_status;
    bool active_count_from_upstream = false;
};
```

`metric_basis_status` 由 `BaselineRelationTask` 在 submit 热路径中注入，必须绑定当前 routed summary 所在的 `(source_series_key, metric, basis_version)`，不能从 `RelationRollingResult.basis_status` 顶层字段回读。不要让 fusion 模块重新读取 `basis_states_`。

`active_count_from_upstream` 只对 `summary = distinct_group_count` 有意义：

- `true`：当前 metric 的 `active_count` 由上游显式提供，fusion 可以使用该 evidence。
- `false`：当前 `distinct_group_count` 来自 B4 可见 group 计数回退，fusion 必须视为 unavailable。
- 该字段从 `RelationProjectedSummary.active_count_from_upstream` 复制；如果实现选择不改投影结构，也必须在 `BaselineRelationTask` 用同一规则显式填充，并补测试锁定。

### 5.2 Expected Evidence Universe

B5 不能只遍历当前 bucket 实际返回的 routed result。部分 summary 可能因为 basis 未形成、`stable_headk_mix_drift` 条件不满足、metric 缺测、metric 非法或投影降级而没有出现在 `routed_results` 中。如果不显式处理缺测，旧 persistence 会被错误继承。

每个 `source_series_key`、`bucket_id` 都必须先构造 expected evidence universe：

```text
ExpectedEvidence(source_series_key, feature_base, bucket_id)
  = task metrics
  × enabled summary set
  × required direction set
  × active basis scope
```

构造规则：

- 外层必须以 task spec 的 metric 列表为准，而不是以当前 bucket 成功投影出的 summaries 为准。
- metric 缺失或非法时，生成该 metric 下 expected evidence 的 unavailable 事件，重置对应 persistence；不调用 routed rolling，不参与 pattern 输入。
- 通用摘要：`entropy_shannon`、`top1_share`、`headk_share` 在 metric 有效时进入 expected set。
- `distinct_group_count`：只有 `active_count_from_upstream = true` 时进入可用 expected set；否则仍可记录 unavailable reason，但不得参与 persistence 和 pattern。
- basis-scoped 摘要：只有当前 metric 有 active basis 时进入 expected set，identity 必须包含 `basis_version`。
- `stable_headk_mix_drift`：只有 active basis 的 `stable_head.size() >= 2` 且 `head_proto_q` 完整时进入 expected set。
- `stable_g_share_i`：按 active basis 的 `stable_head` 数量生成 expected set，但首版只参与 `dominant_single`。
- Bootstrap fusion metadata 可作为 expected set 的可计算性提示；缺失 metadata 时，必须按 task config、summary 固定分类和 active basis 现场生成默认 expected set。

处理规则：

- expected set 中实际有 routed result 的项，按第 5.3、5.4、5.5 节计算。
- expected set 中没有 routed result 的项，当前方向 persistence 必须归零，并输出 `unavailable_reason = summary_missing` 或更具体原因。
- metric 级缺失优先于 summary 级缺失：当前 metric 缺失时使用 `metric_missing`，metric 非法时使用 `metric_invalid`，不要降级成笼统的 `summary_missing`。
- 不在 expected set 中的旧 evidence identity，如果属于旧 `basis_version`，不得参与当前 bucket 计算；其 persistence 可延迟清理，但不能被读取。

### 5.3 Score 归一化

`RollingBaselineResult` 当前没有 public `normalized_score` 字段。B5 内部按以下规则生成：

```text
abs_z = abs(rolling.z_score)
normalized_score = clamp(abs_z / fusion_z_score_cap, 0, 1)
```

默认：

```text
fusion_z_score_cap = 5.0
```

方向：

```text
direction =
  "up"   if rolling.z_score >  eps
  "down" if rolling.z_score < -eps
  "none" otherwise
```

默认：

```text
fusion_z_epsilon = 1.0e-9
```

说明：

- `up / down` 只表达数学偏移方向，不表达业务好坏。
- `normalized_score` 不要求观测值已经穿出 band；弱偏移可以进入诊断，但会被 confidence、persistence 和 trust gate 压低。

### 5.4 Persistence

B5 在 Relation source 内维护 evidence persistence：

```text
PersistenceState[
  source_series_key,
  feature_base,
  metric,
  summary,
  direction,
  basis_version?
]
```

更新规则：

- 当前 evidence `available = true` 且 `normalized_score >= fusion_min_evidence_score` 时，对应方向 persistence 加 1。
- 当前 evidence 不可用、方向为 `none`、或未达到最小证据分时，对应方向 persistence 归零。
- expected set 中当前 bucket 缺失的 evidence，对应方向 persistence 归零。
- `bucket_id` 出现 gap 时，当前 source 的 persistence 全部归零。
- basis-scoped summary 的 key 包含 `basis_version`，basis 切换后不会继承旧 persistence。

默认：

```text
fusion_min_evidence_score = 0.20
fusion_persistence_window = 2
```

归一化：

```text
persistence_factor = min(1, persistence / fusion_persistence_window)
```

### 5.5 Trust Gate

对单条 routed summary，先判定 availability：

```text
available = rolling.status == kOk
         && rolling.can_score
         && direction != "none"
         && normalized_score > 0
         && !(summary == "distinct_group_count" && !active_count_from_upstream)
```

再计算 trust factor：

```text
trust_factor = 0                         if !available
trust_factor = 0                         if rolling.score_trust_status == "score_untrusted"
trust_factor = 0                         if basis_scoped && metric_basis_status != "basis_ready"
trust_factor = 1.0                       if rolling.can_alert && score_trust_status == "score_ready"
trust_factor = fusion_warming_weight     if score_trust_status == "score_warming"
trust_factor = fusion_degraded_weight    if score_trust_status in ["drift_learning", "recalibrating"]
trust_factor = 0                         otherwise
```

默认：

```text
fusion_warming_weight = 0.25
fusion_degraded_weight = 0.25
```

最终 evidence strength：

```text
fusion_confidence = clamp(rolling.effective_confidence, 0, 1)
    if score_trust_status in ["score_ready", "score_warming"]

fusion_confidence = clamp(rolling.learning_confidence, 0, 1)
    if score_trust_status in ["drift_learning", "recalibrating"]

fusion_confidence = 0
    otherwise

a_f = normalized_score * fusion_confidence * persistence_factor * trust_factor
```

设计意图：

- 冷启动、basis 未成熟、`score_untrusted` 不产生高置信 relation risk。
- `score_warming` 使用 B3 的 `effective_confidence`，只能按 `fusion_warming_weight` 低权重参与。
- `drift_learning` / `recalibrating` 当前 rolling 会把 `effective_confidence` 置为 0；B5 若要保留低强度诊断证据，必须显式使用 `learning_confidence`，并继续受 `fusion_degraded_weight` 限制。
- `score_untrusted` 始终不可用，不得通过 `rolling.confidence` fallback 恢复证据强度。
- B4 的 `can_alert = false` 仍然会通过 `score_trust_status` 和 `metric_basis_status` 降权；只有 `score_ready` 分支要求 `rolling.can_alert = true`。
- `distinct_group_count` 缺少上游真实 `active_count` 时，不进入 fusion pattern，避免把 `topk` 可见数量误当成真实活跃广度。

---

## 6. 模式融合算法

### 6.1 局部模式单元

局部模式单元：

```text
Bundle(source_series_key, feature_base, metric, bucket_id)
```

只有同一个 bundle 内的 summary evidence 可以互相组合。不同 `metric` 不在本层直接混合。

辅助函数：

```text
GeomMean(x_1, ..., x_n) = (Π_i x_i)^(1/n)
Top2Mean(values) = 按值降序取前 2 个正项的算术平均
```

边界：

- required evidence absent 时按 0 处理。
- optional evidence absent 时不进入 `Top2Mean`。
- oppose evidence absent 时按 0 处理。

通式：

```text
core_P    = AggCore(required evidence)
support_P = AggSup(optional evidence)
oppose_P  = AggOpp(contradict evidence)

score_P = clip(core_P + λ_sup * support_P - λ_opp * oppose_P, 0, 1)
```

默认：

```text
λ_sup = 0.5
λ_opp = 0.5
```

### 6.2 `support_escape`

语义：当前质量显著逃离历史显式支持集，并伴随整体扩散或历史头部受挤压。

```text
core = a_out_of_support_share^up

support = Top2Mean(
  a_entropy_shannon^up,
  a_distinct_group_count^up,
  a_stable_headk_coverage^down
)

oppose = Top2Mean(
  a_top1_share^up,
  a_headk_share^up,
  a_entropy_shannon^down
)

score_support_escape =
  clip(core + 0.5 * support - 0.5 * oppose, 0, 1)
```

### 6.3 `head_concentration`

语义：当前质量显著集中到单一或少数 group。

```text
core = a_top1_share^up

support = Top2Mean(
  a_headk_share^up,
  a_entropy_shannon^down
)

oppose = Top2Mean(
  a_out_of_support_share^up,
  a_entropy_shannon^up
)

score_head_concentration =
  clip(core + 0.5 * support - 0.5 * oppose, 0, 1)
```

### 6.4 `legacy_head_dilution`

语义：历史稳定头部整体失去份额，不再主导当前结构。

```text
core = a_stable_headk_coverage^down

support = Top2Mean(
  a_out_of_support_share^up,
  a_entropy_shannon^up
)

oppose = a_stable_headk_coverage^up

score_legacy_head_dilution =
  clip(core + 0.5 * support - 0.5 * oppose, 0, 1)
```

### 6.5 `stable_head_mix_shift`

语义：历史固定头部整体仍在场，但内部比例关系显著重排。

```text
core = a_stable_headk_mix_drift^up

support = 0

oppose = Top2Mean(
  a_stable_headk_coverage^down,
  a_out_of_support_share^up
)

score_stable_head_mix_shift =
  clip(core + 0.5 * support - 0.5 * oppose, 0, 1)
```

### 6.6 跨 metric 合成

同一个 pattern 可以在多个 metric 上出现。对同一 `(source_series_key, feature_base, pattern)`：

```text
M_valid(P, t) = { metric | local score_P(metric, t) 可计算 }

ScorePattern(source_series_key, feature_base, P, t)
  = 1 - Π_{m in M_valid(P,t)} (1 - score_P^(m))
```

边界：

- `M_valid` 为空时，不输出该 pattern 的有效 score。
- 只有 1 个 metric 有效时，退化为该 metric 的 local score。
- 2 个及以上 metric 有效时，按饱和型公式提级，不做线性相加。

### 6.7 Relation 风险合成

B5 只输出 Relation 内部风险：

```text
RelationRisk(source_series_key, t)
```

它拆成单特征基础风险和模式风险：

```text
F_single_top =
  当前 bucket 内 evidence_strength 最高的前 K 个 available single evidence

single_risk =
  1 - Π_{f in F_single_top} (1 - a_f)

pattern_risk =
  1 - Π_P (1 - λ_P * ScorePattern(P, t))

relation_risk =
  1 - (1 - single_risk) * (1 - pattern_risk)
```

默认：

```text
K = 3

λ_P =
  0.70 for support_escape
  0.70 for head_concentration
  0.85 for legacy_head_dilution
  0.85 for stable_head_mix_shift
```

说明：

- `F_single_top` 使用 top-K，而不是所有 single evidence 无界相乘，避免 summary 数量或 `stable_g_share_i` 数量导致风险机械升高。
- pattern 负责表达多个 summary 对同一结构模式的一致证据。
- `relation_risk` 不带业务 severity。若未来需要分档，新增 `risk_level`，不要复用 rolling 层 severity。

---

## 7. Runtime State 与热路径

### 7.1 状态模型

新增内部模块：

```text
plugins/baseline/relation/relation_fusion.h
plugins/baseline/relation/relation_fusion.cpp
```

核心内部结构：

```cpp
struct RelationFusionRuntimeConfig {
    bool enable_relation_fusion = true;
    double fusion_z_score_cap = 5.0;
    double fusion_min_evidence_score = 0.20;
    uint32_t fusion_persistence_window = 2;
    double fusion_warming_weight = 0.25;
    double fusion_degraded_weight = 0.25;
    double fusion_support_weight = 0.5;
    double fusion_oppose_weight = 0.5;
    double basic_pattern_weight = 0.70;
    double stable_head_pattern_weight = 0.85;
    uint32_t dominant_single_cap = 3;
    uint32_t dominant_pattern_cap = 2;
};

struct RelationFusionRuntimeState {
    int64_t last_bucket_id = 0;
    bool has_last_bucket = false;
    std::unordered_map<std::string, uint32_t> persistence_by_evidence_dir;
    RelationFusionResult last_result;
};
```

`BaselineRelationTask` 新增：

```cpp
mutable std::mutex fusion_states_mutex_;
std::unordered_map<std::string, RelationFusionRuntimeState> fusion_states_;
```

key 为 `source_series_key`。状态大小上界：

```text
O(source_series_count * metric_count * summary_count * direction_count)
```

不随 group 数增长。

### 7.2 SubmitObservation 顺序

B5 集成在 B4 fan-out 之后：

```text
source ordered lock
  -> for each metric:
       read active basis snapshot
       collect RelationFusionMetricContext
       project summaries
       submit each routed summary to rolling core
       collect RelationFusionRoutedInput
       collect expected evidence metadata
       optionally update basis accumulator
  -> build expected evidence universe
  -> update relation fusion state
  -> fill RelationRollingResult.fusion_result if requested
```

锁边界：

- 更新 fusion state 前，不能持有 routed shard lock。
- fusion update 可在 source ordered lock 内执行，保证同一 source bucket 顺序。
- `fusion_states_mutex_` 只保护小状态，不包住 rolling submit 或 basis update。
- snapshot 读取时只复制 `last_result`，不长时间持锁。

同一调用链需要连续获取多个锁时，锁顺序固定为：

```text
mutex_                         // lifecycle / open 状态检查
  -> source_ordered_locks_      // 同一 source submit 顺序
  -> basis_states_mutex_        // 短持有，读取或更新 basis runtime
  -> routed shard mutex         // 短持有，提交 routed rolling
  -> fusion_states_mutex_       // leaf lock，只更新 / 复制 fusion state
```

约束：

- 不允许在持有 `fusion_states_mutex_` 时再获取 `mutex_`、`basis_states_mutex_` 或 routed shard mutex。
- Submit 必须在释放 routed shard mutex 后再更新 fusion state。
- Bootstrap / Load 在持有 `mutex_` 时可以清空 `fusion_states_`，但清空逻辑不得反向调用 snapshot、routed 查询或 basis 查询。
- Snapshot 读取 fusion 时只复制 `last_result` 和计数，复制后释放锁再写 JSON。

### 7.3 状态生命周期

Fusion runtime state 是在线派生状态，必须跟随 Relation runtime 重建一起清理。

清理规则：

- `Bootstrap()` 成功并调用 `RebuildRuntimeFromRelationSeedsLocked()` 后，必须清空 `fusion_states_`。
- `LoadBootstrapArtifact()` 成功并调用 `RebuildRuntimeFromRelationSeedsLocked()` 后，必须清空 `fusion_states_`。
- Relation task `Close()` 后不得再返回旧 `last_result`。
- `enable_relation_fusion` 从 `true` 改为 `false` 后，snapshot 可以显示 `enabled = false`，但不得继续输出旧 `relation_risk` 作为当前结果。

这样可以避免 artifact / seed 替换后，旧 basis、旧 routed seed 或旧 pattern persistence 污染新运行期。

### 7.4 错误与降级

| 场景 | 行为 |
| --- | --- |
| `enable_relation_fusion = false` | 不更新 fusion state；submit result `has_fusion_result = false` |
| B4 routed rolling disabled | fusion 不计算，diagnostics 可记录 `relation_fusion_no_routed_results` |
| 当前 bucket 无 available evidence | 输出 `relation_risk = 0`，`status = kOk`，diagnostics 记录 `relation_fusion_no_available_evidence` |
| source bucket gap | 清空该 source persistence 后再计算当前 bucket |
| 非递增 bucket | 不更新 fusion state，返回上层已有 status；diagnostics 记录 `relation_fusion_stale_bucket` |
| metric 缺失 | 该 metric 下 expected evidence persistence 归零，`unavailable_reason = metric_missing` |
| metric 非法 | 该 metric 下 expected evidence persistence 归零，`unavailable_reason = metric_invalid` |
| basis-scoped summary basis 未 ready | evidence `available = false`，`unavailable_reason = relation_basis_not_ready` |
| expected summary 当前 bucket 缺失 | 对应 persistence 归零，`unavailable_reason = summary_missing` |
| `distinct_group_count` 缺少上游 `active_count` | evidence `available = false`，`unavailable_reason = distinct_group_count_untrusted` |
| `score_untrusted` | evidence `available = false`，`unavailable_reason = score_untrusted` |
| `score_warming` | evidence 可低权重参与诊断和 pattern，但不能单独形成高风险 |
| `drift_learning` / `recalibrating` | evidence 可用 `learning_confidence * fusion_degraded_weight` 形成低强度诊断证据 |

---

## 8. Bootstrap Fusion Metadata

B5 在 Bootstrap artifact / seed 中增加 metadata，而不是增加 fusion 模型。

新增内部类型建议放在 `plugins/baseline/bootstrap/bootstrap_types.h`：

```cpp
struct RelationFusionSummaryMetadata {
    std::string metric_name;
    std::string summary_name;
    BaselineTaskKind task_kind = BaselineTaskKind::kValue;
    bool basis_scoped = false;
    uint64_t basis_version = 0;
};

struct RelationFusionPatternMetadata {
    std::string pattern;
    std::string scope; // "relation_local"
    double pattern_weight = 1.0;
    std::vector<std::string> required_summaries;
    std::vector<std::string> optional_summaries;
    std::vector<std::string> oppose_summaries;
    std::vector<std::string> metrics;
};

struct RelationFusionBootstrapMetadata {
    uint32_t metadata_version = 1;
    std::string feature_base;
    std::vector<RelationFusionSummaryMetadata> summary_metadata;
    std::vector<RelationFusionPatternMetadata> pattern_metadata;
};
```

扩展：

```cpp
struct BootstrapSeed {
    ...
    RelationFusionBootstrapMetadata relation_fusion_metadata;
};

struct BootstrapArtifact {
    ...
    RelationFusionBootstrapMetadata relation_fusion_metadata;
};
```

JSON schema 最小字段：

```json
{
  "relation_fusion_metadata": {
    "metadata_version": 1,
    "feature_base": "client_group_mix",
    "summary_metadata": [
      {
        "metric_name": "bps",
        "summary_name": "out_of_support_share",
        "task_kind": "ratio",
        "basis_scoped": true,
        "basis_version": 1
      }
    ],
    "pattern_metadata": [
      {
        "pattern": "support_escape",
        "scope": "relation_local",
        "pattern_weight": 0.70,
        "required_summaries": ["out_of_support_share:up"],
        "optional_summaries": [
          "entropy_shannon:up",
          "distinct_group_count:up",
          "stable_headk_coverage:down"
        ],
        "oppose_summaries": [
          "top1_share:up",
          "headk_share:up",
          "entropy_shannon:down"
        ],
        "metrics": ["bps", "pps", "conn_count"]
      }
    ]
  }
}
```

规则：

- metadata 只描述可计算性、scope、pattern 权重和 basis dependency。
- 不保存历史 `relation_risk`。
- 不保存历史 pattern score。
- 不把 fusion metadata 用作 rolling 初始化主路径。
- 旧 artifact / seed 缺少 `relation_fusion_metadata` 时，B5 应根据 task config 和 routed summary seed 生成默认 metadata，并记录 diagnostics，不拒绝加载。

---

## 9. Snapshot 与输出

### 9.1 Submit result

`SubmitObservation()` 成功后，如果启用 fusion 且 `include_fusion_result = true`：

```cpp
result.has_fusion_result = true;
result.fusion_result = last_fusion_result;
```

如果 fusion 未启用或没有 routed evidence：

```cpp
result.has_fusion_result = false;
```

### 9.2 Task snapshot

`QueryTaskSnapshot(JSON)` 在 B4 的 `relation_rolling` 外补充：

```json
{
  "relation_fusion": {
    "enabled": true,
    "source_state_count": 1,
    "persistence_state_count": 12,
    "pattern_count": 4
  }
}
```

### 9.3 Series snapshot

`QuerySeriesSnapshot(source_series_key, JSON)` 在 relation source 视图中补充：

```json
{
  "document_kind": "relation_series_snapshot",
  "series_key": "linkA.client_mix",
  "relation_fusion": {
    "enabled": true,
    "bucket_id": 12345,
    "relation_risk": 0.72,
    "single_risk": 0.31,
    "pattern_risk": 0.59,
    "dominant_single": [
      {
        "metric": "bps",
        "summary": "out_of_support_share",
        "direction": "up",
        "basis_version": 2,
        "basis_scoped": true,
        "normalized_score": 0.83,
        "confidence": 0.81,
        "persistence": 2,
        "evidence_strength": 0.67,
        "score_trust_status": "score_ready",
        "metric_basis_status": "basis_ready"
      }
    ],
    "dominant_pattern": [
      {
        "pattern": "support_escape",
        "score": 0.76,
        "weighted_score": 0.53,
        "metrics_hit": ["bps", "conn_count"],
        "supporting_features": [
          "out_of_support_share:up",
          "entropy_shannon:up"
        ]
      }
    ],
    "pattern_scores": [
      {
        "pattern": "support_escape",
        "score": 0.76,
        "weighted_score": 0.53,
        "metrics_hit": ["bps", "conn_count"]
      }
    ]
  }
}
```

Snapshot 约束：

- `relation_fusion` 是 source 视图的一部分，不出现在 routed summary snapshot 中。
- `dominant_single` 不复制完整 `RollingBaselineResult`。需要深挖时，通过 `routed_series_key` 查询 routed snapshot。
- `pattern_scores` 输出当前 last bucket 的结果，不表达历史时间序列。

---

## 10. 配置

B5 配置进入 `baseline.rolling_config.relation_rolling.relation_fusion`。

建议默认：

```yaml
relation_rolling:
  relation_fusion:
    enable_relation_fusion: true
    fusion_z_score_cap: 5.0
    fusion_min_evidence_score: 0.20
    fusion_persistence_window: 2
    fusion_warming_weight: 0.25
    fusion_degraded_weight: 0.25
    fusion_support_weight: 0.50
    fusion_oppose_weight: 0.50
    basic_pattern_weight: 0.70
    stable_head_pattern_weight: 0.85
    dominant_single_cap: 3
    dominant_pattern_cap: 2
```

解析要求：

- 新增 `BaselineRelationFusionConfig`，并挂到 `BaselineRelationRollingConfig`。
- `ValidateBaselineRollingConfig()` 必须检查：
  - 所有权重在 `[0, 1]`。
  - `fusion_z_score_cap > 0`。
  - `fusion_persistence_window >= 1`。
  - `dominant_single_cap >= 1`。
  - `dominant_pattern_cap >= 1`。
- `ParseRollingConfig()` strict schema 白名单必须允许 nested `relation_fusion` 及其内部 key。
- `baseline-config-template.yaml` 同步默认值。
- 当前运行时 YAML 解析和 strict schema 位于 `config/runtime_config.cpp`。B5 配置不得只修改 task config parser。

---

## 11. 实现任务顺序

| 任务 | 名称 | 设计引用 | 主要文件 | 完成标准 |
| --- | --- | --- | --- | --- |
| `B5-T01` | 补齐 Relation fusion public ABI | 第 3、9 节 | `ibaseline_types.h` | public structs、submit options、result 字段编译通过；旧调用方默认行为兼容 |
| `B5-T02` | 实现 relation fusion 核心模块 | 第 4、5、6、7.1 节 | `relation/relation_fusion.*`、`relation/relation_summary.*` | evidence 标准化、metric 缺测重置、expected evidence universe、persistence、4 个 pattern、跨 metric 合成、relation risk 有单元测试 |
| `B5-T03` | 接入 RelationTask submit 热路径 | 第 5.1、7.2、7.3、7.4 节 | `task/relation_task.*` | fan-out 后更新 fusion；收集 `RelationFusionMetricContext`；透传 `active_count_from_upstream`；不持 routed shard lock 做 fusion；source bucket 有序；runtime rebuild 清理 fusion state |
| `B5-T04` | 补齐 Bootstrap fusion metadata | 第 8 节 | `bootstrap/bootstrap_types.h`、`bootstrap_engine.cpp` | artifact / seed JSON 导出导入稳定；旧 artifact 缺 metadata 可降级加载 |
| `B5-T05` | 接入 task / series snapshot | 第 9 节 | `task/relation_task.*` | source snapshot 能观测 last relation fusion；routed snapshot 不混入 fusion |
| `B5-T06` | 补齐配置模板与解析 | 第 10 节 | `rolling_config.*`、`config/runtime_config.*`、`baseline-config-template.yaml` | 默认值、strict schema、非法配置校验有测试 |
| `B5-T07` | 自动化测试与回归验证 | 全文 | `src/tests/test_baseline/*` | 覆盖 pattern、trust gate、basis gate、cross metric、snapshot、metadata 和无旧链路 |

---

## 12. 测试矩阵

| 场景 | 预期 |
| --- | --- |
| 单个 routed summary 弱异常 | `dominant_single` 可显示低强度证据，但 `relation_risk` 不高 |
| `score_untrusted` | evidence `available = false`，不进入 pattern score |
| `score_warming` | evidence 低权重参与，不能单独形成高 relation risk |
| `drift_learning` / `recalibrating` | 使用 `learning_confidence * fusion_degraded_weight` 形成低强度诊断证据；`effective_confidence = 0` 不导致该分支失效 |
| `can_alert = false` 且 basis 未 ready | basis-scoped evidence 不可用，reason 为 `relation_basis_not_ready` |
| 多 metric basis 状态不同 | 每条 dominant evidence 输出自己的 `metric_basis_status`，不复用顶层 `RelationRollingResult.basis_status` |
| 通用摘要无 basis | 可进入 fusion，只受 B3 score trust 约束 |
| basis_version 切换 | basis-scoped evidence persistence 重置，旧版本不污染新版本 |
| source bucket gap | 当前 source persistence 清空 |
| expected summary 缺测 | 旧 persistence 被归零，不沿用上一 bucket 的 evidence |
| metric 缺失 | 该 metric 下全部 expected evidence persistence 归零，reason 为 `metric_missing` |
| metric 非法 | 该 metric 下全部 expected evidence persistence 归零，reason 为 `metric_invalid` |
| `distinct_group_count` 来自上游 `active_count` | 可作为 `support_escape` support evidence |
| `distinct_group_count` 来自可见 group 回退 | evidence unavailable，不参与 pattern |
| `support_escape` core alone | 得到有限 pattern score；若有 entropy / distinct / coverage 支持则提级 |
| `support_escape` 被集中模式反证 | `top1_share^up`、`headk_share^up` 或 `entropy_shannon^down` 压低 score |
| `head_concentration` | `top1_share^up` 为 core，`headk_share^up` / `entropy_shannon^down` 支持 |
| `legacy_head_dilution` | `stable_headk_coverage^down` 为 core，support 外逃逸和熵上升支持 |
| `stable_head_mix_shift` | `stable_headk_mix_drift^up` 为 core，coverage 下降或 support escape 作为反证 |
| 多 metric 同 pattern 命中 | 跨 metric 采用 `1 - Π(1-score)` 饱和合成 |
| 多 metric 数量增加 | pattern score 不线性无界增长 |
| 多个 unrelated weak single | single risk 只取 top 3，不因 summary 数量机械升高 |
| Bootstrap artifact 导出导入 | `relation_fusion_metadata` 字段稳定，pattern 权重和 basis dependency 保持一致 |
| 旧 artifact 缺 metadata | 加载成功，运行时生成默认 metadata 并记录 diagnostics |
| Bootstrap / Load artifact 后重建 runtime | fusion persistence 和 `last_result` 被清空 |
| source snapshot | 输出 `relation_fusion` 最小 schema |
| routed snapshot | 不输出 `relation_fusion` |
| 并发 submit + snapshot | 若启用 TSAN，锁序无 data race / deadlock；无 TSAN 时至少覆盖 snapshot 复制后写 JSON |
| 关闭 fusion 配置 | submit 不返回 fusion result，snapshot 显示 disabled |
| 旧链路检查 | B5 不调用 `HistoryReader.fetch`、`shadow/candidate/rebuild` |

---

## 13. 完成门禁

B5 完成必须满足：

- B4 routed summary rolling result 可稳定转换为 fusion evidence。
- `normalized_score`、`confidence`、`direction`、`persistence` 和 `can_alert` / `score_trust` 降级语义有明确实现。
- `distinct_group_count` 只有在上游显式提供 `active_count` 时才能参与 fusion。
- expected evidence universe 覆盖缺测 summary、metric 缺失和 metric 非法，缺测不会继承旧 persistence。
- `drift_learning` / `recalibrating` 的低强度诊断证据使用 `learning_confidence`，不依赖为 0 的 `effective_confidence`。
- public evidence 输出使用 `metric_basis_status`，不复用 submit result 顶层 `basis_status`。
- `support_escape`、`head_concentration`、`legacy_head_dilution`、`stable_head_mix_shift` 均按 Sprint 19 v1 公式落地。
- 局部模式融合只在同一 `(source_series_key, feature_base, metric)` 内进行。
- 跨 metric 合成只在同一 `(source_series_key, feature_base, pattern)` 内进行。
- `relation_risk` 是 Relation 内部风险，不冒充全局 `Risk(Key,t)`。
- 单特征基础风险有 top-K 上限，不因 summary 数量机械放大。
- basis 未 ready、`score_untrusted`、summary gap、metric gap 和 `can_alert = false` 均被降权或视为不可用。
- Bootstrap artifact / seed 包含 fusion metadata，且不保存历史 fusion risk。
- Bootstrap / Load artifact 触发 runtime 重建时会清空 fusion state。
- source snapshot 可观测 Relation fusion 输出，routed snapshot 保持底层 rolling 语义。
- Fusion 相关锁序固定，`fusion_states_mutex_` 是 leaf lock。
- Fusion 状态有固定上界，不随 group 数增长。
- 没有恢复旧 rebuild 链路，也没有新增 Relation 专用时间序列模型。

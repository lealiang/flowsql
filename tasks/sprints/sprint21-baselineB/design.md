# Sprint 21 BaselineB 当前算法设计

本文是 BaselineB 与当前 `src/plugins/baseline` 实现对齐后的总设计文档。它描述当前已经落地的算法、接口、状态和边界，不再重复 `B1` 到 `B8` 的阶段实施计划。

相关代码入口：

| 模块 | 代码位置 | 职责 |
| --- | --- | --- |
| Public ABI | `src/framework/interfaces/ibaseline_types.h`、`ibaseline_service.h` | 对外 task、输入、输出、快照和导出类型 |
| Value / Ratio task | `src/plugins/baseline/task/value_task.*`、`ratio_task.*` | 单值 / 比例序列的 bootstrap、rolling、预测和快照 |
| Relation task | `src/plugins/baseline/task/relation_task.*` | Relation block 投影、routed rolling、stream basis、fusion |
| Optional Bootstrap | `src/plugins/baseline/bootstrap/*`、`model/*` | 历史训练 artifact / seed 和 bootstrap 预测 |
| Online Rolling | `src/plugins/baseline/rolling/*` | 在线预测、band、门控更新、成熟度和可信度 |
| Relation | `src/plugins/baseline/relation/*` | basis、summary 投影、fusion 模式分 |
| 配置 | `src/plugins/baseline/config/baseline-config-template.yaml` | 当前运行时配置面和默认值 |

## 1. 总体目标

BaselineB 的主路径是流式在线学习：

```text
Observation
  -> optional bootstrap / empty lazy init
  -> predict
  -> detection band
  -> score trust
  -> gated update
  -> maturity / calibration / diagnostics
```

历史批处理只作为可选启动器，用于生成 `BootstrapArtifact` 和 `BootstrapSeed`，帮助在线状态 warm-up。在线恢复不再依赖 `shadow baseline`、`candidate model`、`HistoryReader.fetch`、正式重建或模型切换验证。

当前支持 3 类 task：

| task | public 接口 | 当前算法 |
| --- | --- | --- |
| Value | `IBaselineValueTask` | `value_basic` / `value_sampled` 转为 `log1p` 模型空间后走在线 rolling |
| Ratio | `IBaselineRatioTask` | `numerator / denominator` 转为裁剪后的 `logit` 模型空间后走在线 rolling |
| Relation | `IBaselineRelationTask` | Relation block 先投影为 routed summary，再复用 Value / Ratio rolling；另有 stream basis 和 relation fusion |

## 2. 身份与输入语义

在线建模单元是：

```text
Series = series_key + task feature identity
```

public 输入只暴露 `series_key`，task 配置携带 `task_kind`、`feature_id`、`feature_type`、`profile`、`clock_spec` 和 `calendar_ref`。Bootstrap seed 初始化 rolling state 时会校验 task identity、clock、calendar、model space 和 `series_key`，不兼容时返回 `kIncompatibleArtifact` 或 `kInsufficientData`。

Value 输入：

| feature type | 输入 | 模型空间 | 样本可靠性 |
| --- | --- | --- | --- |
| `value_basic` | `value` | `log1p(value)` | `sample_count` 不参与语义 |
| `value_sampled` | `value + sample_count` | `log1p(value)` | 低样本会跳过、降权或增加观测噪声 |

Ratio 输入：

```text
denominator > 0
0 <= numerator <= denominator
y_model = logit(clipped(numerator / denominator))
```

低分母时不评分、不更新；中等分母时低权重评分和更新，并增加 `denominator_noise`。

时间口径：

- `bucket_id` 是按 `clock_spec.bucket_seconds` 编号的绝对桶序号。
- 日 / 周相位、月位置、最后一个星期几等 calendar feature 使用配置时区计算。
- 乱序或重复 bucket 不更新状态：`bucket_id <= last_seen_bucket` 返回 `kInvalidArgument`。
- 有 gap 时不补点，只按 gap 长度放大过程噪声，并受 `process_noise_gap_cap_buckets` 限制。

## 3. Optional Bootstrap

Bootstrap 是启动加速器，输出 artifact 和 seed。

Value / Ratio 历史训练使用 `FormalModelTrainer`：

```text
core = intercept + trend + daily harmonic + weekly harmonic
monthpos = day-of-month + days-to-month-end + last-weekday-of-month
event = optional event calendar coefficients
solver = weighted_huber_ridge_irls
```

Value 模型在 `log1p` 空间训练，预测时用 `expm1` 回到观测空间，并用 `sigma_ref` 形成 bootstrap band。Ratio 模型在 `logit` 空间训练，预测时转回概率空间，并用有效样本量的概率方差形成 band。

`BootstrapSeed` 至少包含：

```text
seed_status
coverage_report
enabled_components
theta_init
sigma_init
uncertainty_init
maturity_init
monthpos_hint?
event_hint?
relation_basis_by_metric?
relation_routed_summary_seeds?
relation_fusion_metadata?
```

`seed_status` 由训练状态、覆盖率、覆盖时长、日 / 周组件覆盖质量和初始化字段完整性自动评估为 `none`、`weak`、`partial` 或 `full`。调用方不能直接指定成熟度标签。

Value / Ratio task 在 `Bootstrap()` 或 `LoadBootstrapArtifact()` 成功后，会将 seed 存入 task 内部并尝试批量 warm-up rolling states；若某个 series 没有提前 warm-up，`SubmitObservation()` 仍会按 seed / 空启动规则懒初始化。

## 4. Online Rolling Core

Rolling state 对每个 series 维护有界的参数块，但当前 Value / Ratio 的 `rolling_states_` map 本身不做 TTL / LRU 淘汰，task snapshot 只报告状态数和内存估算，`rolling_state_evicted_total` 固定为 `0`。

核心状态：

```text
level / trend
level-trend 2x2 covariance
daily harmonic coefficients + diagonal covariance
weekly harmonic coefficients + diagonal covariance
sigma / sigma_init
short_ewma / long_ewma / drift evidence
level shift CUSUM evidence
maturity / score trust / calibration / monthpos state
coverage bins and counters
```

模型空间预测：

```text
level_t^- = level_{t-1} + trend_{t-1} * dt
trend_t^- = trend_{t-1}

y_hat_t =
  level_t^-
  + daily_harmonic(bucket_t)
  + weekly_harmonic(bucket_t)
```

`trend` 只进入状态转移，不单独作为观测项。日 / 周 harmonic 使用本地日历相位；协方差采用 `level/trend` 的 2x2 block 加 day/week 对角近似。

首次空启动：

1. 要求输入可更新。
2. `level` 初始化为首个 `y_model`，`sigma` 为 `sigma_floor`。
3. 首个输出以观测值为中心，使用 `cold_start_band_scale * sigma` 生成宽 band。
4. `can_score = false`，避免第一点被解释为成熟异常。

有 seed 启动：

1. 校验 task identity、clock、calendar 和模型空间。
2. 用 seed 的 level、trend、daily / weekly harmonic、sigma、coverage 和 uncertainty 初始化状态。
3. seed 覆盖越好，初始协方差越小；`weak` seed 初始不确定性更大。
4. `monthpos_hint` 可初始化月位置慢学习状态，但月位置必须等在线成熟后才参与检测。

## 5. Detection Band、门控和更新

在线提交每个有效 bucket 的主流程：

```text
validate
  -> auto_init
  -> PredictRollingState
  -> BuildDetectionBand
  -> UpdateDriftEvidence
  -> UpdateDetectionCalibration
  -> UpdateScoreTrust
  -> ComputeUpdateGate
  -> UpdateRollingStateWithObservation
  -> UpdateResidualScale
  -> UpdateMaturityEvidence
  -> UpdateRollingMonthpos
```

检测 band 与 forecast band 分开：

- `SubmitObservation()` 输出检测 band 和检测 `z_score`，用于异常评分和告警可信度。
- `PredictRolling()` 是只读未来预测，不初始化、不学习、不输出异常判定，使用 `forecast_band_z`。

检测 band 当前公式：

```text
calibrated_sigma = max(sigma_floor, sigma * detection_band_multiplier)
extra_obs_var = extra_obs_noise_scale * sigma^2 + sigma_floor^2
detection_var = pred_var + calibrated_sigma^2 + extra_obs_var
band_std = min(sqrt(detection_var), detection_band_std_cap)
z_score = abs((y_model - detection_mu) / band_std)
```

`maturity_uncertainty_*` 和 `missing_*_uncertainty_*` 当前只进入 diagnostics/result 字段，不直接加到 detection band 方差中。月位置 `monthpos` 只有在 `monthly_ready` 后才加入 `detection_mu`。

漂移证据：

```text
resid_norm = clamp(residual / band_std, -z_cap, z_cap)
short_ewma = (1 - alpha_short) * short_ewma + alpha_short * resid_norm
long_ewma  = (1 - alpha_long)  * long_ewma  + alpha_long  * resid_norm
drift_evidence = short_ewma - long_ewma
```

同时维护同向残差的 CUSUM 风格 `level_shift_evidence`。实际自适应使用 `drift_evidence` 和 `level_shift_evidence` 中绝对值更大的证据：

```text
adapt_boost =
  clamp((abs(combined_drift_evidence) - drift_start) /
        (drift_full - drift_start), 0, 1)
```

更新门控使用 estimator 的原始 `z_score`：

```text
skip_threshold = z_skip + adapt_boost * skip_relax

if abs(z) >= skip_threshold:
  update_weight = small_update_weight when adapt_boost == 1
  update_weight = 0 otherwise
else if abs(z) >= z_downweight:
  update_weight = small_update_weight
else:
  update_weight = 1
```

状态更新采用带权 Kalman / RLS 近似：

- `adapt_boost` 放大 `Q_level` 和 level 学习速度。
- trend 更新有单步变化上限和绝对值上限。
- day / week 更新速度慢于 level；drift 期间 seasonal 更新被压低。
- `full` / `partial` seed 过来的 seasonal 组件有更小学习倍率；配置允许 drift 期间冻结 seeded seasonal。
- 残差尺度 `sigma` 用带权 EWMA 更新，残差平方按 `c_sigma * sigma` 裁剪。

## 6. 成熟度、校准和 score trust

当前实现区分 3 层状态：

| 状态 | 取值 | 作用 |
| --- | --- | --- |
| 粗粒度学习状态 | `cold_learning`、`warming`、`ready_hint` | 快速表达 rolling state 的更新数量阶段 |
| 组件成熟度 | `cold_learning -> level_ready -> daily_warming -> daily_ready -> weekly_warming -> weekly_ready -> monthly_warming -> monthly_ready` | 决定组件是否可认为成熟 |
| 检测可信度 | `score_untrusted`、`score_warming`、`score_ready`、`drift_learning`、`recalibrating` | 决定当前异常分是否可用于告警 |

成熟度推进依据：

- `level_ready_min_updates`
- 日 / 周覆盖 bin 的非零比例
- 至少覆盖的天数 / 周数
- 月份切换次数、月位置覆盖率和最后一个星期几覆盖

score trust 规则：

- `can_score = false` 时为 `score_untrusted`。
- drift 或 level shift 证据超过阈值时进入 `drift_learning`，`can_alert = false`。
- drift 后先进入 `recalibrating`，稳定计数达到 `score_recovery_min_updates` 后才恢复。
- `score_ready` 要求 calibration 更新数、coverage、`tail3_ewma`、`tail5_ewma` 和至少 `level_ready` 同时达标。
- 在只达到 level 成熟但还没有日周期成熟时，只有 `z_score >= level_only_extreme_z` 的极端异常才允许告警。

输出中的 `confidence` 等于 `effective_confidence`：

```text
effective_confidence = min(learning_confidence, score_confidence)
```

## 7. Forecast 预测视图

`PredictRolling(series_key, bucket_id)` 只接受未来 bucket，且 series 必须已经有 rolling state。它不触发 seed / empty lazy init。

预测优先级：

1. 若有可用 bootstrap seed，则使用 bootstrap 曲线作为长期形状，并叠加 rolling 的 level / trend 修正。
2. 否则使用 rolling state 的 level、受限 trend 和 seasonal 直接预测。
3. 若 `monthpos` 已 `monthly_ready`，再叠加月位置效果。

forecast 输出使用 `forecast_band_z`，不是在线检测的 `band_z` 和 score trust band。

## 8. Relation 算法

Relation 不实现独立的时间序列模型，而是：

```text
RelationRollingObservation
  -> per metric basis state
  -> summary projection
  -> routed Value / Ratio rolling
  -> relation fusion
```

### 8.1 Relation basis

Relation basis 由 `RelationServiceBasis` 表达：

```text
basis_version
feature_base
metric_name
group_space_id / version
k_head
other_group_idxs
support_explicit
stable_head
head_proto_q
```

历史 bootstrap 使用全量历史统计构建 basis。stream-only 路径使用 `RelationStreamBasisAccumulator` 在线积累固定上限的 group 统计，默认 `basis_stats_max_groups = 256`。当 group 数超过上限时，按当前最小 mass 估计进行替换，并记录误差上界；生成候选 basis 时用保守下界筛选。

stream basis 状态：

```text
no_basis -> collecting -> basis_warming -> basis_ready
                         -> handover_warming -> basis_ready
```

刷新规则：

- 低于 `collect_min_buckets` 不构建。
- 覆盖率低于 `candidate_min_coverage_ratio` 不刷新。
- 候选 support / stable head 变化超过 replacement cap 时拒绝切换。
- basis 切换后进入 handover warming，等待 warm-up bucket 后变为 ready。
- `basis_min_stable_refresh_count` 要求候选 group 连续多次刷新稳定后才进入 basis。

### 8.2 Routed summary

每个 Relation metric 投影为两类 summary：

通用 summary，无 basis 也可输出：

| summary | 类型 | 说明 |
| --- | --- | --- |
| `entropy_shannon` | Value | 当前 group 分布熵 |
| `distinct_group_count` | Value | group 个数；只有上游提供 `active_count` 时 fusion 认为可信 |
| `top1_share` | Ratio | 非 other group 中最大 group 占比 |
| `headk_share` | Ratio | 非 other group 中 Top-K 质量占比 |

basis-scoped summary，必须有 active basis：

| summary | 类型 | 说明 |
| --- | --- | --- |
| `out_of_support_share` | Ratio | 不在 explicit support 中的质量占比 |
| `stable_headk_coverage` | Ratio | stable head 总覆盖率 |
| `stable_g_share_i` | Ratio | stable head 中第 `i` 个 group 的占比 |
| `stable_headk_mix_drift` | Value | stable head 内部归一化混合分布相对 `head_proto_q` 的 TV 距离 |

routed series identity：

```text
source_series_key::metric::summary::feature_type
source_series_key::metric::summary::feature_type::basis:<basis_version>
```

basis-scoped summary 在 basis 未 `basis_ready` 前会继续训练 rolling，但强制 `can_alert = false`。

Relation task 内部按 `routed_state_shard_count` 分 shard 保存 routed seed、routed task spec 和 routed rolling state；这是分片组织，不是并发授权。同一个 task 仍遵守 public ABI 的“同 task 非并发调用”契约。

### 8.3 Relation fusion

Fusion 只在同一个 Relation source 内合成局部结构风险，不输出全局 Key 级风险，也不做业务处置判别。

输入是 routed `RollingBaselineResult` 的轻量证据。证据可用性要求：

- rolling status 为 `kOk`
- `can_score = true`
- direction 能映射到当前 summary
- normalized score 大于 `0`
- `score_trust_status` 不能是 `score_untrusted`
- `score_ready` 时还要求 `can_alert = true`
- basis-scoped evidence 要求 metric basis status 为 `basis_ready`
- `distinct_group_count` 必须来自上游 `active_count`

单证据强度：

```text
normalized_score = min(1, abs(z_score) / fusion_z_score_cap)
persistence_factor = min(1, persistence / fusion_persistence_window)
evidence_strength =
  normalized_score * confidence * persistence_factor * trust_factor
```

当前模式库：

| pattern | 核心证据 | 支持 / 反向证据 |
| --- | --- | --- |
| `support_escape` | `out_of_support_share:up` | 熵升、distinct 升、stable coverage 降支持；top1/headk/熵降反向 |
| `head_concentration` | `top1_share:up` | headk 升、熵降支持；out-of-support 升、熵升反向 |
| `legacy_head_dilution` | `stable_headk_coverage:down` | out-of-support 升、熵升支持；stable coverage 升反向 |
| `stable_head_mix_shift` | `stable_headk_mix_drift:up` | stable coverage 降、out-of-support 升反向 |

局部模式分按：

```text
pattern_score = clamp01(core + support_weight * support - oppose_weight * oppose)
```

跨 metric 使用饱和并集：

```text
union(values) = 1 - Π(1 - clamp01(value))
```

最终：

```text
single_risk = union(dominant_single.evidence_strength)
pattern_risk = union(pattern_scores.weighted_score)
relation_risk = 1 - (1 - single_risk) * (1 - pattern_risk)
```

Fusion runtime state 以 source 为粒度保存 persistence 和最近结果，默认有：

- source TTL：`fusion_state_ttl_buckets = 20160`
- source 数上限：`fusion_state_max_sources = 4096`
- 每次清理扫描上限：`fusion_state_cleanup_scan_limit = 256`
- 每个 source 的 persistence key 上限：`fusion_persistence_max_keys_per_source = 512`

## 9. Snapshot、序列化和生命周期

Baseline 插件只通过 `IBaselineService` 暴露同进程接口，不直接暴露 HTTP。

稳定的观测 / 导出入口：

| 能力 | 接口 |
| --- | --- |
| task 配置 | `ExportConfig()` |
| task 快照 | `QueryTaskSnapshot()` |
| series 快照 | `QuerySeriesSnapshot()` |
| rolling 未来预测 | `PredictRolling()` / `PredictRoutedSummary()` |
| bootstrap 预测 | `PredictBootstrap()` |
| bootstrap artifact | `ExportBootstrapArtifact()` / `LoadBootstrapArtifact()` |
| bootstrap seed | `ExportBootstrapSeed()` |
| Relation routed summary 快照 | `QueryRoutedSummarySnapshot()` |
| Relation basis 查询 | `QueryBootstrapBasis()` |

Snapshot JSON 用于观测、调试和恢复边界，不作为热路径内部交接协议。进程内核心路径使用结构体。

同 task 线程契约：

- 同一个 task 的 public API 调用不得重叠执行。
- 不同 task 可以并行。
- `Id()`、`Name()`、`Kind()` 是 immutable identity getter，可跨线程读取。
- `Close()` 后业务调用应返回非 `kOk`，不得继续修改学习状态。

## 10. 当前边界

当前实现与旧方案的差异和边界如下：

- 在线主路径没有 `shadow/candidate/rebuild`。
- Value / Ratio rolling state 当前没有按 series 的 TTL / LRU 淘汰；高基数调用方需要控制 series 数量，或后续单独实现状态清理。
- Relation group 级状态有 `basis_stats_max_groups` 上限；Relation fusion source / persistence 状态有 TTL 和容量清理。
- detection band 的成熟度 / 缺失组件不确定性当前只进入 diagnostics，不直接扩大 detection band。
- `PredictRolling()` 是 forecast view，不是异常检测接口；检测可信度只由 `SubmitObservation()` 输出。
- Relation fusion 是算法层结构风险，不输出业务告警类别、处置建议或全局 Key 风险。
- `distinct_group_count` 只有在上游显式提供 `active_count` 时才参与 fusion；否则作为不可用证据处理。

## 11. 回归测试落点

当前 baseline 相关测试集中在 `src/tests/test_baseline`：

| 测试 | 覆盖方向 |
| --- | --- |
| `test_baseline_observation_adapter` | Value / Ratio 输入合法性、低样本和低分母语义 |
| `test_baseline_rolling_state` | 空启动、seed 初始化、成熟度初值 |
| `test_baseline_rolling_estimator` | harmonic 特征、预测、Kalman / RLS 更新 |
| `test_baseline_rolling_gate_scale` | update gate、drift evidence、residual scale |
| `test_baseline_rolling_b3` | detection band、score trust、maturity、monthpos |
| `test_baseline_relation_summary` | Relation summary 投影与 routed identity |
| `test_baseline_relation_basis_state` | stream basis、replacement cap、handover |
| `test_baseline_relation_fusion` | evidence 标准化、persistence、pattern risk |
| `test_baseline_link_bootstrap_eval` | bootstrap 历史拟合评估 |
| `test_baseline_link_rolling_eval` | rolling 在线评估 |
| `test_baseline_batch_prediction_perf` | 批量预测特征缓存与性能路径 |

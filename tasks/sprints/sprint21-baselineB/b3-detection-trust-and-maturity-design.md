# B3 Detection Trust, Band Calibration and Monthly Readiness 阶段设计

## 1. 目标与非目标

`B3` 的目标是在 `B2 Online Rolling Core MVP` 之上补齐检测可信度、检测 band 校准、组件成熟度和月位置在线成熟。

`B2` 已经完成：

```text
stream observation -> rolling prediction band -> residual score -> gated update
```

`B3` 要把这条主路径扩展为：

```text
stream observation
  -> rolling prediction
  -> calibrated detection band
  -> score trust gate
  -> maturity/component gate
  -> gated update
```

必须完成：

- 区分 `learning confidence` 与 `score trust`。
- 建立检测可信度状态：`score_untrusted -> score_warming -> score_ready`。
- 在 `drift_learning` / `recalibrating` 期间降低异常判定可信度。
- 校准检测评分 band，避免短期平稳窗口把 band 压得过窄。
- 实现完整 maturity 状态和组件解锁。
- 实现月位置（`monthpos`）的 stream-only 慢成熟。
- 完整 bootstrap seed 可提高初始 maturity 和初始 band 质量，但不能绕过 score trust / coverage 校准直接进入高置信异常判定。

本阶段不做：

- `T3` stream-only basis 刷新，这归 `B4`。
- 长周期自适应预测视图或正式 forecast 产品接口。
- Rolling 反向改写 `BootstrapSeed` 或 bootstrap artifact。
- 新的 batch 重建链路。
- 任何旧 `shadow/candidate/rebuild` 逻辑。

## 2. 与 B2 的边界

`B3` 继续沿用 `B2` 的任务边界：

- 不新增独立 rolling service。
- `SubmitObservation(...)` 仍是 `Value / Ratio` task 的唯一 public rolling 提交入口。
- `PredictRolling(series_key, bucket_id)` 仍是只读预测接口，不更新状态、不触发 lazy init。
- `BootstrapSeed` 仍是 baseline plugin 内部结构，不进入 `framework/interfaces` public ABI。
- `IBaselineRelationTask` 不新增 rolling 提交接口；`T3 routed summary` 后续仍作为 `value_basic`、`value_sampled` 或 `ratio` series 复用 `T1/T2` rolling core。

`B2` 的粗粒度状态：

```text
cold_learning
warming
ready_hint
```

在 `B3` 中必须降级为兼容字段或内部迁移来源，不再作为完整 maturity gate。

`B3` 新增 3 条独立语义线：

```text
learning maturity:  rolling state / component 学到了什么
score trust:        当前 Z-score 是否可信
band calibration:   检测评分 band 是否被可靠校准
```

这 3 条线不能合并成单个 `confidence`。

## 3. Public ABI 与输出语义

### 3.1 RollingBaselineResult 扩展

`B3` 建议采用 append-only 方式扩展 `RollingBaselineResult`，避免破坏已有调用方。

新增字段：

```cpp
std::string maturity_status;
std::string score_trust_status;
std::string calibration_status;

double learning_confidence = 0.0;
double score_confidence = 0.0;
double effective_confidence = 0.0;

bool can_alert = false;

std::vector<std::string> enabled_components;
std::vector<std::string> component_readiness;
```

字段语义：

| 字段 | 语义 |
|---|---|
| `learning_confidence` | rolling state 的学习成熟度，描述模型状态是否稳定 |
| `score_confidence` | 当前检测分数可信度，描述 `Z-score` 是否适合异常判定 |
| `effective_confidence` | 对外综合置信度，取 learning、score trust、输入支撑、drift 状态后的保守值 |
| `confidence` | 兼容字段，`B3` 中应映射为 `effective_confidence` |
| `can_score` | 可以计算数值 score；不表示可以告警 |
| `can_alert` | 可以把当前 score 用作高置信异常判定 |
| `enabled_components` | 当前可参与检测 band 和评分的组件，例如 `level`、`daily` |
| `component_readiness` | 组件成熟度证据，用稳定字符串表达 |

`component_readiness` 在 public ABI 中保持 `std::vector<std::string>`，避免引入新的嵌套 public 类型。字符串格式固定为：

```text
component=status[:reason]
```

示例：

```text
level=ready
daily=warming:coverage_low
weekly=disabled:not_enough_weeks
monthpos=disabled:not_enough_months
```

snapshot 可将这些字符串展开成 JSON object。

`can_score` 与 `can_alert` 的区别：

- `can_score = true, can_alert = false`：允许输出诊断用 `Z-score`，但不能输出高置信异常结论。
- `can_score = true, can_alert = true`：当前检测 band 和 score trust 已达到可判定标准。
- `can_score = false`：当前 score 不可信，通常处于冷启动、低支撑或严重重校准阶段。

### 3.2 Z-score 语义

`B2` 当前只有一个 `z_score`，来自 `RollingStateEstimator` 的内部 band，并同时服务 update gate 与输出。

`B3` 后：

- `RollingBaselineResult.z_score` 表示 calibrated detection band 上的检测 `Z-score`。
- rolling 内部更新门控继续使用 update gate 的内部 `update_z`，即 `B2` estimator band 上的 `z_score`。
- drift evidence 默认也继续使用 `update_z` / `B2` estimator band，避免 calibrated detection band 在重校准阶段过宽而掩盖真实 level shift。
- 若需要调试内部更新分数，放入 `diagnostics` 或 snapshot，不新增必须 public 字段。

原因：

- 对外用户最关心的是异常判定使用的 `Z-score`。
- update gate 是污染防护机制，不应与检测可信度混为一个输出。
- 当前代码的 `SubmitObservedPoint()` 在预测后依次执行 drift、gate、update、scale；B3 实现必须显式区分 `update_z` 与 `detection_z`，不能把 calibrated detection `Z-score` 直接传给 `ComputeUpdateGate()`。

### 3.3 Band 语义

`B3` 中 `RollingBaselineResult` 的 band 字段定义为检测评分 band：

```text
baseline_mu
baseline_lower
baseline_upper
band_width
band_std
```

约束：

- `baseline_lower / baseline_upper` 是异常判定和解释的主依据。
- `baseline_mu` 只是中心线，不单独代表完整基线。
- 检测评分 band 必须经过 maturity、score trust、校准、低支撑输入和 drift 状态共同约束。
- B3 不提供独立 forecast 产品接口；预测展示 band 只允许作为 snapshot / diagnostics 的辅助信息。

`PredictRolling(series_key, bucket_id)` 没有观测值，也没有 sampled value / ratio 的支撑信息，因此不能输出 `can_alert` 或检测结论。B3 对该接口的约束是：

- 保持只读，不更新 calibration / maturity / score trust。
- 可以返回当前状态下的 calibrated prediction band，但 `RollingPrediction` 不表达异常判定。
- 如果后续需要暴露 forecast 产品能力，另行设计接口，不在 B3 中扩展。

## 4. 内部状态模型

`B3` 在 `RollingState` 上新增有界状态。状态必须固定大小，不能保存 per-bucket 历史窗口。

### 4.1 Maturity 状态

```cpp
enum class RollingMaturityStatus : int32_t {
    kColdLearning = 0,
    kLevelReady = 1,
    kDailyWarming = 2,
    kDailyReady = 3,
    kWeeklyWarming = 4,
    kWeeklyReady = 5,
    kMonthlyWarming = 6,
    kMonthlyReady = 7,
};
```

配套状态：

```text
maturity_status
learning_confidence
enabled_components
component_readiness
accepted_update_count
coverage counters
```

`enabled_components` 只包含已经允许进入检测评分的组件。

### 4.2 Score Trust 状态

```cpp
enum class ScoreTrustStatus : int32_t {
    kScoreUntrusted = 0,
    kScoreWarming = 1,
    kScoreReady = 2,
    kDriftLearning = 3,
    kRecalibrating = 4,
};
```

其中：

- `score_untrusted -> score_warming -> score_ready` 是正常推进路径。
- `drift_learning` 和 `recalibrating` 是临时降级状态。
- 临时降级状态恢复后，只能回到 `score_warming` 或 `score_ready`，不能直接绕过校准条件。

配套状态：

```text
score_trust_status
score_confidence
stable_score_count
score_ready_count
degradation_reason
last_degradation_bucket
```

### 4.3 Calibration 状态

```text
detection_band_multiplier
calibrated_sigma
coverage_ewma
tail3_ewma
tail5_ewma
abs_z_ewma
calibration_update_count
calibration_status
```

状态含义：

- `coverage_ewma`：近期观测落在 detection band 内的比例估计。
- `tail3_ewma`：近期 `|Z| > 3` 比例估计。
- `tail5_ewma`：近期 `|Z| > 5` 比例估计。
- `detection_band_multiplier`：检测 band 的保守放大系数。
- `calibration_status`：`uncalibrated`、`warming`、`calibrated`、`expanding`、`recalibrating`。

`tail3_ewma` 和 `tail5_ewma` 只用常数内存 EWMA，不维护滑窗数组。

### 4.4 Coverage 状态

覆盖度使用固定 bin 统计，不保存历史点。

```text
daily coverage bins
weekly coverage bins
monthpos coverage bins
```

建议内部结构：

```text
daily_bin_count[daily_coverage_bins]
weekly_bin_count[weekly_coverage_bins]
monthpos_count[31]
monthpos_seen_month_mask / month_transition_count
```

约束：

- `daily_coverage_bins` 和 `weekly_coverage_bins` 必须有配置上限。
- 默认按 24 小时 bin 和 7 * 24 周 bin 统计。
- `monthpos` 固定 31 个 bin。
- coverage 只记录支撑成熟度，不参与 batch 历史重建。

### 4.5 Monthpos 状态

```text
monthpos_dom_coeff[31]
monthpos_dme_coeff[dme_max + 1]
monthpos_lwd_coeff[7]
monthpos_dom_center[31]
monthpos_dme_center[dme_max + 1]
monthpos_lwd_center[7]
monthpos_update_count
monthpos_ready_count
month_transition_count
monthpos_status
```

月位置组件是慢变量：

- 未成熟时可以学习，但不参与高置信检测评分。
- 成熟后才进入 detection band 和 `enabled_components`。
- 更新速度必须慢于 `level/day/week`。
- 单月短期异常不能使 monthpos ready，也不能快速污染 coeff。
- bootstrap `monthpos_hint` 已包含 `dom/dme/lwd` coeff 和 center。B3 若消费 seed，必须按同一 centered basis 初始化；若 MVP 只实现 dom 子集，必须记录 diagnostics，并且不能宣称完整 `monthly_ready`。

## 5. 在线主流程

B3 的每个有效 bucket 执行。这里的 `base prediction` 指当前代码中 `PredictRollingState()` 给出的 `level + daily + weekly` 预测；`update_z` 用于 update gate，`detection_z` 用于对外异常判定：

```text
validate
  -> auto_init
  -> derive active components
  -> predict base rolling state
  -> evaluate active monthpos effect
  -> build calibrated detection band
  -> compute detection z-score
  -> update score trust and calibration evidence
  -> compute update gate with update_z
  -> update rolling state
  -> update maturity / coverage / monthpos
  -> map result
```

实现约束：

- `PredictRollingState()` 产出的 prediction / residual 是 pre-update evidence。
- `detection_z`、calibration evidence、score trust 和 public result 都必须使用同一 bucket 的 pre-update estimator。
- `UpdateRollingStateWithObservation()` 当前内部会重新预测并推进状态；其 `update_result` 只允许用于更新 state 和 residual scale，不得覆盖 pre-update estimator，不得用于填充 `RollingBaselineResult`。
- `UpdateRollingStateWithObservation()` 的输入可以是扣除 ready monthpos effect 后的 adjusted observation，但输出仍必须按 pre-update detection evidence 映射回原始观测空间。
- `ResidualScale` 继续跟随 update gate 的 `base_update_weight`，不能被 `score_trust` 降级直接冻结；否则 level shift 后可能长期无法恢复尺度。

### 5.1 Active Component 选择

组件分为 3 类：

| 状态 | 行为 |
|---|---|
| learning only | 可更新，不进入高置信评分 |
| warming | 可输出 evidence，可低置信参与诊断 |
| ready | 可进入 detection band 和高置信评分 |

默认 active component：

```text
level_ready       -> level
daily_ready       -> level + daily
weekly_ready      -> level + daily + weekly
monthly_ready     -> level + daily + weekly + monthpos
```

未 ready 的组件不得进入 `enabled_components`。

当前 `B2 PredictRollingState()` 的 `base_mu` 天然包含 `level + daily + weekly` harmonic，即使 daily / weekly 还没有 ready。B3 若要求「未 ready 组件不得进入检测评分」，实现时必须显式选择一种中心线策略：

1. **strict ready center**：重算 `detection_mu`，只叠加 ready components。此方案语义最干净，但需要新增按组件计算 `level/day/week` contribution 的 helper，不能直接复用 `RollingEstimatorResult.model_mu`。
2. **provisional center**：允许未 ready 的 daily / weekly 作为 provisional center hint 进入中心线，但不得进入 `enabled_components`，并且必须在 evidence 中说明它们未 ready，同时用 `component_missing_uncertainty_t` 保守扩宽 detection band。

B3 MVP 推荐使用 `provisional center`，因为它与当前 `B2` estimator 结构最兼容。若采用该方案：

- `enabled_components` 仍只包含 ready components。
- `uncertainty_source` 必须包含 `provisional_daily_component` / `provisional_weekly_component` 或对应缺失原因。
- `component_readiness` 必须明确 daily / weekly 是 `warming` 或 `disabled`，不能因为进入中心线就标记为 ready。
- `component_missing_uncertainty_t` 必须覆盖未 ready 周期项的不确定性；否则 `can_alert` 必须保持 `false`。

### 5.2 Monthpos 与 B2 Estimator 的关系

`B2` 的 estimator 只负责：

```text
level + daily + weekly
```

`B3` 的 monthpos 作为慢变量 wrapper 处理。

当 monthpos 未 ready：

```text
base_mu = B2(level + daily + weekly)
detection_mu = base_mu
monthpos coeff 只学习，不参与高置信评分
```

当 monthpos ready：

```text
active_monthpos = EvaluateRollingMonthpos(bucket_id)
detection_mu = base_mu + active_monthpos
adjusted_y_model_for_update = y_model - active_monthpos
```

这样可以避免月位置效应长期污染 `level/day/week`。

当前 `RollingEstimatorResult.model_mu` 只由状态和时间特征决定，不依赖 `ObservedModelPoint.y_model`。因此 B3 必须按以下口径实现：

```text
1. 用原始 point 调用 PredictRollingState()，得到 base_mu / pred_var。
2. 若 monthpos ready，只在 detection 中令 detection_mu = base_mu + active_monthpos。
3. 若本点允许更新 B2 core，构造 adjusted point：
   adjusted.y_model = original.y_model - active_monthpos。
4. 用 adjusted point 调用 UpdateRollingStateWithObservation()。
5. public result 仍使用第 1-2 步的 pre-update detection evidence。
```

也就是说，预测仍基于当前 state 和 bucket；`y_model - active_monthpos` 只用于更新 B2 core，不用于重新定义 B2 的预测中心线。

`EvaluateRollingMonthpos(bucket_id)` 应复用当前 bootstrap / formal predictor 的 centered basis 语义：

```text
dom effect: day-of-month one-hot - dom_center
dme effect: days-to-month-end one-hot - dme_center
lwd effect: last-weekday one-hot - lwd_center
```

不要把 bootstrap 中的 `dom_coeff` 简化理解为 31 个直接 offset；它在旧模型中是 centered one-hot 系数。

### 5.3 检测 band

B2 的基础预测方差继续使用：

```text
pred_var_t = H_t * P_t^- * H_t^T
```

B3 的检测尺度：

```text
calibrated_sigma_t =
  max(sigma_floor,
      sigma_t * detection_band_multiplier_t)
```

`extra_obs_noise_t` 只表示输入支撑不足带来的额外观测噪声和 `sigma_floor` 保护，不包含基础 residual scale。不能直接使用 `estimator.obs_var - estimator.pred_var`，因为当前 B2 `ExtraObsVariance()` 已经包含：

```text
extra_obs_noise_scale * sigma^2 + sigma^2 + sigma_floor^2
```

B3 detection band 中的基础 residual scale 已由 `calibrated_sigma_t^2` 表达，因此这里必须使用：

```text
extra_obs_noise_t =
  max(0, point.extra_obs_noise_scale) * sigma_t^2
  + sigma_floor^2
```

检测方差：

```text
detection_var_t =
  pred_var_t
  + calibrated_sigma_t^2
  + extra_obs_noise_t
  + maturity_uncertainty_t
  + component_missing_uncertainty_t
```

检测 band：

```text
detection_band_std_t = sqrt(detection_var_t)

baseline_lower_model_t =
  detection_mu_t - band_z * detection_band_std_t

baseline_upper_model_t =
  detection_mu_t + band_z * detection_band_std_t

detection_z_t =
  abs((y_model_t - detection_mu_t) / detection_band_std_t)
```

`maturity_uncertainty_t` 用于冷启动、warming、低 score trust 和低支撑输入时扩宽 band。MVP 可按离散状态给出保守倍率：

```text
cold_learning      -> maturity_uncertainty_cold_scale * sigma^2
score_warming      -> maturity_uncertainty_warming_scale * sigma^2
drift_learning     -> maturity_uncertainty_drift_scale * sigma^2
recalibrating      -> maturity_uncertainty_recalibrating_scale * sigma^2
score_ready        -> 0
```

`component_missing_uncertainty_t` 用于组件缺失时的保守检测：

```text
daily_ready = false  -> missing_daily_uncertainty_scale * sigma^2
weekly_ready = false -> missing_weekly_uncertainty_scale * sigma^2
```

该项不表示模型已经学到对应周期，只表示「当前检测不应把未建模周期波动当作高置信异常」。

## 6. Score Trust 规则

### 6.1 正常推进

```text
score_untrusted
  -> score_warming
  -> score_ready
```

进入 `score_warming` 的最低条件：

- 至少达到 `level_ready`。
- 已有足够 update-eligible 点。
- detection band 有初步 calibration 统计。
- 当前点输入支撑满足 scoring 条件。

进入 `score_ready` 的最低条件：

- `score_warming` 已持续足够点数。
- `coverage_ewma` 不低于下限。
- `tail3_ewma` 和 `tail5_ewma` 未超出保守阈值。
- 当前不处于 drift / recalibration。
- 至少有一个 ready component。
- 若 `daily_ready = false`，只能进入 `score_ready` 的 `level_only_extreme` 子语义：`can_alert` 仅允许对极端突刺成立，并且 detection band 必须加入 `component_missing_uncertainty`。

`score_ready` 不等价于 `monthly_ready`。但 `level_ready` 也不等价于完整可信检测。对于有明显日周期或周周期的数据，在 `daily_ready` / `weekly_ready` 之前，正常周期波动可能穿出 level-only band，因此：

- `level_only_extreme` 只允许识别远超当前 band 的突刺，默认阈值高于常规 `z_skip`。
- `enabled_components` 必须明确只有 `level`。
- `uncertainty_source` 必须包含 `missing_daily_component` 或 `missing_weekly_component`。
- `component_missing_uncertainty_t` 必须显著扩宽 detection band；若配置关闭该扩宽，则 `can_alert` 必须保持 `false`，直到 `daily_ready`。

### 6.2 降级状态

进入 `drift_learning`：

```text
abs(drift_evidence) >= score_drift_degrade_start
```

行为：

- `can_alert = false` 或显著降低 `score_confidence`。
- 允许受控学习新 level。
- 不允许快速改写 day/week/monthpos。
- detection band 不允许因短期残差变小而收窄。

进入 `recalibrating`：

```text
drift_evidence 回落
但新水平下 calibration_update_count / coverage 尚未恢复
```

行为：

- 继续输出诊断 score。
- `can_alert = false`，直到 coverage / tail 统计恢复。
- band 可慢收缩，不可快收缩。

恢复规则：

```text
drift_learning
  -> recalibrating
  -> score_warming
  -> score_ready
```

不得从 `drift_learning` 直接跳回 `score_ready`。

## 7. Band 校准算法

### 7.1 EWMA evidence

对 update-eligible 且 score 支撑足够的点更新。`calibration_update_count < calibration_warmup_min_updates` 时只积累 evidence，不触发快扩张 / 慢收缩。

```text
inside_band_t = abs(detection_z_t) <= band_z
tail3_t = abs(detection_z_t) > 3
tail5_t = abs(detection_z_t) > 5

coverage_ewma =
  (1 - alpha_calibration) * coverage_ewma
  + alpha_calibration * inside_band_t

tail3_ewma =
  (1 - alpha_calibration) * tail3_ewma
  + alpha_calibration * tail3_t

tail5_ewma =
  (1 - alpha_calibration) * tail5_ewma
  + alpha_calibration * tail5_t

abs_z_ewma =
  (1 - alpha_calibration) * abs_z_ewma
  + alpha_calibration * min(abs(detection_z_t), z_cap)
```

低样本数、低分母、`can_score = false` 的点不更新 calibration evidence。

初始值建议：

```text
coverage_ewma = 1.0
tail3_ewma = 0.0
tail5_ewma = 0.0
abs_z_ewma = 0.0
detection_band_multiplier = max(1.0, seed_band_z / config.band_z)
```

没有 seed 时，`seed_band_z = config.band_z`。这样可以避免冷启动前几个点因为 `coverage_ewma = 0` 而立即把 band 放大到上限。

### 7.2 快扩张

触发条件：

```text
coverage_ewma < calibration_coverage_floor
or tail3_ewma > calibration_tail3_limit
or tail5_ewma > calibration_tail5_limit
```

更新：

```text
detection_band_multiplier =
  min(calibration_multiplier_max,
      detection_band_multiplier * (1 + calibration_expand_rate))
```

快扩张用于降低误报，不表示数据异常一定消失。

### 7.3 慢收缩

触发条件：

- 当前为 `score_warming` 或 `score_ready`。
- 不处于 `drift_learning` / `recalibrating`。
- coverage 和 tail evidence 连续满足目标。
- 已达到 `calibration_shrink_min_updates`。

更新：

```text
detection_band_multiplier =
  max(calibration_multiplier_min,
      detection_band_multiplier * (1 - calibration_shrink_rate))
```

慢收缩必须明显慢于快扩张。

## 8. Maturity 与 Component Readiness

### 8.1 状态推进

```text
cold_learning
  -> level_ready
  -> daily_warming
  -> daily_ready
  -> weekly_warming
  -> weekly_ready
  -> monthly_warming
  -> monthly_ready
```

推进条件由 4 类 evidence 共同决定：

- 有效更新数量。
- 时间覆盖度。
- 组件稳定性。
- score trust / calibration 状态。

### 8.2 Level

`level_ready` 条件：

- `accepted_update_count >= level_ready_min_updates`。
- `sigma` 有限且不低于 `sigma_floor`。
- 最近输入支撑不是长期低样本 / 低分母。

`level_ready` 后：

- `enabled_components` 包含 `level`。
- 可以进入 `score_warming`。
- 只允许基于 level 的检测结论，不能暗示日 / 周 / 月周期已经成熟。

### 8.3 Daily

`daily_warming` 条件：

- 已达到 `level_ready`。
- 覆盖到足够 daily bins。

`daily_ready` 条件：

- 覆盖至少 `daily_ready_min_days` 个自然日。
- `daily_coverage_ratio >= daily_ready_coverage_ratio`。
- daily harmonic 单次更新稳定，没有持续大幅漂移。

`daily_ready` 后：

- `enabled_components` 包含 `daily`。
- daily harmonic 可进入检测 band 和评分。

### 8.4 Weekly

`weekly_warming` 条件：

- 已达到 `daily_ready`。
- 覆盖到足够 weekly bins。

`weekly_ready` 条件：

- 覆盖至少 `weekly_ready_min_weeks` 个自然周或等价周期。
- `weekly_coverage_ratio >= weekly_ready_coverage_ratio`。
- weekly harmonic 更新稳定。

`weekly_ready` 后：

- `enabled_components` 包含 `weekly`。
- weekly harmonic 可进入检测 band 和评分。

### 8.5 Monthly / Monthpos

`monthly_warming` 条件：

- 已达到 `weekly_ready`。
- 已跨至少 1 次自然月边界。
- monthpos residual 有有效更新。

`monthly_ready` 条件：

- `month_transition_count >= monthpos_min_month_transitions`。
- `monthpos_coverage_ratio >= monthpos_ready_coverage_ratio`。
- monthpos coeff 更新稳定。
- 未在最近一段时间内处于 `drift_learning`。

`monthly_ready` 后：

- `enabled_components` 包含 `monthpos`。
- monthpos effect 可进入 detection band 和评分。

## 9. Monthpos 在线更新

月位置只从 level/day/week 已解释后的 residual 中学习：

```text
monthpos_residual_t =
  y_model_t
  - level_t
  - daily_t
  - weekly_t
```

更新前提：

- 当前点 update-eligible。
- 不处于强异常跳过更新。
- 不处于 `drift_learning`。
- adapter 支撑足够。
- 当前月位置 basis 可计算。

更新：

```text
delta =
  clamp(monthpos_residual_t,
        -monthpos_delta_max,
        monthpos_delta_max)

active_monthpos_basis =
  BuildMonthposBasis(bucket_id)

for each active basis slot:
  monthpos_coeff[slot] =
    (1 - alpha_monthpos_eff * basis_value^2) * monthpos_coeff[slot]
    + alpha_monthpos_eff * basis_value * delta
```

其中：

```text
alpha_monthpos_eff =
  alpha_monthpos
  * update_weight_t
  * monthpos_stability_weight_t
```

原则：

- `alpha_monthpos` 必须小于 day/week 学习速度。
- 单次 `delta` 必须按 `sigma` 比例裁剪。
- monthpos coeff 不允许因单月异常快速变化。
- bootstrap monthpos seed 可初始化 coeff、center 和 coverage，但不能跳过在线 score trust 校准。

## 10. Bootstrap Seed 交接

B3 不回头修改 B1 bootstrap 训练链路，但需要扩展 rolling 初始化路径。

当前 B2 的 `InitializeRollingStateFromBootstrapSeed()` 只把 seed 映射成：

```text
level / trend / daily / weekly
sigma
covariance
accepted_update_count
confidence
state_status
```

它没有把 seed 的 `enabled_components`、`coverage_report`、`monthpos_hint` 或 `uncertainty_init.band_z` 持久化到 `RollingState`。因此 B3 必须在以下入口补齐 seed 到 B3 状态的映射：

```text
InitializeRollingStateFromBootstrapSeed(...)
WarmupRollingStatesFromBootstrapSeeds(...)
SubmitObservation() bootstrap lazy init
```

初始化完成后，`RollingState` 与 `BootstrapSeed` 仍然解耦；在线更新不能反写 seed，也不能依赖后续重新读取 seed。

对 B3 代码上线前已经存在、但缺少 B3 字段的 rolling state，一律按保守默认迁移：

```text
maturity_status = StatusFromLegacyStateStatus(state_status)
score_trust_status = score_untrusted
detection_band_multiplier = 1.0
monthpos_status = disabled
```

完整 seed 可初始化：

```text
maturity_status
learning_confidence
enabled_components candidate
coverage counters
monthpos coeff / center
monthpos coverage
detection_band_multiplier initial hint
```

但必须遵守：

- `score_trust_status` 最高只能初始化为 `score_warming`。
- 进入 `score_ready` 必须依赖流式阶段的 calibration evidence。
- seed 的 `confidence` 不能直接映射为 `can_alert = true`。
- seed 中缺少 monthpos support 时，不允许直接进入 `monthly_ready`。
- `detection_band_multiplier initial hint` 从 `seed.uncertainty_init.band_z / config.band_z` 派生，并按 `calibration_multiplier_min/max` 夹紧；seed 没有该字段时使用 `1.0`。
- `monthpos_hint` 初始化必须保留 centered basis 的 center 信息；只拷贝 coeff 会改变预测语义。

弱 seed / partial seed：

- 可提高 `level` 初值和初始尺度。
- 只能进入较低 maturity。
- score trust 必须从 `score_untrusted` 或 `score_warming` 开始。

## 11. Snapshot JSON 契约

### 11.1 Series Snapshot 扩展

在 B2 `rolling_series_snapshot` 基础上增加：

```json
{
  "maturity": {
    "status": "daily_ready",
    "learning_confidence": 0.72,
    "enabled_components": ["level", "daily"],
    "component_readiness": {
      "level": "ready",
      "daily": "ready",
      "weekly": "warming",
      "monthpos": "disabled"
    },
    "coverage": {
      "daily_ratio": 0.91,
      "weekly_ratio": 0.42,
      "monthpos_ratio": 0.13
    }
  },
  "score_trust": {
    "status": "score_warming",
    "score_confidence": 0.45,
    "can_alert": false,
    "reason": "calibration_warming"
  },
  "calibration": {
    "status": "warming",
    "band_multiplier": 1.35,
    "coverage_ewma": 0.96,
    "tail3_ewma": 0.01,
    "tail5_ewma": 0.0,
    "abs_z_ewma": 0.7,
    "calibration_update_count": 128
  },
  "monthpos": {
    "status": "monthly_warming",
    "month_transition_count": 1,
    "ready_count": 0
  }
}
```

### 11.2 Task Snapshot 扩展

`rolling_task_snapshot` 增加：

```json
{
  "maturity_status_counts": {
    "cold_learning": 0,
    "level_ready": 0,
    "daily_warming": 0,
    "daily_ready": 0,
    "weekly_warming": 0,
    "weekly_ready": 0,
    "monthly_warming": 0,
    "monthly_ready": 0
  },
  "score_trust_status_counts": {
    "score_untrusted": 0,
    "score_warming": 0,
    "score_ready": 0,
    "drift_learning": 0,
    "recalibrating": 0
  },
  "calibration_status_counts": {
    "uncalibrated": 0,
    "warming": 0,
    "calibrated": 0,
    "expanding": 0,
    "recalibrating": 0
  }
}
```

## 12. 配置

新增配置归属 `BaselineRollingConfig` 或等价内部配置结构。新增 / 修改配置必须同步：

- C++ 默认值。
- `plugins/baseline/config/baseline-config-template.yaml`。
- strict YAML schema。
- 配置解析和配置测试。

建议默认值：

| 参数 | 默认值 | 说明 |
|---|---:|---|
| `level_ready_min_updates` | `30` | level 可进入 ready 的最少有效更新数 |
| `score_warming_min_updates` | `60` | 进入 `score_warming` 的最少 calibration 点数 |
| `score_ready_min_updates` | `240` | 进入 `score_ready` 的最少 calibration 点数 |
| `score_recovery_min_updates` | `120` | drift 后恢复 score trust 的最少点数 |
| `score_drift_degrade_start` | `1.5` | 进入检测降级的 drift evidence 阈值 |
| `calibration_alpha` | `0.02` | coverage / tail EWMA 系数 |
| `calibration_warmup_min_updates` | `60` | 校准 evidence 暖机点数，暖机前不扩张 / 收缩 |
| `calibration_shrink_min_updates` | `240` | 允许 band 慢收缩的最少 calibration 点数 |
| `calibration_expand_rate` | `0.15` | band 快扩张比例 |
| `calibration_shrink_rate` | `0.01` | band 慢收缩比例 |
| `calibration_coverage_floor` | `0.98` | coverage 下限 |
| `calibration_tail3_limit` | `0.02` | `|Z| > 3` 比例上限 |
| `calibration_tail5_limit` | `0.002` | `|Z| > 5` 比例上限 |
| `calibration_multiplier_min` | `1.0` | band multiplier 下限 |
| `calibration_multiplier_max` | `6.0` | band multiplier 上限 |
| `daily_coverage_bins` | `24` | daily coverage bin 数 |
| `weekly_coverage_bins` | `168` | weekly coverage bin 数 |
| `daily_ready_min_days` | `2` | daily ready 最少自然日 |
| `daily_ready_coverage_ratio` | `0.75` | daily coverage ready 阈值 |
| `weekly_ready_min_weeks` | `2` | weekly ready 最少自然周 |
| `weekly_ready_coverage_ratio` | `0.70` | weekly coverage ready 阈值 |
| `maturity_uncertainty_cold_scale` | `9.0` | cold 阶段检测方差附加项，单位为 `sigma^2` |
| `maturity_uncertainty_warming_scale` | `4.0` | warming 阶段检测方差附加项，单位为 `sigma^2` |
| `maturity_uncertainty_drift_scale` | `4.0` | drift 阶段检测方差附加项，单位为 `sigma^2` |
| `maturity_uncertainty_recalibrating_scale` | `2.0` | recalibrating 阶段检测方差附加项，单位为 `sigma^2` |
| `missing_daily_uncertainty_scale` | `9.0` | daily 未 ready 时的组件缺失检测方差附加项 |
| `missing_weekly_uncertainty_scale` | `4.0` | weekly 未 ready 时的组件缺失检测方差附加项 |
| `level_only_extreme_z` | `8.0` | 仅 level ready 时允许 `can_alert` 的极端突刺阈值 |
| `monthpos_alpha` | `0.005` | monthpos 慢更新系数 |
| `monthpos_delta_max_scale` | `0.5` | monthpos 单次更新裁剪，单位为 `sigma` |
| `monthpos_min_month_transitions` | `2` | monthly ready 最少跨月次数 |
| `monthpos_ready_coverage_ratio` | `0.60` | monthpos ready coverage 阈值 |

默认值是保守 MVP 起点，后续可通过评估数据校准。

## 13. 实现任务

编码时以本设计各章节为准，任务表只描述实现顺序。

| 顺序 | 任务 | 参考章节 | 主要文件 | 完成标志 |
|---|---|---|---|---|
| `B3-T01` | 扩展 public result 字段与兼容映射 | 第 3 节 | `framework/interfaces/ibaseline_types.h`、`rolling_task_runner.*` | `RollingBaselineResult` 输出 maturity、score trust、confidence、`can_alert` 和 component evidence |
| `B3-T02` | 新增 maturity / score trust / calibration 状态 | 第 4 节、第 10 节 | `rolling/rolling_state.*` | `RollingState` 持有有界 B3 状态；bootstrap 初始化可把 seed 映射到 B3 状态；旧 `state_status` 可由 maturity 兼容映射 |
| `B3-T03` | 实现 detection band calibration | 第 5 节、第 7 节 | `rolling/detection_calibration.*`、`rolling/rolling_estimator.*` | 输出 calibrated detection band，支持快扩张、慢收缩和 tail evidence |
| `B3-T04` | 实现 score trust 状态机 | 第 6 节 | `rolling/score_trust.*` | 支持 `score_untrusted/warming/ready` 和 `drift_learning/recalibrating` 降级恢复 |
| `B3-T05` | 实现 maturity 与 component readiness | 第 8 节 | `rolling/maturity_gate.*` | level/day/week/monthpos 按 coverage 和稳定性逐步 ready |
| `B3-T06` | 实现 monthpos state 与慢更新 | 第 9 节 | `rolling/monthpos_state.*` | monthpos 可 stream-only warming/ready，未成熟前不参与高置信评分 |
| `B3-T07` | 接入 Value / Ratio task 与 snapshot | 第 10 节、第 11 节 | `task/value_task.*`、`task/ratio_task.*`、`rolling_task_runner.*` | task 输出和 snapshot 包含 B3 evidence |
| `B3-T08` | 补齐配置、模板、schema、CMake 和自动化测试 | 第 12 节、第 14 节 | `plugins/baseline/config/*`、`plugins/baseline/CMakeLists.txt`、`tests/test_baseline/*` | 配置解析和 B3 测试矩阵通过 |

## 14. 测试矩阵

| 场景 | 断言 |
|---|---|
| 无历史空启动 | 首点和冷启动阶段 `can_alert = false`，不会输出高置信异常 |
| score trust 正常推进 | `score_untrusted -> score_warming -> score_ready` 按配置阈值推进 |
| warming 阶段 | 可输出诊断 `Z-score`，但 `can_alert = false` 或 confidence capped |
| full bootstrap seed | 可提高 maturity 和初始 band 质量，但不能直接 `score_ready` 或 `can_alert = true` |
| weak / partial seed | 只能初始化较低 maturity，score trust 不能跳过校准 |
| 稳定窗口校准 | coverage、`|Z| > 3`、`|Z| > 5` 比例处于合理范围 |
| 短期平稳窗口 | detection band 不会快速收窄到误报 |
| 短期 tail 升高 | `detection_band_multiplier` 快速扩张 |
| 校准暖机 | `calibration_update_count` 未达阈值前只积累 evidence，不触发 band 扩张到上限 |
| 长期稳定 | `detection_band_multiplier` 慢速收缩且不低于下限 |
| update_z 与 detection_z 分离 | update gate 使用内部 `update_z`，输出 `z_score` 使用 calibrated detection band |
| pre-update 输出语义 | public result、detection_z 和 calibration evidence 使用 pre-update estimator；update result 只更新 state |
| level-only 检测 | daily 未 ready 前只能对 `level_only_extreme_z` 以上突刺 `can_alert`，或因组件缺失扩宽 band 后保持低置信 |
| level shift | 进入 `drift_learning`，降低 score trust，允许 level 加速学习 |
| drift 恢复 | 从 `drift_learning -> recalibrating -> score_warming/ready` 恢复，不直接跳回 ready |
| sampled value 低样本 | 降低 score trust 或禁用 score，不更新 calibration evidence |
| ratio 低分母 | 降低 score trust 或禁用 score，不更新 calibration evidence |
| daily readiness | 覆盖足够 daily bins 后 `daily_ready`，`enabled_components` 包含 `daily` |
| weekly readiness | 覆盖足够 weekly bins 后 `weekly_ready`，`enabled_components` 包含 `weekly` |
| monthpos 未跨月 | 不进入 `monthly_ready`，不参与高置信评分 |
| monthpos ready 更新 | prediction 使用 state + bucket；detection center 加 monthpos effect；B2 core update 使用 `y_model - active_monthpos` |
| monthpos 单月异常 | 不快速污染 monthpos coeff，不使 monthpos ready |
| monthpos 跨月成熟 | 满足跨月次数和 coverage 后进入 `monthly_ready` |
| bootstrap monthpos hint | centered basis 的 coeff 和 center 均被映射；缺失 center 时不得声明完整 monthly ready |
| snapshot schema | series / task snapshot 包含 maturity、score trust、calibration 和 monthpos 字段 |
| public ABI 边界 | public header 不暴露 `BootstrapSeed` 或内部状态结构 |
| relation 非目标 | Relation task 不新增 rolling 提交接口 |
| 旧链路隔离 | 测试证明不触发 `shadow/candidate/rebuild` |

## 15. 阶段完成门禁

`B3` 完成前必须满足：

- 冷启动、warming、`drift_learning`、`recalibrating` 阶段不会输出高置信异常判定。
- 稳定窗口的 detection band 经过 coverage / tail evidence 校准。
- `learning confidence` 与 `score trust` 在输出中可区分。
- 月位置可以 stream-only 学习和保守成熟。
- 完整 bootstrap seed 不能绕过 score trust 校准。
- 代码和测试均不恢复旧 `shadow/candidate/rebuild` 主路径。

如果 `B3` 只实现组件成熟标签，而没有承担 `Z-score` 可信度和 detection band 校准职责，本阶段不得标记完成。

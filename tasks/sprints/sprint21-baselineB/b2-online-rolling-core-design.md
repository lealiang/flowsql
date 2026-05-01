# B2 Online Rolling Core MVP 阶段设计

## 1. 目标与非目标

`B2` 的目标是实现 `T1/T2` 的在线滚动基线主路径：

```text
stream observation -> rolling prediction band -> residual score -> gated update
```

必须完成：

- `T1/T2` 以 `SubmitObservation(...)` 作为唯一对外滚动入口，首次提交时根据同一 task 内部的 `BootstrapSeed[series_key]` 或空启动语义内部懒初始化。
- 每个有效 bucket 输出 baseline band，而不是只输出中心点。
- 强异常点可跳过或降权更新，避免污染 baseline。
- `level/trend/day/week`、残差尺度、漂移证据在线滚动更新。
- 高基数 `Series` 状态有固定上限、TTL / LRU 清理和可观测指标。

本阶段不做：

- 完整 `Maturity Gate`、组件成熟度状态机和月位置在线成熟，这些归 `B3`。
- `T3` stream-only basis 刷新，这归 `B4`。
- 任何旧 `shadow/candidate/rebuild` 恢复链路。
- `RollingState` 持久化格式；重启恢复后续单独设计。
- relation 分布本身的在线 rolling 建模。

`B2` 只允许使用粗粒度状态：

```text
cold_learning
warming
ready_hint
```

这些状态只服务冷启动保护，不表示组件成熟。

## 2. Public ABI 与任务边界

`B2` 沿用 `B1` 的统一任务模型，不新增独立 rolling service。

- `IBaselineService` 继续负责创建任务。
- `IBaselineValueTask` 和 `IBaselineRatioTask` 只对外暴露 rolling 提交能力，初始化由 task 内部 lazy 触发。
- `IBaselineRelationTask` 不增加 rolling 提交接口，只能继续查询 `B1` bootstrap basis / seed。
- `BootstrapSeed` 仍是 baseline plugin 内部结构，不进入 `framework/interfaces` public ABI。

新增 public 类型：

```cpp
namespace flowsql {

struct RollingSubmitOptions {
    bool allow_auto_init_from_bootstrap = true;
    bool allow_auto_init_from_empty = true;
};

struct ValueRollingObservation {
    std::string series_key;
    int64_t bucket_id = 0;
    double value = 0.0;
    uint64_t sample_count = 0;
};

struct RatioRollingObservation {
    std::string series_key;
    int64_t bucket_id = 0;
    double numerator = 0.0;
    double denominator = 0.0;
};

struct RollingBaselineResult {
    BaselineStatus status = BaselineStatus::kOk;

    std::string series_key;
    int64_t bucket_id = 0;

    double observed = 0.0;
    double observed_model = 0.0;

    double baseline_mu = 0.0;
    double baseline_lower = 0.0;
    double baseline_upper = 0.0;
    double band_width = 0.0;

    double model_mu = 0.0;
    double model_lower = 0.0;
    double model_upper = 0.0;

    double residual = 0.0;
    double band_std = 0.0;
    double z_score = 0.0;

    bool is_outside_band = false;
    bool can_score = false;
    bool can_update = false;
    double score_weight = 0.0;
    double update_weight = 0.0;

    double confidence = 0.0;
    std::string state_status;
    std::vector<std::string> uncertainty_source;

    double drift_evidence = 0.0;
    double adapt_boost = 0.0;

    uint64_t sample_count = 0;
    bool skipped_low_sample_count = false;

    double numerator = 0.0;
    double denominator = 0.0;
    bool skipped_low_denominator = false;

    std::string diagnostics;
};

struct RollingPrediction {
    BaselineStatus status = BaselineStatus::kOk;
    double baseline_mu = 0.0;
    double baseline_lower = 0.0;
    double baseline_upper = 0.0;
    double band_z = 0.0;

    bool ok() const { return status == BaselineStatus::kOk; }
};

}  // namespace flowsql
```

Value task 接口扩展示意：

```cpp
virtual RollingBaselineResult SubmitObservation(
    const ValueRollingObservation& obs,
    const RollingSubmitOptions& options) = 0;

virtual RollingPrediction PredictRolling(
    std::string_view series_key,
    int64_t bucket_id) const = 0;
```

Ratio task 接口扩展示意：

```cpp
virtual RollingBaselineResult SubmitObservation(
    const RatioRollingObservation& obs,
    const RollingSubmitOptions& options) = 0;

virtual RollingPrediction PredictRolling(
    std::string_view series_key,
    int64_t bucket_id) const = 0;
```

输出边界：

- B2 core 输出 `RollingBaselineResult`。
- `PredictRolling()` 是测试与验收用的只读预测接口，只消费已有 rolling state，不更新状态、不触发 bootstrap / empty lazy init，不携带 `options` 或 `diagnostics`。`band_z` 用于评估程序根据实际观测值计算 directional z-score，并统计 `|Z| > 3` / `|Z| > 5`。
- 旧 `DetectorResult` 映射只允许放在 task 层或兼容层，core 不直接依赖旧异常检测结果结构。
- snapshot 可输出 band、state_status、update_weight、drift_evidence 和状态规模。

Seed 交接约定：

- `BootstrapSeed` 归属 task，但模型参数粒度是 `series_key`。
- task 内部形态可以理解为 `BootstrapSeedStore[series_key]`。
- B2 在 task 内部应优先对 `BootstrapSeedStore` 做批量 warm-up：一次性遍历全部 seed，批量初始化对应的 `RollingState`。
- warm-up 只是一条内部优化路径，不是对外 API；`SubmitObservation()` 在未命中预热状态时必须仍然能按 seed / 空启动语义完成懒初始化。
- B2 核心初始化只校验 rolling-critical 字段：`task_identity`、`series_key`、`feature_type`、`profile`、`clock_spec`、`calendar_ref`、`theta_init`、`sigma_init`、`uncertainty_init`、`maturity_init` 和 `coverage_report`。`monthpos_hint`、`event_hint`、`relation_basis_by_metric`、`relation_routed_summary_seeds` 这类扩展项不参与 B2 核心初始化，仅保留诊断或原样存储，不作为兼容性判定条件。
- `ExportBootstrapSeed(format)` 是 task 级全量序列化接口，导出全部 series seed；这不等于 task 级模型参数。
- `B1 -> B2` 主路径不得走 `ExportBootstrapSeed(JSON) -> parse/load -> 内部 bootstrap 懒初始化`。

### Snapshot JSON 契约

`QuerySeriesSnapshot(series_key, format)` 返回单个 `Series` 的滚动状态快照，`QueryTaskSnapshot(format)` 返回 task 级汇总快照。两者都必须保留 `schema_version`、`document_kind`、`task_identity`，并使用稳定、可扩展的 JSON 结构。

`QuerySeriesSnapshot` 最小 schema：

```json
{
  "schema_version": 1,
  "document_kind": "rolling_series_snapshot",
  "task_identity": {},
  "series_identity": {
    "series_key": ""
  },
  "state_status": "cold_learning|warming|ready_hint",
  "has_seen_observation": false,
  "last_seen_bucket": 0,
  "accepted_update_count": 0,
  "confidence": 0.0,
  "band": {
    "baseline_mu": 0.0,
    "baseline_lower": 0.0,
    "baseline_upper": 0.0,
    "band_width": 0.0
  },
  "control": {
    "can_score": false,
    "can_update": false,
    "update_weight": 0.0,
    "drift_evidence": 0.0
  },
  "state_size_bytes": 0,
  "diagnostics": ""
}
```

`QueryTaskSnapshot` 最小 schema：

```json
{
  "schema_version": 1,
  "document_kind": "rolling_task_snapshot",
  "task_identity": {},
  "rolling_series_count": 0,
  "state_status_counts": {
    "cold_learning": 0,
    "warming": 0,
    "ready_hint": 0
  },
  "rolling_state_created_total": 0,
  "rolling_state_evicted_total": 0,
  "rolling_state_memory_estimate_bytes": 0,
  "diagnostics": ""
}
```

`SubmitObservation()` 自动初始化顺序：

```text
已有 RollingState
  -> 直接使用已有状态
无 RollingState 且同 key BootstrapSeed 可用
  -> 内部 bootstrap 懒初始化
无 RollingState 且无可用 seed
  -> 内部空启动懒初始化
```

后两步分别由 `allow_auto_init_from_bootstrap` 和 `allow_auto_init_from_empty` 控制。

## 3. 输入语义与观测适配

`B2` 明确区分 3 类输入语义，但共享同一套 rolling estimator：

```text
value_basic   -> ValueBasicObservationAdapter
value_sampled -> ValueSampledObservationAdapter
ratio         -> RatioObservationAdapter

ObservedModelPoint -> RollingStateEstimator
```

分类边界：

- `value_basic`：对应 `T1a`，用于非负总量、次数、基数、固定窗口强度量，例如 `bps`、`pps`、`conn_count_total`。`sample_count` 对它没有语义。
- `value_sampled`：对应 `T1b`，用于 bucket 内由原始样本聚合出的连续统计值，例如 `avg_rtt`、`p95_rtt`。`sample_count` 是可靠性输入。
- `ratio`：对应 `T2`，用于成功率、失败率、占比等比例指标，输入为 `numerator / denominator`。

模型空间：

```text
value: log1p(value)              // MVP 默认
ratio: logit(clipped(numerator / denominator))
```

约束：

- `value` 使用 `log1p` 时，输入必须非负；负值返回 `kInvalidArgument`。
- profile 后续可指定 `identity`，但 `identity` 必须显式配置 `sigma_floor`。
- seed 的 `theta_init.model_space`、`sigma_init.model_space`、任务 transform 必须一致；不一致返回 `kIncompatibleArtifact`，不做静默降级。

观测适配器输出：

```text
ObservedModelPoint = {
  y_model_t,
  extra_obs_noise_t,
  score_weight_t,
  update_weight_t,
  can_score,
  can_update,
  uncertainty_source
}
```

`value_basic`：

```text
can_score = true
can_update = true
score_weight_t = 1
update_weight_t = 1
extra_obs_noise_t = 0
```

`value_sampled` 分档：

| 条件 | 行为 |
|---|---|
| `sample_count <= 0` | `kInvalidArgument` |
| `0 < sample_count < n_min_score` | 不评分、不更新，`skipped_low_sample_count = true` |
| `n_min_score <= sample_count < n_min_update` | 低置信评分，低权重更新，band 扩宽 |
| `sample_count >= n_min_update` | 正常评分和更新 |

工程规则：

```text
sample_weight = min(1, sample_count / n_ref)
update_weight_t *= sample_weight
extra_obs_noise_t += sample_count_noise * sigma_t^2 / max(sample_count, 1)
```

`ratio` 合法性：

```text
denominator > 0
0 <= numerator <= denominator
```

不满足时返回 `kInvalidArgument`，不进入 `logit`。

合法 ratio 分档：

| 条件 | 行为 |
|---|---|
| `0 < denominator < d_min_score` | 不评分、不更新，`skipped_low_denominator = true` |
| `d_min_score <= denominator < d_min_update` | 低置信评分，低权重更新，band 扩宽 |
| `denominator >= d_min_update` | 正常评分和更新 |

工程规则：

```text
update_weight_t *= min(1, denominator / d_ref)
extra_obs_noise_t += ratio_denominator_noise * sigma_t^2 / max(denominator, 1)
```

## 4. RollingState 与配置

每个 `Series = (series_key, feature)` 维护一个 `RollingState`。

状态字段必须有界：

```text
level / trend
P_level_trend 2x2 block
day harmonic coeffs + diagonal covariance
week harmonic coeffs + diagonal covariance
sigma
short_ewma / long_ewma / drift_evidence
state_status
has_seen_observation / last_seen_bucket / counters
diagnostics summary
```

禁止保存：

```text
per-bucket 历史窗口
HistoryReader 句柄
shadow/candidate/rebuild 状态
relation group 级动态状态
无上限 diagnostics 列表
```

配置归属：

- 所有 rolling 参数归 `BaselineRollingConfig` 或等价内部配置结构。
- 默认值必须同时落在 C++ 默认构造和 `plugins/baseline/config/baseline-config-template.yaml`；新增 / 修改 rolling 参数时必须同步更新 strict YAML schema、配置模板和测试。
- B2 rolling 的 day/week 阶数以 `rolling_config.daily_harmonic_order` / `rolling_config.weekly_harmonic_order` 为准，不再隐式读取 B1 bootstrap 的 `shared_profile_config` 覆盖值。
- 观测适配器、状态估计器、gate、drift、residual scale 只能读取已解析配置。
- 新增 `rolling/*.cpp` 必须加入 `plugins/baseline/CMakeLists.txt`。

MVP 默认值：

| 参数 | 默认值 | 说明 |
|---|---:|---|
| `n_min_score` | `3` | sampled value 低于该样本数不评分、不更新 |
| `n_min_update` | `10` | sampled value 低于该样本数低权重更新 |
| `n_ref` | `10` | sampled value 样本数权重归一化 |
| `sample_count_noise` | `1.0` | 低样本数额外观测噪声系数 |
| `d_min_score` | `10` | ratio 低于该分母不评分、不更新 |
| `d_min_update` | `100` | ratio 低于该分母低权重更新 |
| `d_ref` | `100` | ratio 分母权重归一化 |
| `ratio_denominator_noise` | `1.0` | 低分母额外观测噪声系数 |
| `z_downweight` | `3.0` | 开始降权更新的 `z_score` |
| `z_skip` | `5.0` | 强异常跳过更新的 `z_score` |
| `small_update_weight` | `0.2` | 疑似异常低权重更新 |
| `daily_harmonic_order` | `6` | 日周期 harmonic 阶数 |
| `weekly_harmonic_order` | `3` | 周周期 harmonic 阶数 |
| `level_learning_scale` | `1.0` | level 学习速度 |
| `day_learning_scale` | `0.2` | day 相对 level 学习速度 |
| `week_learning_scale` | `0.05` | week 相对 level 学习速度 |
| `cold_day_learning_scale` | `0.05` | 冷启动 day 学习速度 |
| `cold_week_learning_scale` | `0.01` | 冷启动 week 学习速度 |
| `seasonal_drift_min_scale` | `0.1` | drift 时 day/week 学习下限 |
| `day_delta_coeff_max` | `0.05 * sigma` | day 单系数单次更新上限 |
| `week_delta_coeff_max` | `0.02 * sigma` | week 单系数单次更新上限 |
| `Q_day` | `1e-4 * sigma^2` | day 过程噪声 |
| `Q_week` | `2e-5 * sigma^2` | week 过程噪声 |
| `Q_level` | `1e-3 * sigma^2` | level 过程噪声 |
| `Q_trend` | `1e-5 * sigma^2` | trend 过程噪声 |
| `trend_update_scale` | `0.05` | trend 相对 level 更新速度 |
| `cold_trend_update_scale` | `0.0` | 冷启动 trend 默认不更新 |
| `trend_delta_max` | `0.01 * sigma` | trend 单次更新上限 |
| `trend_abs_max` | `0.1 * sigma` | trend 绝对值上限 |
| `P_level_init` | `9.0 * sigma_init^2` | 空启动 level 初始不确定性 |
| `P_trend_init` | `16.0 * sigma_init^2` | 空启动 trend 初始不确定性 |
| `P_day_init` | `4.0 * sigma_init^2` | day 初始不确定性 |
| `P_week_init` | `9.0 * sigma_init^2` | week 初始不确定性 |
| `P_floor` | `1e-6 * sigma_floor^2` | 协方差下限 |
| `P_cap` | `1e6 * sigma_floor^2` | 协方差上限 |
| `alpha_short` | `0.05` | drift 短期 EWMA 系数 |
| `alpha_long` | `0.005` | drift 长期 EWMA 系数 |
| `z_cap` | `5.0` | drift residual 裁剪上限 |
| `drift_start` | `1.0` | 开始增强学习的 drift evidence |
| `drift_full` | `2.0` | 达到最大增强的 drift evidence |
| `max_level_boost` | `4.0` | level 学习最多增强到 `5` 倍 |
| `max_q_boost` | `9.0` | `Q_level` 最多增强到 `10` 倍 |
| `skip_relax` | `2.0` | drift 时 `z_skip` 放宽量 |
| `process_noise_gap_cap_buckets` | `7 * day_buckets` | gap 过程噪声放大上限 |
| `alpha_sigma` | `0.02` | sigma EWMA 系数 |
| `c_sigma` | `3.0` | sigma 更新 residual 裁剪倍数 |
| `sigma_floor` | `0.05` | `log1p/logit` 模型空间默认下限 |
| `cold_start_band_scale` | `3.0` | 冷启动 band 放大倍数 |
| `band_z` | `3.0` | 模型空间 band 倍数 |
| `confidence_cold` | `0.2` | 冷启动 confidence |
| `confidence_warming` | `0.5` | warming confidence |
| `confidence_ready_hint_cap` | `0.8` | B2 ready hint confidence 上限 |
| `min_warming_updates` | `3` | 进入 warming 的最少有效更新数 |
| `min_ready_hint_updates` | `day_buckets` | 进入 ready hint 的最少有效更新数 |

`alpha_short` / `alpha_long` 是可配置的 EWMA 系数，不是固定窗口长度。默认值在分钟级 bucket 下的近似含义：

- `alpha_short = 0.05`：有效观察长度约 `1 / alpha = 20` 个点，半衰期约 `13.5` 个点，约 14 分钟。
- `alpha_long = 0.005`：有效观察长度约 `1 / alpha = 200` 个点，半衰期约 `138` 个点，约 2 小时 18 分钟。
- 非分钟级 bucket 时，时间长度随 `bucket_seconds` 等比例缩放。

派生时间参数：

```text
day_buckets = max(1, ceil(86400 / bucket_seconds))
week_buckets = max(1, ceil(604800 / bucket_seconds))
```

`bucket_seconds` 必须大于 `0`。日 / 周 phase 必须用 `bucket_id * bucket_seconds` 转换到配置 `timezone` 后计算，不能只用 `bucket_id % day_buckets`。

## 5. 在线算法主流程

B2 的算法组合来源是 Dynamic Harmonic Regression + block-diagonal Kalman/RLS + robust residual scale + anomaly-gated update + lightweight adaptive forgetting；参考资料见 [reference.md](reference.md)。`ADWIN` 只作为后续可演进方向参考，MVP 不实现完整自适应滑窗。

每个可处理 bucket 执行：

```text
validate -> auto_init -> predict -> band -> score -> gate -> update
```

### 5.1 时间推进

```text
dt = bucket_id - last_seen_bucket
```

| 场景 | 行为 |
|---|---|
| 首个有效 bucket | 创建状态或使用 pending empty state，`dt = 0`，走首点初始化 |
| `bucket_id > last_seen_bucket` | 按 `dt` 推进状态 |
| `bucket_id > last_seen_bucket + 1` | 不补点，放大过程噪声，记录 `gap` |
| `bucket_id == last_seen_bucket` | 返回 `kInvalidArgument`，不二次更新 |
| `bucket_id < last_seen_bucket` | 返回 `kInvalidArgument`，不回滚 |

gap 过程噪声：

```text
dt_q = min(max(dt, 1), process_noise_gap_cap_buckets)
Q_eff = Q_base * dt_q
```

### 5.2 模型形式

```text
y_hat_model_t =
  level_t
  + daily_harmonic_t
  + weekly_harmonic_t
```

`trend` 只进入状态转移：

```text
level_t^- = level_{t-1} + trend_{t-1} * dt
trend_t^- = trend_{t-1}
```

Harmonic 特征：

```text
daily_t =
  Σ_k a_day_k * sin(2πk * phase_day_t)
    + b_day_k * cos(2πk * phase_day_t)

weekly_t =
  Σ_k a_week_k * sin(2πk * phase_week_t)
    + b_week_k * cos(2πk * phase_week_t)
```

MVP 使用：

```text
level/trend: 2x2 covariance block
day/week: diagonal covariance
```

不使用 `3x3 local quadratic trend`；二次趋势和更复杂外推不进入 B2。

### 5.3 预测、band 与评分

```text
theta_t^- = F_t * theta_{t-1}
P_t^-     = F_t * P_{t-1} * F_t^T + Q_t

y_hat_t   = H_t * theta_t^-
residual  = y_model_t - y_hat_t

pred_var_t = H_t * P_t^- * H_t^T
R_t        = sigma_t^2 + sigma_floor^2 + extra_obs_noise_t
obs_var_t  = pred_var_t + R_t
band_std_t = sqrt(obs_var_t)

lower_model_t = y_hat_t - band_z * band_std_t
upper_model_t = y_hat_t + band_z * band_std_t
z_score       = abs(residual / band_std_t)
```

输出必须反变换到观测空间：

- `log1p` value：`expm1()`，下界裁剪到 `0`。
- `identity` value：不做非负裁剪。
- ratio：`sigmoid()`。

`can_score = false` 时，仍可输出预测 band，但异常评分无效。

### 5.4 异常门控与自适应遗忘

漂移证据使用短 / 长 EWMA，不引入粘性 `drift_mode`：

```text
resid_norm_t = clamp(residual / band_std_t, -z_cap, z_cap)

short_ewma_t =
  (1 - alpha_short) * short_ewma_{t-1}
  + alpha_short * resid_norm_t

long_ewma_t =
  (1 - alpha_long) * long_ewma_{t-1}
  + alpha_long * resid_norm_t

drift_evidence_t = short_ewma_t - long_ewma_t

adapt_boost_t =
  clamp((abs(drift_evidence_t) - drift_start) /
        (drift_full - drift_start),
        0,
        1)

Q_level_eff =
  Q_level * (1 + adapt_boost_t * max_q_boost)
```

门控：

```text
skip_threshold_t = z_skip + adapt_boost_t * skip_relax

if abs(z_t) >= skip_threshold_t:
    gate_update_weight_t = 0
elif abs(z_t) >= z_downweight:
    gate_update_weight_t = small_update_weight
else:
    gate_update_weight_t = 1

base_update_weight_t = adapter.update_weight_t * gate_update_weight_t
```

原则：

- 孤立突刺通常跳过或降权。
- 持续偏移提高 `adapt_boost_t`，主要加快 level。
- drift 时不快速改写 day/week。

### 5.5 状态更新

Kalman/RLS gain：

```text
S_t = R_t
      + h_lt * P_lt^- * h_lt^T
      + Σ_i h_day_i^2 * P_day_i^-
      + Σ_j h_week_j^2 * P_week_j^-

K_lt     = P_lt^- * h_lt^T / S_t
K_day_i  = P_day_i^- * h_day_i / S_t
K_week_j = P_week_j^- * h_week_j / S_t
```

组件更新权重：

```text
seasonal_drift_scale =
  max(seasonal_drift_min_scale, 1 - adapt_boost_t)

level_boost_t = 1 + adapt_boost_t * max_level_boost

W_level = min(1, base_update_weight_t * level_learning_scale * level_boost_t)
W_trend = min(W_level, base_update_weight_t * trend_update_scale)

W_day_i =
  base_update_weight_t * day_learning_scale * seasonal_drift_scale

W_week_j =
  base_update_weight_t * week_learning_scale * seasonal_drift_scale
```

冷启动时：

```text
W_trend = min(W_level, base_update_weight_t * cold_trend_update_scale)
W_day_i = base_update_weight_t * cold_day_learning_scale * seasonal_drift_scale
W_week_j = base_update_weight_t * cold_week_learning_scale * seasonal_drift_scale
```

状态增量：

```text
D_lt = diag(W_level, W_trend)
delta_lt = D_lt * K_lt * residual_t

delta_day_i = W_day_i * K_day_i * residual_t
delta_week_j = W_week_j * K_week_j * residual_t
```

裁剪：

```text
abs(delta_trend) <= trend_delta_max
abs(delta_day_i) <= day_delta_coeff_max
abs(delta_week_j) <= week_delta_coeff_max
abs(trend) <= trend_abs_max
```

协方差更新：

```text
A_lt = I - D_lt * K_lt * h_lt
P_lt = A_lt * P_lt^- * A_lt^T
       + D_lt * K_lt * R_t * K_lt^T * D_lt^T

P_day_i =
  (1 - W_day_i * K_day_i * h_day_i)^2 * P_day_i^-
  + (W_day_i * K_day_i)^2 * R_t

P_week_j =
  (1 - W_week_j * K_week_j * h_week_j)^2 * P_week_j^-
  + (W_week_j * K_week_j)^2 * R_t
```

更新后强制：

- `P_lt` 对称化。
- 所有 `P_*` 执行 `P_floor/P_cap` 夹紧。
- 非有限值、`S_t <= 0`、`R_t <= 0` 返回 `kInvalidArgument`，不更新该点。

### 5.6 Residual Scale

```text
clipped_resid2 = min(residual_t^2, c_sigma^2 * sigma_t^2)
scale_update_weight = can_update ? base_update_weight_t : 0
alpha_eff = alpha_sigma * scale_update_weight

sigma_t^2 =
  (1 - alpha_eff) * sigma_{t-1}^2
  + alpha_eff * clipped_resid2

sigma_t >= sigma_floor
```

`base_update_weight_t = 0` 时，`sigma_t` 保持不变。

## 6. 启动与自动初始化

### 6.1 空启动的内部懒初始化

空启动不再作为 public ABI 暴露；它是 `SubmitObservation()` 在未命中 bootstrap seed、且允许空启动时的内部懒初始化分支。其作用域仍然只针对指定 `series_key`。

`series_key` 为空时返回 `kInvalidArgument`，不创建 state。

空启动：

```text
theta = zero
sigma_init = sigma_floor
P_level/P_trend/P_day/P_week = config init P
confidence = confidence_cold
state_status = cold_learning
```

首个 update-eligible 点：

```text
level = y_model_0
trend = 0
sigma = sigma_init
last_seen_bucket = bucket_id
accepted_update_count = 1

can_score = false
can_update = true
z_score = 0
is_outside_band = false
uncertainty_source includes cold_start, first_observation
```

首点可以输出宽 band，但不能输出有效异常评分。

pending empty state 遇到非 update-eligible 点时，返回对应错误或低支撑状态，不设置 `last_seen_bucket`，不消耗首点初始化机会。

已有 rolling state 时：

| `force_reset_existing_state` | 行为 |
|---|---|
| `false` | 返回 `kInvalidArgument`，不清空该 key 状态 |
| `true` | 先验证 seed 是否存在且兼容，再用 seed 原子性重建该 `series_key` 的 rolling state；校验失败时保留旧 state，不产生中间态 |

### 6.2 Bootstrap Seed 的内部初始化

从 `BootstrapSeed[series_key]` 内部初始化：

```text
anchor_bucket =
  max(seed.theta_init.reference_bucket_id,
      seed.coverage_report.train_end_bucket)

level_at_anchor =
  seed.theta_init.level
  + seed.theta_init.trend
    * (anchor_bucket - seed.theta_init.reference_bucket_id)

state.theta.level = level_at_anchor
state.theta.trend = seed.theta_init.trend
state.theta.daily = seed.theta_init.daily_harmonic
state.theta.weekly = seed.theta_init.weekly_harmonic
state.sigma_init = seed.sigma_init.value
state.sigma = seed.sigma_init.value
state.P = MapSeedUncertaintyToCovariance(seed, config)
state.confidence = min(seed.maturity_init.confidence, confidence_ready_hint_cap)
state.has_seen_observation = true
state.last_seen_bucket = anchor_bucket
state.accepted_update_count =
  min(seed.maturity_init.accepted_count, min_ready_hint_updates)
state.state_status = StatusFromAcceptedUpdateCount(state.accepted_update_count)
state.short_ewma = 0
state.long_ewma = 0
state.drift_evidence = 0
```

`theta_init.reference_bucket_id` 是 `theta.level` 的时间锚点。B2 初始化时必须把 `level` 平移到 `anchor_bucket`，否则长历史训练后第一次流式点会被误判为一个超大 gap。

兼容性必须校验：

```text
task identity
series_key
feature_type
profile
clock_spec
calendar_ref
model_space / transform_kind
```

`BootstrapSeed -> RollingState.P` 映射：

```text
seed_status_scale:
  full    = 0.25
  partial = 0.5
  weak    = 2.0

coverage_scale =
  clamp(1 / max(seed.coverage_report.coverage_ratio, 0.25), 1, 4)

P_component =
  config.P_component_init
  * seed.uncertainty_init.component_uncertainty.<component>_scale
  * seed_status_scale
  * coverage_scale
```

约束：

- `seed_status = none` 不能初始化。
- `seed.uncertainty_init.available = false` 返回 `kInsufficientData`。
- 缺失 harmonic 项补 `0`，超出当前 profile 阶数的项忽略并记录 diagnostics。
- 初始化后 `RollingState[series_key]` 与 `BootstrapSeed[series_key]` 解耦；在线更新不能反写 seed。

失败语义：

| 场景 | 返回 | 状态变化 |
|---|---|---|
| task 内无同 key seed | `kNotTrained` | 不创建 rolling state |
| seed 不兼容 | `kIncompatibleArtifact` | 不改变已有 state |
| seed 缺少 `theta_init/sigma_init/uncertainty_init` | `kInsufficientData` | 不改变已有 state |
| 该 key 已有 state 且不强制 reset | `kInvalidArgument` | 不覆盖 state |
| 该 key 已有 state 且强制 reset | `kOk` 或具体失败码 | 只重置该 key |
| seed 为 `weak/partial` | `kOk` | 可初始化，但使用更大初始 `P` 和更低 confidence |

### 6.3 SubmitObservation 自动初始化失败语义

| 场景 | 返回 | 状态变化 |
|---|---|---|
| `series_key` 为空 | `kInvalidArgument` | 不创建 state |
| 观测基础合法性失败 | `kInvalidArgument` | 不创建 state，不更新已有 state |
| 无 state，且两个 auto init 开关都为 `false` | `kNotTrained` | 不创建 state |
| 无 state，允许 bootstrap，但无同 key seed，且禁止空启动 | `kNotTrained` | 不创建 state |
| 无 state，同 key seed 不兼容 | `kIncompatibleArtifact` | 不回退空启动 |
| 无 state，首点支撑不足 | `kInsufficientData` | 不创建 state |
| 已有 state，观测支撑不足 | `kOk` | 输出低置信预测，按 adapter 规则禁用评分或更新 |

### 6.4 批量 warm-up

B2 的推荐初始化路径是 task 内部批量 warm-up，而不是外部按 `series_key` 逐个拉取 seed。

```text
WarmupRollingFromBootstrapStore(task)
  -> 遍历 BootstrapSeedStore[series_key]
  -> 对每个 series_key 构造或更新 RollingState
  -> 记录成功 / 失败 / 跳过统计
```

约束：

- 该过程只使用 task 内部的内存态 seed，不重新 parse JSON。
- 该过程应尽量在 task 进入 rolling 可用状态时完成，避免首次流式点触发大规模冷启动抖动。
- 已存在的 `RollingState[series_key]` 不应被无条件覆盖，除非显式 reset。
- 新到达、但不在 seed store 中的 `series_key`，仍按 `SubmitObservation()` 的空启动或自动初始化语义处理。
- 批量 warm-up 的实现必须可观测：至少记录 warm-up 成功数、失败数、跳过数和耗时。

## 7. 状态边界与阶段边界

高基数配置：

```yaml
rolling_state:
  max_series_per_task: 100000
  inactive_ttl_buckets: "max(7 * day_buckets, 2 * week_buckets)"
  cleanup_interval_buckets: "day_buckets"
  max_cleanup_per_run: 1000
```

清理策略：

1. 清理关闭任务下的状态。
2. 清理超过 `inactive_ttl_buckets` 且成熟度低的状态。
3. 超过 `max_series_per_task` 时按 `last_seen_bucket` 做 LRU。
4. 被淘汰的 series 再次到达时优先同 key seed，没有 seed 再空启动。

必须可观测：

```text
rolling_series_count
rolling_state_created_total
rolling_state_evicted_total
rolling_state_evicted_ttl_total
rolling_state_evicted_limit_total
rolling_state_cleanup_runs_total
rolling_state_cleanup_scanned_total
rolling_state_memory_estimate_bytes
```

B2 最小状态推进：

```text
accepted_update_count < min_warming_updates:
  state_status = cold_learning
  confidence = confidence_cold

min_warming_updates <= accepted_update_count < min_ready_hint_updates:
  state_status = warming
  confidence = confidence_warming

accepted_update_count >= min_ready_hint_updates:
  state_status = ready_hint
  confidence <= confidence_ready_hint_cap
```

B3 负责：

```text
score_trust / z-score trust
detection band calibration
level_ready
daily_warming / daily_ready
weekly_warming / weekly_ready
monthly_warming / monthly_ready
component_readiness
maturity/confidence 绑定
```

B3 需要把 learning confidence 与 score trust 分开处理：B2 可以输出预测 band 和粗粒度 `can_score`，但冷启动、warming、level shift 学习和 band 重新校准期间，`Z-score` 是否能用于异常判定由 B3 负责。

Relation 边界：

- B2 不提供 relation rolling 提交接口。
- `T3 routed summary` 后续作为 `value_basic` / `value_sampled` / `ratio` series 复用 T1/T2 rolling core。
- relation basis 在线刷新归 `B4`。

## 8. 实现任务与测试矩阵

### 8.1 实现任务

编码时以“参考章节”为准，任务表只描述实现顺序，不重复算法细节。

| 顺序 | 任务 | 参考章节 | 主要文件 | 完成标志 |
|---|---|---|---|---|
| `B2-T01` | 补齐 rolling public 类型与提交 / 预测接口 | 第 2 节 | `framework/interfaces/ibaseline_types.h`、`ibaseline_service.h`、`task/value_task.*`、`task/ratio_task.*` | Value / Ratio 暴露 rolling 提交接口和只读预测接口；初始化仅作为 task 内部 lazy 行为；Relation 不新增 rolling 提交 / 预测接口；concrete task stub 可独立编译 |
| `B2-T02` | 解析 rolling 配置与默认值 | 第 4 节 | `plugins/baseline/config/*`、`plugins/baseline/rolling/rolling_config.*`、`plugins/baseline/CMakeLists.txt` | `BaselineRollingConfig` 覆盖默认值、配置模板、strict schema、派生时间参数和合法性校验 |
| `B2-T03` | 实现观测适配器 | 第 3 节 | `plugins/baseline/rolling/observation_adapter.*` | `value_basic/value_sampled/ratio` 转成 `ObservedModelPoint` |
| `B2-T04` | 建立 `RollingState` 与初始化 | 第 4 节、第 6 节 | `plugins/baseline/rolling/rolling_state.*` | 支持空启动、bootstrap 启动、自动初始化失败语义、`force_reset_existing_state` 和 seed uncertainty 映射 |
| `B2-T05` | 实现 `RollingStateEstimator` | 第 5.1 至 5.3 节、第 5.5 节 | `plugins/baseline/rolling/rolling_estimator.*` | 完成 predict、band、residual、Kalman/RLS update、gap、协方差夹紧，以及 `adapt_boost` 对 `Q_level` / level 更新权重的接入 |
| `B2-T06` | 实现 gate、scale、drift | 第 5.4 节、第 5.6 节 | `plugins/baseline/rolling/update_gate.*`、`residual_scale.*`、`drift_adapt.*` | 强异常跳过或降权；sigma 不被异常收窄；level shift 通过 drift evidence 触发 level 加速学习，不触发 shadow/rebuild |
| `B2-T07` | 接入 Value / Ratio task | 第 2 节、第 6.3 节 | `task/value_task.*`、`task/ratio_task.*` | `SubmitObservation()` 返回 `RollingBaselineResult`；不触发 shadow / rebuild |
| `B2-T08` | 实现状态管理与批量 warm-up | 第 7 节、第 6.4 节、第 2 节 snapshot 契约 | `rolling/rolling_state_store.*`、`task/*snapshot*` | 支持状态上限、TTL、LRU、批量 warm-up、snapshot schema、指标和 snapshot |
| `B2-T09` | 补齐自动化测试 | 第 8.2 节 | `tests/test_baseline/*` | 覆盖下方测试矩阵 |

### 8.2 测试矩阵

| 场景 | 断言 |
|---|---|
| `value_basic` 空启动 | 首点创建 state，`can_score = false`，输出宽 band |
| 显式空启动低支撑首点 | pending empty state 保留，不设置 `last_seen_bucket`，不消耗首点初始化机会 |
| `value_sampled` 非法样本数 | `sample_count <= 0` 返回 `kInvalidArgument` |
| `value_sampled` 低样本数 | 已有 state 时不评分、不更新，记录 `skipped_low_sample_count` |
| `value_sampled` 中等样本数 | 低权重评分 / 更新，band 扩宽 |
| ratio 非法输入 | `denominator <= 0`、`numerator < 0` 或 `numerator > denominator` 返回 `kInvalidArgument` |
| ratio 低 denominator | 已有 state 时不评分、不更新，记录 `skipped_low_denominator` |
| ratio clipping | 接近 `0/1` 的 ratio 进入 `logit` 后有限 |
| bootstrap 启动 | 只初始化指定 `series_key`，后续滚动更新不反写 seed |
| bootstrap 时间锚点 | `level` 平移到 `anchor_bucket`，首个流式 bucket 不按完整训练跨度放大 gap |
| 批量 warm-up | 多个 `series_key` 的 rolling state 一次性初始化，且不走 JSON 解析 |
| bootstrap 已有 state 且不强制 reset | 返回 `kInvalidArgument`，不覆盖 state |
| bootstrap 已有 state 且强制 reset | 只重置该 `series_key` 的 state |
| bootstrap 无 seed | `SubmitObservation()` 在 bootstrap 分支找不到 seed 时返回 `kNotTrained` |
| bootstrap 不兼容 | 返回 `kIncompatibleArtifact`，已有 state 不变 |
| 自动初始化关闭 | `SubmitObservation()` 返回 `kNotTrained`，不创建 state |
| 自动初始化不兼容 seed | `SubmitObservation()` 不回退空启动 |
| 自动初始化低支撑首点 | 返回 `kInsufficientData`，不创建 state |
| series snapshot schema | `schema_version`、`document_kind`、`task_identity`、`series_identity`、`state_status`、`band`、`control` 必填 |
| task snapshot schema | `schema_version`、`document_kind`、`task_identity`、`rolling_series_count`、`state_status_counts` 必填 |
| rolling 只读预测 | 已有 state 可预测未来 bucket，预测不更新 state；无 state 返回 `kNotTrained`；旧 bucket 返回 `kInvalidArgument` |
| link rolling eval | 复用 `link_data_2_month.csv`，输出第 4 周 Bootstrap/Rolling walk-forward 对比、过渡带学习检查点、最后 7 天 rolling 预测评估，以及 absolute / level-scaled / calibrated level-scaled 三个临时 adaptive forecast prototype；指标包含 `RMSE`、`MAPE`、coverage、`|Z| > 3`、`|Z| > 5` |
| 同 bucket 重复 | 返回 `kInvalidArgument`，状态不二次更新 |
| 乱序旧 bucket | 返回 `kInvalidArgument`，状态不回滚 |
| gap jump | 不补点，band 因过程噪声变宽 |
| 孤立突刺 | 跳过或降权，状态和 sigma 不被显著污染 |
| level shift | `drift_evidence` 上升，level 加快跟随，不触发 rebuild |
| day/week 学习速度 | `W_level >= W_day >= W_week`，week 慢于 day |
| 高基数清理 | 状态数受控，清理指标可观测 |
| relation 非目标 | Relation task 无 rolling 提交接口 |
| public ABI 边界 | public header 不暴露 `BootstrapSeed` |
| CMake 接入 | 新增 rolling 源文件进入 `flowsql_baseline` 构建 |

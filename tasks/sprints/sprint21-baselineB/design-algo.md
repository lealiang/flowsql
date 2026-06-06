# Baseline 能力与算法设计

本文是 Baseline 面向能力和算法的精简设计说明。它只描述系统能做什么、核心算法如何工作、输出如何解释；不展开代码目录、类名和阶段实施细节。

## 1. 核心能力

Baseline 是一套 stream-first、self-maturing 的在线基线系统。它的核心能力是：

1. **无历史可启动**：没有历史数据时，系统从第一批流式 bucket 开始学习，并以低置信、宽 band 的方式保护冷启动阶段。
2. **有历史可加速**：历史数据只用于生成可选 bootstrap seed，帮助初始化水平、趋势、周期、尺度和成熟度，不作为在线可用性的前置条件。
3. **持续在线学习**：每个有效 bucket 都经过预测、评分、门控更新和成熟度推进，基线可以跟随长期水平变化逐步适配。
4. **输出基线条带**：基线不是单点预测，而是 `baseline_mu / baseline_lower / baseline_upper / band_width` 组成的正常波动范围。
5. **显式表达可信度**：输出携带 maturity、score trust、confidence、enabled components 和 uncertainty source，避免冷启动或漂移阶段被误解释为成熟异常。
6. **统一 Value / Ratio / Relation**：单值、比例和关系分布最终都转化为可在线学习的时间序列证据，Relation 通过摘要投影和模式融合表达结构性异常。

主流程：

```text
Observation
  -> initialize from bootstrap or empty state
  -> predict baseline band
  -> score residual
  -> gate abnormal update
  -> update rolling state
  -> update maturity / trust / calibration
  -> emit evidence
```

## 2. 统一建模对象

在线建模单元是 `Series`：

```text
Series = series_key + feature identity
```

其中：

- `series_key` 表示被建模对象，如某条链路、某个服务或某个 Relation source。
- `feature identity` 表示检测的特征语义，如流量、错误率、熵、Top-K 占比等。
- `bucket_id` 表示固定时间粒度下的绝对时间桶。

Baseline 不在算法层判断业务含义。算法层只输出数学偏离、方向、置信度、持续性和证据；是否告警、如何处置属于上层业务判别。

## 3. 特征类型

当前算法覆盖 3 类输入。

| 类型 | 输入 | 变换 | 适用场景 |
| --- | --- | --- | --- |
| Value basic | `value` | `log1p(value)` | 流量、次数、连接数、基数等非负量 |
| Value sampled | `value + sample_count` | `log1p(value)` | 时延、分位数、均值等由样本聚合出的连续指标 |
| Ratio | `numerator / denominator` | `logit(clipped ratio)` | 成功率、错误率、占比、份额等 `[0, 1]` 比例 |

可靠性规则：

- `value_sampled` 的低 `sample_count` 会导致不评分、不更新或低权重更新。
- `ratio` 的低 `denominator` 会导致不评分、不更新或低权重更新。
- 低样本和低分母都会扩大观测不确定性，避免小样本比例波动制造高置信异常。

## 4. Optional Bootstrap

Bootstrap 是启动加速器，不是服务模型生命周期。

历史训练拟合以下结构：

```text
core(t) =
  level
  + trend * t
  + daily harmonic(t)
  + weekly harmonic(t)

optional(t) =
  month position effect(t)
  + event calendar effect(t)
```

Value 在 `log1p` 空间训练和预测；Ratio 在 `logit` 空间训练，预测时回到概率空间。

Bootstrap 产出的 seed 包含：

- `theta_init`：level、trend、日 / 周周期系数。
- `sigma_init`：初始残差尺度。
- `uncertainty_init`：初始不确定性和 band 来源。
- `maturity_init`：历史覆盖带来的初始成熟度。
- `enabled_components`：历史足以支持的组件。
- Relation seed：初始 basis、routed summary seed 和 fusion metadata。

`seed_status` 自动评估为：

```text
none -> weak -> partial -> full
```

评估依据是训练是否成功、覆盖率、覆盖时长、日 / 周相位覆盖质量，以及初始化字段是否完整。在线主路径可以完全不依赖 seed；有 seed 时只提高初始质量和初始成熟度。

## 5. Online Rolling Core

在线 rolling core 使用动态 harmonic regression 加有界 Kalman / RLS 更新。

状态形式：

```text
level / trend
daily harmonic coefficients
weekly harmonic coefficients
residual scale sigma
parameter uncertainty
drift evidence
maturity and score trust state
```

预测模型：

```text
level_t^- = level_{t-1} + trend_{t-1} * dt

y_hat_t =
  level_t^-
  + daily_harmonic(local_phase_day_t)
  + weekly_harmonic(local_phase_week_t)
```

说明：

- `trend` 只用于推进 level，不单独作为观测项。
- 日 / 周周期使用业务时区下的本地日历相位。
- gap 不补点，只放大过程噪声。
- 月位置项是慢学习组件，只有成熟后才进入检测预测。

## 6. Baseline Band 与评分

每次在线提交都会先得到检测 band：

```text
residual_t = y_model_t - baseline_mu_model_t
band_std_t = sqrt(prediction_uncertainty + calibrated_residual_scale + observation_noise)
z_t = abs(residual_t / band_std_t)
```

输出再从模型空间反变换到观测空间：

- Value：`expm1`，下界裁剪到 `0`。
- Ratio：`sigmoid`，输出落在 `[0, 1]`。

检测 band 与未来预测 band 分离：

- 在线检测 band 服务 `SubmitObservation`，用于评分、更新门控和 `can_alert`。
- 未来 forecast band 服务只读预测，用于展示和评估，不产生异常判定。

## 7. 异常门控与漂移适配

Baseline 同时保护两类场景：

- 孤立突刺：避免把异常点写入基线。
- 持续水平变化：允许基线逐步学习新水平。

漂移证据由短 / 长 EWMA 残差差异和同向残差 CUSUM 共同表达：

```text
resid_norm = clamp(residual / band_std, -z_cap, z_cap)
drift_evidence = EWMA_short(resid_norm) - EWMA_long(resid_norm)
level_shift_evidence = CUSUM(same-direction residual)
combined = dominant(abs(drift_evidence), abs(level_shift_evidence))
```

自适应强度：

```text
adapt_boost =
  clamp((abs(combined) - drift_start) / (drift_full - drift_start), 0, 1)
```

更新门控：

```text
if z >= z_skip + adapt_boost * skip_relax:
  skip isolated spike, or downweight when drift is strong
else if z >= z_downweight:
  downweight update
else:
  normal update
```

更新策略：

- level 学习速度随 `adapt_boost` 增强。
- trend 有单步和绝对值上限。
- 日 / 周周期慢于 level；drift 期间压低 seasonal 更新。
- 残差尺度用裁剪后的 residual EWMA 更新，避免少量极端点把 band 拉宽。

## 8. 成熟度与检测可信度

Baseline 把“学到了多少”和“能否可信评分”分开。

学习成熟度：

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

检测可信度：

```text
score_untrusted
  -> score_warming
  -> score_ready
```

临时降级状态：

```text
drift_learning
recalibrating
```

成熟度推进依赖：

- 有效更新数量。
- 日 / 周相位覆盖。
- 覆盖天数和周数。
- 月份切换次数和月位置覆盖。
- 检测 band 校准质量。

`score_ready` 需要 residual coverage、tail rate 和成熟度同时达标。处于冷启动、warming、drift learning 或 recalibrating 时，即使有较大偏离，也不会输出高置信异常。

置信度分为：

```text
learning_confidence：状态学习成熟度
score_confidence：检测评分可信度
effective_confidence = min(learning_confidence, score_confidence)
```

## 9. Relation 能力

Relation 的目标是检测关系分布结构变化。它不为每个 group 建立无界时间序列，而是把 relation block 投影为有界摘要，再复用 Value / Ratio rolling core。

流程：

```text
Relation block
  -> stream basis
  -> routed summary observations
  -> rolling band / score / trust
  -> relation pattern fusion
```

### 9.1 Stream Basis

Relation basis 表达历史或在线学习到的稳定支持集：

```text
support_explicit：稳定出现且有足够质量占比的 group
stable_head：support 中更小的稳定头部
head_proto_q：stable_head 内部的历史原型分布
other_group_idxs：业务定义的 other group
basis_version：basis 版本
```

stream-only basis 使用固定容量的 group 统计，保守估计每个 group 的质量下界和活跃覆盖。刷新时要求：

- 收集桶数达到下限。
- 覆盖率达标。
- 候选 group 连续刷新稳定。
- 新旧 support / stable head 替换数量不超过 replacement cap。

basis 切换后进入 handover warming，避免新 basis 立即产生高置信结构异常。

### 9.2 Routed Summary

无 basis 也能输出的通用摘要：

| 摘要 | 类型 | 能力 |
| --- | --- | --- |
| `entropy_shannon` | Value | 分布离散程度 |
| `distinct_group_count` | Value | 活跃 group 数 |
| `top1_share` | Ratio | 最大 group 集中度 |
| `headk_share` | Ratio | Top-K 集中度 |

有 basis 后输出的 basis-scoped 摘要：

| 摘要 | 类型 | 能力 |
| --- | --- | --- |
| `out_of_support_share` | Ratio | 逃离稳定支持集的比例 |
| `stable_headk_coverage` | Ratio | 稳定头部覆盖率 |
| `stable_g_share_i` | Ratio | 稳定头部单 group 份额 |
| `stable_headk_mix_drift` | Value | 稳定头部内部混合漂移 |

basis-scoped 摘要在 basis 未 ready 前可以训练 rolling state，但不能产生高置信告警证据。

### 9.3 Relation Pattern Fusion

Fusion 将多个 routed summary 的 rolling 结果转换为结构风险。单条证据强度为：

```text
normalized_score = min(1, abs(z_score) / fusion_z_score_cap)
persistence_factor = min(1, persistence / fusion_persistence_window)
evidence_strength =
  normalized_score * confidence * persistence_factor * trust_factor
```

当前支持 4 类 Relation 局部模式：

| 模式 | 表达的结构变化 |
| --- | --- |
| `support_escape` | 流量或行为从稳定支持集逃逸到新 group |
| `head_concentration` | 分布向少数头部 group 过度集中 |
| `legacy_head_dilution` | 原有稳定头部被稀释 |
| `stable_head_mix_shift` | 稳定头部内部比例发生变化 |

模式分：

```text
pattern_score = clamp01(core + support_weight * support - oppose_weight * oppose)
```

风险聚合：

```text
union(values) = 1 - product(1 - value)

single_risk = union(single evidence strengths)
pattern_risk = union(weighted pattern scores)
relation_risk = 1 - (1 - single_risk) * (1 - pattern_risk)
```

Relation risk 只表示同一个 Relation source 内部的结构异常强度，不等价于全局业务风险。

## 10. 输出解释

在线结果至少表达以下语义：

| 字段 | 语义 |
| --- | --- |
| `baseline_mu` | 当前正常中心估计 |
| `baseline_lower / baseline_upper` | 当前正常波动范围 |
| `band_width` | band 宽度，越宽表示不确定性越大 |
| `z_score` | 观测值穿出模型空间 band 的标准化距离 |
| `is_outside_band` | 当前观测是否在 band 外 |
| `can_score` | 当前样本是否足以评分 |
| `can_update` | 当前样本是否进入状态更新 |
| `can_alert` | 当前异常证据是否可作为高置信告警输入 |
| `maturity_status` | 学习成熟阶段 |
| `score_trust_status` | 检测可信状态 |
| `confidence` | 当前有效置信度 |
| `uncertainty_source` | band 或置信度降低的原因 |

解释原则：

- `z_score` 大但 `can_alert = false`：通常表示冷启动、低样本、drift learning 或 recalibrating，不应作为高置信异常。
- `band_width` 大：通常表示低成熟、低样本、低分母、残差尺度高或校准扩张。
- `score_ready` 且 `can_alert = true`：该证据才具备进入上层告警或风险合成的资格。

## 11. 设计边界

当前算法边界：

- 不恢复旧的 `shadow baseline`、`candidate model`、正式重建和模型切换链路。
- 不把批处理历史作为在线可用性前置条件。
- 不在算法层输出业务处置结论。
- 不为 Relation 的每个 group 建立无界 rolling baseline。
- Relation fusion 输出的是 source 内部结构风险，不是全局 Key 级风险。
- `distinct_group_count` 只有在上游提供可信活跃数时才进入 fusion。

Baseline 的长期约束是：任何新特征或新模式都应先明确它属于 Value、Ratio 还是 Relation summary；能复用 rolling core 的，不新增独立时间序列算法；能作为业务判别的，不下沉到算法层。

## 12. 建议

尽管设计已经很优秀，但在实际工业落地时，以下几个点可能会成为隐患，建议在细节上补充考虑：
1. Relation Basis 切换时的“盲区问题” (Handover Warming)
风险：在 9.1 中提到，Basis 切换后进入 handover warming，避免新 basis 产生高置信告警。如果此时恰好发生真实的网络结构突变（攻击者刚好在 basis 切换时发起攻击），系统可能会漏报（False Negative）。
建议补充：引入 “双 Basis 阴影刷新 (Shadow Basis)”。即在后台提前预计算下一个版本的 Basis，当新的 Basis 成熟度（覆盖率）达到一定阈值时再做无缝平滑切换，而不是切换后再 Warming。
2. 冷启动阶段 sigma_init (初始残差尺度) 的设定
风险：如果没有 Bootstrap（无历史数据），系统从第一条流式数据开始。在只有 1 个点或前几个点时，band_std_t 的初始值如何设定？如果设得太小，第 2 个点就会触发极大的 z_score 导致状态乱飞；设得太大，又会导致很长一段时间处于盲区。
建议补充：在冷启动策略中明确声明 sigma_init 需要依赖全局默认配置（比如基于同一业务的全局方差启发值），或者在前 N 个 bucket（如 1 小时内）强制锁定 z_score < 1.0 的权重更新模式，纯积累残差样本而不做漂移判定。
3. 月位置项 (Month Position Effect) 的在线学习极难
风险：在 Section 5 中，日/周周期可以通过一两周的数据很快建立，但月份（月头、月末效应）一年只有 12 次观测机会。完全依靠在线滚动（RLS）去学习月周期是非常不稳定的。
建议补充：对于长周期（如节假日、月、年），强烈建议只通过 Bootstrap (Batch 历史) 初始化和更新，在线 Rolling core 只维持它的值，不进行或极小幅度进行在线微调。
4. Ratio 型数据中低分母的方差膨胀问题
风险：你提到了“低样本和低分母都会扩大观测不确定性”。对于 logit 变换而言，当分母极小（比如总共只有 2 次请求，错了 1 次，错误率 50%），变换后的方差会极大。
建议补充：在 Ratio 计算 logit 之前，可以使用 拉普拉斯平滑（Laplace Smoothing / 贝叶斯平均）。例如 (num + alpha) / (den + beta)，这样在分母很小时，Ratio 会天然向全局先验均值收缩，从根本上压制小样本带来的剧烈波动。
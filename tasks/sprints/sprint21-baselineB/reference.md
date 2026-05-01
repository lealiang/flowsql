# Sprint 21 BaselineB 参考资料

## 1. 文档目的

本文记录 `Sprint 21 BaselineB` 设计中涉及的主要算法来源、产品参照和工程启发。

BaselineB 的目标不是照搬某一篇论文或某个产品，而是将以下思想组合成适合 FlowSQL 的基线框架：

```text
stream-first online baseline
  + optional batch bootstrap
  + self-maturing component gate
  + residual-based anomaly scoring
  + bounded state for high-cardinality series
```

---

## 2. 核心算法来源

### 2.1 Kalman Filter / 状态空间模型

- 参考：R. E. Kalman, 1960, *A New Approach to Linear Filtering and Prediction Problems*
- 链接：https://doi.org/10.1115/1.3662552

对 BaselineB 的启发：

- 将基线参数视为随时间演化的状态，而不是一次训练后长期冻结的参数。
- 在线路径采用“先预测、再根据 residual 更新状态”的递推结构。
- `level`、`trend`、季节项系数都可以统一放进状态表示。

在 BaselineB 中的落点：

```text
Online Rolling Core:
  predict(theta_{t-1})
  -> score(residual_t)
  -> gate update
  -> theta_t
```

MVP 不要求完整 dense Kalman，而是后续细化为 fixed-size small block / diagonal 近似，以适配高基数 `Series`。

---

### 2.2 Dynamic Harmonic Regression（DHR）

- 参考：Peter C. Young, D. Pedregal, W. Tych, 1999, *Dynamic Harmonic Regression*
- 期刊：Journal of Forecasting, 18(6), 369-394
- 链接：https://doi.org/10.1002/(SICI)1099-131X(199911)18:6%3C369::AID-FOR748%3E3.0.CO;2-K
- Lancaster 页面：https://www.research.lancs.ac.uk/portal/en/publications/dynamic-harmonic-regression(37750abd-e84f-45ef-bb02-0dded3e859bb).html

对 BaselineB 的启发：

- 用 harmonic / Fourier 项表示周期结构。
- 支持将趋势、周期和噪声放在状态空间模型中做自适应估计。
- 适合表达日周期、周周期等多周期季节项。

在 BaselineB 中的落点：

```text
y_t =
  level_t
  + trend_t
  + daily_harmonic_t
  + weekly_harmonic_t
  + residual_t
```

后续算法细化时，`T1/T2` 的 `day_sin/cos_coeffs`、`week_sin/cos_coeffs` 可基于该方向展开。

---

### 2.3 Recursive Least Squares（RLS）与遗忘因子

- 参考：Recursive Least Squares / forgetting factor 在线参数估计方法
- 工程参考：MathWorks Recursive Least Squares Estimator
- 链接：https://www.mathworks.com/help/ident/ref/recursiveleastsquaresestimator.html

对 BaselineB 的启发：

- 在线模型可以写成固定时间特征上的线性递推：

```text
y_hat_t = theta_t · x_t
theta_t = theta_{t-1} + gain_t * residual_t * x_t
```

- 遗忘因子可以控制新旧数据权重，用于应对水平变化和缓慢漂移。

在 BaselineB 中的落点：

- `Online Rolling Core` 的工程实现可优先采用 RLS / small block Kalman 形态。
- `Optional Bootstrap Engine` 输出的 `level_0`、`trend_0`、`day/week coeffs` 可以直接作为 `theta_0`。
- `uncertainty_0` 或 `learning_rate_hint` 决定初始学习速度。

---

### 2.4 ADWIN / 自适应滑窗

- 参考：Albert Bifet, Ricard Gavaldà, 2007, *Learning from Time-Changing Data with Adaptive Windowing*
- 会议：SIAM International Conference on Data Mining (SDM)
- 链接：https://epubs.siam.org/doi/10.1137/1.9781611972771.42

对 BaselineB 的启发：

- 数据分布稳定时，模型应保留更长历史记忆。
- 发生变化时，模型应缩短有效历史、加快适应。
- 自适应遗忘可以减少手工选择固定窗口长度的问题。

在 BaselineB 中的落点：

- MVP 不强制每个 `Series` 维护完整 ADWIN 窗口。
- ADWIN 作为“自适应遗忘 / drift response”的理论参照。
- 后续可用轻量漂移分数、EWMA、Page-Hinkley 或简化 ADWIN 来调节 `learning_rate` / `process_noise`。

---

### 2.5 Robust residual anomaly scoring

参考方向：

- 基于趋势 / 季节分解后，在 residual 上做异常检测。
- 使用 robust scale、MAD、Tukey fence、Huber loss 等降低异常点对基线的污染。

对 BaselineB 的启发：

- 异常检测应主要发生在 residual 空间。
- 强异常点不应无条件进入基线更新。
- `sigma_0`、`sigma_floor`、robust residual scale 是 `Optional Bootstrap` 能直接赋能在线模型的重要输出。

---

## 3. 产品和工程参照

### 3.1 Prophet / Forecasting at Scale

- 参考：Sean J. Taylor, Benjamin Letham, 2018, *Forecasting at Scale*
- 链接：https://doi.org/10.1080/00031305.2017.1380080
- Prophet 项目：https://github.com/facebook/prophet

对 BaselineB 的启发：

- 可解释加性模型：

```text
trend + seasonality + holiday/event
```

- 适合作为 `Optional Bootstrap Engine` 的参照：用历史数据训练可解释的趋势、周期和事件 seed。

在 BaselineB 中的落点：

- `Optional Bootstrap` 可以训练 `level_0`、`trend_0`、`day/week coeffs`、`monthpos seed`、`event seed`。
- 这些输出只作为在线状态初值或慢变量 seed，不作为长期冻结的服务模型。

---

### 3.2 Azure Data Explorer / Kusto time series anomaly detection

- 参考：Microsoft Learn，Time series anomaly detection and forecasting
- 链接：https://learn.microsoft.com/en-us/azure/data-explorer/anomaly-detection
- `series_decompose_anomalies()` 文档：https://learn.microsoft.com/en-us/kusto/query/series-decompose-anomalies-function

对 BaselineB 的启发：

- 将时间序列拆成 seasonal、trend、residual 和 baseline。
- 异常检测在 residual 上完成。
- 工程上强调批量处理大量时间序列和近实时监控。

在 BaselineB 中的落点：

- 输出 evidence 应包含 `baseline`、`residual`、`score` 等可解释字段。
- `Online Rolling Core` 的异常评分应尽量围绕 residual 组织。

---

### 3.3 Datadog Anomaly Detection

- 参考：Datadog，Introducing anomaly detection in Datadog
- 链接：https://www.datadoghq.com/blog/introducing-anomaly-detection-datadog/

对 BaselineB 的启发：

- 工程产品中需要同时支持快速适应和稳定季节基线。
- Datadog 的 `Agile` 强调适应 level shift，`Robust` 强调稳定季节趋势分解。

在 BaselineB 中的落点：

- `Online Rolling Core` 需要处理持续水平变化，而不是只等待重建。
- `Maturity Gate` 和 robust update 需要避免短期异常污染稳定周期结构。

---

### 3.4 Twitter AnomalyDetection / S-H-ESD

- 参考：Twitter AnomalyDetection
- GitHub：https://github.com/twitter/AnomalyDetection
- 文档：https://rdrr.io/github/twitter/AnomalyDetection/

对 BaselineB 的启发：

- 对带季节性和趋势的时间序列，先做分解，再在 residual 上做 robust anomaly detection。
- 使用 robust 统计量降低季节峰值和异常点的干扰。

在 BaselineB 中的落点：

- `Optional Bootstrap` 和 `Online Rolling Core` 都应围绕 residual 组织异常评分。
- 强异常点应通过 gate 降权或跳过更新。

不直接采用的原因：

- S-H-ESD 更偏批处理 / 窗口检测，不是 stream-first 的在线状态模型。

---

### 3.5 Prometheus double_exponential_smoothing / Holt Linear

- 参考：Prometheus Query functions
- 链接：https://prometheus.io/docs/prometheus/latest/querying/functions/

对 BaselineB 的启发：

- 监控系统里常用轻量平滑方法跟踪 level 和 trend。
- 简单递推方法在监控热路径中有工程价值。

在 BaselineB 中的落点：

- 可作为 `level/trend` 轻量递推和 fallback 的工程参照。
- 不足以覆盖日 / 周 / 月周期，因此不能作为 BaselineB 的完整方案。

---

### 3.6 Numenta Anomaly Benchmark（NAB）

- 参考：Alexander Lavin, Subutai Ahmad, 2015, *Evaluating Real-Time Anomaly Detection Algorithms -- The Numenta Anomaly Benchmark*
- DOI：https://doi.org/10.1109/ICMLA.2015.141
- 代码：https://github.com/numenta/NAB

对 BaselineB 的启发：

- 评价实时异常检测时，应关注边预测边学习、尽早发现、减少误报、适应统计变化。
- 流式异常检测不能按纯 batch 评估方式设计。

在 BaselineB 中的落点：

- 测试矩阵应覆盖无历史启动、边学习边评分、漂移适应和成熟度推进。
- 不能把“批处理历史充足”作为唯一验证场景。

---

## 4. 与 BaselineB 设计的映射

| BaselineB 组件 | 主要参考 | 设计落点 |
|---|---|---|
| `Online Rolling Core` | Kalman Filter、DHR、RLS | 在线预测、评分、门控更新、状态递推 |
| `Optional Bootstrap Engine` | Prophet、旧 Sprint 19 训练方案 | 用历史数据生成 `BootstrapSeed` |
| `Maturity Gate` | 流式异常检测工程经验、NAB 评价目标 | 组件按覆盖度和稳定性逐步启用 |
| 残差异常评分 | Kusto、Twitter S-H-ESD、robust statistics | 在 residual 空间输出异常分与解释 |
| 自适应遗忘 | ADWIN、RLS forgetting factor、Datadog Agile 思路 | 漂移时加快在线状态适应 |
| `T3` routed 摘要 | Sprint 19 `T3` 设计、Kusto 分解思想 | 摘要特征路由到 `T1/T2` rolling core |

---

## 5. 当前不直接采用的内容

### 5.1 不直接采用完整 dense Kalman

原因：

- 高基数 `Series` 场景下，完整协方差矩阵状态量和热路径计算成本过高。
- MVP 更适合 fixed-size small block / diagonal 近似。

### 5.2 不直接采用完整 ADWIN

原因：

- 每个 `Series` 维护完整自适应窗口会增加状态和内存压力。
- 首版只把 ADWIN 作为自适应遗忘的理论来源，具体实现可采用轻量漂移分数。

### 5.3 不直接采用 S-H-ESD 作为主路径

原因：

- S-H-ESD 更适合窗口或批量检测。
- BaselineB 要求无历史可启动，并且每个 bucket 都能在线学习。

### 5.4 不直接采用 Prophet 作为在线主路径

原因：

- Prophet 更适合作为可解释 batch bootstrap。
- BaselineB 的主路径必须是 stream-first 的在线滚动状态。

---

## 6. 后续引用规则

后续在 `scope.md`、`planning.md` 或阶段设计文档中引用本文时，应明确区分：

- **算法来源**：说明某个数学结构或工程取舍来自哪里。
- **产品参照**：说明目标行为或产品能力参考了哪个系统。
- **不直接采用原因**：说明为什么 BaselineB 只吸收部分思想，而不是照搬完整方案。

任何新增参考资料都应补充：

```text
名称
链接
核心观点
对应 BaselineB 落点
是否进入 MVP
```

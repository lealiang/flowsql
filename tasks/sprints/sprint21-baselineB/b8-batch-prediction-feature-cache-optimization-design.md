# B8 Baseline 批量预测特征缓存与谐波递推优化设计

## 1. 背景与结论

Baseline 已经补齐 Value / Ratio 的多点预测接口，调用方可以通过：

```text
PredictRolling(series_key, start_bucket_id, point_count)
PredictBootstrap(series_key, start_bucket_id, point_count, options)
```

一次预测连续多个 bucket。多点预测会成为 Baseline 的高频能力，典型场景包括未来 24 小时、未来 7 天、分钟级链路带宽预测和报表预渲染。

当前代码已经避免了最早的「多点接口逐点调用 task 单点接口」问题，但序列内部仍然逐点执行完整特征构建：

- `rolling/rolling_task_runner.cpp` 的 `PredictRollingSequenceForSeries()` 循环调用 `PredictRollingForState()`。
- `PredictRollingForState()` 进入 `BuildFusionForecastEstimator()` 或 `PredictRollingForecastState()`，二者都会调用 `BuildRollingFeatureVector()`。
- `BuildRollingFeatureVector()` 每个 bucket 都重新调用 `PhaseDayLocal()` / `PhaseWeekLocal()`，也就是至少 2 次本地时间解析。
- 月位置成熟后，`EvaluateRollingMonthpos()` 又会调用 `DayOfMonthLocal()` / `DaysToMonthEndLocal()` / `IsLastWeekdayOfMonthLocal()` / `LastWeekdayIndex()`，额外产生 3 到 4 次本地时间解析。
- Bootstrap 预测序列同样在 `PredictFormalModel()` 内逐点计算 core Fourier、month position 和 event block。

核心结论：

1. 第一优先级不是直接上复杂 LRU 或跨请求缓存，而是把同一次连续序列预测中的日历特征和 Fourier 特征变成批量上下文。
2. 首版必须先做「单 bucket 一次本地时间解析 + 扁平化特征批缓存」，这是确定性收益最大、正确性风险最低的优化。
3. 第二步在连续 bucket 且本地 wall-clock 步长稳定时，对日 / 周 Fourier 特征使用三角递推，跳过绝大多数 `sin()` / `cos()` 调用。
4. 所有缓存都应是函数局部临时对象，不进入 task runtime state，不新增后台线程，不引入新的线程竞态。
5. 本优化不需要新增业务配置；chunk size、递推重锚间隔作为内部常量即可。如果后续要暴露为运行时配置，必须进入 `src/plugins/baseline/config/baseline-config-template.yaml` 并闭合 parser / strict schema / tests。

---

## 2. 当前热路径拆解

### 2.1 Rolling 批量预测

当前调用链：

```text
BaselineValueTask::PredictRolling(start_bucket_id, point_count)
  -> PredictRollingSequenceForSeries()
     -> for each bucket_id
        -> PredictRollingForState()
           -> BuildFusionForecastEstimator()       # 有可用 bootstrap seed 时优先走 fusion forecast
              -> BuildRollingFeatureVector()
           -> PredictRollingForecastState()        # fusion 不可用时 fallback
              -> BuildRollingFeatureVector()
           -> EvaluateRollingMonthpos()            # monthpos ready 时额外计算
```

关键文件：

```text
src/plugins/baseline/rolling/rolling_task_runner.cpp
src/plugins/baseline/rolling/rolling_estimator.cpp
src/plugins/baseline/rolling/monthpos_state.cpp
src/plugins/baseline/model/calendar_feature_helper.cpp
```

按当前默认谐波阶数 `daily_harmonic_order = 6`、`weekly_harmonic_order = 3` 粗估，一个 future bucket 会产生：

| 操作 | 当前次数 |
| --- | ---: |
| 日 / 周 phase 本地时间解析 | 2 次 |
| 日 / 周 `sin()` / `cos()` | 18 次 |
| 月位置本地时间解析（月位置成熟时） | 3 到 4 次 |
| `RollingFeatureVector` vector 分配 | 每点 4 个 vector |

对于 1440 个点的分钟级日预测，仅 rolling feature path 就会产生约 2880 次本地时间解析、25920 次三角函数调用；月位置成熟时本地时间解析会进一步扩大到 7000 次以上。

### 2.2 Bootstrap 批量预测

当前调用链：

```text
BootstrapEngine::PredictValueSequence()
  -> for each bucket_id
     -> PredictFormalModel(ValueFormalModel)
        -> EvaluateCore()
           -> PhaseDayLocal()
           -> PhaseWeekLocal()
           -> EvaluateFourier()
        -> EvaluateMonthPos()
        -> EvaluateEventBlock()

BootstrapEngine::PredictRatioSequence()
  -> 同上，只是输出空间从 log value 改为 probability
```

关键文件：

```text
src/plugins/baseline/bootstrap/bootstrap_engine.cpp
src/plugins/baseline/model/formal_predictor.cpp
src/plugins/baseline/model/calendar_feature_helper.cpp
```

Bootstrap 的 core Fourier 和 month position 与 Rolling 使用同一类日历特征，因此可以共享同一套 batch calendar feature 设计。Event block 仍需要逐点判断事件命中；它不属于本次 Fourier 优化范围。

---

## 3. 目标与非目标

### 3.1 目标

- 多点预测只 resolve 一次 rolling config，并在当前代码已有基础上继续消除逐点重复特征构建。
- 对同一序列中的每个 future bucket，最多解析一次本地日历时间。
- 连续 bucket 且本地 wall-clock 步长稳定时，使用谐波递推替代逐点 `sin()` / `cos()`。
- Rolling 和 Bootstrap 的批量预测尽量复用同一套日历特征结构，避免两套实现漂移。
- 保持单点预测接口行为不变。
- 保持输出与现有逐点预测结果数值等价；递推误差必须低于测试容忍阈值。
- 控制批量缓存内存占用，避免 `point_count` 极大时一次性申请过大临时内存。

### 3.2 非目标

- 不做跨 API 调用的缓存，不缓存到 task state。
- 不引入 LRU / TTL / 全局 cache manager。
- 不改变 Rolling 状态更新、maturity、score trust、fusion forecast 的业务语义。
- 不改变 Bootstrap artifact schema。
- 不优化 event calendar 命中查找；事件预测可在后续独立优化。
- 不新增后台线程或异步预计算。
- 不新增运行时 YAML 配置。

---

## 4. 总体设计

### 4.1 两级优化

本方案分两级落地：

```text
Level 1：批量日历特征缓存
  -> 单 bucket 一次 ResolveOneLocalCalendarFeature()
  -> 同时产出 day_phase / week_phase / monthpos index / DST 连续性信息
  -> Rolling / Bootstrap 共享

Level 2：连续序列 Fourier 递推
  -> 对 day_sin/day_cos/week_sin/week_cos 使用三角加法公式
  -> 遇到 DST、本地 wall-clock 非连续、非连续 bucket 或达到重锚间隔时重新锚定
```

首批实现建议同时完成 Level 1 和 Level 2，但提交上可以分两步：

1. 先落 Level 1，并用严格等价测试证明结果和旧路径一致。
2. 再落 Level 2，继续用等价测试和 DST 边界测试兜底。

### 4.2 批量对象只在函数栈内流转

不做持久化缓存，原因：

- 预测序列通常是一次请求内连续点，局部性很强，函数局部缓存已经覆盖主要浪费。
- task 内状态会被 submit 推进，跨请求缓存要处理失效条件，复杂度和收益不成比例。
- 函数局部缓存天然线程安全，不会扩大 B6 里已经收口的同 task 串行调用契约。

---

## 5. 数据结构设计

### 5.1 日历特征

建议在 `model/calendar_feature_helper.h` 增加：

```cpp
struct LocalCalendarFeature {
    bool valid = false;
    int64_t bucket_id = 0;
    int64_t epoch_second = 0;

    int32_t hour = 0;
    int32_t minute = 0;
    int32_t second = 0;
    int32_t weekday = 0;          // 与 std::tm::tm_wday 一致，0=Sunday
    int32_t monday_weekday = 0;   // 0=Monday
    int32_t day_of_month = 0;     // 1..31
    int32_t days_to_month_end = 0;
    bool is_last_weekday_of_month = false;

    int32_t second_of_day = 0;
    int32_t second_of_week = 0;
    int64_t local_wall_second = 0;
    double day_phase = 0.0;
    double week_phase = 0.0;
};

// 单点日历特征解析函数，不是批量 API。批量场景由 feature batch 循环调用。
// delta 表示 clock_spec.bucket_seconds；epoch_second = bucket_id * delta。
bool ResolveOneLocalCalendarFeature(int64_t bucket_id,
                                    int64_t delta,
                                    const std::string& tz,
                                    LocalCalendarFeature* out);
```

实现要求：

- UTC / GMT 继续走 `gmtime_r()` 快路径。
- 非 UTC 时区仍复用现有 `thread_local UCalendar`。
- 同一次 `ResolveOneLocalCalendarFeature()` 只调用一次 `ucal_setMillis()`。
- `ResolveOneLocalCalendarFeature()` 只解析一个 `bucket_id`。对于连续批量预测，
  `BuildRollingFeatureBatch(start_bucket_id, point_count, ...)` 负责按
  `start_bucket_id + i` 循环调用它。
- `days_to_month_end`、`is_last_weekday_of_month` 在同一个本地时间结果上计算，不再重复进入 ICU。
- `local_wall_second` 使用本地年月日时分秒计算序列化秒值，只用于判断相邻 bucket 本地时间是否连续，不参与业务输出。

### 5.2 扁平化 Fourier 批缓存

建议新增文件：

```text
src/plugins/baseline/rolling/rolling_feature_batch.h
src/plugins/baseline/rolling/rolling_feature_batch.cpp
```

核心结构：

```cpp
struct RollingFeatureView {
    // RollingFeatureView 不拥有内存；父 RollingFeatureBatch 必须覆盖所有 view 的使用期。
    const double* day_sin = nullptr;
    const double* day_cos = nullptr;
    std::size_t day_size = 0;
    const double* week_sin = nullptr;
    const double* week_cos = nullptr;
    std::size_t week_size = 0;
    const LocalCalendarFeature* calendar = nullptr;
};

struct RollingFeatureBatch {
    int64_t start_bucket_id = 0;
    uint32_t point_count = 0;
    int32_t daily_order = 0;
    int32_t weekly_order = 0;
    std::vector<LocalCalendarFeature> calendar;
    std::vector<double> day_sin;   // point_count * daily_order
    std::vector<double> day_cos;
    std::vector<double> week_sin;  // point_count * weekly_order
    std::vector<double> week_cos;

    RollingFeatureView View(uint32_t index) const;
};
```

使用扁平数组而不是 `std::vector<RollingFeatureVector>`，原因：

- 避免每个 bucket 产生 4 个小 vector 分配。
- 缓存布局连续，遍历时 cache locality 更好。
- `View()` 只返回指针和长度，不拥有内存，适合热路径。

首版不把 month position 字段拆成单独数组。虽然拆成 `MonthPosCalendarFeature`
可以在月位置未成熟时少分配约几十 KB 的 chunk 内存，但这些字段都来自同一次
本地时间解析结果，额外存储成本远小于 ICU 和三角函数成本。拆分会增加
`View()` 生命周期和可选字段判断复杂度，因此作为后续内存压缩优化保留。

### 5.3 Chunk 化

`point_count` 是 `uint32_t`，不能假设调用方只预测几百点。批量特征缓存必须按 chunk 构建：

```text
per_point_fourier_doubles = 2 * (daily_order + weekly_order)
target_fourier_bytes = 2 MiB
if per_point_fourier_doubles == 0:
  internal_chunk_size = 4096
else:
  internal_chunk_size =
    clamp(target_fourier_bytes / (per_point_fourier_doubles * sizeof(double)),
          256,
          4096)
for offset in [0, point_count):
  chunk_count = min(internal_chunk_size, point_count - offset)
  BuildRollingFeatureBatch(start_bucket_id + offset, chunk_count, config, &batch)
  consume batch
```

`4096` 是最大 chunk 上限，不写入 YAML。chunk size 根据谐波阶数动态收缩，避免高阶配置下一次性分配过大的 Fourier 数组。按默认日 / 周阶数 6 / 3 估算：

```text
每点 Fourier double 数量 = 2 * (6 + 3) = 18
每点 Fourier 内存约 144 bytes
加 LocalCalendarFeature 后约 200 bytes 级
4096 点临时缓存约 0.8 MB 到 1.2 MB
```

对于高阶配置，例如 daily 24、weekly 24，每点 Fourier 数量为 96 个 double。此时 chunk size 会按 2 MiB 目标自动降到约 2730 点，并继续受最小 256 点保护。对于 10080 点的周预测，默认阶数下分 3 个 chunk 完成，高阶配置下分更多 chunk，避免一次性临时内存尖峰。

---

## 6. Fourier 递推算法

### 6.1 递推公式

对任意谐波阶数 `k`，设当前角度为 `a`，相邻 bucket 的本地相位增量为 `d`：

```text
sin(a + d) = sin(a) * cos(d) + cos(a) * sin(d)
cos(a + d) = cos(a) * cos(d) - sin(a) * sin(d)
```

递推步长 `d` 始终使用配置里的 `bucket_seconds` 计算，而不是动态使用相邻点的实际本地秒差。实际本地秒差只用于判断能否递推；一旦检测到本地 wall-clock 不连续，当前点直接重锚。

对于 daily：

```text
d = 2π * k * bucket_seconds / 86400
```

对于 weekly：

```text
d = 2π * k * bucket_seconds / 604800
```

如果遇到 DST 切换、本地时间跳变或任何 `local_wall_second` 不连续情况，则不能用递推结果，必须重新锚定。这样避免在春季跳时、秋季回拨或时区规则异常时用错误步长继续传播误差。

### 6.2 重锚条件

以下任一条件成立时，当前点直接用 `sin(phase)` / `cos(phase)` 计算，并从该点重新开始递推：

- 当前点是 chunk 第一项。
- `bucket_id` 不连续。
- 上一个点或当前点 `LocalCalendarFeature.valid == false`。
- `current.local_wall_second - previous.local_wall_second != bucket_seconds`。
- 已连续递推超过内部重锚间隔，例如 `kFourierReanchorInterval = 1024`。

`local_wall_second` 判断可以覆盖 DST 春季跳时和秋季回拨。周末换周、跨日、跨月不需要特殊处理，只要本地 wall-clock 秒差仍等于 `bucket_seconds`，递推公式天然成立。注意，`current.local_wall_second - previous.local_wall_second` 只作为连续性检查，不作为动态递推步长。

### 6.3 数值误差

递推会引入极小浮点误差。控制策略：

- 每个 chunk 首点直接锚定。
- 每 1024 点重锚一次。
- DST 或本地时间不连续时重锚。
- 单元测试用旧路径逐点 `BuildRollingFeatureVector()` 作为 oracle，比对误差：

```text
abs(actual - expected) <= 1e-12
```

如果不同平台 libm 差异导致 1e-12 偶发失败，可放宽到 `1e-11`，但不能再宽；预测输出层可使用 `1e-9` 量级容忍。

---

## 7. Rolling 预测改造

### 7.1 新增 view-based estimator 接口

在 `rolling/rolling_estimator.h` 增加内部使用的 overload：

```cpp
BaselineStatus PredictRollingForecastStateWithFeature(
    const RollingState& state,
    int64_t bucket_id,
    const BaselineRollingConfig& config,
    RollingFeatureView feature,
    RollingEstimatorResult* out);
```

原 `PredictRollingForecastState()` 保留，内部可以变成：

```text
BuildRollingFeatureVector()
  -> Wrap as RollingFeatureView
  -> PredictRollingForecastStateWithFeature()
```

这样单点预测行为不变，批量预测可绕过 per-point feature 构建。

### 7.2 Fusion forecast 使用同一特征 view

`BuildFusionForecastEstimator()` 当前在 `rolling_task_runner.cpp` 内部静态函数中直接构建 `RollingFeatureVector`。需要拆成：

```text
BuildFusionForecastEstimatorWithFeature(state, seed, bucket_id, config, feature, out)
BuildFusionForecastEstimator(...)  # 单点兼容 wrapper
```

`EvaluateBootstrapHarmonic()` 增加接受 `RollingFeatureView` 的版本，避免把 view 再拷贝回 vector。

### 7.3 Monthpos 使用缓存日历特征

在 `rolling/monthpos_state.h` 增加：

```cpp
double EvaluateRollingMonthposWithFeature(const RollingState& state,
                                          const LocalCalendarFeature& feature);
```

实现从 `feature.day_of_month`、`feature.days_to_month_end`、`feature.is_last_weekday_of_month` 和 `feature.weekday` 计算 active index，不再调用 `DayOfMonthLocal()` 等 helper。

原 `EvaluateRollingMonthpos(state, bucket_id, config)` 保留为单点兼容 wrapper。

### 7.4 Sequence 主循环

`PredictRollingSequenceForSeries()` 调整为：

```text
resolve config once
find state and seed once
reserve predictions
for each chunk:
  build feature batch only for future bucket range
  for each point in chunk:
    if bucket_id <= last_seen_bucket:
      append invalid prediction
    else:
      feature = batch.View(local_index)
      PredictRollingForStateWithFeature(...)
```

注意：

- 对 past bucket 不需要构建 feature。
- 如果一个 sequence 中既有 past 又有 future bucket，可以只对 future 子区间建 batch。
- sequence status 仍保持当前语义：只要出现第一个非 OK prediction，sequence.status 从 `kOk` 变为该 status。
- 不能改变 `start_bucket_id`、`point_count`、`predictions.size()` 的契约。

---

## 8. Bootstrap 预测改造

### 8.1 Formal predictor 增加 prepared feature path

当前 `PredictFormalModel()` 接口只接受 `bucket_id`，内部自己解析日历。建议新增不破坏现有接口的内部 overload：

```cpp
struct FormalPredictFeatureView {
    // FormalPredictFeatureView 不拥有内存；父 batch 必须覆盖所有 view 的使用期。
    const LocalCalendarFeature* calendar = nullptr;
    const double* day_sin = nullptr;
    const double* day_cos = nullptr;
    std::size_t day_size = 0;
    const double* week_sin = nullptr;
    const double* week_cos = nullptr;
    std::size_t week_size = 0;
};

int PredictFormalModelWithFeature(const ValueFormalModel* model,
                                  const FormalPredictContext& context,
                                  FormalPredictFeatureView feature,
                                  FormalPrediction* out);

int PredictFormalModelWithFeature(const RatioFormalModel* model,
                                  const FormalPredictContext& context,
                                  FormalPredictFeatureView feature,
                                  FormalPrediction* out);
```

单点 `PredictFormalModel()` 保持不变，内部仍走原逻辑或临时构建单点 feature。

### 8.2 Core 和 MonthPos 拆成 feature-aware helper

`formal_predictor.cpp` 中建议拆出：

```text
EvaluateCoreWithFeature(core_block, bucket_id, train_start, feature)
EvaluateMonthPosWithFeature(monthpos_block, feature.calendar)
```

`EvaluateEventBlock()` 暂不改。它仍按 `bucket_id` 逐点调用 `ResolveBucketEvents()`。原因是 event calendar 的瓶颈在事件日历查找，而不是 `sin()` / `cos()` 计算；即使批量构建日历特征，每个 bucket 的事件命中结果仍然相互独立，不能通过 Fourier 递推优化。后续如果有性能需求，可以对连续 bucket 做 event range query 或批量命中索引，而不是逐点查找。

### 8.3 BootstrapEngine sequence 使用 batch feature

`BootstrapEngine::PredictValueSequence()` / `PredictRatioSequence()` 调整为：

```text
validate artifact once
resolve delta / timezone / orders once
for each chunk:
  BuildFormalFeatureBatch(...)
  for each bucket:
    context.bucket_id = bucket_id
    PredictFormalModelWithFeature(...)
    fill BootstrapPrediction
```

Value / Ratio 的序列构建逻辑仍可继续复用当前已抽出的公共骨架；本优化只替换 formal prediction 的内部 feature path。

---

## 9. 配置策略

本优化首版不新增 YAML 配置。

原因：

- `daily_harmonic_order` / `weekly_harmonic_order` 已经是业务配置，决定特征维度。
- 本文的 `chunk_size`、`reanchor_interval`、目标 chunk 内存是纯实现参数，不影响预测业务语义。
- 暴露过多性能开关会增加 `baseline-config-template.yaml`、strict schema 和默认值解释负担。

内部默认常量建议：

| 内部常量 | 默认值 | 说明 |
| --- | ---: | --- |
| `kMaxBatchFeatureChunkSize` | `4096` | 每次最多构建 4096 个 bucket 的临时特征。 |
| `kMinBatchFeatureChunkSize` | `256` | 高阶谐波配置下仍保持至少 256 点一个 chunk，避免 chunk 过碎。 |
| `kTargetBatchFourierBytes` | `2 * 1024 * 1024` | 单个 chunk 的 Fourier 数组目标控制在约 2 MiB。 |
| `kFourierReanchorInterval` | `1024` | 连续递推 1024 点后强制直接重算一次 sin/cos。 |

如果后续确实需要把这些参数开放给运维，必须按配置闭环规则处理：

```text
src/plugins/baseline/config/baseline-config-template.yaml
src/plugins/baseline/config/runtime_config.cpp
src/plugins/baseline/rolling/rolling_config.h
src/plugins/baseline/rolling/rolling_config.cpp
src/tests/test_baseline/test_baseline_rolling_config.cpp
```

---

## 10. 性能收益评估

### 10.1 理论收益

以默认日 / 周阶数 6 / 3、分钟级预测 1440 点为例：

| 项目 | 当前 | Level 1 后 | Level 2 后 |
| --- | ---: | ---: | ---: |
| 日 / 周 phase 本地时间解析 | 2880 次 | 1440 次 | 1440 次 |
| 月位置本地时间解析（月位置成熟） | 4320 到 5760 次 | 0 次额外解析 | 0 次额外解析 |
| `sin()` / `cos()` 调用 | 25920 次 | 25920 次 | 约 18 次锚定 + 少量重锚 |
| 小 vector 分配 | 5760 个 | 0 个 per-point 分配 | 0 个 per-point 分配 |

Level 1 的主要收益来自减少 ICU / 本地时间解析和小对象分配。Level 2 的主要收益来自消除三角函数调用。

### 10.2 预期端到端提升

性能收益取决于数据状态：

- 月位置未成熟、UTC 时区、预测点数少于 50：收益有限，主要减少 vector 分配和少量 trig。
- 非 UTC 时区、月位置成熟、预测 200 点以上：收益明显。
- 1440 点分钟级预测：特征构建自身预计可降低到原来的 20% 到 40%；端到端 Rolling 序列预测预计提升 1.5x 到 3x。
- 如果当前环境 ICU 调用是主瓶颈，月位置成熟路径可能达到更高收益。

是否能「大幅提升」需要用本仓库测试数据实测确认。理论上可以显著降低 CPU，但最终端到端速度还受 JSON 输出、event calendar、prediction vector 构建和调用方处理影响。

### 10.3 性能代价

- 临时内存：默认阶数下 chunk 约 1 MB 级；高阶谐波下通过动态 chunk size 把 Fourier 数组控制在约 2 MiB。
- 代码复杂度：新增 feature batch 和 view-based overload，接口数量增加。
- 数值测试成本：需要维护单点旧路径与批量新路径的等价测试。
- DST 测试成本：需要覆盖 `America/New_York` 等有夏令时切换的时区。

---

## 11. 线程与竞态评估

本方案不引入新的共享可变状态：

- `RollingFeatureBatch` 是函数局部对象。
- `LocalCalendarFeature` 是值对象。
- Fourier 递推状态是构建 batch 时的局部变量。
- 现有 `thread_local UCalendar` 继续按线程隔离。

因此不会引入新的线程竞态。

需要注意的是：如果上游违反 B6 的同 task 非并发调用契约，让 submit 与 predict 同时进入同一个 task，当前 Baseline task state 本身就存在一致性风险。本优化不扩大也不修复这个边界。

---

## 12. 实施任务

### 12.1 日历特征抽取

修改：

```text
src/plugins/baseline/model/calendar_feature_helper.h
src/plugins/baseline/model/calendar_feature_helper.cpp
```

任务：

- [ ] 增加 `LocalCalendarFeature`。
- [ ] 增加 `ResolveOneLocalCalendarFeature()`。
- [ ] 复用现有 UTC 快路径和 `thread_local UCalendar`。
- [ ] 保持现有 `PhaseDayLocal()`、`PhaseWeekLocal()`、`DayOfMonthLocal()` 等接口不变，并可内部改为复用新 helper。

### 12.2 Rolling feature batch

新增：

```text
src/plugins/baseline/rolling/rolling_feature_batch.h
src/plugins/baseline/rolling/rolling_feature_batch.cpp
```

修改：

```text
src/plugins/baseline/rolling/rolling_estimator.h
src/plugins/baseline/rolling/rolling_estimator.cpp
```

任务：

- [ ] 定义 `RollingFeatureView` / `RollingFeatureBatch`。
- [ ] 实现 `BuildRollingFeatureBatch()`。
- [ ] 实现直接锚定和递推两种路径。
- [ ] 增加 view-based harmonic evaluate / variance helper。
- [ ] 增加 `PredictRollingForecastStateWithFeature()`，保留旧接口兼容。

### 12.3 Rolling sequence 接入

修改：

```text
src/plugins/baseline/rolling/rolling_task_runner.cpp
src/plugins/baseline/rolling/rolling_task_runner.h
src/plugins/baseline/rolling/monthpos_state.h
src/plugins/baseline/rolling/monthpos_state.cpp
```

任务：

- [ ] 增加 `BuildFusionForecastEstimatorWithFeature()`。
- [ ] 增加 `PredictRollingForStateWithFeature()`。
- [ ] 增加 `EvaluateRollingMonthposWithFeature()`。
- [ ] `PredictRollingSequenceForSeries()` 按 chunk 构建 feature batch。
- [ ] 单点 `PredictRollingForSeries()` 保持旧调用方式，避免无关风险。

### 12.4 Bootstrap sequence 接入

修改：

```text
src/plugins/baseline/model/formal_predictor.h
src/plugins/baseline/model/formal_predictor.cpp
src/plugins/baseline/bootstrap/bootstrap_engine.cpp
```

任务：

- [ ] 增加 `PredictFormalModelWithFeature()` overload。
- [ ] 拆出 `EvaluateCoreWithFeature()`。
- [ ] 拆出 `EvaluateMonthPosWithFeature()`。
- [ ] `PredictValueSequence()` / `PredictRatioSequence()` 使用 batch feature。
- [ ] 保持单点 `PredictValue()` / `PredictRatio()` 行为不变。

### 12.5 构建系统

修改：

```text
src/plugins/baseline/CMakeLists.txt
src/tests/test_baseline/CMakeLists.txt
```

任务：

- [ ] 新增 `rolling_feature_batch.cpp` 到 Baseline 插件目标。
- [ ] 新增或挂接 feature batch 等价测试。

---

## 13. 测试矩阵

### 13.1 单元等价测试

新增或扩展测试：

```text
src/tests/test_baseline/test_baseline_rolling_feature_batch.cpp
```

覆盖：

- UTC，`bucket_seconds = 60`，连续 1440 点。
- `Asia/Shanghai`，`bucket_seconds = 300`，连续 288 点。
- `America/New_York`，覆盖 DST 春季跳时和秋季回拨：
  - 2026-03-08 01:59:59 EST -> 03:00:00 EDT，本地 02:00 到 02:59 不存在，验证 `local_wall_second` 跳变触发重锚。
  - 2026-11-01 01:59:59 EDT -> 01:00:00 EST，本地 01:00 到 01:59 重复，验证 `local_wall_second` 回拨触发重锚。
- `daily_harmonic_order = 0` / `weekly_harmonic_order = 0`。
- 高阶配置，例如 daily 24、weekly 24。

断言：

- batch feature 与逐点 `BuildRollingFeatureVector()` 等价。
- `LocalCalendarFeature` 与现有 `DayOfMonthLocal()`、`DaysToMonthEndLocal()`、`IsLastWeekdayOfMonthLocal()` 等价。

### 13.2 Rolling 预测等价测试

扩展：

```text
src/tests/test_baseline/test_baseline_plugin.cpp
src/tests/test_baseline/test_baseline_link_rolling_eval.cpp
```

覆盖：

- 同一状态下，单点 `PredictRolling(bucket)` 与批量 `PredictRolling(start, count)` 对应点输出一致。
- 带 bootstrap seed 的 fusion forecast 路径一致。
- 无 bootstrap seed 的 rolling forecast fallback 路径一致。
- monthpos ready 后输出一致。
- sequence 包含 past bucket 时，past bucket 仍返回 `kInvalidArgument`，future bucket 正常预测。

### 13.3 Bootstrap 预测等价测试

扩展：

```text
src/tests/test_baseline/test_baseline_task_headers.cpp
src/tests/test_baseline/test_baseline_link_bootstrap_eval.cpp
```

覆盖：

- Value formal model：单点预测与序列预测对应点一致。
- Ratio formal model：单点预测与序列预测对应点一致。
- monthpos enabled / disabled。
- event calendar enabled 时，事件命中结果不因 feature batch 改造变化。

### 13.4 性能验证

建议在现有 eval summary 中增加可选耗时字段，不作为硬断言：

```json
{
  "rolling_sequence_predict_ms": 0.0,
  "bootstrap_sequence_predict_ms": 0.0,
  "prediction_point_count": 1440
}
```

验证命令：

```bash
cmake --build /mnt/d/working/flowSQL/build --target test_baseline_link_rolling_eval
/mnt/d/working/flowSQL/build/output/test_baseline_link_rolling_eval --output-dir /mnt/d/working/flowSQL/build/output

cmake --build /mnt/d/working/flowSQL/build --target test_baseline_link_bootstrap_eval
/mnt/d/working/flowSQL/build/output/test_baseline_link_bootstrap_eval --output-dir /mnt/d/working/flowSQL/build/output
```

输出必须放在 `build/output`，不要写 `/tmp`。

---

## 14. 风险与缓解

| 风险 | 说明 | 缓解 |
| --- | --- | --- |
| DST 语义错误 | 本地时间跳变时递推步长不再等于 `bucket_seconds` | 使用 `local_wall_second` 检测，不连续即重锚 |
| 数值漂移 | 长序列递推累计浮点误差 | 每 1024 点重锚，测试约束误差 |
| 内存尖峰 | `point_count` 极大或谐波阶数很高 | 动态 chunk 化，按 Fourier 数组目标内存收缩，临时缓存不跨 chunk |
| 接口复杂度上升 | 新增 view-based overload | 单点旧接口保留，批量路径才使用 view |
| Bootstrap / Rolling 两套实现漂移 | 两边都需要 calendar / Fourier feature | calendar feature 放 model 层，rolling batch 可被 formal predictor 适配复用 |
| Event calendar 未优化 | Bootstrap 序列仍逐点查事件 | 明确列为非目标，后续单独评估 event batch index |

---

## 15. 验收标准

- [ ] Rolling 批量预测不再在每个 future bucket 内调用 `BuildRollingFeatureVector()`。
- [ ] Bootstrap 批量预测不再在每个 future bucket 内重复调用 `PhaseDayLocal()` / `PhaseWeekLocal()`。
- [ ] 非 UTC 时区下，每个 bucket 最多一次本地日历解析。
- [ ] 默认配置下，1440 点序列预测的 `sin()` / `cos()` 调用数量从每点 18 次降为锚定点级别。
- [ ] 单点预测接口行为不变。
- [ ] Rolling / Bootstrap 的批量预测结果与单点逐点结果等价。
- [ ] DST 边界测试通过。
- [ ] `test_baseline_link_rolling_eval` 和 `test_baseline_link_bootstrap_eval` 使用批量预测接口，并输出到 `build/output`。

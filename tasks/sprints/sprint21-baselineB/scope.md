# Sprint 21 BaselineB 目标与范围

## 1. 背景

`Sprint 19` 的基线方案已经基本实现，但测试和方案复盘暴露出一个核心问题：旧方案把正式基线建模为一段历史窗口上的固定模型，在线阶段主要做评分、漂移证据和 `shadow baseline` 桥接；当数据水平发生持续变化时，需要通过 `shadow baseline -> candidate validate -> baseline rebuild -> formal switch` 才能恢复正式基线。

这条链路带来几个问题：

- `shadow baseline` 进入后主要依赖正式重建成功退出，容易长期停留在临时状态。
- 正式重建需要足够长的新阶段历史，历史不足时会导致正式基线长时间不可用或不可信。
- `HistoryReader`、`candidate model`、`holdout validation`、`full model switch` 等慢路径状态机复杂，且不适合新上线系统没有历史数据的场景。
- FlowSQL 是批流一体平台，但基线系统不能把批处理历史作为可用性前置条件；历史数据只能是锦上添花。

因此，`Sprint 21 BaselineB` 的目标不是继续修补旧的重建链路，而是重新定义基线生命周期：以流式在线学习为主路径，批处理历史只作为可选初始化能力。

---

## 2. 总体目标

BaselineB 的一句话目标：

```text
基线系统必须是 stream-first、self-maturing 的在线基线；batch 历史只作为可选 bootstrap，加速成熟，不作为可用性前置条件。
```

展开为以下原则：

1. **无历史可启动**  
   被检测系统刚上线、没有任何历史数据时，基线任务仍必须能进入流式学习状态，不能等待批处理历史。

2. **有历史可加速**  
   若启动时一次性提交了历史数据，历史数据只用于初始化在线状态、提高初始成熟度、生成初始 `T3` basis；它不是后续在线恢复能力的依赖。

3. **流式是唯一持续更新路径**  
   进入流式处理后，每个有效 bucket 都按在线状态进行预测、评分、门控更新和成熟度推进。后续不再依赖批处理重建。

4. **能力按成熟度解锁**  
   基线组件按 `level -> day -> week -> monthpos -> T3 stable head` 逐步成熟。未成熟组件不得参与高置信异常判断。

5. **置信度必须表达成熟度**  
   输出必须显式携带 `maturity`、`enabled_components`、`confidence` 等证据，避免把冷启动期结果伪装成成熟基线。

6. **基线必须是 band，不是 point**  
   基线输出不能只是一条中心预测线 `baseline_mu`。每次预测都必须同时给出正常波动条带（baseline band），即 `baseline_lower`、`baseline_upper` 或等价的不确定性表达。异常判断应基于观测值相对条带的位置和距离，而不是只比较观测值与中心点的差值。

7. **老方案降级为可选启动器**  
   旧方案中有价值的历史拟合能力保留为 `Optional Bootstrap Engine`，但 `shadow baseline`、`candidate model`、正式重建和切换验证链路退出主路径。

---

## 3. 文档定位与范围

本文档是 `Sprint 21 BaselineB` 的目标与范围说明，不作为后续代码实现的详细设计文档。

后续每个阶段应独立编写聚焦的阶段设计文档，避免把 `B1/B2/B3/B4` 的算法、接口、测试细节都堆入一个大而全的总设计。

### 3.1 本轮要定下来的内容

- BaselineB 的总体目标和生命周期。
- 三层架构：`Online Rolling Core`、`Maturity Gate`、`Optional Bootstrap`。
- 无历史、弱历史、完整历史 3 类启动方式。
- 老方案迁移成 `Optional Bootstrap Engine` 的保留项和删除项。
- `T1/T2/T3` 在新框架下的职责边界。

### 3.2 本轮暂不展开的内容

以下内容后续单独细化：

- `T1/T2` 在线滚动模型的具体数学形式。
- 简化 Kalman/RLS 的状态块、协方差近似和更新公式。
- 异常门控、漂移适配和遗忘因子的阈值。
- `monthpos` 在线成熟与更新规则。
- `T3` basis 在线统计、低频刷新和 warm-up handover 细节。
- 状态持久化格式、API 契约和测试矩阵。

---

## 4. 统一术语

### 4.1 Series

`Series = (key, feature)`，表示一个独立建模单元。

所有在线状态、成熟度、基线来源、`T3` routed 摘要特征，都必须最终收口到明确的 `Series` 身份上。

### 4.2 Online Rolling Core

在线滚动核心，负责每个有效 bucket 的主路径：

```text
predict -> score -> gate_update -> update_state -> update_maturity
```

它是基线生命周期的主引擎。只要流式数据继续到达，`Online Rolling Core` 就必须能持续服务和学习。

### 4.3 Maturity Gate

成熟度门控，负责判断当前 `Series` 的哪些组件已经可用于预测、评分和高置信异常输出。

成熟度不是展示字段，而是算法门控的一部分。未成熟组件可以学习，但不能直接贡献高置信异常结论。

### 4.4 Baseline Band

基线条带（Baseline Band）是 `Online Rolling Core` 对当前 bucket 的正常范围预测。

每次在线预测至少应形成以下语义：

```text
baseline_mu_t
baseline_lower_t
baseline_upper_t
band_width_t
uncertainty_source
```

其中：

- `baseline_mu_t`：中心预测值，不单独代表完整基线。
- `baseline_lower_t / baseline_upper_t`：当前成熟度、残差尺度和状态不确定性共同决定的正常范围。
- `band_width_t`：条带宽度，可用于解释当前预测是否稳定。
- `uncertainty_source`：条带主要由残差尺度、参数不确定性、低成熟度、低样本支撑或 `T2` 分母不足等因素扩大。

设计约束：

- 冷启动和低成熟阶段，band 应更宽，confidence 应更低。
- 历史完整或在线状态稳定时，band 可以收敛，但不得收窄到忽略正常波动。
- 异常分数应基于观测值穿出 band 的程度，而不是简单使用 `|y_t - baseline_mu_t|`。
- `Optional Bootstrap` 输出的 `sigma_init`、`uncertainty_init` 和成熟度信息，应直接影响初始 band。

### 4.5 Optional Bootstrap Engine

可选启动器，由旧基线方案中可复用的批处理历史拟合能力改造而来。

它只在任务启动时可选执行一次，输出 `BootstrapSeed`，不参与后续在线重建。

### 4.6 BootstrapSeed

批处理历史或弱初始化产生的在线状态种子。

```text
BootstrapSeed = {
  seed_status,
  coverage_report,
  enabled_components,
  theta_init,
  uncertainty_init,
  sigma_init,
  maturity_init,
  t3_basis_init?,
  diagnostics
}
```

字段语义：

- `seed_status`：`none`、`weak`、`partial`、`full`
- `coverage_report`：历史覆盖情况，如有效 bucket 数、覆盖天数、覆盖周数、覆盖月数
- `enabled_components`：由历史数据支撑、可初始启用的组件
- `theta_init`：在线模型的初始参数
- `uncertainty_init`：初始不确定性，历史越少，不确定性越高
- `sigma_init`：初始残差尺度或尺度下限
- `maturity_init`：初始成熟度
- `t3_basis_init`：可选的 `T3` 初始 support / stable head / head prototype
- `diagnostics`：解释 seed 质量和降级原因

`BootstrapSeed` 不是正式服务模型。正式服务对象是流式阶段不断演化的 `RollingState`。

---

## 5. 三层架构

### 5.1 Layer 1：Online Rolling Core

`Online Rolling Core` 是 BaselineB 的主路径。

职责：

- 接收每个有效 `Observation` 或 routed 摘要特征。
- 基于上一时刻状态预测当前 bucket。
- 计算残差、异常分数和解释证据。
- 根据异常门控决定正常更新、降权更新或跳过更新。
- 在持续偏移时调节学习速度，推动基线逐步追踪新状态。
- 更新残差尺度、覆盖率、成熟度和可观测统计。

原则：

- 不能依赖 `HistoryReader`。
- 不能依赖后续批处理重建。
- 不能在热路径中执行全窗口优化。
- 状态量必须有界，适配高基数 `Series`。

### 5.2 Layer 2：Maturity Gate

`Maturity Gate` 管理能力解锁。

建议的成熟度阶段：

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

语义：

- `cold_learning`：只学习或输出极低置信结果。
- `level_ready`：可使用当前水平和残差尺度，识别极端突刺。
- `daily_ready`：可使用日周期解释和评分。
- `weekly_ready`：可使用周周期解释和评分。
- `monthly_ready`：可使用月位置相关解释和评分。

成熟度推进由覆盖度、有效样本数、稳定性和组件可解释性共同决定。具体阈值后续设计。

### 5.3 Layer 3：Optional Bootstrap

`Optional Bootstrap` 是启动加速器，不是在线可用性的依赖。

输入可能为空，也可能是一次性批处理历史数据。

启动方式分 3 类：

```text
no_history_start:
  无历史，直接 InitFromEmpty()

weak_history_start:
  有少量历史，只初始化 level / scale / weak maturity

full_history_bootstrap:
  有足够历史，初始化 day / week / monthpos / T3 basis 等更多组件
```

进入流式阶段后，不再提交批处理历史，也不再执行批处理重建。

当历史数据完整时，`Optional Bootstrap` 的价值不是生成一个长期冻结的服务模型，而是把批处理训练得到的稳定结构直接映射成 `Online Rolling Core` 的初始状态。至少以下输出可以被在线核心直接消费：

```text
level_0                  -> 在线水平状态初值
trend_0                  -> 在线趋势状态初值
day_sin/cos_coeffs       -> 日周期 harmonic 状态初值
week_sin/cos_coeffs      -> 周周期 harmonic 状态初值
sigma_0 / sigma_floor    -> 初始残差尺度和尺度下限
uncertainty_0            -> 初始参数可信度 / 初始学习速度依据
maturity_init            -> 初始成熟度
enabled_components       -> 初始启用组件
monthpos_coeffs          -> 月位置组件 seed（若历史覆盖足够）
t3_basis_init            -> T3 初始 support / stable head / head prototype
```

这些参数带来的直接优势：

- 起点更准：流式第一批 bucket 的 `y_hat_t` 已经接近历史基线，不需要从空状态慢慢摸索。
- 波动尺度更准：初始 `sigma_0` 能让早期异常分数和置信区间更稳定，减少冷启动误报。
- 周期结构更准：日周期、周周期和月位置不需要等流式数据重新跑满完整周期后才可用。
- 成熟度更高：完整历史可以让任务直接进入 `daily_ready`、`weekly_ready` 或 `monthly_ready`，而不是停在 `cold_learning`。
- T3 结构更早可用：完整历史可以直接生成 `stable_head` 和 `head_proto_q`，使 stable head 相关摘要特征从启动期就具备解释基础。

因此，完整历史的 bootstrap 是准确性加速器：它提升在线模型的初始预测、初始尺度和初始成熟度，但不改变“进入流式后由 `Online Rolling Core` 持续学习”的主路径。

---

## 6. 启动流程

### 6.1 无历史启动

当没有批处理历史时：

```text
RollingState.InitFromEmpty(task_spec)
state.maturity = cold_learning
state.enabled_components = []
```

流式数据到达后，系统逐步学习：

```text
stream bucket
  -> update level / scale candidates
  -> reach level_ready
  -> accumulate day coverage
  -> reach daily_ready
  -> accumulate week coverage
  -> reach weekly_ready
  -> accumulate month coverage
  -> reach monthly_ready
```

无历史是正常启动方式，不是异常路径。

### 6.2 弱历史启动

当历史不足一个完整学习周期时：

```text
BootstrapSeed.seed_status = weak | partial
```

允许初始化：

- level 初值
- 初始残差尺度
- 部分覆盖率统计
- 低成熟度状态

不允许因为历史不足而生成高置信日 / 周 / 月周期结论。

### 6.3 完整历史启动

当历史覆盖足够完整周期时：

- 2 周左右历史可初始化日周期和周周期。
- 2 个月左右历史可初始化月位置相关组件。
- 更长或更完整的历史可以提高初始成熟度和降低初始不确定性。

这些只是初始条件。进入流式后，在线状态继续更新。

---

## 7. T1 / T2 / T3 职责边界

### 7.1 T1 / T2

`T1 / T2` 是 `Online Rolling Core` 的主要承载对象。

目标：

- 支持无历史启动。
- 支持从弱 seed 或完整 seed 初始化。
- 支持 level / trend / day / week 的在线滚动更新。
- 支持 monthpos 的 stream-only 成熟目标，但具体算法后续细化。

本轮只确定方向：

```text
T1/T2 不再以固定离线模型作为长期服务主体。
T1/T2 的服务主体是持续演化的 RollingState。
```

### 7.2 月位置组件

月位置不是 batch-only 能力。

新目标：

- 有足够 batch 历史时，batch 可生成月位置 seed。
- 没有 batch 历史时，月位置从流式数据中逐步 `warming -> ready`。
- 月位置未成熟时，不参与高置信异常判断。
- 月位置在线学习必须慢、保守、强门控，避免被单月异常污染。

具体月位置状态、门控和更新公式后续单独设计。

### 7.3 T3

`T3` 不单独实现一套新的时间基线。

目标：

- `T3` 摘要特征继续路由到 `T1 / T2`，复用 `Online Rolling Core`。
- `T3` 初始 support / stable head / head prototype 可由 batch bootstrap 提供。
- 无 batch 历史时，`T3` 先输出通用形状特征，stable head 相关能力随在线统计成熟后启用。
- 后续 `T3` basis 刷新必须支持 stream-only 模式，通过有界在线统计和低频保守切换完成。

本轮不展开 `T3` basis 刷新算法。

---

## 8. 老方案迁移策略

### 8.1 迁移目标

老方案不再作为 `Baseline Engine`，而是改造成：

```text
Optional Bootstrap Engine
```

新主引擎是：

```text
Online Rolling Core
```

旧方案中所有服务于“固定模型失配后重建恢复”的逻辑，都不再属于主路径。

### 8.2 保留能力

保留以下有价值能力：

1. **历史窗口特征拟合**  
   用历史数据估计初始 level、trend、day、week、monthpos、event 等参数。其中 level、trend、day harmonic、week harmonic 可以直接作为 `Online Rolling Core` 的状态初值。

2. **稳健训练**  
   保留 Huber / Ridge / 分阶段拟合等方法，用于降低历史异常对 seed 的污染。

3. **覆盖率与成熟度评估**  
   判断历史是否足以启用某个组件，并输出降级原因。

4. **T2 比例类初始化**  
   使用历史 numerator / denominator 初始化比例类特征的初始中心、尺度和 profile 信息。

5. **T3 初始 basis**  
   使用历史分布生成初始 `support_explicit`、`stable_head`、`head_proto_q`。

6. **初始残差尺度**  
   输出 `sigma_init`、`sigma_floor`、初始置信度和诊断信息。

7. **初始不确定性与学习速度提示**  
   根据历史覆盖度和拟合质量输出 `uncertainty_init` 或等价信息。历史越完整、拟合越稳定，在线核心的初始不确定性越低；历史不足或质量较差时，在线核心应以更高不确定性和更快学习速度进入流式阶段。

完整历史下，老方案到新核心的直接映射关系如下：

```text
β0 / level                 -> level_0
k / trend                  -> trend_0
daily sin/cos coefficients -> day harmonic state
weekly sin/cos coefficients-> week harmonic state
residual sigma             -> sigma_0 / sigma_floor
coverage / sample quality  -> maturity_init / enabled_components
parameter reliability      -> uncertainty_0 / learning_rate_hint
monthpos coefficients      -> monthpos seed
T3 support/stable head     -> T3 basis seed
```

这份映射是 `Optional Bootstrap Engine` 的核心产物。它只负责给在线核心一个更准的起点，不负责后续在线生命周期管理。

### 8.3 删除或移出主路径

以下能力不进入 BaselineB 主路径：

```text
shadow baseline 作为漂移桥接层
candidate model
candidate vs incumbent holdout validation
HistoryReader.fetch 作为在线恢复依赖
RebuildRequest
full model 再训练后正式切换
incumbent = shadow baseline replay
rebuild_blocked / insufficient_data 长期桥接状态
```

删除原因：

- 它们服务于旧假设：固定基线失配后，需要重新训练正式模型才能恢复。
- BaselineB 的新假设是：在线 rolling state 本身负责持续适应。
- 历史不足不能阻塞在线服务，因此不应保留依赖历史重建的恢复链路。

### 8.4 保留但降级的概念

部分概念可以保留名称或诊断意义，但必须改语义：

- `formal model`：不再表示长期冻结服务模型，后续若保留，只能表示某次 bootstrap seed 的来源版本。
- `model_version`：用于追踪 seed 或状态格式版本，不表示必须通过重建切换。
- `baseline_provider`：默认应为 `rolling`；`bootstrap` 只表示初始来源。
- `history_reader`：不属于主路径。若未来保留，只能作为离线评估或人工回放工具，不参与在线可用性。

---

## 9. 输出语义

`DetectorResult` 或等价输出必须体现成熟度与来源。

建议新增或在 `evidence` 中稳定暴露：

```text
maturity
enabled_components
bootstrap_status
rolling_state_age
effective_confidence
baseline_provider
component_readiness
baseline_mu
baseline_lower
baseline_upper
band_width
uncertainty_source
```

语义要求：

- 冷启动期可以输出学习状态，但不能输出高置信数学异常。
- 未启用组件不得参与高置信评分。
- 有历史 seed 时，输出要能解释 seed 质量。
- 基线必须按条带输出；`baseline_mu` 只是中心线，不能作为完整基线语义。
- `baseline_lower / baseline_upper` 是异常判断和解释的主依据。
- 当 maturity 低、分母支撑不足、残差尺度不稳定或状态不确定性高时，band 必须变宽或 confidence 必须降低。
- 无历史启动时，输出要能解释当前正在自学习。

---

## 10. 风险与约束

### 10.1 高基数状态边界

BaselineB 的在线状态会常驻在 `Series` 粒度上，必须严格控制状态数量和状态大小。

后续实现必须遵守：

- 状态结构固定有界。
- 清理策略有界、可观测。
- 不允许按高基数 group 动态创建无限 mutex 或无限状态。
- `T3` 在线候选统计必须有 `topN` 上限。

### 10.2 冷启动误报

无历史启动时，最主要风险是过早输出高置信异常。

必须通过成熟度和置信度解决，而不是禁止启动。

### 10.3 月位置在线成熟

月位置周期长、样本少，在线成熟很慢。

设计上必须支持 stream-only 成熟，但实现上要保守：

- 未成熟前禁用高置信评分。
- 成熟阈值高于 day / week。
- 更新速度慢于 level / day / week。

### 10.4 T3 basis 漂移

`T3` basis 变化会改变摘要特征身份，不能每个 bucket 动态变化。

后续刷新必须满足：

- basis 版本化。
- 低频刷新。
- replacement cap。
- warm-up handover。
- 输出 evidence 带 `basis_version`。

---

## 11. 后续待细化主题

后续设计按以下顺序推进：

1. `Online Rolling Core` 的数学规格  
   定义 `T1/T2` 的状态、预测、更新、门控和尺度估计。

2. `Maturity Gate` 的组件成熟规则  
   定义 level、day、week、monthpos 的成熟门槛和 confidence 计算。

3. `Optional Bootstrap Engine` 的输出契约  
   将旧训练输出改造成 `BootstrapSeed`。

4. 老方案删除清单与代码落点  
   明确 `shadow/candidate/rebuild` 相关代码如何下线或隔离。

5. `T3` routed rolling 与 stream-only basis 设计
   阶段设计见 [B4 T3 Routed Rolling and Stream Basis 阶段设计](b4-t3-routed-rolling-and-stream-basis-design.md)。
   定义在线有界统计、低频刷新、warm-up handover 和版本语义。

6. 状态持久化与恢复  
   定义 `RollingState` 的持久化格式、版本兼容和恢复语义。

7. 测试矩阵  
   覆盖无历史启动、弱历史启动、完整历史启动、漂移适应、成熟度推进和 `T3` basis 成熟。

---

## 12. 本 Sprint 的设计结论

`Sprint 21 BaselineB` 的设计结论如下：

1. 基线主路径从“批量训练固定模型”改为“流式在线滚动状态”。
2. 批处理历史从“正式模型来源”改为“可选 bootstrap seed 来源”。
3. 无历史启动是正常路径，不能作为异常降级处理。
4. 旧方案的历史拟合能力保留，`shadow/candidate/rebuild` 链路退出主路径。
5. `T1/T2` 是在线滚动核心的主要承载对象。
6. `T3` 摘要继续路由到 `T1/T2`，basis 支持 stream-only 成熟和低频刷新。
7. 月位置不是 batch-only 能力，但在成熟前必须保守，不参与高置信异常判断。

后续所有算法细节必须服从以上目标：不能再引入任何“缺少历史数据就无法建立或恢复基线”的主路径依赖。

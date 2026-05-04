# Sprint 21 BaselineB 阶段规划

## Sprint 信息

- **Sprint 周期**：Sprint 21 BaselineB
- **设计来源**：
  - [Sprint 21 BaselineB 目标与范围](scope.md)
  - [Sprint 21 BaselineB 参考资料](reference.md)
- **核心目标**：
  - 将基线体系从“批量训练固定模型 + shadow/candidate/rebuild 恢复链路”迁移为 “stream-first、self-maturing 的在线基线”。
  - batch 历史只作为可选 bootstrap，不作为在线可用性前置条件。
  - 基线输出必须是 band，而不是单点预测。

---

## 1. 文档结构约定

`scope.md` 只记录 BaselineB 的目标、边界、术语和迁移方向，不承载阶段级详细设计。

后续每个阶段进入代码实现前，单独编写聚焦的阶段设计文档：

```text
b1-optional-bootstrap-design.md
b2-online-rolling-core-design.md
b3-detection-trust-and-maturity-design.md
b4-relation-routed-rolling-and-stream-basis-design.md
b5-relation-pattern-fusion-design.md
b6-baseline-task-serialization-lock-optimization-design.md
b7-relation-fusion-state-cleanup-design.md
b8-batch-prediction-feature-cache-optimization-design.md
```

阶段设计文档只覆盖当前阶段的接口、算法取舍、迁移步骤、测试矩阵和完成门禁。已关闭阶段的遗留语义不得继续写入后续阶段设计。

旧基线算法方案来自 [Sprint 19 Baseline 设计](../sprint19-baseline/design.md)。该文档体量较大，后续只允许按阶段问题定向参考相关章节，禁止在阶段设计中全文引用或复制。引用时必须明确区分：

- 需要继承的历史训练 / 预测能力。
- 需要删除、隔离或降级的旧生命周期语义。
- 是否进入当前阶段范围。

`shadow baseline`、`candidate model`、`baseline rebuild` 等内容只能作为 `B1` 迁移对象或反例参考，不得重新进入 `B2` 之后的在线主路径设计。

---

## 2. 总体阶段划分

BaselineB 分 5 个阶段推进，每个阶段对应一个独立迭代。阶段之间只通过明确产物衔接，禁止把上一阶段遗留语义带入下一阶段。

```text
B1 Optional Bootstrap Engine
  -> B2 Online Rolling Core MVP
  -> B3 Detection Trust, Band Calibration and Monthly Readiness
  -> B4 Relation Routed Rolling and Stream Basis
  -> B5 Relation Pattern Fusion and Risk Output
```

关键约束：

- `B1` 必须彻底闭合旧基线改造。`B2` 之后不得再处理旧 `shadow/candidate/rebuild` 主路径遗留问题。
- `B2` 只消费同一 task 内部按 `series_key` 持有的内存态 `BootstrapSeed`，不把 seed JSON 或 bootstrap prediction 作为 rolling 初始化主路径，也不回头修改旧训练语义。
- `B3` 只扩展 Online Rolling Core 的检测可信度、band 校准、成熟度和组件解锁，不回头改 bootstrap 训练链路。
- `B4` 接入 Relation block 的 routed summary rolling 训练，并实现 stream-only basis 成熟；不回头恢复旧重建链路，也不实现 Relation 模式融合。
- `B5` 基于 B4 产出的 routed summary rolling result，实现 Relation 同源摘要模式融合、关系风险输出，以及 Bootstrap 侧必要的 fusion metadata；B5 开工前必须先完成独立设计文档。

补充设计：

- [B6 Baseline 同 task 串行调用与锁优化方案](b6-baseline-task-serialization-lock-optimization-design.md)
- [B7 Relation Fusion Runtime State Cleanup 设计](b7-relation-fusion-state-cleanup-design.md)
- [B8 Baseline 批量预测特征缓存与谐波递推优化设计](b8-batch-prediction-feature-cache-optimization-design.md)

---

## 3. B1：Optional Bootstrap Engine

阶段设计：[B1 Optional Bootstrap Engine 阶段设计](b1-optional-bootstrap-design.md)

### 3.1 阶段目标

将旧基线代码彻底改造成 `Optional Bootstrap Engine`。

本阶段完成后，旧基线能力只允许以以下形式存在：

1. 历史数据训练 bootstrap seed。
2. bootstrap model 对未来 bucket 做 band 预测。
3. 导出后续 `Online Rolling Core` 可消费的 `BootstrapSeed`。

旧 `shadow/candidate/rebuild` 不能继续作为在线恢复主路径存在。

### 3.2 范围

必须完成：

- 抽离历史训练能力，形成 `OptionalBootstrapEngine` 或等价模块。
- 提供 `TrainBootstrap` 能力，输入历史 `ValueObservation` / `RatioObservation` 序列，输出 bootstrap model 或 `BootstrapSeed`。
- 提供 `PredictBootstrap(bucket_id)` 能力，输出：

```text
baseline_mu
baseline_lower
baseline_upper
band_width
confidence
uncertainty_source
```

- 保留 `T1/T2` 历史拟合、残差尺度、成熟度和 band 预测能力。
- Relation 只导出初始 `basis seed`，不做未来分布预测。
- 明确删除或隔离旧主路径：
  - `shadow baseline`
  - `candidate model`
  - `candidate validator`
  - `RebuildRequest`
  - `HistoryReader.fetch` 在线恢复依赖
  - `rebuild_blocked`
  - `incumbent shadow replay`

### 3.3 非目标

本阶段不实现：

- Online Rolling 状态递推。
- 无历史自学习。
- 成熟度自动推进。
- Relation routed rolling 接入和 stream-only basis 刷新。

### 3.4 验收标准

- [ ] 完整历史数据可训练出 bootstrap model / `BootstrapSeed`。
- [ ] `BootstrapSeed.seed_status` 由 Bootstrap 自动评价，覆盖 `full/partial/weak/none`，调用方只声明 profile 目标，不直接写成熟度标签。
- [ ] 可对未来 `bucket_id` 调用预测接口，返回 baseline band。
- [ ] 合成数据验证：正常点大多落在 band 内，突刺点可穿出 band。
- [ ] 测试证明 bootstrap 训练 / 预测路径不触发 `shadow/candidate/rebuild` 状态。
- [ ] 代码层不再存在作为在线恢复主路径的旧重建链路。

### 3.5 阶段完成门禁

若 `shadow/candidate/rebuild` 仍以在线恢复主路径存在，本阶段不得标记完成。该问题不得带入 `B2`。

---

## 4. B2：Online Rolling Core MVP

### 4.1 阶段目标

实现新的在线滚动基线主路径，让 `T1/T2` 能在流式数据上持续预测、评分、更新。

### 4.2 范围

必须完成：

- `SubmitObservation(...)`：唯一 public rolling 入口，首次提交时根据同一任务内部持有的 `BootstrapSeed[series_key]` 或空启动语义内部懒初始化。
- `T1/T2` 支持 `level/trend/day/week` 的在线递推更新。
- 首次空启动必须能创建 key 级可学习的 `RollingState`，但冷启动输出必须带低置信和宽 band，避免把少量早期样本包装成成熟基线。
- `B2` 只实现最小冷启动保护，例如 `cold_learning / warming / ready_hint` 这类粗粒度状态、`sigma_floor`、`cold_start_band_scale` 和低 confidence；完整成熟度状态机留给 `B3`。
- 每个有效 bucket 执行：

```text
predict -> band -> score -> gate_update -> update_state
```

- 输出 baseline band，而不是只输出中心预测。
- 强异常点支持跳过或降权更新。
- `gate_update` 在 `B2` 中只表示异常污染防护更新门，例如强异常跳过或降权更新；它不是 `B3` 的 `Maturity Gate`。
- 高基数 `Series` 状态有明确边界和清理策略。

### 4.3 非目标

本阶段不实现：

- 月位置在线成熟。
- 完整 `Maturity Gate`。
- Relation routed rolling 接入和 stream-only basis。
- 任何旧 `shadow/candidate/rebuild` 逻辑。

### 4.4 验收标准

- [ ] 无历史数据时，`T1/T2` 可启动并进入在线学习。
- [ ] 使用 `B1` 的 seed 时，初始预测和 band 明显优于空启动。
- [ ] level shift 后，模型能逐步追踪新水平，不依赖 shadow/rebuild。
- [ ] 输出包含 `baseline_mu / baseline_lower / baseline_upper / band_width / confidence`。
- [ ] 高基数状态数量、清理次数和当前状态规模可观测。

---

## 5. B3：Detection Trust, Band Calibration and Monthly Readiness

### 5.1 阶段目标

实现检测可信度门控、band 校准、成熟度推进和月位置在线成熟，让基线不仅能持续学习，还能明确说明什么时候 `Z-score` 可以用于异常判定。

### 5.2 范围

必须完成：

- 引入检测可信度状态，与学习成熟度分离：

```text
score_untrusted
  -> score_warming
  -> score_ready
```

  `drift_learning` / `recalibrating` 作为临时检测降级状态，用于水平变化和 band 重新校准期间。
- 实现成熟度状态：

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

- 区分 learning confidence 与 score trust：
  - learning confidence 表示 rolling state 的学习程度。
  - score trust 表示当前 `Z-score` / 异常判定是否可信。
  - 冷启动、warming、`drift_learning`、`recalibrating` 阶段不得输出高置信异常结论。
- 实现检测 band 校准：
  - 用 `raw_z` 的平方 EWMA 估计 residual scale，避免 band 被短期平稳段压得过窄。
  - 支持最小 band 宽度或等价 `sigma_floor`。
  - 基于近期 coverage / `|Z|` 分布做保守校准。
  - 区分检测评分 band 与预测展示 band，B3 只保证检测评分 band 的可信度。
- 水平变化期间进入 `drift_learning` 或 `recalibrating`，允许受控学习新水平，但降低异常判定置信度。
- `enabled_components`、`component_readiness`、`uncertainty_source` 输出稳定化。
- 月位置支持 stream-only 成熟：
  - 未成熟前不参与高置信异常判断。
  - 成熟后才进入 band 和评分。
  - 更新速度慢于 `level/day/week`。
- 完整 bootstrap seed 可提高初始成熟度和初始 band 质量，但不能绕过 score trust / coverage 校准直接进入高置信异常判定。

### 5.3 非目标

本阶段不实现：

- Relation routed rolling 接入和 stream-only basis 刷新。
- 长周期自适应 forecast 产品接口、批量 forecast API 或 Rolling 反向改写 Bootstrap artifact；`PredictRolling(series_key, bucket_id)` 只承担基础 Rolling/Bootstrap 融合 forecast view。
- Rolling 反向改写 Bootstrap artifact。
- 新的 batch 重建链路。
- 旧 `shadow/candidate/rebuild` 逻辑。

### 5.4 验收标准

- [ ] 无历史启动时不会输出高置信成熟基线。
- [ ] 冷启动、warming、`drift_learning`、`recalibrating` 阶段不会输出可信异常判定。
- [ ] 覆盖 1 天、1 周、跨月数据后，成熟度按预期推进。
- [ ] 稳定窗口内检测 band 的 coverage 和 `|Z| > 3` / `|Z| > 5` 比例处于合理范围。
- [ ] 水平变化过渡期能进入检测降级状态，并在新水平稳定后恢复 score trust。
- [ ] maturity 低或 score trust 低时 band 更宽、confidence 更低，或 `can_score = false`。
- [ ] 月位置不会被单月短期异常快速污染。
- [ ] 完整 bootstrap seed 可提高初始 maturity 和初始 band 质量，但不阻塞 stream-only 启动，也不能跳过 score trust 校准。

---

## 6. B4：Relation Routed Rolling and Stream Basis

阶段设计：[B4 Relation Routed Rolling and Stream Basis 阶段设计](b4-relation-routed-rolling-and-stream-basis-design.md)

### 6.1 阶段目标

让关系分布类 Relation 也符合 stream-first 目标：Relation 流式 block 到达后，能够被投影为 routed summary observations，并复用 `T1/T2 Online Rolling Core` 完成预测、band、score、更新和 B3 检测可信度；同时，Relation basis 在无历史时可在线保守成熟，有历史时只作为初始 basis seed。

### 6.2 范围

必须完成：

- 为 `IBaselineRelationTask` 设计并实现流式提交路径，输入 `RelationBootstrapBlock` 的在线等价结构，输出 routed summary 的 rolling 结果。
- Relation block 基于当前 basis 投影为稳定的 routed summary observations，包括已在 B1 定义的 value summary 和 ratio summary。
- routed summary 按明确的 `series_key` / `feature_id` / `feature_type` 路由到 `T1/T2 Online Rolling Core`，复用 B2/B3 的 rolling state、band、score trust、maturity 和 forecast 语义。
- 无历史时先输出通用形状特征，并允许这些通用摘要进入 rolling 训练；stable head 相关摘要在 basis 未成熟前不得高置信启用。
- 在线维护有界 basis 统计，不保存无界 group 历史。
- 低频形成 / 刷新 `support_explicit`、`stable_head`、`head_proto_q`。
- 实现 replacement cap、warm-up handover 和 `basis_version` evidence。
- `B1` 导出的 Relation basis seed 只作为可选初始值。
- `B1` 导出的 relation routed summary seed 可用于同 key routed summary 的 rolling 初始化；没有 seed 时必须 stream-only 空启动。

### 6.3 非目标

本阶段不实现：

- 为 Relation 单独实现新的时间序列基线算法。
- Relation 分布整体的长期 forecast 产品接口。
- Relation 摘要特征模式融合、跨 metric 模式合成或 Key 级统一风险输出；这些归入 `B5`。
- 旧 `candidate vs incumbent` 验证。
- 基于 `HistoryReader.fetch` 的正式重建。
- 每个 bucket 动态改变 support / stable head。
- 每个 group 都单独建立无界 rolling baseline；B4 只对 routed summary 建模。

### 6.4 验收标准

- [x] 无 Relation 历史时，Relation 任务可接收流式 block，并输出通用 routed summary 的 rolling band / score / trust。
- [x] 有 `B1` relation basis seed 和 routed summary seed 时，同 key routed summary 可从 seed warm-up，且不能绕过 B3 score trust。
- [x] 在线统计积累后，stable head 相关摘要可进入 warming / ready，并开始参与 routed rolling。
- [x] basis 切换有版本、有 evidence，不破坏摘要特征解释。
- [x] basis 统计有固定上限，不随 group 数无界增长。
- [x] routed summary 的 rolling state 可在 task / series snapshot 中观测到 `basis_version`、summary identity、maturity 和 score trust。
- [x] 旧 rebuild 链路不参与 Relation basis 成熟。

---

## 7. B5：Relation Pattern Fusion and Risk Output

阶段设计：[B5 Relation Pattern Fusion and Risk Output 阶段设计](b5-relation-pattern-fusion-design.md)

### 7.1 阶段目标

补齐 Sprint 19 设计中 Relation 的 fusion 能力：把 B4 产出的 routed summary rolling result 转换为同源摘要特征证据，计算结构性模式分，并输出关系分布层面的风险解释。

B5 只处理 Relation 内部的局部模式融合和关系风险输出，不把 Value / Ratio / Relation 全部合并成全局 Key 级统一风险引擎。全局统一风险引擎如需实现，应作为后续独立阶段。

### 7.2 范围

必须完成：

- 定义 `RelationFusionResult` 或等价输出结构，至少包含：

```text
relation_risk
dominant_single[<=3]
dominant_pattern[<=2]
pattern_scores
```

- 将 routed summary 的 `RollingBaselineResult` 标准化为 fusion evidence：
  - `normalized_score`
  - `confidence`
  - `direction`
  - `persistence`
  - `can_alert` / `score_trust` 降级语义
- 实现 Sprint 19 定义的 Relation v1 局部模式库：
  - `support_escape`
  - `head_concentration`
  - `legacy_head_dilution`
  - `stable_head_mix_shift`
- 实现同一 `(source_series_key, feature_base, metric)` 内的局部模式融合。
- 实现同一 `(source_series_key, feature_base, pattern)` 下的跨 metric 模式合成。
- 输出最小解释信息，说明：
  - 哪些 summary 是主导单特征证据。
  - 哪些 pattern 是主导结构模式。
  - 哪些 metric 参与了该 pattern 的合成。
- Bootstrap 侧补齐 fusion metadata：
  - 记录哪些 metric / summary / pattern 可计算。
  - 记录 pattern 权重和 scope。
  - 记录 basis-scoped summary 对 `basis_version` 的依赖。
  - 不训练新的 fusion 时间序列模型，不把历史 fusion risk 当成 rolling 初始化主路径。

### 7.3 非目标

本阶段不实现：

- 全局 `Risk(Key,t)` 统一融合引擎。
- 业务语义判别，例如攻击、割接、上线或专家规则判断。
- 新的 Relation 时间序列模型。
- 旧 `shadow/candidate/rebuild` 或 `HistoryReader.fetch`。
- Fusion 反向修改 routed rolling state、bootstrap artifact 或 basis。

### 7.4 验收标准

- [x] 单个 routed summary 弱异常不会被机械放大为高风险。
- [x] 多个 summary 对同一结构模式给出一致证据时，pattern score 可显式提级。
- [x] `support_escape`、`head_concentration`、`legacy_head_dilution`、`stable_head_mix_shift` 均有单元测试覆盖。
- [x] `can_alert = false`、`score_untrusted`、basis 未 ready 或 summary 缺测时，fusion evidence 被降权或视为不可用。
- [x] 跨 metric 合成遵循饱和型公式，不因 metric 数量线性无界增长。
- [x] Bootstrap artifact / seed 中包含 B5 所需 fusion metadata，导出 / 导入后语义一致。
- [x] B5 输出可在 Relation source snapshot 中观测，且不替代 routed summary 的原始 rolling result。

---

## 8. 阶段依赖与交付口径

### 8.1 阶段依赖

```text
B1 输出内部 BootstrapSeed[series_key]（bootstrap prediction 仅用于 B1 验证）
  B2 在同一 task 内按 series_key 消费 BootstrapSeed，开发 Online Rolling Core
    B3 扩展 Online Rolling Core 的检测可信度、band 校准、成熟度与月位置能力
      B4 接入 Relation routed summary rolling，并扩展 stream-only basis 能力
        B5 消费 B4 的 routed summary rolling result，形成 Relation pattern fusion 输出
```

### 8.2 交付口径

每个阶段交付时必须包含：

- 设计补充或代码设计文档。
- 明确的接口契约。
- 自动化测试。
- 验证命令和输出证据。
- 阶段回顾，确认是否有遗留语义会污染下一阶段。

### 8.3 禁止跨阶段遗留

以下问题不得跨阶段遗留：

- `B1` 不得遗留旧 `shadow/candidate/rebuild` 主路径。
- `B2` 不得引入新的 batch 依赖。
- `B3` 不得把检测可信度、band 校准或月位置成熟问题推给 Relation 或 bootstrap。
- `B4` 不得恢复 `HistoryReader` 作为 basis 刷新前置条件，也不得绕过 B2/B3 另建 Relation 专用时间基线。
- `B5` 不得回头改变 B4 的 routed rolling 初始化 / 更新主路径，也不得把 fusion 结果回写为单特征 rolling state。

---

## 9. 当前下一步

当前执行进度（更新时间：2026-05-03）：

- `B2：Online Rolling Core MVP` 已完成，Value / Ratio 的 rolling submit、预测、band、gate、状态管理、seed warm-up 和回归测试已接入。
- `B3：Detection Trust, Band Calibration and Monthly Readiness` 已完成，score trust、maturity、detection band calibration、monthpos、snapshot 和配置测试已接入。
- `B4：Relation Routed Rolling and Stream Basis` 已完成，Relation routed summary rolling、stream basis accumulator、basis refresh / handover、routed forecast、source / routed snapshot、配置解析和回归测试已接入。
- `B5-T01：补齐 Relation fusion public ABI` 已完成，public fusion 结果、evidence、pattern、metadata 和 submit / snapshot 字段已接入。
- `B5-T02：实现 relation fusion 核心模块` 已完成，覆盖 evidence 标准化、expected evidence universe、persistence、4 个 Relation v1 pattern、跨 metric 饱和合成和 relation risk 输出。
- `B5-T03：接入 RelationTask submit 热路径` 已完成，fan-out 后更新 fusion，按 task spec metric universe 收集上下文，并在无 routed result / metric 缺测时清理旧 persistence。
- `B5-T04：补齐 Bootstrap fusion metadata` 已完成，artifact / seed JSON 导出导入包含 `relation_fusion_metadata`，旧 artifact 可降级加载。
- `B5-T05：接入 task / series snapshot` 已完成，Relation source snapshot 可观测 `relation_fusion`，routed snapshot 保持底层 rolling 语义。
- `B5-T06：补齐配置模板与解析` 已完成，`relation_fusion` 默认值、strict schema、非法配置校验和配置模板已接入。
- `B5-T07：自动化测试与回归验证` 已完成，覆盖 pattern、trust gate、basis gate、cross metric、snapshot、metadata、缺测清理、负向 evidence 和 metric 同序契约。
- `B5-C00：审查问题修复` 已完成，修复负向 evidence 被过滤、全 metric 缺失时 persistence 不清零、task spec 外 metric 进入 fusion，以及关键负向模式测试缺口。
- `B5-C01：RelationRollingObservation metric 同序契约收口` 已完成，public ABI 注释、B4 / B5 设计文档和错序 metric 回归测试已补齐。
- `B6：Baseline 同 task 串行调用与锁优化` 已进入设计阶段，阶段设计见 [B6 Baseline 同 task 串行调用与锁优化方案](b6-baseline-task-serialization-lock-optimization-design.md)。

当前状态：

- B5 代码实现、设计文档补充和回归测试已完成，当前工作区尚未提交。
- 最近验证命令：
  - `git diff --check`
  - `cmake --build /mnt/d/working/flowSQL/build --target test_baseline`
  - `/mnt/d/working/flowSQL/build/output/test_baseline`

下一步建议：

1. 对当前 B5 diff 做一次最终代码审查，重点看 fusion 输出 schema、metadata 兼容和 RelationTask 热路径锁边界。
2. 审查通过后提交 B5 实现与文档状态更新。
3. B6 实施前先审查上游同 task 串行调度边界，确认同一 task 不并发调用的契约可落地；不要求同一 task 固定线程。

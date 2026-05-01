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
b3-maturity-gate-design.md
b4-t3-stream-basis-design.md
```

阶段设计文档只覆盖当前阶段的接口、算法取舍、迁移步骤、测试矩阵和完成门禁。已关闭阶段的遗留语义不得继续写入后续阶段设计。

旧基线算法方案来自 [Sprint 19 Baseline 设计](../sprint19-baseline/design.md)。该文档体量较大，后续只允许按阶段问题定向参考相关章节，禁止在阶段设计中全文引用或复制。引用时必须明确区分：

- 需要继承的历史训练 / 预测能力。
- 需要删除、隔离或降级的旧生命周期语义。
- 是否进入当前阶段范围。

`shadow baseline`、`candidate model`、`baseline rebuild` 等内容只能作为 `B1` 迁移对象或反例参考，不得重新进入 `B2` 之后的在线主路径设计。

---

## 2. 总体阶段划分

BaselineB 分 4 个阶段推进，每个阶段对应一个独立迭代。阶段之间只通过明确产物衔接，禁止把上一阶段遗留语义带入下一阶段。

```text
B1 Optional Bootstrap Engine
  -> B2 Online Rolling Core MVP
  -> B3 Maturity Gate and Monthly Readiness
  -> B4 T3 Stream-Only Basis
```

关键约束：

- `B1` 必须彻底闭合旧基线改造。`B2` 之后不得再处理旧 `shadow/candidate/rebuild` 主路径遗留问题。
- `B2` 只消费同一 task 内部按 `series_key` 持有的内存态 `BootstrapSeed`，不把 seed JSON 或 bootstrap prediction 作为 rolling 初始化主路径，也不回头修改旧训练语义。
- `B3` 只做成熟度和组件解锁，不回头改 bootstrap 训练链路。
- `B4` 只做 `T3` 的 stream-only basis，不回头恢复旧重建链路。

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
- `T3` 只导出初始 `basis seed`，不做未来分布预测。
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
- `T3` stream-only basis 刷新。

### 3.4 验收标准

- [ ] 完整历史数据可训练出 bootstrap model / `BootstrapSeed`。
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

- `InitRollingFromEmpty(series_key, options)`：无历史启动，作用域为指定 `series_key`。
- `InitRollingFromBootstrap(series_key, options)`：从同一任务内部持有的 `BootstrapSeed[series_key]` 启动，不接收外部 seed 参数。
- `T1/T2` 支持 `level/trend/day/week` 的在线递推更新。
- `InitRollingFromEmpty(series_key, options)` 必须能创建 key 级可学习的 `RollingState`，但冷启动输出必须带低置信和宽 band，避免把少量早期样本包装成成熟基线。
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
- `T3` stream-only basis。
- 任何旧 `shadow/candidate/rebuild` 逻辑。

### 4.4 验收标准

- [ ] 无历史数据时，`T1/T2` 可启动并进入在线学习。
- [ ] 使用 `B1` 的 seed 时，初始预测和 band 明显优于空启动。
- [ ] level shift 后，模型能逐步追踪新水平，不依赖 shadow/rebuild。
- [ ] 输出包含 `baseline_mu / baseline_lower / baseline_upper / band_width / confidence`。
- [ ] 高基数状态数量、清理次数和当前状态规模可观测。

---

## 5. B3：Maturity Gate and Monthly Readiness

### 5.1 阶段目标

实现成熟度门控和月位置在线成熟，让基线能力按覆盖度与稳定性逐步解锁。

### 5.2 范围

必须完成：

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

- confidence 与 maturity 绑定。
- `enabled_components`、`component_readiness`、`uncertainty_source` 输出稳定化。
- 月位置支持 stream-only 成熟：
  - 未成熟前不参与高置信异常判断。
  - 成熟后才进入 band 和评分。
  - 更新速度慢于 `level/day/week`。
- 完整 bootstrap 历史可直接初始化较高成熟度。

### 5.3 非目标

本阶段不实现：

- `T3` stream-only basis 刷新。
- 新的 batch 重建链路。
- 旧 `shadow/candidate/rebuild` 逻辑。

### 5.4 验收标准

- [ ] 无历史启动时不会输出高置信成熟基线。
- [ ] 覆盖 1 天、1 周、跨月数据后，成熟度按预期推进。
- [ ] maturity 低时 band 更宽或 confidence 更低。
- [ ] 月位置不会被单月短期异常快速污染。
- [ ] 完整 bootstrap seed 可提高初始 maturity，但不阻塞 stream-only 启动。

---

## 6. B4：T3 Stream-Only Basis

### 6.1 阶段目标

让关系分布类 `T3` 也符合 stream-first 目标：无历史时可逐步成熟，有历史时只作为初始 basis seed。

### 6.2 范围

必须完成：

- `T3` routed 摘要继续复用 `T1/T2 Online Rolling Core`。
- 无历史时先输出通用形状特征。
- 在线维护有界 basis 统计。
- 低频形成 / 刷新 `support_explicit`、`stable_head`、`head_proto_q`。
- 实现 replacement cap、warm-up handover 和 `basis_version` evidence。
- `B1` 导出的 `T3 basis seed` 只作为可选初始值。

### 6.3 非目标

本阶段不实现：

- 旧 `candidate vs incumbent` 验证。
- 基于 `HistoryReader.fetch` 的正式重建。
- 每个 bucket 动态改变 support / stable head。

### 6.4 验收标准

- [ ] 无 `T3` 历史时，任务可运行并输出通用形状特征。
- [ ] 在线统计积累后，stable head 相关特征可进入 warming / ready。
- [ ] basis 切换有版本、有 evidence，不破坏摘要特征解释。
- [ ] basis 统计有固定上限，不随 group 数无界增长。
- [ ] 旧 rebuild 链路不参与 `T3` basis 成熟。

---

## 7. 阶段依赖与交付口径

### 7.1 阶段依赖

```text
B1 输出内部 BootstrapSeed[series_key]（bootstrap prediction 仅用于 B1 验证）
  B2 在同一 task 内按 series_key 消费 BootstrapSeed，开发 Online Rolling Core
    B3 扩展 Online Rolling Core 的成熟度与月位置能力
      B4 扩展 T3 的 stream-only basis 能力
```

### 7.2 交付口径

每个阶段交付时必须包含：

- 设计补充或代码设计文档。
- 明确的接口契约。
- 自动化测试。
- 验证命令和输出证据。
- 阶段回顾，确认是否有遗留语义会污染下一阶段。

### 7.3 禁止跨阶段遗留

以下问题不得跨阶段遗留：

- `B1` 不得遗留旧 `shadow/candidate/rebuild` 主路径。
- `B2` 不得引入新的 batch 依赖。
- `B3` 不得把月位置成熟问题推给 `T3` 或 bootstrap。
- `B4` 不得恢复 `HistoryReader` 作为 basis 刷新前置条件。

---

## 8. 当前下一步

当前建议在 `B1：Optional Bootstrap Engine` 完成收尾验证后，进入 `B2-T01：rolling public 类型与接口`。

`B2` 编码前必须确认：

1. public ABI 只新增 rolling 类型和 Value / Ratio rolling 接口，不暴露内部 `BootstrapSeed`。
2. `B2` rolling 配置默认值、profile override 和合法性校验已按设计文档第 4 节收口。
3. bucket 时间推进、gap、重复 / 乱序 bucket 的处理规则已按设计文档第 5.1 节收口。
4. `InitRollingFromEmpty(series_key, options)`、`InitRollingFromBootstrap(series_key, options)` 和 task 内批量 warm-up 的作用域、失败语义、`force_reset_existing_state` 行为已按设计文档第 6.1 / 6.2 / 6.4 节收口。
5. B2-T01 会同步补 concrete task stub，确保 public 接口新增后仍可独立编译。
6. 测试矩阵至少覆盖空启动、bootstrap 启动、时间顺序、异常门控、低样本支撑、高基数清理和 relation 非目标边界。

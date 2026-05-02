# Baseline 锁分析与重构输入

## 1. 目标

本文记录当前 baseline 插件中的锁使用情况、并发保护边界、热路径影响和后续重构建议。它不定义新的功能范围，只作为后续锁粒度拆分、rolling 热路径优化和控制面 / 数据面隔离的设计输入。

当前代码的同步策略整体偏向正确性优先：任务表锁粒度基本合理，运行时配置快照设计较干净，主要风险集中在每个 task 的粗粒度 mutex。

## 2. 锁清单

| 同步点 | 位置 | 保护对象 | 当前判断 |
| --- | --- | --- | --- |
| atomic runtime config snapshot | `src/plugins/baseline/config/runtime_config.cpp` | 全局 runtime config | 合理。读路径无锁，通过不可变 snapshot 和 atomic load/store 整体替换 |
| `TaskRegistry::mutex_` | `src/plugins/baseline/task/task_registry.h` | `tasks_`、`next_seq_` | 基本合理。`Snapshot()` 复制 shared_ptr 后释放锁，避免 callback under lock |
| `BaselineTaskBase::mutex_` | `src/plugins/baseline/task/baseline_task_base.h` | `closed_` 和派生 task 的内部状态 | 正确但过粗。热路径、慢路径、导出和 snapshot 共用同一把 task 级锁 |

## 3. 共享状态边界

### 3.1 Runtime config

`runtime_config.cpp` 使用 `std::shared_ptr<const RuntimeConfigState>` 保存不可变配置快照：

```text
SnapshotRef()
  -> atomic_load / atomic_store
  -> readers copy out needed config
```

读路径通过 `Snapshot()` 获取 shared_ptr，再复制目标配置字段，例如 `TryGetBaselineRollingConfigOverride()`。这类配置读取不需要额外 mutex。

当前设计优点：

1. 读路径不阻塞。
2. 写路径整体替换，不会产生半更新配置。
3. snapshot 内容是 `const`，减少读写竞态面。

注意点：

1. `LoadBaselineRuntimeConfigFromYaml()` / `ResetBaselineRuntimeConfig()` 是全局配置替换能力，应确认插件生命周期或管理接口是否允许运行中重载。
2. 如果未来支持运行时重载，已创建 task 是否应感知新配置，需要单独定义契约；当前 rolling submit 每次 resolve config，语义上会读最新全局默认值。

### 3.2 TaskRegistry

`TaskRegistry` 保护全局任务表：

```text
tasks_: task_id -> shared_ptr<BaselineTaskBase>
next_seq_: task id 分配计数
```

当前实现里：

1. `AllocateTaskId()`、`Register()`、`Unregister()`、`Size()` 直接短锁访问。
2. `Snapshot()` 锁内复制 shared_ptr 列表，锁外遍历。
3. `List()` 调用 `Snapshot()` 后锁外执行 callback。

这套边界合理，后续不需要优先重构。

### 3.3 BaselineTaskBase task mutex

`BaselineTaskBase::mutex_` 被 value / ratio / relation task 复用，用于保护：

1. `closed_` 生命周期状态。
2. `artifacts_by_series_`。
3. `seeds_by_series_`。
4. `rolling_states_`（value / ratio task）。
5. bootstrap engine store 相关读写和序列化。

当前 value / ratio 的状态结构是：

```text
Task
  artifacts_by_series_[series_key] -> BootstrapArtifact
  seeds_by_series_[series_key]     -> BootstrapSeed
  rolling_states_[series_key]      -> RollingState
```

也就是说 `rolling_states_` 是单个 task 内的 series 状态表，不同 task 之间不共享同一个 map。

## 4. 问题分析

### 4.1 高：`SubmitObservation()` 全程持有 task 级锁

`BaselineValueTask::SubmitObservation()` 和 `BaselineRatioTask::SubmitObservation()` 在入口加 `std::lock_guard<std::mutex> lock(mutex_)`，随后完整执行 rolling submit。

锁内包含：

1. `EnsureOpenLocked()` 生命周期检查。
2. runtime rolling config resolve。
3. observation adapter。
4. `rolling_states_` 查找、插入和更新。
5. rolling predict / detection band / drift / calibration / score trust / maturity / monthpos。
6. update gate 和 state update。
7. public result 填充。
8. diagnostics 字符串拼接。

影响：

1. 同一个 task 下不同 `series_key` 完全串行。
2. 高基数 key 场景无法利用不同 series 之间的天然独立性。
3. `BuildBandDiagnostics()` 这类字符串构造也在锁内执行，扩大临界区。
4. `PredictRolling()`、snapshot、bootstrap、export 都会和 rolling submit 互相阻塞。

当前 correctness 是成立的：`unordered_map` 和 `RollingState` 都不是线程安全对象，确实需要同步。但 task 粒度锁不适合作为长期热路径方案。

### 4.2 中高：`Bootstrap()` 在 task 锁内执行完整训练

Value / ratio / relation 的 `Bootstrap()` 都在 task mutex 内执行完整训练和 artifact store 更新。训练属于慢路径，但会阻塞同 task 的：

1. rolling submit。
2. rolling predict。
3. bootstrap predict。
4. artifact / seed export。
5. snapshot query。

这保证了 store 一致性，但会让慢路径影响热路径。

建议后续改为三阶段：

```text
短锁：检查 task open、force_replace、series 冲突
锁外：执行 TrainValue / TrainRatio / TrainRelation
短锁：再次检查版本和冲突，提交 artifact / seed / rolling warmup
```

需要额外定义并发 bootstrap 同一 `series_key` 时的冲突语义，例如 version token、in-flight marker 或 last-writer-wins 禁止策略。

### 4.3 中：导出和 snapshot 在 task 锁内完整序列化

`ExportBootstrapArtifactStore()`、`ExportBootstrapSeedStore()`、`QueryRollingTaskSnapshot()` 和 `QueryRollingSeriesSnapshot()` 都在 task 锁内读取 store 并构造 JSON。

影响：

1. 全量 artifact / seed 较大时，序列化会明显拉长锁持有时间。
2. task snapshot 遍历全部 rolling states 时会阻塞 submit。
3. Query 接口本身是控制面读操作，不应长期阻塞数据面更新。

建议：

1. 锁内复制必要的轻量状态或目标 series 状态。
2. 锁外排序和 JSON 序列化。
3. 明确 snapshot 是强一致还是弱一致。大多数观测类 snapshot 可以接受某一时刻的弱一致视图。

### 4.4 中：`PredictRolling()` 和 rolling submit 共用 task 锁

`PredictRolling()` 只读 `rolling_states_` 和 seed store，但因为共用 task mutex，会和 submit 串行。

建议：

1. 如果只预测已存在 state：短锁复制单个 `RollingState`，锁外预测。
2. 如果需要从 seed fallback 预测：短锁复制目标 seed 或明确 seed store 的并发读策略。
3. 更彻底的方案是 per-series 或 shard 级读写锁。

### 4.5 中：插件生命周期对象缺少独立并发保护

`BaselinePlugin` 持有 `std::unique_ptr<TaskRegistry> task_registry_`。`Unload()` 会替换整个 registry，`Stop()` 会 snapshot 并关闭 task。

如果框架保证 `Load()` / `Unload()` / `Stop()` 不与 service API 并发，则当前实现可接受。若不保证，下面路径之间存在潜在竞态：

1. `CreateValueTask()` / `CreateRatioTask()` / `CreateRelationTask()` 访问 `task_registry_`。
2. `QueryServiceSnapshot()` 访问 `task_registry_`。
3. `Unload()` 替换 `task_registry_`。

建议在框架契约中明确插件生命周期串行保证；若没有保证，`BaselinePlugin` 需要 plugin 级生命周期锁或 atomic shared ownership。

## 5. 当前合理点

1. `TaskRegistry::List()` 不在 registry 锁内执行 callback，避免回调重入和长临界区。
2. `Stop()` 先通过 `TaskRegistry::Snapshot()` 获取任务列表，再逐个 `Close()`，没有持有 registry 锁关闭 task。
3. `Close()` 在 task 锁内设置 `closed_`，随后锁外 `Unregister()`，避免 task 锁和 registry 锁长期嵌套。
4. Runtime config 使用不可变 snapshot，读路径没有 mutex 阻塞。

## 6. 重构建议优先级

### P0：先减少锁内非必要工作

1. 给 `RollingSubmitOptions` 增加 `include_diagnostics = false`。
2. `BuildBandDiagnostics()` 只在明确开启时执行。
3. 将 result diagnostics、JSON 构造、字符串拼接等格式化工作从热路径锁内移出。

### P1：拆分 rolling 热路径锁粒度

目标是避免同一 task 下不同 `series_key` 串行。

可选方案：

1. **Shard lock**：固定数量 shard，每个 shard 有 mutex 和局部状态 map。
2. **Series entry lock**：task map 管理 `series_key -> shared_ptr<Entry>`，Entry 内有 `RollingState` 和 mutex。
3. **Striped lock + 全局 map**：map 结构变更用 task lock，单个 state 更新用 striped lock。

建议优先考虑 shard lock，因为它能给锁对象数量设上界，符合高基数 key 的边界控制原则。

### P2：Bootstrap 慢路径锁外训练

将 bootstrap 改成“锁内检查、锁外训练、锁内提交”。同时定义：

1. 同 series 并发 bootstrap 的冲突规则。
2. bootstrap 完成后是否覆盖已有 rolling state。
3. rolling 已经在线学习后，是否允许 bootstrap seed 再 warmup 覆盖 state。

### P3：Snapshot / export 锁内复制、锁外序列化

对查询类接口做统一改造：

1. 锁内复制需要输出的状态。
2. 锁外排序和 JSON writer。
3. 对全量 task snapshot 增加上限或分页规划，避免一次性复制过大。

### P4：明确插件生命周期并发契约

需要在插件框架文档或 baseline 设计中明确：

1. `Load()` / `Unload()` / `Stop()` 是否可能和 service API 并发。
2. 若可能并发，`BaselinePlugin` 必须增加生命周期锁。
3. 若框架保证串行，应在注释或设计文档中写明，不让后续实现重复加锁。

## 7. 目标锁模型草案

后续重构可以朝下面结构收敛：

```text
BaselinePlugin
  plugin_lifecycle_lock?     # 仅当框架不保证生命周期串行时需要
  TaskRegistry
    registry_mutex           # 只保护 task 表和 task id 分配

BaselineTask
  task_lifecycle_mutex       # 保护 closed、artifact/seed store 替换、控制面元数据
  RollingShard[N]
    shard_mutex              # 保护本 shard 的 rolling state map
    states[series_key]       # RollingState
```

热路径目标：

```text
SubmitObservation(series_key)
  1. 短读 task 生命周期 / 配置 / seed 引用
  2. 按 series_key 定位 shard
  3. 只锁对应 shard 更新 RollingState
  4. 锁外生成可选 diagnostics
```

慢路径目标：

```text
Bootstrap(series_key)
  1. 短锁检查
  2. 锁外训练
  3. 短锁提交 artifact / seed
  4. 必要时按 shard 更新或初始化 rolling state
```

## 8. 后续验证建议

重构锁粒度后，至少补以下测试：

1. 同一 task 下不同 `series_key` 并发 `SubmitObservation()` 不 crash、不串状态。
2. 同一 `series_key` 并发 submit 保持确定的更新顺序或明确拒绝无序并发。
3. `SubmitObservation()` 与 `PredictRolling()` 并发读写无数据竞争。
4. `SubmitObservation()` 与 `QuerySeriesSnapshot()` 并发无 crash，snapshot 输出 schema 稳定。
5. `Bootstrap()` 与 rolling submit 并发时，artifact / seed / rolling state 语义符合设计。
6. `Close()` 与 submit / predict / snapshot 并发时返回稳定状态，不访问已释放对象。

